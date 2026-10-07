// Verify the actual engine VCA stage against a high-rate physical-law
// reference, not against a separately voiced saturator. Exact-bin projections
// distinguish the wanted harmonics from folded lines; linear/control tests
// also detect hidden response loss, sample-phase errors and envelope delay.
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace { bool guardAllocation = false; unsigned allocations = 0; }
void* operator new(std::size_t n)
{
    if (guardAllocation) ++allocations;
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc {};
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace youknow
{
struct YouKnowTestAccess
{
    static float finish(YouKnowEngine& e, float signal, float gain)
    {
        auto& v = e.voices_[0];
        v.active = true;
        v.vca = gain;
        v.vcaInputTrim = 1;
        return e.finishVoiceFilter(v, signal);
    }
    static auto random(const YouKnowEngine& e)
    { return std::tuple {e.noiseState_, e.noiseGaussianSpare_, e.noiseGaussianSpareValid_}; }
    static void select(YouKnowEngine& e, bool enabled)
    {
        auto p = e.activeParameters_;
        p.enableVoiceVcaAntialias = enabled;
        e.setParameters(p);
    }
    static void fastGate(YouKnowEngine& e)
    {
        auto p = e.activeParameters_;
        p.attack = p.release = 0;
        p.vcaMode = VcaMode::Gate;
        e.setParameters(p);
    }
    static void thermalParameters(YouKnowEngine& e)
    {
        auto p = e.activeParameters_;
        p.calibration = 1;
        p.enableSpatialThermalGradient = false;
        p.enableVoiceVcaTemperature = true;
        e.setParameters(p);
    }
    static void warmup(YouKnowEngine& e, double fraction)
    { e.thermalWarmupFraction_ = static_cast<float>(fraction); }
    static float energy(const YouKnowEngine& e) { return e.voices_[0].energy; }
    static bool allInactive(const YouKnowEngine& e)
    { return std::all_of(e.voices_.begin(), e.voices_.end(), [](const auto& v) {return !v.active;}); }
    static bool qualifiedRebuild(YouKnowEngine& e, int factor)
    {
        e.voices_[0].active = false;
        e.anyVoiceActive_ = false;
        e.oversamplingIdleSamples_ = e.oversamplingQuietSamples_;
        e.rateTransition_ = YouKnowEngine::RateTransition::FadingOut;
        e.rateTransitionGain_ = 0;
        return e.setOversamplingFactor(factor);
    }
    static bool empty(const YouKnowEngine& e)
    {
        const auto& a = e.voices_[0].vcaAntialias;
        return std::all_of(a.inputs.begin(), a.inputs.end(), [](float x) {return x == 0;})
            && std::all_of(a.outputs.begin(), a.outputs.end(), [](float x) {return x == 0;})
            && a.inputWrite == 0 && a.outputWrite == 0;
    }
    static double impulseCentre(YouKnowEngine& e)
    {
        e.firstDecimator_.reset();
        e.secondDecimator_.reset();
        e.refreshLatencyPad();
        e.voices_[0].vcaAntialias.reset();
        const int factor = e.oversampling_;
        double weighted = 0, total = 0;
        for (int frame = 0; frame < 512; ++frame)
        {
            std::array<float, 4> stage {};
            for (int step = 0; step < factor; ++step)
            {
                // Include the existing oscillator reconstruction delay,
                // then exercise the actual local FIR, decimators and pad.
                const float x = frame * factor + step == YouKnowEngine::correctionHalfWidth ? 1.f : 0.f;
                stage[static_cast<std::size_t>(step)] = e.voices_[0].vcaAntialias.process(
                    x, 1, e.voiceVcaAntialiasKernel_, [](float signal) {return signal;});
            }
            float left = 0, right = 0;
            if (factor == 4)
            {
                float a = 0, ar = 0, b = 0, br = 0;
                e.downsamplePair(e.firstDecimator_, stage[0], stage[0], stage[1], stage[1], a, ar);
                e.downsamplePair(e.firstDecimator_, stage[2], stage[2], stage[3], stage[3], b, br);
                e.downsamplePair(e.secondDecimator_, a, ar, b, br, left, right);
            }
            else if (factor == 2)
                e.downsamplePair(e.firstDecimator_, stage[0], stage[0], stage[1], stage[1], left, right);
            else left = right = stage[0];
            e.applyLatencyPad(left, right);
            weighted += frame * static_cast<double>(left);
            total += left;
        }
        return weighted / total;
    }
};
}

namespace
{
using namespace youknow;
constexpr double pi = std::numbers::pi;
void require(bool ok, const char* message) {if (!ok) throw std::runtime_error(message);}
// Frozen pre-batching arithmetic with explicitly sequenced shape/noise calls.
// The original shape(...)*gain + noise(...) did not define which callback ran
// first, so compiler-selected operand order cannot serve as the oracle. Use
// the same prepared kernel to check batching against the original FIR design.
struct FrozenVoiceVcaAntialias : VoiceVcaAntialias
{
    template <bool addNoise, bool withTemperature=false, class Shape, class OutputNoise>
    [[nodiscard]] float processOriginal(float input, float gain, const Kernel& k,
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
        for (int phase = 0; phase < k.factor; ++phase)
        {
            double drive = 0;
            if (phase == 0)
                drive = inputs[static_cast<std::size_t>(
                    (inputWrite - lookBack + inputRingSize) & (inputRingSize - 1))];
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
            const auto shaped = shape(static_cast<float>(drive));
            if constexpr (withTemperature) {
                const auto outputNoise = noise(drive, g, t0 + fraction * (t1 - t0));
                outputs[static_cast<std::size_t>(outputWrite)] =
                    static_cast<float>(shaped * g + outputNoise);
            } else if constexpr (addNoise) {
                const auto outputNoise = noise(drive, g);
                outputs[static_cast<std::size_t>(outputWrite)] =
                    static_cast<float>(shaped * g + outputNoise);
            } else
                outputs[static_cast<std::size_t>(outputWrite)] =
                    static_cast<float>(shaped * g);
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
};
std::unique_ptr<YouKnowEngine> prepared(double rate, bool corrected, bool nonlinear = true)
{
    auto e = std::make_unique<YouKnowEngine>();
    e->prepare(rate, 128, 1);
    EngineParameters p;
    p.enableVoiceVcaAntialias = corrected;
    p.enableVoiceVcaSignalSaturation = nonlinear;
    p.calibration = p.aging = p.chorusNoise = p.noiseLevel = 0;
    p.enableCommonVcaNoise = false;
    p.cutoff = p.sustain = 1;
    p.resonance = .2f;
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    e->setParameters(p);
    return e;
}
std::complex<double> projection(const std::vector<float>& values, int bin)
{
    std::complex<double> sum {};
    for (int n = 0; n < static_cast<int>(values.size()); ++n)
        sum += static_cast<double>(values[static_cast<std::size_t>(n)])
            * std::polar(1., -2 * pi * bin * n / values.size());
    return sum * (2. / values.size());
}
std::vector<float> sine(double rate, bool corrected, double amplitude,
                        int bin, int count, bool nonlinear = true)
{
    auto e = prepared(rate, corrected, nonlinear);
    std::vector<float> values(static_cast<std::size_t>(count));
    for (int n = -count; n < count; ++n)
    {
        const float signal = static_cast<float>(amplitude * std::cos(2 * pi * bin * n / count));
        const float y = YouKnowTestAccess::finish(*e, signal, 1);
        if (n >= 0) values[static_cast<std::size_t>(n)] = y;
    }
    return values;
}
void phaseBatchingBitEquivalence()
{
    const auto sameFloat = [](float a, float b) {
        return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
    };
    const auto sameHistory = [&](const VoiceVcaAntialias& a,
                                 const VoiceVcaAntialias& b) {
        return a.inputWrite == b.inputWrite && a.outputWrite == b.outputWrite
            && std::equal(a.inputs.begin(), a.inputs.end(), b.inputs.begin(), sameFloat)
            && std::equal(a.gains.begin(), a.gains.end(), b.gains.begin(), sameFloat)
            && std::equal(a.temperatures.begin(), a.temperatures.end(), b.temperatures.begin(), sameFloat)
            && std::equal(a.outputs.begin(), a.outputs.end(), b.outputs.begin(), sameFloat);
    };
    struct Callbacks
    {
        struct Event
        {
            int kind {};
            std::uint64_t drive {}, gain {}, kelvin {};
            bool operator==(const Event&) const = default;
        };
        std::array<Event, 8> events {};
        std::size_t used {};
        unsigned shapes {}, noises {};
        std::uint32_t noiseState {0x4197d5cb};

        float shape(float drive)
        {
            events[used++] = {1, std::bit_cast<std::uint64_t>(double(drive)), 0, 0};
            // Actual physical law plus a small stateful witness makes
            // startup/zero-gain shape calls part of the observable result.
            return YouKnowEngine::VoiceVcaSignalLaw::shape(drive) + float(++shapes % 7) * .00001f;
        }
        double noise(double drive, double gain, double kelvin)
        {
            events[used++] = {2, std::bit_cast<std::uint64_t>(drive),
                                std::bit_cast<std::uint64_t>(gain),
                                std::bit_cast<std::uint64_t>(kelvin)};
            ++noises;
            noiseState ^= noiseState << 13;
            noiseState ^= noiseState >> 17;
            noiseState ^= noiseState << 5;
            const double normal = double(noiseState) / 4294967295. * 2 - 1;
            // Signal/control/temperature dependent current-noise witness,
            // drawn even behind a closed gain, as the real stage does.
            return normal * 1e-4 * std::sqrt(std::max(0., gain))
                * std::sqrt(kelvin / 298.15) * (1 + .125 * std::tanh(drive / 2.8));
        }
    };
    std::uint32_t random = 0x95ad7823;
    const auto next = [&]() {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        return double(random) / 4294967295.;
    };
    unsigned compared = 0;
    for (double rate : {44100., 48000., 96000., 192000.})
        for (int mode : {0, 1, 2})
        {
            VoiceVcaAntialias candidate;
            FrozenVoiceVcaAntialias reference;
            Callbacks actual, expected;
            auto kernel = VoiceVcaAntialias::prepare(rate);
            for (int frame = 0; frame < 2048; ++frame)
            {
                if (frame == 513) {candidate.reset(); reference.reset();}
                // Test retained history across grids as well as reset startup.
                if (frame == 777) kernel = VoiceVcaAntialias::prepare(rate < 88200. ? 96000. : 48000.);
                if (frame == 1234) kernel = VoiceVcaAntialias::prepare(rate);
                float input = float((next() - .5) * 24);
                if (frame < 96) input = frame == 0 ? 17.f : frame == 7 ? -19.f : 0.f;
                if (frame % 197 == 0) input = -0.f;
                const float gain = frame % 73 < 21 ? 0.f : float(next() * 1.4);
                const float kelvin = float(273.15 + next() * 60);
                candidate.storeTemperatureContext(kelvin);
                reference.storeTemperatureContext(kelvin);
                actual.used = expected.used = 0;
                const auto shape = [&](float x) {return actual.shape(x);};
                const auto refShape = [&](float x) {return expected.shape(x);};
                const auto noise = [&](double x, double g) {return actual.noise(x, g, kelvin);};
                const auto refNoise = [&](double x, double g) {return expected.noise(x, g, kelvin);};
                const auto temperatureNoise = [&](double x, double g, double t) {return actual.noise(x, g, t);};
                const auto refTemperatureNoise = [&](double x, double g, double t) {return expected.noise(x, g, t);};
                float want = 0, got = 0;
                if (mode == 0)
                    want = reference.processOriginal<false>(input, gain, kernel, refShape, refNoise, 0);
                else if (mode == 1)
                    want = reference.processOriginal<true>(input, gain, kernel, refShape, refNoise, 0);
                else
                    want = reference.processOriginal<true, true>(input, gain, kernel, refShape, refTemperatureNoise, kelvin);
                guardAllocation = true;
                if (mode == 0)
                    got = candidate.process(input, gain, kernel, shape);
                else if (mode == 1)
                    got = candidate.processWithOutputNoise(input, gain, kernel, shape, noise);
                else
                    got = candidate.processWithTemperatureNoise(input, gain, kelvin, kernel, shape, temperatureNoise);
                guardAllocation = false;
                require(sameFloat(got, want), "VCA phase batching changed output bits");
                require(sameHistory(candidate, reference), "VCA phase batching changed full FIR history");
                const auto callsPerPhase = mode == 0 ? 1u : 2u;
                require(actual.used == callsPerPhase * static_cast<unsigned>(kernel.factor),
                    "VCA did not invoke callbacks once per phase");
                for (std::size_t event = 0; event < actual.used; ++event)
                    require(actual.events[event].kind == (event % callsPerPhase == 0 ? 1 : 2),
                        "VCA noise callback must follow shape within each phase");
                if (actual.used == expected.used)
                    for (std::size_t event = 0; event < actual.used; ++event)
                        if (!(actual.events[event] == expected.events[event])) {
                            const auto& a = actual.events[event];
                            const auto& b = expected.events[event];
                            std::cerr << "VCA callback mismatch rate=" << rate << " mode=" << mode
                                << " frame=" << frame << " event=" << event << " kinds="
                                << a.kind << '/' << b.kind << " drive bits=" << a.drive << '/' << b.drive
                                << " gain bits=" << a.gain << '/' << b.gain
                                << " kelvin bits=" << a.kelvin << '/' << b.kelvin << '\n';
                            break;
                        }
                require(actual.used == expected.used && actual.shapes == expected.shapes
                    && actual.noises == expected.noises && actual.noiseState == expected.noiseState
                    && std::equal(actual.events.begin(), actual.events.begin() + actual.used, expected.events.begin()),
                    "VCA phase batching changed shape/noise drive, control, temperature or RNG chronology");
                ++compared;
            }
        }
    require(allocations == 0, "VCA phase batching allocated");
    std::cout << "VCA batched interpolation original bit/history/callback parity: "
              << compared << " random/hot/startup/reset/rate frames\n";
}
void linearResponseAndDelay()
{
    double worst = 0;
    for (double rate : {8000., 44100., 48000., 96000., 192000.})
    {
        const auto kernel = VoiceVcaAntialias::prepare(rate);
        const int delay = kernel.factor == 1 ? 0 : VoiceVcaAntialias::delaySamples;
        constexpr int count = 4096;
        const int last = static_cast<int>((rate >= 44100. ? 20000. : .45 * rate) * count / rate);
        for (int point = 0; point <= 13; ++point)
        {
            const int bin = std::max(16, last * point / 13);
            const auto old = projection(sine(rate, false, .01, bin, count, false), bin);
            const auto current = projection(sine(rate, true, .01, bin, count, false), bin);
            const auto ratio = current / old * std::polar(1., 2 * pi * bin * delay / count);
            const double dB = 20 * std::log10(std::abs(ratio));
            worst = std::max(worst, std::abs(dB));
            require(std::abs(dB) < .041, "VCA antialias filter changed wanted linear response");
            require(std::abs(std::arg(ratio)) < 2e-6, "VCA FIR delay/phase was not aligned");
        }
        VoiceVcaAntialias a;
        double dc = 0;
        for (int n = 0; n < 256; ++n) dc = a.process(.75f, .6f, kernel, [](float x) {return x;});
        require(std::abs(dc - .45) < 1e-7, "local VCA reconstruction changed DC gain");
    }
    std::cout << "Worst wanted linear magnitude error: " << worst << " dB\n";
}
void wantedHarmonicsAndAliasing()
{
    constexpr int count = 8192;
    for (double rate : {44100., 48000., 96000.})
    {
        const int tone = static_cast<int>(std::round(10000. * count / rate));
        const auto raw = sine(rate, false, 4.8, tone, count);
        const auto current = sine(rate, true, 4.8, tone, count);
        const int alias = count - 3 * tone;
        require(alias > 0 && alias < count / 2 || rate == 96000., "alias witness was invalid");
        if (rate < 96000.)
        {
            const double rawLine = std::abs(projection(raw, alias));
            const double currentLine = std::abs(projection(current, alias));
            const double reduction = 20 * std::log10(currentLine / rawLine);
            std::cout << rate << " Hz folded H3 reduction: " << reduction << " dB\n";
            require(reduction < -45, "actual engine BA662 folded H3 did not fall sufficiently");
        }
        const auto high = sine(rate * 4, false, 4.8, tone, count * 4);
        const double currentH1 = std::abs(projection(current, tone));
        const double highH1 = std::abs(projection(high, tone));
        require(std::abs(20 * std::log10(currentH1 / highH1)) < .02,
                "actual VCA wanted fundamental changed against high-rate law");
        // Low note retains its physical H3/H5 instead of removing saturation.
        constexpr int lowTone = 128;
        const auto low = sine(rate, true, 4.8, lowTone, count);
        const auto reference = sine(rate * 4, false, 4.8, lowTone, count * 4);
        for (int harmonic : {1, 3, 5})
        {
            const double actual = std::abs(projection(low, lowTone * harmonic));
            const double expected = std::abs(projection(reference, lowTone * harmonic));
            require(std::abs(20 * std::log10(actual / expected)) < .025,
                    "physical baseband BA662 harmonic changed");
        }
    }
}
void envelopeTransient()
{
    constexpr int count = 1536;
    constexpr double rate = 48000., f = 1500., attack = .002;
    auto e = prepared(rate, true);
    double sum = 0, peak = 0;
    for (int n = -count; n < count; ++n)
    {
        const double time = n / rate;
        const double gain = time < 0 ? 0 : -std::expm1(-time / attack);
        const float y = YouKnowTestAccess::finish(*e,
            static_cast<float>(4.8 * std::cos(2 * pi * f * time)), static_cast<float>(gain));
        if (n < 0) continue;
        const double delayed = (n - VoiceVcaAntialias::delaySamples) / rate;
        const double expectedGain = delayed < 0 ? 0 : -std::expm1(-delayed / attack);
        // C59's settled physical transfer at this frequency is effectively
        // unity; keep its tiny phase in the independent continuous oracle.
        const double wc = 2 * pi * YouKnowEngine::vcaInputCouplingCornerHz();
        const double w = 2 * pi * f;
        const double coupled = 4.8 * w / std::hypot(w, wc)
            * std::cos(w * delayed + std::atan(wc / w));
        const double expected = YouKnowEngine::VoiceVcaSignalLaw::shape(static_cast<float>(coupled))
            * expectedGain * YouKnowEngine::VoiceVcaSignalLaw::serviceGain() / 2.6;
        const double error = std::abs(y - expected);
        peak = std::max(peak, error);
        sum += error * error;
    }
    std::cout << "Aligned 2ms-control transient peak/RMS error: " << peak << "/"
              << std::sqrt(sum / count) << " model output units\n";
    require(peak < .003 && std::sqrt(sum / count) < .0003,
            "VCA antialias control reconstruction was delayed/smoothed incorrectly");
}
void changingTemperatureAndControl()
{
    constexpr int count = 1536, multiple = 4;
    constexpr double rate = 48000.;
    auto high = prepared(rate * multiple, false);
    auto current = prepared(rate, true);
    YouKnowTestAccess::thermalParameters(*high);
    YouKnowTestAccess::thermalParameters(*current);
    const auto sample = [](YouKnowEngine& e, double time) {
        YouKnowTestAccess::warmup(e, std::clamp((time + .01) / .03, 0., 1.));
        return YouKnowTestAccess::finish(e,
            static_cast<float>(4.8 * std::cos(2 * pi * 1500 * time)),
            static_cast<float>(.5 + .3 * std::sin(2 * pi * 125 * time)));
    };
    std::vector<float> reference(2 * count * multiple);
    for (int n = -count * multiple; n < count * multiple; ++n)
        reference[static_cast<std::size_t>(n + count * multiple)] = sample(*high, n / (rate * multiple));
    double sum = 0, peak = 0;
    for (int n = -count; n < count; ++n)
    {
        const float y = sample(*current, n / rate);
        if (n < 0) continue;
        const int index = (n - VoiceVcaAntialias::delaySamples + count) * multiple;
        const double error = std::abs(y - reference[static_cast<std::size_t>(index)]);
        sum += error * error;
        peak = std::max(peak, error);
    }
    std::cout << "Changing temperature/control high-rate peak/RMS error: " << peak << "/"
              << std::sqrt(sum / count) << " output units\n";
    require(peak < .003 && std::sqrt(sum / count) < .0005,
            "VCA applied current temperature/control to delayed audio history");
}
void undelayedPhysicalEnergy()
{
    auto raw = prepared(48000, false);
    auto current = prepared(48000, true);
    YouKnowTestAccess::thermalParameters(*raw);
    YouKnowTestAccess::thermalParameters(*current);
    for (int n = 0; n < 4096; ++n)
    {
        const double time = n / 48000.;
        const float signal = static_cast<float>(4.8 * std::cos(2 * pi * 7500 * time));
        const float gain = static_cast<float>(.5 + .3 * std::sin(2 * pi * 125 * time));
        for (auto* e : {raw.get(), current.get()})
        {
            YouKnowTestAccess::warmup(*e, std::min(1., time / .03));
            (void) YouKnowTestAccess::finish(*e, signal, gain);
        }
        require(YouKnowTestAccess::energy(*raw) == YouKnowTestAccess::energy(*current),
                "VCA numerical FIR delay moved the physical rail-energy proxy");
    }
}
struct Render
{
    std::vector<float> left;
    std::tuple<std::uint32_t, float, bool> random;
    bool retiredAll {};
};
Render render(bool corrected, int block, double rate = 48000., bool retrigger = false)
{
    auto e = prepared(rate, corrected);
    if (retrigger)
    {
        YouKnowTestAccess::fastGate(*e);
        e->noteOn(117, 1);
    }
    else for (int note : {69, 81, 93, 105, 117, 123}) e->noteOn(note, 1);
    Render r;
    // A fixed wall-clock tail also lets the envelope/hold retire on the
    // 192k grid, where4096 samples alone span only21ms.
    r.left.resize(retrigger ? static_cast<std::size_t>(std::ceil(rate * .14)) : 4096);
    std::vector<float> right(r.left.size());
    guardAllocation = true;
    for (int from = 0; from < static_cast<int>(r.left.size());)
    {
        int end = static_cast<int>(r.left.size());
        if (retrigger)
            for (int edge : {400, 900, 1500, 2200, 2600})
                if (edge > from) {end = std::min(end, edge); break;}
        if (retrigger && (from == 400 || from == 2600)) e->noteOff(117);
        if (retrigger && from == 1500) e->noteOff(123);
        if (retrigger && from == 900) e->noteOn(123, 1);
        if (retrigger && from == 2200) e->noteOn(117, 1);
        const int size = std::min(block, end - from);
        e->process(r.left.data() + from, right.data() + from, size);
        from += size;
    }
    guardAllocation = false;
    r.random = YouKnowTestAccess::random(*e);
    r.retiredAll = YouKnowTestAccess::allInactive(*e);
    for (float x : r.left) require(std::isfinite(x), "full VCA engine became nonfinite");
    return r;
}
void engineState()
{
    require(!EngineParameters {}.enableVoiceVcaAntialias, "raw VCA reference convention changed");
    EngineParameters p;
    ProductFidelityProfile::applyTo(p);
    require(p.enableVoiceVcaAntialias, "product omitted VCA antialias correction");
    {
        // Match the actual processor order: prepare the raw engine first,
        // then apply product parameters, then ask for the host latency.
        auto e = prepared(48000, false);
        require(e->getProcessingLatencySamples() == 41, "raw latency changed before profile application");
        YouKnowTestAccess::select(*e, true);
        require(e->getProcessingLatencySamples() == 72,
                "post-prepare product profile did not install final host latency");
        for (int n = 0; n < 80; ++n) (void) YouKnowTestAccess::finish(*e, 1, .5f);
        YouKnowTestAccess::select(*e, false);
        require(e->getProcessingLatencySamples() == 41 && YouKnowTestAccess::empty(*e),
                "diagnostic selector retained wrong latency/FIR history");
        YouKnowTestAccess::select(*e, true);
        require(e->getProcessingLatencySamples() == 72 && YouKnowTestAccess::empty(*e),
                "returning to product retained old FIR history");
    }
    for (double rate : {8000., 44100., 48000., 96000., 192000.})
    {
        const auto current = render(true, 128, rate);
        const auto old = render(false, 128, rate);
        require(current.left == render(true, 1, rate).left, "VCA engine depends on block partition");
        require(current.random == old.random, "VCA local oversampling changed RNG chronology");
        auto e = prepared(rate, true);
        const int latency = e->getProcessingLatencySamples();
        for (int factor : {2, 4, 1})
        {
            require(YouKnowTestAccess::qualifiedRebuild(*e, factor), "qualified VCA quality rebuild failed");
            require(e->getProcessingLatencySamples() == latency, "quality changed VCA reported latency");
            require(YouKnowTestAccess::empty(*e), "quality retained stale VCA FIR timeline");
        }
        (void) YouKnowTestAccess::finish(*e, 2, .5f);
        e->reset();
        require(YouKnowTestAccess::empty(*e), "hard reset retained VCA FIR history");
    }
    for (double rate : {44100., 48000., 96000., 192000.})
        for (int factor : {1, 2, 4})
        {
            auto e = prepared(rate, true);
            require(YouKnowTestAccess::qualifiedRebuild(*e, factor), "latency impulse quality setup failed");
            const double centre = YouKnowTestAccess::impulseCentre(*e);
            require(std::abs(centre - e->getProcessingLatencySamples()) <= .5 + 1e-5,
                    "actual VCA/decimator/pad impulse missed reported latency");
        }
    for (double rate : {176400., 192000.})
        require(render(true, 128, rate).left == render(false, 128, rate).left,
                "already-high-grid actual engine VCA bypass changed samples");
    require(allocations == 0, "full VCA engine allocated in the audio callback");
    for (double rate : {44100., 48000., 192000.})
    {
        const auto current = render(true, 128, rate, true);
        require(current.left == render(true, 1, rate, true).left,
                "note-off/retrigger VCA tails depend on block partition");
        const auto old = render(false, 128, rate, true);
        require(current.random == old.random, "VCA retirement/retrigger changed random sequence");
        require(current.retiredAll && old.retiredAll,
                "fast-gate lifecycle did not exercise complete voice retirement");
        double oldPeak = 0, newPeak = 0;
        for (float x : old.left) oldPeak = std::max(oldPeak, std::abs(static_cast<double>(x)));
        for (float x : current.left) newPeak = std::max(newPeak, std::abs(static_cast<double>(x)));
        require(newPeak <= oldPeak * 1.08 + .001,
                "VCA note-off/retrigger produced an unexpected output excursion");
    }
    require(allocations == 0, "VCA retirement/retrigger allocated in the audio callback");
}
double benchmark(bool corrected)
{
    auto e = prepared(48000, corrected);
    for (int note : {69, 81, 93, 105, 117, 123}) e->noteOn(note, 1);
    std::array<float, 128> left {}, right {};
    for (int n = 0; n < 16; ++n) e->process(left.data(), right.data(), 128);
    const auto start = std::chrono::steady_clock::now();
    for (int n = 0; n < 375; ++n) e->process(left.data(), right.data(), 128);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
}
int main()
{
    try
    {
        phaseBatchingBitEquivalence();
        linearResponseAndDelay();
        wantedHarmonicsAndAliasing();
        envelopeTransient();
        changingTemperatureAndControl();
        undelayedPhysicalEnergy();
        engineState();
        const double raw = benchmark(false), corrected = benchmark(true);
        std::cout << "Six active voices,1s@48k CPU raw/corrected: " << raw << "/"
                  << corrected << " ms (" << corrected / raw << "x)\n";
        std::cout << "Actual VCA alias, response/control and engine-state checks passed\n";
    }
    catch (const std::exception& e) {guardAllocation = false; std::cerr << e.what() << '\n'; return 1;}
}
