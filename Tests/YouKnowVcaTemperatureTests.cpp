// Conditional bipolar-pair response at fixed control current. These checks
// do not assign a temperature coefficient to the installed Tr20 current path.
#include "DSP/YouKnowEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace youknow
{
struct YouKnowTestAccess
{
    static void temperature(YouKnowEngine& engine, float fraction)
    {
        engine.thermalWarmupFraction_ = fraction;
    }
    static double scale(const YouKnowEngine& engine, int card)
    {
        return engine.voiceVcaThermalDriveScale(engine.activeParameters_, card);
    }
    static void switchThermal(YouKnowEngine& engine, bool enabled)
    {
        engine.activeParameters_.enableVoiceVcaTemperature = enabled;
    }
    static void seed(YouKnowEngine& engine)
    {
        for (int card = 0; card < 6; ++card)
        {
            auto& voice = engine.voices_[static_cast<std::size_t>(card)];
            voice.active = true;
            voice.vca = 0.42f;
            voice.vcaInputTrim = 1.017f;
            voice.vcaControl = 0.625;
            voice.vcaControlTarget = 0.75f;
            voice.vcaInputCoupling.reset();
        }
    }
    static double finish(YouKnowEngine& engine, int card, float input)
    {
        return engine.finishVoiceFilter(
            engine.voices_[static_cast<std::size_t>(card)], input);
    }
    static double input(const YouKnowEngine& engine, int card)
    {
        return engine.voices_[static_cast<std::size_t>(card)].vcaInputVolts;
    }
    static std::array<double, 4> retained(const YouKnowEngine& engine, int card)
    {
        const auto& voice = engine.voices_[static_cast<std::size_t>(card)];
        return { voice.vcaInputCoupling.state, voice.vca,
                 voice.vcaControl, voice.vcaControlTarget };
    }
};
}

namespace
{
using youknow::EngineParameters;
using youknow::YouKnowEngine;
using Probe = youknow::YouKnowTestAccess;
constexpr double pi = std::numbers::pi_v<double>;
constexpr double headroom = 2.4 / 0.21453375; // Existing fixed service trim.
constexpr double inputTrim = static_cast<double>(1.017f);
constexpr double outputScale = static_cast<double>(0.42f)
                            / static_cast<double>(2.6f);

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void near(double actual, double expected, double tolerance, const char* message)
{
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance)
    {
        std::cerr << message << ": " << actual << " vs " << expected << '\n';
        throw std::runtime_error(message);
    }
}
double physicalRatio(double character, int card, double fraction, bool spatial)
{
    const double gradient = spatial ? 4.0 * std::exp(-card / 2.5) : 0.0;
    return (298.15 + character * (15.0 + gradient))
         / (298.15 + character * (15.0 * fraction + gradient));
}
std::unique_ptr<YouKnowEngine> makeEngine(float character = 1.0f,
    bool enabled = true, bool spatial = false, bool nonlinear = true,
    double rate = 48000.0, int factor = 1, bool settled = false)
{
    auto engine = std::make_unique<YouKnowEngine>();
    require(engine->configureThermalStart(settled), "thermal start rejected");
    EngineParameters parameters;
    parameters.calibration = character;
    parameters.aging = 0.0f;
    parameters.enableVoiceVcaTemperature = enabled;
    parameters.enableSpatialThermalGradient = spatial;
    parameters.enableVoiceVcaSignalSaturation = nonlinear;
    parameters.vcfTanhMode = youknow::VcfTanhMode::PolyZoned;
    parameters.vcfFastEarlyMode = youknow::VcfFastEarlyMode::Cubic;
    parameters.vcfSolverMode = youknow::VcfSolverMode::Rk4Single;
    engine->setParameters(parameters);
    engine->prepare(rate, 128, factor);
    return engine;
}

void checkTemperatureLawAndReference()
{
    require(EngineParameters {}.enableVoiceVcaTemperature,
            "voice VCA thermal response is disabled by default");
    for (float character : { 0.0f, 0.5f, 1.0f, 2.0f })
        for (bool spatial : { false, true })
        {
            auto enabled = makeEngine(character, true, spatial);
            auto disabled = makeEngine(character, false, spatial);
            Probe::seed(*enabled);
            Probe::seed(*disabled);
            for (float fraction : { 0.0f, 0.25f, 0.75f, 1.0f })
            {
                Probe::temperature(*enabled, fraction);
                Probe::temperature(*disabled, fraction);
                for (int card = 0; card < 6; ++card)
                {
                    near(Probe::scale(*enabled, card),
                         physicalRatio(character, card, fraction, spatial),
                         2e-7, "card temperature missed the independent Kelvin ratio");
                    near(Probe::scale(*disabled, card), 1.0, 0.0,
                         "disabled response changed reference drive");
                    if (fraction == 1.0f || character == 0.0f)
                    {
                        for (float volts : { -30.0f, -2.4f, 0.0f, 0.01f, 6.8f })
                            require(Probe::finish(*enabled, card, volts)
                                        == Probe::finish(*disabled, card, volts),
                                    "settled or Character-zero response changed reference samples");
                    }
                }
            }
        }
}

void checkSignalPathAndContinuity()
{
    for (bool nonlinear : { false, true })
    {
        auto engine = makeEngine(1.0f, true, false, nonlinear);
        auto reference = makeEngine(1.0f, false, false, nonlinear);
        Probe::seed(*engine);
        Probe::seed(*reference);
        for (float fraction : { 1.0f, 0.0f, 0.5f, 1.0f })
        {
            const auto previous = Probe::retained(*engine, 0);
            Probe::temperature(*engine, fraction);
            Probe::temperature(*reference, fraction);
            Probe::switchThermal(*engine, false);
            Probe::switchThermal(*engine, true);
            require(Probe::retained(*engine, 0) == previous,
                    "temperature edit reset C59 charge or the current-control state");
            for (float volts : { 0.001f, 2.4f, -6.8f, 1000.0f, -1000.0f })
            {
                const auto controlBefore = Probe::retained(*engine, 0);
                const double actual = Probe::finish(*engine, 0, volts);
                static_cast<void>(Probe::finish(*reference, 0, volts));
                require(Probe::retained(*engine, 0) == Probe::retained(*reference, 0),
                        "temperature response changed C59 charge or fixed control current");
                const double drive = Probe::input(*engine, 0) * inputTrim
                    * physicalRatio(1.0, 0, fraction, false);
                const double expected = (nonlinear
                    ? headroom * std::tanh(drive / headroom) : drive) * outputScale;
                near(actual, expected, 3e-6 * std::max(1.0, std::abs(expected)),
                     "finishVoiceFilter did not apply fixed-ceiling thermal drive");
                if (nonlinear && std::abs(volts) == 1000.0f)
                    near(std::abs(actual), headroom * outputScale, 2e-6,
                         "temperature moved the fixed-current saturation ceiling");
                const auto controlAfter = Probe::retained(*engine, 0);
                require(std::equal(controlBefore.begin() + 1, controlBefore.end(),
                                   controlAfter.begin() + 1),
                        "signal temperature modified the frozen Tr20 control path");
            }
        }
    }
}

struct Harmonics { double fundamental, third; };
Harmonics measure(double amplitude, float fraction, double rate)
{
    auto engine = makeEngine(1.0f, true, false, true, rate);
    Probe::seed(*engine);
    Probe::temperature(*engine, fraction);
    std::array<double, 4> sums {};
    const int count = static_cast<int>(rate / 8.0);
    for (int sample = 0; sample < 2 * count; ++sample)
    {
        const double phase = 2.0 * pi * 1000.0 * sample / rate;
        const double output = Probe::finish(*engine, 0,
            static_cast<float>(amplitude * std::sin(phase)));
        if (sample < count) continue;
        sums[0] += output * std::sin(phase);
        sums[1] += output * std::cos(phase);
        sums[2] += output * std::sin(3.0 * phase);
        sums[3] += output * std::cos(3.0 * phase);
    }
    return { 2.0 * std::hypot(sums[0], sums[1]) / count,
             2.0 * std::hypot(sums[2], sums[3]) / count };
}
void checkFixedCurrentGainAndDistortion()
{
    for (double rate : { 48000.0, 96000.0 })
    {
        const auto coldSmall = measure(0.01, 0.0f, rate);
        const auto warmSmall = measure(0.01, 1.0f, rate);
        near(coldSmall.fundamental / warmSmall.fundamental, 313.15 / 298.15,
             2e-6, "cold small-signal gain is not inverse absolute temperature");
        const auto cold = measure(2.4, 0.0f, rate);
        const auto warm = measure(2.4, 1.0f, rate);
        const double coldHd3 = cold.third / cold.fundamental;
        const double warmHd3 = warm.third / warm.fundamental;
        require(coldHd3 > warmHd3 * 1.09 && coldHd3 < warmHd3 * 1.11,
                "cold fixed-current pair did not gain the predicted third harmonic");
        require(cold.fundamental > warm.fundamental,
                "cold pair lost its fixed-current signal gain");
    }
}

void checkAudioBlockAndRate()
{
    for (double rate : { 48000.0, 96000.0 })
        for (int factor : { 1, 4 })
        {
            auto whole = makeEngine(1.0f, true, true, true, rate, factor);
            auto split = makeEngine(1.0f, true, true, true, rate, factor);
            whole->noteOn(60, 1.0f);
            split->noteOn(60, 1.0f);
            const int count = static_cast<int>(rate * 0.04);
            std::vector<float> left(count), right(count), splitLeft(count), splitRight(count);
            const auto render = [&](YouKnowEngine& engine, float* l, float* r, int block) {
                for (int offset = 0; offset < count; offset += block)
                    engine.process(l + offset, r + offset, std::min(block, count - offset));
            };
            render(*whole, left.data(), right.data(), 128);
            render(*split, splitLeft.data(), splitRight.data(), 17);
            require(left == splitLeft && right == splitRight,
                    "thermal VCA audio depends on host block partition");
            require(std::any_of(left.begin(), left.end(),
                               [](float sample) { return std::abs(sample) > 1e-5f; }),
                    "thermal VCA audio probe was silent");
            for (int card = 0; card < 6; ++card)
                near(Probe::scale(*whole, card),
                     physicalRatio(1.0, card, 1.0 - std::exp(-0.04 / 3.0), true),
                     2e-7, "render rate or quality changed voice VCA warmup");
        }
}
}

int main()
{
    try
    {
        checkTemperatureLawAndReference();
        checkSignalPathAndContinuity();
        checkFixedCurrentGainAndDistortion();
        checkAudioBlockAndRate();
        std::cout << "Voice VCA temperature checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
