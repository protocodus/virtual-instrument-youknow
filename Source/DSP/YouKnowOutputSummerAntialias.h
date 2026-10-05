#pragma once

#include "YouKnowVoiceVcaAntialias.h"

namespace youknow
{
// Roland JUNO-106 jack-board IC6, TA75558S, p15. Its existing static
// clipping policy expands bandwidth before the sourced slew/GBW response.
// Direct evaluation on a coarse grid folds ultrasonic harmonics into audio;
// the physical 3MHz/noise-gain pole cannot remove already-folded components.
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=15
// https://datasheet.datasheetarchive.com/originals/scans/Scans-99/DSAIHSC000102822.pdf#page=3
// Numerical reconstruction/antialiasing, not a different analogue knee:
// https://www.dafx.de/paper-archive/2016/dafxpapers/20-DAFx-16_paper_41-PN.pdf
//
// Reuse the separately qualified polyphase kernel (4x/2x/direct, 48 input
// samples roundtrip). L/R retain independent histories. The wet-switch
// conductance belongs to this SAME delayed IC6 timestamp: downstream noise
// gain, bandwidth and resistor-power selection must not see a future gate.
// Context delay is numerical, not another transistor/RC switching delay.
// Product/reference selection is diagnostic, not a host/preset control.
// Changing it resets FIR/context and latency padding, like the VCA selector;
// the disabled path incurs no reconstruction cost. Live quality changes reset
// numerical state at the engine's existing zero-gain rebuild boundary.
struct OutputSummerAntialias
{
    struct Context { double wetRatio {}; bool wetConnected {}; };
    struct Result { float left {}, right {}; Context context {}; };
    VoiceVcaAntialias left {}, right {};
    std::array<Context, VoiceVcaAntialias::inputRingSize> contexts {};
    int write {};

    void reset() noexcept { *this = {}; }

    template<class Shape>
    [[nodiscard]] Result process(float l, float r, Context context,
        const VoiceVcaAntialias::Kernel& kernel, Shape&& shape) noexcept
    {
        contexts[static_cast<std::size_t>(write)] = context;
        const int delay = kernel.factor > 1 ? VoiceVcaAntialias::delaySamples : 0;
        const auto delayed = contexts[static_cast<std::size_t>(
            (write-delay+VoiceVcaAntialias::inputRingSize)
            & (VoiceVcaAntialias::inputRingSize-1))];
        write = (write+1) & (VoiceVcaAntialias::inputRingSize-1);
        return {left.process(l,1,kernel,shape), right.process(r,1,kernel,shape), delayed};
    }

    // IC6 has one fixed unity gain on both channels. Reconstruct the two
    // independent signals together, sharing phase/tap indices and weights.
    // Each lane keeps the general routine's original accumulation order;
    // adjacent double accumulators permit ordinary compiler SLP without
    // forcing a different FMA policy or reducing either channel's precision.
    // Shape still runs left phases first, then right phases, exactly as the
    // two general calls do. Retain even their initial zero gain history and
    // output-ring writes: this changes work, not FIR response or latency.
    template<class Shape>
    [[nodiscard]] Result processStereo(float l, float r, Context context,
        const VoiceVcaAntialias::Kernel& kernel, Shape&& shape) noexcept
    {
        // Direct-rate processing does not advance either FIR cursor. Public
        // independent histories remain supported by the general fallback.
        if (kernel.factor == 1 || left.inputWrite != right.inputWrite
            || left.outputWrite != right.outputWrite)
            return process(l, r, context, kernel, shape);

        contexts[static_cast<std::size_t>(write)] = context;
        const auto delayed = contexts[static_cast<std::size_t>(
            (write-VoiceVcaAntialias::delaySamples+VoiceVcaAntialias::inputRingSize)
            & (VoiceVcaAntialias::inputRingSize-1))];
        write = (write+1) & (VoiceVcaAntialias::inputRingSize-1);

        constexpr int inputMask = VoiceVcaAntialias::inputRingSize - 1;
        constexpr int outputMask = VoiceVcaAntialias::outputRingSize - 1;
        constexpr int lookBack = VoiceVcaAntialias::delaySamples / 2;
        const int inputCursor = left.inputWrite;
        const int outputCursor = left.outputWrite;
        left.inputs[static_cast<std::size_t>(inputCursor)] = l;
        right.inputs[static_cast<std::size_t>(inputCursor)] = r;
        left.gains[static_cast<std::size_t>(inputCursor)] = 1.0f;
        right.gains[static_cast<std::size_t>(inputCursor)] = 1.0f;
        const int gain0 = (inputCursor-lookBack+VoiceVcaAntialias::inputRingSize) & inputMask;
        const int gain1 = (inputCursor-lookBack+1+VoiceVcaAntialias::inputRingSize) & inputMask;
        const double leftGain0 = left.gains[static_cast<std::size_t>(gain0)];
        const double leftGain1 = left.gains[static_cast<std::size_t>(gain1)];
        const double rightGain0 = right.gains[static_cast<std::size_t>(gain0)];
        const double rightGain1 = right.gains[static_cast<std::size_t>(gain1)];

        std::array<std::array<double, 2>, VoiceVcaAntialias::maximumFactor> drives {};
        drives[0] = { left.inputs[static_cast<std::size_t>(gain0)],
                      right.inputs[static_cast<std::size_t>(gain0)] };
        for (int phase = 1; phase < kernel.factor; ++phase)
        {
            auto& drive = drives[static_cast<std::size_t>(phase)];
            for (int tap = 0; tap < VoiceVcaAntialias::delaySamples; ++tap)
            {
                const int sample = (inputCursor-tap+VoiceVcaAntialias::inputRingSize) & inputMask;
                const double weight = kernel.interpolation[static_cast<std::size_t>(phase)]
                                                          [static_cast<std::size_t>(tap)];
                drive[0] += weight * left.inputs[static_cast<std::size_t>(sample)];
                drive[1] += weight * right.inputs[static_cast<std::size_t>(sample)];
            }
        }
        for (int phase = 0; phase < kernel.factor; ++phase)
        {
            const double fraction = static_cast<double>(phase) / kernel.factor;
            const double gain = leftGain0 + fraction * (leftGain1-leftGain0);
            left.outputs[static_cast<std::size_t>((outputCursor+phase) & outputMask)] =
                static_cast<float>(shape(static_cast<float>(drives[static_cast<std::size_t>(phase)][0])) * gain);
        }
        for (int phase = 0; phase < kernel.factor; ++phase)
        {
            const double fraction = static_cast<double>(phase) / kernel.factor;
            const double gain = rightGain0 + fraction * (rightGain1-rightGain0);
            right.outputs[static_cast<std::size_t>((outputCursor+phase) & outputMask)] =
                static_cast<float>(shape(static_cast<float>(drives[static_cast<std::size_t>(phase)][1])) * gain);
        }

        // General processing reads phase-zero decimation before writing
        // phases 1..M-1. Those future slots cannot be in its past window:
        // 48*M+1 <=193 taps, strictly inside the 256-slot output ring.
        const int centre = (kernel.taps - 1) / 2;
        const int centreSample = (outputCursor-centre+VoiceVcaAntialias::outputRingSize) & outputMask;
        const double centreWeight = kernel.decimation[static_cast<std::size_t>(centre)];
        std::array<double, 2> sum {
            centreWeight * left.outputs[static_cast<std::size_t>(centreSample)],
            centreWeight * right.outputs[static_cast<std::size_t>(centreSample)] };
        for (int index = 0; index < kernel.pairCount; ++index)
        {
            const auto& pair = kernel.pairs[static_cast<std::size_t>(index)];
            const int opposite = kernel.taps - 1 - pair.tap;
            const int first = (outputCursor-pair.tap+VoiceVcaAntialias::outputRingSize) & outputMask;
            const int second = (outputCursor-opposite+VoiceVcaAntialias::outputRingSize) & outputMask;
            const double leftFirst = left.outputs[static_cast<std::size_t>(first)];
            const double leftSecond = left.outputs[static_cast<std::size_t>(second)];
            const double rightFirst = right.outputs[static_cast<std::size_t>(first)];
            const double rightSecond = right.outputs[static_cast<std::size_t>(second)];
            sum[0] += pair.weight * (leftFirst+leftSecond);
            sum[1] += pair.weight * (rightFirst+rightSecond);
        }
        left.inputWrite = right.inputWrite = (inputCursor+1) & inputMask;
        left.outputWrite = right.outputWrite = (outputCursor+kernel.factor) & outputMask;
        return { static_cast<float>(sum[0]), static_cast<float>(sum[1]), delayed };
    }
};
}
