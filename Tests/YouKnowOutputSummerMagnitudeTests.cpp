#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace youknow
{
struct YouKnowTestAccess
{
    static double rate(const YouKnowEngine& engine) { return engine.oversampledRate_; }
    static auto coefficients(const YouKnowEngine& engine)
    { return engine.processingCoefficients_.outputSummerMagnitudePole; }
    static float step(YouKnowEngine& engine, float input)
    { return engine.outputSummerMagnitudeLeft_.process(input, coefficients(engine)); }
    static auto history(const YouKnowEngine& engine)
    {
        return std::array { engine.outputSummerMagnitudeLeft_.previousInput,
                            engine.outputSummerMagnitudeLeft_.difference,
                            engine.outputSummerMagnitudeRight_.previousInput,
                            engine.outputSummerMagnitudeRight_.difference };
    }
    static void poison(YouKnowEngine& engine)
    {
        engine.outputSummerMagnitudeLeft_ = {0.875, -0.123};
        engine.outputSummerMagnitudeRight_ = {-0.75, 0.087};
    }
    static void changeRate(YouKnowEngine& engine)
    {
        engine.oversamplingApplied_ = engine.oversampling_ == 1 ? 4 : 1;
        engine.updateProcessingRate(true);
        engine.clearRateDependentOutputPath(true);
    }
};
}

namespace
{
using namespace youknow;
constexpr double pi = std::numbers::pi;
// Independent circuit oracle: TA75558's typical 3MHz GBW, p15's IC6
// 100k feedback and 47k/39k inputs. No engine helper supplies this pole.
constexpr double noiseGain = 1.0 + 100000.0 / 47000.0 + 100000.0 / 39000.0;
constexpr double pole = 3.0e6 / noiseGain;

void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

double frequencyResponse()
{
    double worst = 0;
    require(std::abs(YouKnowEngine::outputSummerBandwidthHz() / pole - 1) < 2e-7,
            "IC6 component pole differs from the independent circuit oracle");
    for (double host : {8000., 11025., 22050., 44100., 48000., 88200.,
                        96000., 192000., 384000., 768000.})
        for (int factor : {1, 2, 4})
        {
            auto engine = std::make_unique<YouKnowEngine>();
            engine->prepare(host, 128, factor);
            const auto c = YouKnowTestAccess::coefficients(*engine);
            const double rate = YouKnowTestAccess::rate(*engine);
            require(std::isfinite(c.a1) && std::isfinite(c.correction) && std::abs(c.a1) < 1,
                    "IC6 magnitude filter is unstable on a supported grid");
            std::array<float, 256> impulse {};
            for (std::size_t i = 0; i < impulse.size(); ++i)
                impulse[i] = YouKnowTestAccess::step(*engine, i == 0 ? 1.f : 0.f);
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
                const double analog = 1.0 / std::sqrt(1.0 + (hz / pole) * (hz / pole));
                worst = std::max(worst, std::abs(20 * std::log10(std::abs(measured) / analog)));
            }
            if (rate >= 48000)
            {
                const auto z = std::polar(1., -2 * pi * 20000. / rate);
                const auto digital = 1.0 + c.correction * (1.0 - z) / (1.0 + c.a1 * z);
                const double loss = 20 * std::log10(std::abs(digital));
                require(loss < -.006 && loss > -.0064,
                        "IC6's audible-band tilt vanished on an ordinary internal grid");
            }
        }
    require(worst < .0006, "IC6 audio-band magnitude error exceeds 0.0006dB");
    return worst;
}

void statePolicy()
{
    auto engine = std::make_unique<YouKnowEngine>();
    engine->prepare(48000, 128, 1);
    YouKnowTestAccess::poison(*engine);
    const auto before = YouKnowTestAccess::history(*engine);
    YouKnowTestAccess::changeRate(*engine);
    require(YouKnowTestAccess::history(*engine) == before,
            "live quality rebuild discarded IC6 signal histories");
    engine->reset();
    require(YouKnowTestAccess::history(*engine) == std::array<double, 4> {},
            "hard reset retained IC6 magnitude-filter history");
    for (int i = 0; i != 32; ++i)
        require(YouKnowTestAccess::step(*engine, 0) == 0,
                "zero-input reset leaked IC6 filter history");
    for (int i = 0; i != 256; ++i)
        (void) YouKnowTestAccess::step(*engine, .75f);
    require(YouKnowTestAccess::step(*engine, .75f) == .75f,
            "IC6 magnitude-filter realization moved settled DC");
    YouKnowTestAccess::changeRate(*engine);
    require(YouKnowTestAccess::step(*engine, .75f) == .75f,
            "coefficient change disturbed settled DC");
}

std::vector<float> render(bool enabled, int factor, int block)
{
    auto engine = std::make_unique<YouKnowEngine>();
    engine->prepare(48000, 128, factor);
    EngineParameters p;
    p.enableOutputSummerMagnitudePole = enabled;
    p.calibration = p.aging = p.chorusNoise = p.noiseLevel = p.subLevel = 0;
    p.chorus = ChorusMode::Off;
    p.sawEnabled = true;
    p.pulseEnabled = false;
    p.cutoff = 1;
    p.resonance = p.envDepth = p.keyFollow = 0;
    p.vcaMode = VcaMode::Gate;
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    engine->setParameters(p);
    engine->noteOn(84, 1);
    std::vector<float> result(4096), right(result.size());
    for (std::size_t i = 0; i < result.size();)
    {
        const int count = std::min(block, static_cast<int>(result.size() - i));
        engine->process(result.data() + i, right.data() + i, count);
        i += static_cast<std::size_t>(count);
    }
    for (float sample : result) require(std::isfinite(sample), "nonfinite IC6 rendered audio");
    return result;
}

void fullEngine()
{
    require(!EngineParameters {}.enableOutputSummerMagnitudePole,
            "raw engine default lost its exponential reference");
    EngineParameters product;
    ProductFidelityProfile::applyTo(product);
    require(product.enableOutputSummerMagnitudePole, "product omitted IC6 magnitude correction");
    for (int factor : {1, 2, 4})
    {
        const auto after = render(true, factor, 128);
        require(after == render(true, factor, 1), "IC6 pole audio depends on block partition");
        const auto before = render(false, factor, 128);
        double power = 0, difference = 0;
        for (std::size_t i = 0; i < after.size(); ++i)
        {
            const double delta = static_cast<double>(after[i]) - before[i];
            difference += delta * delta;
            power += static_cast<double>(before[i]) * before[i];
        }
        require(power > .01 && difference > 1e-12 && difference / power < .0001,
                "full-engine IC6 correction is absent or excessive");
        std::cout << "IC6 dry saw, " << factor << "x: delta "
                  << 10 * std::log10(difference / power) << " dBc\n";
    }
}
}

int main()
{
    try
    {
        const double worst = frequencyResponse();
        statePolicy();
        fullEngine();
        std::cout << "IC6 matched magnitude: worst in-band error " << worst
                  << " dB; nominal 20kHz loss "
                  << -10 * std::log10(1 + (20000. / pole) * (20000. / pole))
                  << " dB; state/reset checks passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
