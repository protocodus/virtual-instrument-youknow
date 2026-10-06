#pragma once

#include <algorithm>
#include <array>
#include <cmath>
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

    [[nodiscard]] static Kernel prepare(double rate) noexcept
    {
        Kernel k;
        k.factor = factorForRate(rate);
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
    // timestamp and runs once per local-rate phase, including zero current.
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
            if constexpr (withTemperature)
                return static_cast<float>(shape(input)*gain+noise(input,gain,kelvin));
            if constexpr (addNoise)
                if constexpr (!withTemperature)
                    return static_cast<float>(shape(input) * gain + noise(input, gain));
            return shape(input) * gain;
        }
        inputs[static_cast<std::size_t>(inputWrite)] = input;
        gains[static_cast<std::size_t>(inputWrite)] = gain;
        if constexpr (withTemperature)
            temperatures[static_cast<std::size_t>(inputWrite)] = kelvin;
        float result = 0;
        constexpr int lookBack = delaySamples / 2;
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
        if (k.factor == 4)
        {
            // These phases read the same 48 past samples. Share each ring
            // index/load while retaining every phase's tap accumulation
            // order and double precision. Adjacent quarter-phase lanes allow
            // ordinary compiler SLP; the half phase stays a separate sum.
            // Only reconstruction is batched: the output loop below retains
            // shape/noise callbacks, RNG chronology and phase-zero decimation.
            std::array<double, 2> quarter {};
            double half = 0;
            for (int tap = 0; tap < delaySamples; ++tap)
            {
                const double sample = inputs[static_cast<std::size_t>(
                    (inputWrite-tap+inputRingSize) & (inputRingSize-1))];
                quarter[0] += k.interpolation[1][static_cast<std::size_t>(tap)] * sample;
                quarter[1] += k.interpolation[3][static_cast<std::size_t>(tap)] * sample;
                half += k.interpolation[2][static_cast<std::size_t>(tap)] * sample;
            }
            reconstructed[1] = quarter[0];
            reconstructed[2] = half;
            reconstructed[3] = quarter[1];
        }
        for (int phase = 0; phase < k.factor; ++phase)
        {
            double drive = 0;
            if (phase == 0)
                drive = inputs[static_cast<std::size_t>(
                    (inputWrite - lookBack + inputRingSize) & (inputRingSize - 1))];
            else if (k.factor == 4)
                drive = reconstructed[static_cast<std::size_t>(phase)];
            else
                for (int tap = 0; tap < delaySamples; ++tap)
                    drive += k.interpolation[static_cast<std::size_t>(phase)]
                        [static_cast<std::size_t>(tap)]
                        * inputs[static_cast<std::size_t>(
                            (inputWrite - tap + inputRingSize) & (inputRingSize - 1))];
            // The physical control hold is slow and continuous; reconstruct
            // it linearly at the SAME delayed timestamp as the audio input.
            // Gain is inside the high-rate nonlinear path, so its sidebands
            // also cross the antialias filter. This does not smooth a stored
            // envelope or add a new circuit time constant.
            const double fraction = static_cast<double>(phase) / k.factor;
            const double g = g0 + fraction * (g1 - g0);
            if constexpr (withTemperature)
                outputs[static_cast<std::size_t>(outputWrite)]=static_cast<float>(
                    shape(static_cast<float>(drive))*g+noise(drive,g,t0+fraction*(t1-t0)));
            else if constexpr (addNoise)
                outputs[static_cast<std::size_t>(outputWrite)] = static_cast<float>(
                    shape(static_cast<float>(drive)) * g + noise(drive, g));
            else
                outputs[static_cast<std::size_t>(outputWrite)] =
                    static_cast<float>(shape(static_cast<float>(drive)) * g);
            if (phase == 0)
            {
                const int centre = (k.taps - 1) / 2;
                double y = k.decimation[static_cast<std::size_t>(centre)]
                    * outputs[static_cast<std::size_t>(
                        (outputWrite - centre + outputRingSize) & (outputRingSize - 1))];
                for (int index = 0; index < k.pairCount; ++index)
                {
                    const auto& pair = k.pairs[static_cast<std::size_t>(index)];
                    const int opposite = k.taps - 1 - pair.tap;
                    const double a = outputs[static_cast<std::size_t>(
                        (outputWrite - pair.tap + outputRingSize) & (outputRingSize - 1))];
                    const double b = outputs[static_cast<std::size_t>(
                        (outputWrite - opposite + outputRingSize) & (outputRingSize - 1))];
                    y += pair.weight * (a + b);
                }
                result = static_cast<float>(y);
            }
            outputWrite = (outputWrite + 1) & (outputRingSize - 1);
        }
        inputWrite = (inputWrite + 1) & (inputRingSize - 1);
        return result;
    }
    std::array<float, inputRingSize> inputs {}, gains {};
    std::array<float, inputRingSize> temperatures {};
    std::array<float, outputRingSize> outputs {};
    int inputWrite {}, outputWrite {};
};
}
