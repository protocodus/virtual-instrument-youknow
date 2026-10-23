#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>
#include "YouKnowCompatibility.h"

namespace youknow
{
// The original 80017A BA662 is downstream of the VCF (Roland p.13, p.19
// service levels). Its existing differential-pair law expands bandwidth;
// running that law only at the output grid folds its ultrasonic harmonics.
// https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=19
// Oversampling plus reconstruction/antialias filters is the numerical remedy,
// not another physical pole or a revised headroom/gain calibration:
// https://dafx.de/paper-archive/2024/papers/DAFx24_paper_33.pdf#page=1
//
// Polyphase Kaiser-windowed sinc, 48*M+1 taps, cutoff Fs/2, beta=7.85726
// (the approximately 80dB window convention). The roundtrip delay is exactly
// 48 input samples; the engine reports/pads it. Combined small-signal loss
// stays below .04dB through 20k at44.1k and .01dB at48k. The transition near
// Nyquist is intentional numerical bandlimiting. No ADAA averaging response
// or uncompensated control delay is hidden in the physical amplifier law.
// Select 4x below88.2k,2x below176.4k,1x above: already-high grids retain the
// direct law. These rates/taps are numerical accuracy/cost conventions.
struct VoiceVcaAntialias
{
    static constexpr int delaySamples = 48;
    static constexpr int maximumFactor = 4;
    static constexpr int maximumTaps = delaySamples * maximumFactor + 1;
    static constexpr int inputRingSize = 64;
    static constexpr int outputRingSize = 256;
    struct Kernel
    {
        struct Pair {double weight {}; int tap {};};
        int factor {1};
        int taps {1};
        int pairCount {};
        std::array<std::array<double, delaySamples + 1>, maximumFactor> interpolation {};
        std::array<double, maximumTaps> decimation {};
        std::array<Pair, maximumTaps / 2> pairs {};
    };

    [[nodiscard]] static int factorForRate(double rate) noexcept
    { return rate < 88200.0 ? 4 : rate < 176400.0 ? 2 : 1; }
    // The halved law keeps a local grid of at least 88.2 kHz instead.
    [[nodiscard]] static int factorForRate(double rate, bool halved) noexcept
    {
        const int full = factorForRate(rate);
        return halved && full > 1 ? full / 2 : full;
    }

    [[nodiscard]] static Kernel prepare(double rate) noexcept
    {
        return prepareFactor(factorForRate(rate));
    }

    // The kernel depends only on its integer factor (1, 2 or 4); a caller
    // that selects a lower local rate than factorForRate prepares that one.
    [[nodiscard]] static Kernel prepareFactor(int factor) noexcept
    {
        Kernel k;
        k.factor = factor >= 4 ? 4 : factor >= 2 ? 2 : 1;
        if (k.factor == 1) return k;
        k.taps = delaySamples * k.factor + 1;
        const int centre = (k.taps - 1) / 2;
        const auto i0 = [](double x) noexcept {
            double sum = 1, term = 1;
            for (int n = 1; n < 64; ++n)
            {
                const double r = x / (2 * n);
                term *= r * r;
                sum += term;
                if (term < 1e-18 * sum) break;
            }
            return sum;
        };
        constexpr double beta = 7.85726;
        const double denominator = i0(beta);
        std::array<double, maximumTaps> h {};
        for (int tap = 0; tap <= centre; ++tap)
        {
            const int offset = tap - centre;
            const double x = numbers::pi * offset / k.factor;
            const double ideal = offset == 0 ? 1.0
                : offset % k.factor == 0 ? 0.0 : std::sin(x) / x;
            const double t = static_cast<double>(offset) / centre;
            const double window = i0(beta * std::sqrt(std::max(0.0, 1 - t * t)))
                / denominator;
            h[static_cast<std::size_t>(tap)] = ideal * window;
            h[static_cast<std::size_t>(k.taps - 1 - tap)] = ideal * window;
        }
        // Every reconstructed phase passes DC exactly. The phase-zero sinc
        // is the unchanged delayed input. Decimation uses the same prototype
        // divided by M, so interpolation images and new harmonics are removed.
        for (int phase = 0; phase < k.factor; ++phase)
        {
            double sum = 0;
            for (int tap = phase; tap < k.taps; tap += k.factor)
                sum += h[static_cast<std::size_t>(tap)];
            for (int tap = phase; tap < k.taps; tap += k.factor)
            {
                const double value = h[static_cast<std::size_t>(tap)] / sum;
                k.interpolation[static_cast<std::size_t>(phase)]
                    [static_cast<std::size_t>(tap / k.factor)] = value;
                k.decimation[static_cast<std::size_t>(tap)] = value / k.factor;
            }
        }
        // Keep the symmetric accumulation exact and skip analytic zeros
        // without divisions/branches in the hot decimation loop.
        for (int tap = 0; tap < centre; ++tap)
        {
            const int opposite = k.taps - 1 - tap;
            const double weight = .5 * (k.decimation[static_cast<std::size_t>(tap)]
                + k.decimation[static_cast<std::size_t>(opposite)]);
            k.decimation[static_cast<std::size_t>(tap)] = weight;
            k.decimation[static_cast<std::size_t>(opposite)] = weight;
            if (weight != 0)
                k.pairs[static_cast<std::size_t>(k.pairCount++)] = {weight, tap};
        }
        return k;
    }

    void reset() noexcept { *this = {}; }
    // Keep context warm when a live diagnostic noise selection is disabled;
    // process/processWithOutputNoise still advance this same input cursor.
    void storeTemperatureContext(float kelvin) noexcept
    { temperatures[static_cast<std::size_t>(inputWrite)]=kelvin; }

    template <class Shape>
    [[nodiscard]] float process(float input, float gain, const Kernel& k,
                                Shape&& shape) noexcept
    {
        return processCore<false>(input, gain, k, shape,
            [](double, double) noexcept { return 0.0; }, 0.0f);
    }
    // Additional output current/signal AFTER shape*gain, in the same output
    // coordinate. The callback receives the same reconstructed drive/gain
    // timestamp and runs after shape once per local-rate phase, including
    // zero current. Separate statements make that callback order portable;
    // the operands of shape(...)*gain + noise(...) are not sequenced by +.
    template <class Shape, class OutputNoise>
    [[nodiscard]] float processWithOutputNoise(float input, float gain,
        const Kernel& k, Shape&& shape, OutputNoise&& noise) noexcept
    {
        return processCore<true>(input, gain, k, shape, noise, 0.0f);
    }
    // Slow temperature context uses the SAME history timestamp as the held
    // gain. This is numerical alignment for a T-dependent current source;
    // it adds neither a physical thermal pole nor a different FIR delay.
    template <class Shape, class OutputNoise>
    [[nodiscard]] float processWithTemperatureNoise(float input,float gain,float kelvin,
        const Kernel& k,Shape&& shape,OutputNoise&& noise) noexcept
    {
        return processCore<true,true>(input,gain,k,shape,noise,kelvin);
    }
    template <bool addNoise, bool withTemperature=false, class Shape, class OutputNoise>
    [[nodiscard]] float processCore(float input, float gain, const Kernel& k,
        Shape&& shape, OutputNoise&& noise,float kelvin) noexcept
    {
        if (k.factor == 1) {
            const auto shaped = shape(input);
            if constexpr (withTemperature) {
                const auto outputNoise = noise(input, gain, kelvin);
                return static_cast<float>(shaped * gain + outputNoise);
            } else if constexpr (addNoise) {
                const auto outputNoise = noise(input, gain);
                return static_cast<float>(shaped * gain + outputNoise);
            } else return shaped * gain;
        }
        inputs[static_cast<std::size_t>(inputWrite)] = input;
        inputs[static_cast<std::size_t>(inputWrite + inputRingSize)] = input;
        gains[static_cast<std::size_t>(inputWrite)] = gain;
        if constexpr (withTemperature)
            temperatures[static_cast<std::size_t>(inputWrite)] = kelvin;
        float result = 0;
        constexpr int lookBack = delaySamples / 2;
        // The mirrored second half makes the last inputRingSize inputs one
        // contiguous descending window behind this pointer: recent[-t] is the
        // input t samples ago, with no wrap mask per tap.
        const float* const recent = inputs.data() + inputWrite + inputRingSize;
        const double g0 = gains[static_cast<std::size_t>(
            (inputWrite - lookBack + inputRingSize) & (inputRingSize - 1))];
        const double g1 = gains[static_cast<std::size_t>(
            (inputWrite - lookBack + 1 + inputRingSize) & (inputRingSize - 1))];
        double t0=0,t1=0;
        if constexpr (withTemperature) {
            t0=temperatures[static_cast<std::size_t>((inputWrite-lookBack+inputRingSize)&(inputRingSize-1))];
            t1=temperatures[static_cast<std::size_t>((inputWrite-lookBack+1+inputRingSize)&(inputRingSize-1))];
        }
        std::array<double, maximumFactor> reconstructed {};
        // Every reconstructed phase is a 48-tap dot product over the same past
        // samples. Share each ring load across the phases, and give each phase
        // four interleaved partial sums instead of one 48-term dependency
        // chain: the same taps and double products, with only the rounding of
        // the final additions changed (2026-10-16 CPU pass). Only
        // reconstruction is batched: the output loop below retains shape/noise
        // callbacks, RNG chronology and phase-zero decimation.
        const auto reconstruct = [&](auto phaseCount) noexcept {
            constexpr int phases = decltype(phaseCount)::value;
            std::array<std::array<double, 4>, phases> partial {};
            for (int tap = 0; tap < delaySamples; tap += 4)
                for (int lane = 0; lane < 4; ++lane)
                {
                    const double sample = recent[-(tap + lane)];
                    for (int phase = 0; phase < phases; ++phase)
                        partial[static_cast<std::size_t>(phase)][static_cast<std::size_t>(lane)]
                            += k.interpolation[static_cast<std::size_t>(phase + 1)]
                                   [static_cast<std::size_t>(tap + lane)] * sample;
                }
            for (int phase = 0; phase < phases; ++phase)
            {
                const auto& sum = partial[static_cast<std::size_t>(phase)];
                reconstructed[static_cast<std::size_t>(phase + 1)] =
                    (sum[0] + sum[1]) + (sum[2] + sum[3]);
            }
        };
        static_assert(delaySamples % 4 == 0);
        if (k.factor == 4)
            reconstruct(std::integral_constant<int, 3>{});
        else
            reconstruct(std::integral_constant<int, 1>{});
        for (int phase = 0; phase < k.factor; ++phase)
        {
            const double drive = phase == 0
                ? static_cast<double>(recent[-lookBack])
                : reconstructed[static_cast<std::size_t>(phase)];
            // The physical control hold is slow and continuous; reconstruct
            // it linearly at the SAME delayed timestamp as the audio input.
            // Gain is inside the high-rate nonlinear path, so its sidebands
            // also cross the antialias filter. This does not smooth a stored
            // envelope or add a new circuit time constant.
            const double fraction = static_cast<double>(phase) / k.factor;
            const double g = g0 + fraction * (g1 - g0);
            const auto shaped = shape(static_cast<float>(drive));
            float produced;
            if constexpr (withTemperature) {
                const auto outputNoise = noise(drive, g, t0 + fraction * (t1 - t0));
                produced = static_cast<float>(shaped * g + outputNoise);
            } else if constexpr (addNoise) {
                const auto outputNoise = noise(drive, g);
                produced = static_cast<float>(shaped * g + outputNoise);
            } else
                produced = static_cast<float>(shaped * g);
            outputs[static_cast<std::size_t>(outputWrite)] = produced;
            outputs[static_cast<std::size_t>(outputWrite + outputRingSize)] = produced;
            if (phase == 0)
            {
                const int centre = (k.taps - 1) / 2;
                // As for the inputs: recentOutput[-t] is the output t local
                // samples ago, read without a wrap mask per tap.
                const float* const recentOutput =
                    outputs.data() + outputWrite + outputRingSize;
                // The same symmetric pair products, in four partial sums.
                std::array<double, 4> partial {};
                const auto pairTerm = [&](int index) noexcept {
                    const auto& pair = k.pairs[static_cast<std::size_t>(index)];
                    const int opposite = k.taps - 1 - pair.tap;
                    const double a = recentOutput[-pair.tap];
                    const double b = recentOutput[-opposite];
                    return pair.weight * (a + b);
                };
                int index = 0;
                for (; index + 4 <= k.pairCount; index += 4)
                    for (int lane = 0; lane < 4; ++lane)
                        partial[static_cast<std::size_t>(lane)] += pairTerm(index + lane);
                for (; index < k.pairCount; ++index)
                    partial[0] += pairTerm(index);
                const double y = k.decimation[static_cast<std::size_t>(centre)]
                    * recentOutput[-centre]
                    + ((partial[0] + partial[1]) + (partial[2] + partial[3]));
                result = static_cast<float>(y);
            }
            outputWrite = (outputWrite + 1) & (outputRingSize - 1);
        }
        inputWrite = (inputWrite + 1) & (inputRingSize - 1);
        return result;
    }
    // inputs and outputs are rings kept twice over: every write lands at
    // [cursor] and [cursor + ringSize], so the ring's last ringSize samples
    // are always one contiguous block ending at [cursor + ringSize]. The FIR
    // loops read that block through a plain pointer instead of masking every
    // tap index; the first half remains a valid ring for any masked reader.
    std::array<float, 2 * inputRingSize> inputs {};
    std::array<float, inputRingSize> gains {};
    std::array<float, inputRingSize> temperatures {};
    std::array<float, 2 * outputRingSize> outputs {};
    int inputWrite {}, outputWrite {};
};
}
