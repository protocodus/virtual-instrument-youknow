#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <new>
#include <numbers>
#include <stdexcept>
#include <vector>
#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace
{
bool countAllocations = false;
std::size_t allocations = 0;
}
void* operator new(std::size_t size)
{
    if (countAllocations) ++allocations;
    if (void* result = std::malloc(std::max(size, std::size_t {1}))) return result;
    throw std::bad_alloc {};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void* operator new(std::size_t size, std::align_val_t alignment)
{
    if (countAllocations) ++allocations;
#if defined(_MSC_VER)
    if (void* result = _aligned_malloc(std::max(size, std::size_t {1}),
                                      static_cast<std::size_t>(alignment))) return result;
#else
    void* result = nullptr;
    if (posix_memalign(&result, static_cast<std::size_t>(alignment),
                      std::max(size, std::size_t {1})) == 0) return result;
#endif
    throw std::bad_alloc {};
}
void* operator new[](std::size_t size, std::align_val_t alignment)
{ return ::operator new(size, alignment); }
void operator delete(void* pointer, std::align_val_t) noexcept
{
#if defined(_MSC_VER)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}
void operator delete[](void* pointer, std::align_val_t alignment) noexcept
{ ::operator delete(pointer, alignment); }
void operator delete(void* pointer, std::size_t, std::align_val_t alignment) noexcept
{ ::operator delete(pointer, alignment); }
void operator delete[](void* pointer, std::size_t, std::align_val_t alignment) noexcept
{ ::operator delete(pointer, alignment); }

namespace youknow
{
struct YouKnowTestAccess
{
    static auto coefficients(const YouKnowEngine& engine)
    { return engine.processingCoefficients_.commonVcaOutputPole; }
    static double rate(const YouKnowEngine& engine) { return engine.oversampledRate_; }
    static auto history(const YouKnowEngine& engine)
    {
        return std::array { engine.commonVcaOutputPole_.previousInput,
                            engine.commonVcaOutputPole_.difference };
    }
    static void poison(YouKnowEngine& engine)
    { engine.commonVcaOutputPole_ = {0.875, -0.123}; }
    static float step(YouKnowEngine& engine, float input)
    { return engine.commonVcaOutputPole_.process(input, coefficients(engine)); }
    static void clearForQuality(YouKnowEngine& engine)
    { engine.clearRateDependentOutputPath(true); }
};
}

namespace
{
using namespace youknow;
constexpr double pi = std::numbers::pi;
// Independent reduction of Roland p.15, R16/C5. Do not call the engine's
// helper to obtain the analogue oracle used below.
constexpr double tau = 33000.0 * 22e-12;
constexpr double pole = 1.0 / (2.0 * pi * tau);
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

double checkFrequencyResponse()
{
    require(std::abs(YouKnowEngine::commonVcaOutputPoleHz() / pole - 1) < 1e-13,
            "R16/C5 nominal pole differs from the schematic");
    double worst = 0;
    for (double host : {8000., 11025., 22050., 44100., 48000., 88200.,
                        96000., 192000., 384000., 768000.})
        for (int factor : {1, 2, 4})
        {
            YouKnowEngine engine;
            engine.prepare(host, 128, factor);
            const auto c = YouKnowTestAccess::coefficients(engine);
            const double rate = YouKnowTestAccess::rate(engine);
            require(std::isfinite(c.a1) && std::isfinite(c.correction)
                        && std::abs(c.a1) < 1,
                    "common-VCA pole is unstable on a supported processing grid");
            std::array<float, 256> impulse {};
            for (std::size_t i = 0; i < impulse.size(); ++i)
                impulse[i] = YouKnowTestAccess::step(engine, i == 0 ? 1.f : 0.f);
            for (int bin = 0; bin <= 160; ++bin)
            {
                const double hz = std::min(20000., .45 * rate) * bin / 160.;
                const auto z = std::polar(1., -2 * pi * hz / rate);
                std::complex<double> measured {}, power {1, 0};
                for (float sample : impulse)
                {
                    measured += static_cast<double>(sample) * power;
                    power *= z;
                }
                // Independent z-transform of y=x+d, d=c(x-x[-1])-a*d[-1].
                const auto digital = 1. + c.correction * (1. - z) / (1. + c.a1 * z);
                require(std::abs(measured - digital) < 2e-7,
                        "actual impulse does not match the complex digital transfer");
                const double analog = 1. / std::sqrt(1. + std::pow(2 * pi * hz * tau, 2));
                const double error = std::abs(20 * std::log10(std::abs(measured) / analog));
                worst = std::max(worst, error);
                require(error < .003,
                        "common-VCA magnitude fit exceeds the audible-band error bound");
            }
            // Unity DC, finite memory and unchanged DC under a quality-grid
            // coefficient change are checked separately from the spectrum.
            for (int i = 0; i < 128; ++i)
                require(std::isfinite(YouKnowTestAccess::step(engine, .75f)),
                        "common-VCA DC settling is nonfinite");
            require(YouKnowTestAccess::step(engine, .75f) == .75f,
                    "common-VCA pole changes settled DC gain");
        }
    return worst;
}

EngineParameters patch(bool enabled)
{
    EngineParameters p;
    p.enableCommonVcaOutputPole = enabled;
    p.calibration = 0;
    p.aging = 0;
    p.chorus = ChorusMode::Off;
    p.chorusNoise = p.noiseLevel = p.subLevel = 0;
    p.sawEnabled = true;
    p.pulseEnabled = false;
    p.cutoff = 1;
    p.resonance = p.envDepth = p.keyFollow = 0;
    p.vcaMode = VcaMode::Gate;
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    return p;
}

std::vector<float> render(bool enabled, int factor, int block)
{
    YouKnowEngine engine;
    engine.prepare(48000, 128, factor);
    engine.setParameters(patch(enabled));
    engine.noteOn(84, 1);
    std::vector<float> result(4096), right(result.size());
    countAllocations = true;
    for (std::size_t i = 0; i < result.size();)
    {
        const int count = std::min(block, static_cast<int>(result.size() - i));
        engine.process(result.data() + i, right.data() + i, count);
        i += static_cast<std::size_t>(count);
    }
    countAllocations = false;
    for (float sample : result) require(std::isfinite(sample), "nonfinite rendered audio");
    const auto history = YouKnowTestAccess::history(engine);
    require(!enabled || (std::abs(history[0]) > 1e-6 && std::abs(history[1]) > 1e-9),
            "actual dry audio never reached the common-VCA pole");
    require(enabled || history == std::array {0., 0.},
            "raw bypass unexpectedly ran the pole");
    return result;
}

void checkRenderAndNoisePlacement()
{
    require(!EngineParameters {}.enableCommonVcaOutputPole,
            "raw engine default lost its reference bypass");
    EngineParameters product;
    ProductFidelityProfile::applyTo(product);
    require(product.enableCommonVcaOutputPole, "product omitted R16/C5");
    for (int factor : {1, 2, 4})
    {
        const auto on = render(true, factor, 128);
        require(on == render(true, factor, 1), "pole audio depends on block partition");
        const auto off = render(false, factor, 128);
        double difference = 0, power = 0;
        for (std::size_t i = 0; i < on.size(); ++i)
        {
            difference += std::pow(static_cast<double>(on[i]) - off[i], 2);
            power += static_cast<double>(off[i]) * off[i];
        }
        require(power > .01 && difference > 1e-10 && difference / power < .001,
                "actual dry path omits the pole or applies an excessive effect");
    }

    // The published -94dBV is already measured at the I/V output. Turning
    // that output-referred source on must not affect upstream C5 histories.
    YouKnowEngine quiet, noisy;
    for (auto* engine : {&quiet, &noisy}) engine->prepare(48000, 128, 1);
    auto p = patch(true);
    p.calibration = 1;
    p.enableCommonVcaNoise = false;
    quiet.setParameters(p);
    p.enableCommonVcaNoise = true;
    noisy.setParameters(p);
    std::array<float, 512> ql {}, qr {}, nl {}, nr {};
    quiet.process(ql.data(), qr.data(), static_cast<int>(ql.size()));
    noisy.process(nl.data(), nr.data(), static_cast<int>(nl.size()));
    require(YouKnowTestAccess::history(quiet) == YouKnowTestAccess::history(noisy),
            "output-referred common-VCA noise was filtered twice");
    require(ql != nl, "noise-placement probe did not produce the published noise source");
}

void checkStateAndRates()
{
    YouKnowEngine engine;
    engine.prepare(48000, 128, 1);
    engine.setParameters(patch(true));
    const auto cleared = [&engine] {
        return YouKnowTestAccess::history(engine) == std::array {0., 0.};
    };
    YouKnowTestAccess::poison(engine);
    engine.reset();
    require(cleared(), "reset retained common-VCA history");
    YouKnowTestAccess::poison(engine);
    engine.resetForHostStop();
    require(cleared(), "host stop retained common-VCA history");
    YouKnowTestAccess::poison(engine);
    YouKnowTestAccess::clearForQuality(engine);
    require(cleared(), "muted quality rebuild retained sample-grid history");
    YouKnowTestAccess::poison(engine);
    engine.setParameters(patch(false));
    require(cleared(), "comparison bypass retained stale common-VCA history");
    engine.setParameters(patch(true));
    YouKnowTestAccess::poison(engine);
    engine.prepare(44100, 128, 4);
    require(cleared() && YouKnowTestAccess::rate(engine) == 176400,
            "host rate change did not rebuild and clear the common-VCA pole");

    engine.prepare(48000, 128, 1);
    engine.setParameters(patch(true));
    std::array<float, 128> left {}, right {};
    const auto old = YouKnowTestAccess::coefficients(engine);
    countAllocations = true;
    (void) engine.setOversamplingFactor(4);
    for (int i = 0; i < 64; ++i)
        engine.process(left.data(), right.data(), static_cast<int>(left.size()));
    countAllocations = false;
    const auto current = YouKnowTestAccess::coefficients(engine);
    require(engine.getOversamplingFactor() == 4 && old.correction != current.correction,
            "live quality change did not refresh the actual common-VCA coefficients");
    require(allocations == 0, "render or live quality change allocated memory");
}
}

int main()
{
    try
    {
        const double worst = checkFrequencyResponse();
        checkRenderAndNoisePlacement();
        checkStateAndRates();
        std::cout << "PASS: R16/C5 analogue magnitude and complex digital transfer; "
                  << "product/raw selection, actual dry audio, output-noise placement, "
                  << "block invariance, resets, host/quality rates, no allocations. "
                  << "Worst magnitude error " << worst << " dB (8k–768k hosts, 1x/2x/4x requests).\n";
    }
    catch (const std::exception& e)
    {
        countAllocations = false;
        std::cerr << "FAIL: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
