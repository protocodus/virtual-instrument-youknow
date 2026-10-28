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
        bool clockSourceSpread {}, inverterSpread {}, largeSignalWetSwitch {};
        bool summerLegSpread {};
    };
    // Mirrored rings, as VoiceVcaAntialias keeps them: each write lands at
    // [cursor] and [cursor + size], so the FIR taps read one contiguous
    // window ending at [cursor + size] without a wrap mask per tap.
    std::array<float,2*inputSize> inputs {};
    std::array<Controls,inputSize> controls {};
    std::array<Context,inputSize> contexts {};
    std::array<std::array<float,2*outputSize>,2> outputs {};
    int inputWrite {}, outputWrite {}, priming {};

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
        inputs[static_cast<std::size_t>(inputWrite)] = input;
        inputs[static_cast<std::size_t>(inputWrite+inputSize)] = input;
        controls[static_cast<std::size_t>(inputWrite)] = control;
        const int source = (inputWrite-halfDelay+inputSize)&im;
        // recentInput[-t] is the input t parent samples ago.
        const float* const recentInput = inputs.data()+inputWrite+inputSize;
        const auto& delayed = controls[static_cast<std::size_t>(source)];
        Context context {};
        // Zero history before the first input is not physical elapsed time.
        // Wait for timestamp0 before advancing the free-running chorus. This
        // keeps a cold A/B's LFO/clock chronology aligned after its FIR delay.
        const bool ready = priming >= halfDelay;
        if (!ready) ++priming;
        for (int phase=0; phase<kernel.factor; ++phase)
        {
            double drive=0;
            if (phase==0) drive=inputs[static_cast<std::size_t>(source)];
            else for (int tap=0; tap<VoiceVcaAntialias::delaySamples; ++tap)
                drive += kernel.interpolation[static_cast<std::size_t>(phase)]
                                             [static_cast<std::size_t>(tap)]
                    * recentInput[-tap];
            const auto result = ready
                ? process(static_cast<float>(drive), delayed) : Result {};
            const auto slot = static_cast<std::size_t>((outputWrite+phase)&om);
            outputs[0][slot] = outputs[0][slot+outputSize] = result.left;
            outputs[1][slot] = outputs[1][slot+outputSize] = result.right;
            // The decimator's centre is its phase-zero output sample.
            if (phase==0) context=result.context;
        }
        contexts[static_cast<std::size_t>(inputWrite)] = context;
        Result result {};
        result.context=contexts[static_cast<std::size_t>(source)];
        const int centre=(kernel.taps-1)/2;
        // recentOutput[c][-t] is channel c's output t local samples ago.
        const float* const recentOutput[2] {
            outputs[0].data()+outputWrite+outputSize,
            outputs[1].data()+outputWrite+outputSize };
        std::array<double,2> sum {
            kernel.decimation[static_cast<std::size_t>(centre)] * recentOutput[0][-centre],
            kernel.decimation[static_cast<std::size_t>(centre)] * recentOutput[1][-centre] };
        for (int i=0; i<kernel.pairCount; ++i)
        {
            const auto& pair=kernel.pairs[static_cast<std::size_t>(i)];
            const int opposite=kernel.taps-1-pair.tap;
            for (int channel=0; channel<2; ++channel)
                sum[static_cast<std::size_t>(channel)] += pair.weight
                    * (static_cast<double>(recentOutput[channel][-pair.tap])
                     + static_cast<double>(recentOutput[channel][-opposite]));
        }
        result.left=static_cast<float>(sum[0]);
        result.right=static_cast<float>(sum[1]);
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
                c.continuousMuteLoading,c.clockSourceSpread,c.inverterSpread,
                c.largeSignalWetSwitch,c.summerLegSpread);
            result.left=clip(result.left);
            result.right=clip(result.right);
            result.context={chorus.wetInputConductanceRatio(),chorus.isWetInputConnected()};
            return result;
        });
    }
};
} // namespace youknow
