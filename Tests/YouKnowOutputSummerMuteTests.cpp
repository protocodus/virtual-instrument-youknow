#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace youknow
{
struct YouKnowTestAccess
{
    static void advance(Chorus& chorus, bool muted) { chorus.advanceMuteDrive(muted); }
    static double hold(const Chorus& chorus) { return chorus.muteDriveHoldVolts_; }
    static bool gate(const YouKnowEngine& engine) { return engine.chorus_.isWetInputConnected(); }
    static auto randomStates(const YouKnowEngine& engine)
    {
        return std::array {engine.outputNoiseStateLeft_, engine.outputNoiseStateRight_,
                           engine.outputWiperNoiseStateLeft_, engine.outputWiperNoiseStateRight_,
                           engine.commonVcaNoiseState_};
    }
};
}

namespace
{
using namespace youknow;
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

void componentOracle()
{
    // Independent ideal-op-amp Norton solve. IC6 pin3 is virtual ground;
    // every live branch's 4kT/R current PSD becomes Rf^2 times that PSD.
    // When Tr11 is open, R72's outer terminal floats and its current is zero.
    // Neither the engine's helper nor its stored coefficients supply these.
    constexpr double k = 1.380649e-23, t = 298.15;
    constexpr double rf = 100000, rd = 47000, rw = 39000;
    for (bool wet : {false, true})
    {
        const double resistance = rf + rf * rf / rd + (wet ? rf * rf / rw : 0);
        const double density = std::sqrt(4 * k * t * resistance);
        const double bandwidth = 3e6 / (1 + rf / rd + (wet ? rf / rw : 0));
        require(std::abs(YouKnowEngine::outputSummerResistorNoiseDensity(wet) / density - 1) < 2e-7,
                "IC6 resistor PSD does not follow the connected/open Norton solve");
        require(std::abs(YouKnowEngine::outputSummerBandwidthHz(wet) / bandwidth - 1) < 2e-7,
                "IC6 bandwidth does not follow the connected/open noise gain");
    }
    const double reduction = 20 * std::log10(
        YouKnowEngine::outputSummerResistorNoiseDensity(false)
        / YouKnowEngine::outputSummerResistorNoiseDensity(true));
    require(reduction < -2.59 && reduction > -2.61, "IC6 resistor-floor reduction is wrong");
    std::cout << "IC6 open/connected resistor-layer change " << reduction << " dB\n";
}

void prime(Chorus& chorus, ChorusMode mode)
{
    float left {}, right {};
    chorus.process(0, mode, 0, left, right, 1, true, true, false, ChorusTimingProfile::Shipping, true);
}

void delayedGate()
{
    constexpr double threshold = -15.0 + .6 * (560000.0 + 39000.0) / 39000.0;
    for (double rate : {8000., 48000., 192000.})
    {
        auto chorus = std::make_unique<Chorus>();
        chorus->prepare(rate);
        prime(*chorus, ChorusMode::One);
        require(chorus->isWetInputConnected(), "engaged chorus did not prime a connected wet input");
        int firstMuted = -1;
        for (int frame = 0; frame != static_cast<int>(rate * .5); ++frame)
        {
            YouKnowTestAccess::advance(*chorus, true);
            require(chorus->isWetInputConnected() == (YouKnowTestAccess::hold(*chorus) < threshold),
                    "wet-input readout differs from the resolved physical gate");
            if (!chorus->isWetInputConnected() && firstMuted < 0) firstMuted = frame + 1;
        }
        require(firstMuted > 0 && std::abs(firstMuted / rate - .0818) < .0005,
                "IC6 loading followed the button instead of the existing delayed gate");
        int firstConnected = -1;
        for (int frame = 0; frame != static_cast<int>(rate * .25); ++frame)
        {
            YouKnowTestAccess::advance(*chorus, false);
            if (chorus->isWetInputConnected() && firstConnected < 0) firstConnected = frame + 1;
        }
        require(firstConnected > 0 && firstConnected / rate > .11 && firstConnected / rate < .12,
                "IC6 loading did not follow the existing delayed restart gate");
    }
}

struct Render
{
    std::vector<float> left;
    std::array<std::uint32_t, 5> randomStates {};
};

Render silence(bool corrected, bool commonNoise, int block)
{
    auto engine = std::make_unique<YouKnowEngine>();
    engine->prepare(48000, 128, 1);
    EngineParameters p;
    p.enableOutputSummerMagnitudePole = true;
    p.enableOutputSummerMuteLoading = corrected;
    p.enableCommonVcaNoise = commonNoise;
    p.calibration = 1;
    p.aging = p.chorusNoise = p.noiseLevel = p.subLevel = 0;
    p.chorus = ChorusMode::Off;
    p.sawEnabled = p.pulseEnabled = false;
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    engine->setParameters(p);
    Render result;
    result.left.resize(65536);
    std::vector<float> right(result.left.size());
    for (std::size_t i = 0; i < result.left.size();)
    {
        const int count = std::min(block, static_cast<int>(result.left.size() - i));
        engine->process(result.left.data() + i, right.data() + i, count);
        i += static_cast<std::size_t>(count);
    }
    require(!YouKnowTestAccess::gate(*engine), "settled chorus Off left IC6's wet input connected");
    result.randomStates = YouKnowTestAccess::randomStates(*engine);
    return result;
}

double power(const std::vector<float>& signal)
{
    double sum = 0;
    for (std::size_t i = 4096; i < signal.size(); ++i)
    {
        require(std::isfinite(signal[i]), "nonfinite muted-output noise");
        sum += static_cast<double>(signal[i]) * signal[i];
    }
    return sum;
}

void fullIdleNoise()
{
    require(!EngineParameters {}.enableOutputSummerMuteLoading,
            "raw engine lost its always-connected reference convention");
    EngineParameters product;
    ProductFidelityProfile::applyTo(product);
    require(product.enableOutputSummerMuteLoading, "product omitted series-wet loading");
    for (bool commonNoise : {false, true})
    {
        const auto before = silence(false, commonNoise, 128);
        const auto after = silence(true, commonNoise, 128);
        require(after.left == silence(true, commonNoise, 1).left,
                "mute-loading noise depends on block partition");
        require(before.randomStates == after.randomStates,
                "mute-loading correction changed stochastic-source chronology");
        const double change = 10 * std::log10(power(after.left) / power(before.left));
        if (!commonNoise)
            require(change < -2.5 && change > -2.61,
                    "actual isolated IC6/passive floor did not follow the sourced reduction");
        else
            require(change < -.03 && change > -.4,
                    "whole idle floor has absent/excessive mute-loading change");
        std::cout << "Actual idle floor, common VCA noise " << (commonNoise ? "on" : "off")
                  << ": " << change << " dB\n";
    }
}

void engineGateToggle()
{
    auto engine = std::make_unique<YouKnowEngine>();
    engine->prepare(48000, 128, 1);
    EngineParameters p;
    p.enableOutputSummerMuteLoading = p.enableOutputSummerMagnitudePole = true;
    p.enableChorusClockMuteCircuit = p.enableChorusMuteDrive = true;
    p.calibration = p.chorusNoise = p.noiseLevel = p.subLevel = 0;
    p.chorus = ChorusMode::One;
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    engine->setParameters(p);
    std::array<float, 4800> left {}, right {};
    engine->process(left.data(), right.data(), 1);
    require(YouKnowTestAccess::gate(*engine), "engine did not read connected wet gate");
    p.chorus = ChorusMode::Off;
    engine->setParameters(p);
    engine->process(left.data(), right.data(), 2400);
    require(YouKnowTestAccess::gate(*engine), "engine closed wet input before the drive delay");
    engine->process(left.data(), right.data(), 2400);
    require(!YouKnowTestAccess::gate(*engine), "engine did not follow delayed wet disconnection");
    p.chorus = ChorusMode::One;
    engine->setParameters(p);
    engine->process(left.data(), right.data(), 4800);
    engine->process(left.data(), right.data(), 4800);
    require(YouKnowTestAccess::gate(*engine), "engine did not restore wet loading on delayed engage");
    for (float x : left) require(std::isfinite(x), "gate loading toggle produced nonfinite audio");
}
}

int main()
{
    try
    {
        componentOracle();
        delayedGate();
        fullIdleNoise();
        engineGateToggle();
        std::cout << "IC6 muted loading, physical gate and random-history checks passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
