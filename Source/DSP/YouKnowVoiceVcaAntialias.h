#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>
#include "YouKnowCompatibility.h"

// The two FIR dot products below have an SSE2 form on x86-64 GCC/Clang. It
// issues the same IEEE double multiplies and adds, lane for lane and in the
// same order, as the portable loops; SSE2 has no fused multiply-add, so the
// results are bit-identical to the scalar form on that target. Other targets
// (including arm64, whose compilers may contract the scalar expressions into
// FMAs) keep the portable loops, and with them their established rounding.
#if !defined(YOUKNOW_EMBEDDED_TARGET) && defined(__x86_64__) && defined(__SSE2__)
#define YOUKNOW_VCA_ANTIALIAS_SSE2 1
#include <emmintrin.h>
#endif

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
        // `opposite` is the mirrored tap, taps - 1 - tap, kept beside it so
        // the decimation loop reads both ages without recomputing it.
        struct Pair {double weight {}; int tap {}; int opposite {};};
        int factor {1};
        int taps {1};
        int pairCount {};
        std::array<std::array<double, delaySamples + 1>, maximumFactor> interpolation {};
        std::array<double, maximumTaps> decimation {};
        std::array<Pair, maximumTaps / 2> pairs {};
        // The same interpolation phases 1..M-1, with each aligned group of
        // four taps stored in reverse: entry tap + j holds the weight of tap
        // tap + 3 - j. That matches the ascending memory order of the four
        // input samples aged tap + 3 ... tap, so the SIMD loop loads both
        // with straight vector loads. A layout copy, not a new kernel.
        std::array<std::array<double, delaySamples>, maximumFactor - 1>
            reversedInterpolation {};
        // The decimation pairs again, as separate weight and tap columns.
        std::array<double, maximumTaps / 2> pairWeights {};
        std::array<int, maximumTaps / 2> pairTaps {};
        std::array<int, maximumTaps / 2> pairOpposites {};
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
            {
                const auto index = static_cast<std::size_t>(k.pairCount++);
                k.pairs[index] = {weight, tap, opposite};
                k.pairWeights[index] = weight;
                k.pairTaps[index] = tap;
                k.pairOpposites[index] = opposite;
            }
        }
        for (int phase = 1; phase < k.factor; ++phase)
            for (int tap = 0; tap < delaySamples; tap += 4)
                for (int lane = 0; lane < 4; ++lane)
                    k.reversedInterpolation[static_cast<std::size_t>(phase - 1)]
                        [static_cast<std::size_t>(tap + lane)] =
                        k.interpolation[static_cast<std::size_t>(phase)]
                            [static_cast<std::size_t>(tap + 3 - lane)];
        return k;
    }

    void reset() noexcept { *this = {}; }
    // Keep context warm when a live diagnostic noise selection is disabled;
    // process/processWithOutputNoise still advance this same input cursor.
    void storeTemperatureContext(float kelvin) noexcept
    { writeRing(temperatures, inputWrite, kelvin); }

    // Each ring is stored twice, back to back: slot i and slot i + size
    // always hold the same value. A sample `age` slots older than the cursor
    // is then slot cursor + size - age for every age in 0..size, so the FIR
    // loops below read straight runs of memory instead of wrapping every
    // index through a mask. Only the storage layout changes: the values,
    // taps and accumulation order are the same as a single masked ring.
    // Whole-ring fill() and reset() keep both copies equal by construction;
    // a single-slot writer must go through writeRing.
    // The output ring holds doubles: every stored value is still a float
    // (each writer rounds to float first), widened once here instead of at
    // each of its many FIR reads. The value read back is unchanged.
    template <class Sample, std::size_t Stored>
    static void writeRing(std::array<Sample, Stored>& ring, int cursor,
                          float value) noexcept
    {
        static_assert(Stored % 2 == 0);
        ring[static_cast<std::size_t>(cursor)] = static_cast<Sample>(value);
        ring[static_cast<std::size_t>(cursor) + Stored / 2] =
            static_cast<Sample>(value);
    }

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
        writeRing(inputs, inputWrite, input);
        writeRing(gains, inputWrite, gain);
        if constexpr (withTemperature)
            writeRing(temperatures, inputWrite, kelvin);
        float result = 0;
        constexpr int lookBack = delaySamples / 2;
        // Age a of each input ring is newest - a (see writeRing).
        const std::size_t newestInput =
            static_cast<std::size_t>(inputWrite + inputRingSize);
        const float* const newestSample = inputs.data() + newestInput;
        const double g0 = gains[newestInput - lookBack];
        const double g1 = gains[newestInput - lookBack + 1];
        double t0=0,t1=0;
        if constexpr (withTemperature) {
            t0=temperatures[newestInput - lookBack];
            t1=temperatures[newestInput - lookBack + 1];
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
#if defined(YOUKNOW_VCA_ANTIALIAS_SSE2)
            // Lanes (3, 2) and (1, 0) of the portable loop's four partial
            // sums, per phase: the four samples aged tap + 3 ... tap are one
            // ascending float load, widened in two halves.
            // Plain arrays: std::array<__m128d> drops the vector type's
            // attributes (GCC -Wignored-attributes).
            __m128d high[phases];
            __m128d low[phases];
            for (int phase = 0; phase < phases; ++phase)
                high[phase] = low[phase] = _mm_setzero_pd();
            for (int tap = 0; tap < delaySamples; tap += 4)
            {
                const __m128 samples = _mm_loadu_ps(newestSample - tap - 3);
                const __m128d oldest = _mm_cvtps_pd(samples);
                const __m128d newest = _mm_cvtps_pd(_mm_movehl_ps(samples, samples));
                for (int phase = 0; phase < phases; ++phase)
                {
                    const double* weights = k.reversedInterpolation[
                        static_cast<std::size_t>(phase)].data() + tap;
                    high[phase] = _mm_add_pd(
                        high[phase],
                        _mm_mul_pd(_mm_loadu_pd(weights), oldest));
                    low[phase] = _mm_add_pd(
                        low[phase],
                        _mm_mul_pd(_mm_loadu_pd(weights + 2), newest));
                }
            }
            for (int phase = 0; phase < phases; ++phase)
            {
                // (sum[0] + sum[1]) + (sum[2] + sum[3]) of the portable form;
                // each register holds its two lanes in descending order.
                std::array<double, 2> lanes10 {}, lanes32 {};
                _mm_storeu_pd(lanes10.data(), low[phase]);
                _mm_storeu_pd(lanes32.data(), high[phase]);
                reconstructed[static_cast<std::size_t>(phase + 1)] =
                    (lanes10[1] + lanes10[0]) + (lanes32[1] + lanes32[0]);
            }
#else
            std::array<std::array<double, 4>, phases> partial {};
            for (int tap = 0; tap < delaySamples; tap += 4)
                for (int lane = 0; lane < 4; ++lane)
                {
                    const double sample = newestSample[-(tap + lane)];
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
#endif
        };
        static_assert(delaySamples % 4 == 0);
        if (k.factor == 4)
            reconstruct(std::integral_constant<int, 3>{});
        else
            reconstruct(std::integral_constant<int, 1>{});
        for (int phase = 0; phase < k.factor; ++phase)
        {
            const double drive = phase == 0
                ? static_cast<double>(newestSample[-lookBack])
                : reconstructed[static_cast<std::size_t>(phase)];
            // The physical control hold is slow and continuous; reconstruct
            // it linearly at the SAME delayed timestamp as the audio input.
            // Gain is inside the high-rate nonlinear path, so its sidebands
            // also cross the antialias filter. This does not smooth a stored
            // envelope or add a new circuit time constant.
            const double fraction = static_cast<double>(phase) / k.factor;
            const double g = g0 + fraction * (g1 - g0);
            const auto shaped = shape(static_cast<float>(drive));
            if constexpr (withTemperature) {
                const auto outputNoise = noise(drive, g, t0 + fraction * (t1 - t0));
                writeRing(outputs, outputWrite,
                    static_cast<float>(shaped * g + outputNoise));
            } else if constexpr (addNoise) {
                const auto outputNoise = noise(drive, g);
                writeRing(outputs, outputWrite,
                    static_cast<float>(shaped * g + outputNoise));
            } else
                writeRing(outputs, outputWrite,
                    static_cast<float>(shaped * g));
            if (phase == 0)
            {
                const int centre = (k.taps - 1) / 2;
                // The same symmetric pair products, in four partial sums.
                std::array<double, 4> partial {};
                const double* const newestOutput = outputs.data()
                    + static_cast<std::size_t>(outputWrite + outputRingSize);
                const auto pairTerm = [&](int index) noexcept {
                    const auto slot = static_cast<std::size_t>(index);
                    const double a = newestOutput[-k.pairTaps[slot]];
                    const double b = newestOutput[-k.pairOpposites[slot]];
                    return k.pairWeights[slot] * (a + b);
                };
                int index = 0;
#if defined(YOUKNOW_VCA_ANTIALIAS_SSE2)
                {
                    __m128d lanes01 = _mm_setzero_pd();
                    __m128d lanes23 = _mm_setzero_pd();
                    const auto pairSums = [&](int first) noexcept {
                        const auto slot = static_cast<std::size_t>(first);
                        const __m128d a = _mm_loadh_pd(
                            _mm_load_sd(newestOutput - k.pairTaps[slot]),
                            newestOutput - k.pairTaps[slot + 1]);
                        const __m128d b = _mm_loadh_pd(
                            _mm_load_sd(newestOutput - k.pairOpposites[slot]),
                            newestOutput - k.pairOpposites[slot + 1]);
                        return _mm_mul_pd(_mm_loadu_pd(k.pairWeights.data() + slot),
                                          _mm_add_pd(a, b));
                    };
                    for (; index + 4 <= k.pairCount; index += 4)
                    {
                        lanes01 = _mm_add_pd(lanes01, pairSums(index));
                        lanes23 = _mm_add_pd(lanes23, pairSums(index + 2));
                    }
                    _mm_storeu_pd(partial.data(), lanes01);
                    _mm_storeu_pd(partial.data() + 2, lanes23);
                }
#else
                for (; index + 4 <= k.pairCount; index += 4)
                    for (int lane = 0; lane < 4; ++lane)
                        partial[static_cast<std::size_t>(lane)] += pairTerm(index + lane);
#endif
                for (; index < k.pairCount; ++index)
                    partial[0] += pairTerm(index);
                const double y = k.decimation[static_cast<std::size_t>(centre)]
                    * newestOutput[-centre]
                    + ((partial[0] + partial[1]) + (partial[2] + partial[3]));
                result = static_cast<float>(y);
            }
            outputWrite = (outputWrite + 1) & (outputRingSize - 1);
        }
        inputWrite = (inputWrite + 1) & (inputRingSize - 1);
        return result;
    }
    std::array<float, 2 * inputRingSize> inputs {}, gains {};
    std::array<float, 2 * inputRingSize> temperatures {};
    std::array<double, 2 * outputRingSize> outputs {};
    int inputWrite {}, outputWrite {};
};
}
