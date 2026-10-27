#pragma once

#include "YouKnowChorus.h"
#include "YouKnowOutputSummerAntialias.h"

namespace youknow
{
// The low-grid pre-BBD bilinear approximation changes the drawn filter's
// response (at48k, about-3.25dB at12k). Run the existing physical chorus and
// IC6 static law on the existing output-summer reconstruction grid instead.
// No analogue values, delay coordinates or BBD clock rates change.
// Holters/Parker2018,sections3-4 explain why the surrounding physical filters
// and sufficient numerical sampling are needed to preserve BBD aliasing
// without adding numerical aliases:
// https://www.dafx.de/paper-archive/2018/papers/DAFx2018_paper_12.pdf
//
// One mono interpolator and two output decimators replace the former stereo
// IC6-only island. The qualified VoiceVcaAntialias kernel still contributes
// exactly48 parent samples:24 before the physical process,24 after it.
// Commands are held at the SAME reconstructed timestamp as their input.
// Gate/loading context follows only the24-sample output half afterwards.
// Firmware CPUs and converter events never execute in these local substeps.
struct ChorusRateIsland
{
    using Context = OutputSummerAntialias::Context;
    using Result = OutputSummerAntialias::Result;
    using Kernel = VoiceVcaAntialias::Kernel;
    static constexpr int halfDelay = VoiceVcaAntialias::delaySamples / 2;
    static constexpr int inputSize = VoiceVcaAntialias::inputRingSize;
    static constexpr int outputSize = VoiceVcaAntialias::outputRingSize;
    [[nodiscard]] static int factorForRate(double parentRate, int globalFactor,
                                           bool halved = false) noexcept
    {
        // Reuse the engine's existing prepared 1/2/4 host-rate grids.
        // Ordinary hosts reach 176.4/192 kHz. If an unusually low host needs
        // an unprepared grid, keep the complete legacy chorus/IC6 path:
        // reducing the shared factor would also reduce IC6's existing AA.
        // This avoids two extra 57 KiB support caches per Rack instance.
        // The halved (CircuitRate::Normal) law lands on the next prepared grid
        // down, so it never needs a support cache Max does not already have.
        const int needed=VoiceVcaAntialias::factorForRate(parentRate,halved);
        const int available=std::max(1,4/std::max(1,globalFactor));
        return needed<=available ? needed : 1;
    }
    [[nodiscard]] static Kernel prepare(double parentRate, int globalFactor,
                                        bool halved = false) noexcept
    {
        // The qualified FIR depends only on its integer factor; these rates
        // select the same normalized prototype without changing its cutoff.
        const int factor=factorForRate(parentRate,globalFactor,halved);
        return VoiceVcaAntialias::prepare(factor==4?48000.0:factor==2?96000.0:192000.0);
    }
    struct Controls
    {
        ChorusMode mode { ChorusMode::Off };
        ChorusMode hardwareMode { ChorusMode::Off };
        ChorusTimingProfile timing { ChorusTimingProfile::Shipping };
        float noise {}, calibration {1};
        float transferFraction {}, transferCorrelation {1};
        bool narrow {}, muteDrive {}, clockMute {}, finiteMute {}, correlatedNoise {}, finiteTr5 {};
        bool continuousMuteLoading {};
    };
    // The input ring is mirrored (slot i and i + inputSize always agree), so
    // the interpolator reads a sample `age` frames old at newest - age with no
    // index wrap. The output ring is mirrored too, and stores each frame's
    // left/right pair side by side as doubles (each written from a float, so
    // the value read back is unchanged): one pair load serves both channels.
    // Layout only; the taps, values and accumulation order are unchanged.
    std::array<float,2*inputSize> inputs {};
    std::array<Controls,inputSize> controls {};
    std::array<Context,inputSize> contexts {};
    std::array<double,2*2*outputSize> outputs {};
    int inputWrite {}, outputWrite {}, priming {};

    void writeOutput(int slot, float left, float right) noexcept
    {
        for (const int copy : { slot, slot + outputSize })
        {
            outputs[2*static_cast<std::size_t>(copy)] = left;
            outputs[2*static_cast<std::size_t>(copy)+1] = right;
        }
    }

    void reset() noexcept { *this = {}; }
    void resetWarm(const Controls& current) noexcept
    {
        // A live quality rebuild preserves physical time. Its empty numerical
        // history is covered by the existing zero-gain fade, not by stopping
        // the analogue clock for another24 input samples.
        *this = {};
        priming=halfDelay;
        controls.fill(current);
    }

    template<class Process>
    [[nodiscard]] Result processWith(float input, const Controls& control,
        const Kernel& kernel, Process&& process) noexcept
    {
        if (kernel.factor == 1) return process(input, control);
        constexpr int im = inputSize-1, om = outputSize-1;
        VoiceVcaAntialias::writeRing(inputs, inputWrite, input);
        const float* const newestInput = inputs.data()
            + static_cast<std::size_t>(inputWrite + inputSize);
        controls[static_cast<std::size_t>(inputWrite)] = control;
        const int source = (inputWrite-halfDelay+inputSize)&im;
        const auto& delayed = controls[static_cast<std::size_t>(source)];
        Context context {};
        // Zero history before the first input is not physical elapsed time.
        // Wait for timestamp0 before advancing the free-running chorus. This
        // keeps a cold A/B's LFO/clock chronology aligned after its FIR delay.
        const bool ready = priming >= halfDelay;
        if (!ready) ++priming;
        // Every phase's drive is its own 48-term chain, summed tap by tap in
        // the same order as before; interleaving the phases only lets the
        // independent chains share each sample load.
        std::array<double,VoiceVcaAntialias::maximumFactor> drives {};
        drives[0]=newestInput[-halfDelay];
        for (int tap=0; tap<VoiceVcaAntialias::delaySamples; ++tap)
        {
            const double sample=newestInput[-tap];
            for (int phase=1; phase<kernel.factor; ++phase)
                drives[static_cast<std::size_t>(phase)] += kernel.interpolation[static_cast<std::size_t>(phase)]
                                             [static_cast<std::size_t>(tap)] * sample;
        }
        for (int phase=0; phase<kernel.factor; ++phase)
        {
            const double drive=drives[static_cast<std::size_t>(phase)];
            const auto result = ready
                ? process(static_cast<float>(drive), delayed) : Result {};
            writeOutput((outputWrite+phase)&om, result.left, result.right);
            // The decimator's centre is its phase-zero output sample.
            if (phase==0) context=result.context;
        }
        contexts[static_cast<std::size_t>(inputWrite)] = context;
        Result result {};
        result.context=contexts[static_cast<std::size_t>(source)];
        const int centre=(kernel.taps-1)/2;
        // Frame `age` slots before outputWrite is newestOutput - 2*age.
        const double* const newestOutput = outputs.data()
            + 2*static_cast<std::size_t>(outputWrite + outputSize);
        const double centreWeight=kernel.decimation[static_cast<std::size_t>(centre)];
#if defined(YOUKNOW_VCA_ANTIALIAS_SSE2)
        // Both channels in one register: the same per-channel multiplies and
        // adds as the portable loop below (SSE2 has no fused multiply-add).
        __m128d sum=_mm_mul_pd(_mm_set1_pd(centreWeight),
                               _mm_loadu_pd(newestOutput-2*centre));
        for (int i=0; i<kernel.pairCount; ++i)
        {
            const auto slot=static_cast<std::size_t>(i);
            const __m128d first=_mm_loadu_pd(newestOutput-2*kernel.pairTaps[slot]);
            const __m128d second=_mm_loadu_pd(newestOutput-2*kernel.pairOpposites[slot]);
            sum=_mm_add_pd(sum,_mm_mul_pd(_mm_set1_pd(kernel.pairWeights[slot]),
                                          _mm_add_pd(first,second)));
        }
        std::array<double,2> sums {};
        _mm_storeu_pd(sums.data(),sum);
        result.left=static_cast<float>(sums[0]);
        result.right=static_cast<float>(sums[1]);
#else
        std::array<double,2> sum {
            centreWeight * newestOutput[-2*centre],
            centreWeight * newestOutput[-2*centre+1] };
        for (int i=0; i<kernel.pairCount; ++i)
        {
            const auto slot=static_cast<std::size_t>(i);
            const double* const first=newestOutput-2*kernel.pairTaps[slot];
            const double* const second=newestOutput-2*kernel.pairOpposites[slot];
            for (int channel=0; channel<2; ++channel)
                sum[static_cast<std::size_t>(channel)] += kernel.pairWeights[slot]
                    * (first[channel] + second[channel]);
        }
        result.left=static_cast<float>(sum[0]);
        result.right=static_cast<float>(sum[1]);
#endif
        inputWrite=(inputWrite+1)&im;
        outputWrite=(outputWrite+kernel.factor)&om;
        return result;
    }

    template<class Clip>
    [[nodiscard]] Result process(float input, const Controls& control,
        const Kernel& kernel, Chorus& chorus, Clip&& clip) noexcept
    {
        return processWith(input,control,kernel,[&](float drive,const Controls& c) {
            Result result {};
            chorus.setHardwareModeSelection(c.hardwareMode);
            chorus.process(drive,c.mode,c.noise,result.left,result.right,c.calibration,
                c.narrow,c.muteDrive,true,c.timing,c.clockMute,c.finiteMute,
                c.correlatedNoise,c.transferFraction,c.transferCorrelation,c.finiteTr5,
                c.continuousMuteLoading);
            result.left=clip(result.left);
            result.right=clip(result.right);
            result.context={chorus.wetInputConductanceRatio(),chorus.isWetInputConnected()};
            return result;
        });
    }
};
} // namespace youknow
