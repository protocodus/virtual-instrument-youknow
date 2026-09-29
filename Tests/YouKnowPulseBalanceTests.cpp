#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace youknow
{
struct YouKnowTestAccess
{
    static double prime(YouKnowEngine& engine)
    {
        auto& voice = engine.voices_[0];
        engine.primeVoiceWaveNode(voice, engine.activeParameters_);
        return voice.moduleCoupling.state;
    }
    static double freewheel(YouKnowEngine& engine, int count)
    {
        auto& voice = engine.voices_[0];
        for (int i = 0; i < count; ++i) engine.freewheelVoiceCard(voice);
        return voice.moduleCoupling.state;
    }
    static float filterInput(YouKnowEngine& engine)
    {
        return engine.prepareVoiceFilter(engine.voices_[0], engine.activeParameters_, 0).input;
    }
};
}

namespace
{
using namespace youknow;
constexpr float chosenScale = 0.857f;
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
EngineParameters patch()
{
    EngineParameters p;
    p.calibration = 0;
    p.sawEnabled = false;
    p.pulseEnabled = true;
    p.subLevel = p.noiseLevel = 0;
    p.pwmSource = PwmSource::Manual;
    p.pwmDepth = .42f;
    p.chorus = ChorusMode::Off;
    p.chorusNoise = 0;
    p.cutoff = 1;
    p.resonance = p.envDepth = p.keyFollow = 0;
    p.attack = p.release = 0;
    p.sustain = 1;
    p.vcaMode = VcaMode::Gate;
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfFastEarlyMode = VcfFastEarlyMode::Cubic;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    return p;
}

std::vector<float> render(float scale, int note, int factor, int block,
                          bool pulse = true)
{
    YouKnowEngine engine;
    require(engine.configurePulseLevelScale(scale), "pulse scale rejected");
    engine.prepare(48000, 128, factor);
    auto p = patch();
    p.pulseEnabled = pulse;
    p.sawEnabled = !pulse;
    engine.setParameters(p);
    engine.noteOn(note, 1);
    std::vector<float> result(24000), right(result.size());
    for (int segment = 0; segment < 6; ++segment)
    {
        if (segment == 1) p.pwmDepth = .7f;
        if (segment == 2) engine.noteOff(note);
        if (segment == 3) p.pulseEnabled = false;
        if (segment == 4)
        {
            p.pulseEnabled = pulse;
            p.pwmDepth = .23f;
            engine.noteOn(note, 1);
        }
        engine.setParameters(p);
        for (int at = segment * 4000; at < (segment + 1) * 4000;)
        {
            const int count = std::min(block, (segment + 1) * 4000 - at);
            engine.process(result.data() + at, right.data() + at, count);
            at += count;
        }
    }
    for (float value : result) require(std::isfinite(value), "nonfinite pulse audio");
    return result;
}

void checkWaveNodePaths(int note, int factor)
{
    YouKnowEngine reference, scaled;
    require(scaled.configurePulseLevelScale(chosenScale), "scale setup failed");
    for (auto* engine : { &reference, &scaled })
    {
        engine->prepare(48000, 128, factor);
        engine->setParameters(patch());
        engine->noteOn(note, 1);
        std::array<float, 4096> left {}, right {};
        engine->process(left.data(), right.data(), static_cast<int>(left.size()));
    }
    const double nominal = YouKnowTestAccess::prime(reference);
    const double reduced = YouKnowTestAccess::prime(scaled);
    require(std::abs(nominal) > .1, "priming fixture has no pulse DC");
    require(std::abs(reduced - chosenScale * nominal) < 2e-6,
            "C56 priming did not scale the pulse DC");
    const double idle = YouKnowTestAccess::freewheel(reference, 2048);
    const double idleScaled = YouKnowTestAccess::freewheel(scaled, 2048);
    require(std::abs(idleScaled - chosenScale * idle) < 2e-6,
            "closed-VCA endpoint/mean tracking lost pulse balance");
    // Verify the relative source before the nonlinear filter/VCA. Output
    // waveform equality would wrongly prohibit the intended change in drive.
    double energy = 0, error = 0;
    for (int i = 0; i < 4096; ++i)
    {
        const double expected = chosenScale * static_cast<double>(
            YouKnowTestAccess::filterInput(reference));
        const double difference = YouKnowTestAccess::filterInput(scaled) - expected;
        energy += expected * expected;
        error += difference * difference;
    }
    require(energy > .01 && std::sqrt(error / energy) < 2e-6,
            "rendered pulse source missed the relative level scale");
}
}

int main()
{
    try
    {
        YouKnowEngine engine;
        require(engine.pulseLevelScale() == 1, "raw pulse reference changed");
        require(engine.configurePulseLevelScale(chosenScale), "valid pulse scale rejected");
        for (float invalid : { 0.f, .249f, 2.001f,
                 std::numeric_limits<float>::infinity(),
                 std::numeric_limits<float>::quiet_NaN() })
            require(!engine.configurePulseLevelScale(invalid)
                        && engine.pulseLevelScale() == chosenScale,
                    "invalid pulse scale mutated configuration");
        engine.prepare(48000, 128, 1);
        require(!engine.configurePulseLevelScale(1), "live pulse configuration accepted");
        engine.reset();
        engine.prepare(96000, 128, 2);
        require(engine.pulseLevelScale() == chosenScale, "lifecycle lost pulse scale");

        const CoupledSubMixer::Calibration circuit { 10000, 47000, .5, 0, .6, .1 };
        YouKnowEngine coupled, independent;
        require(coupled.configureCoupledMixer(circuit)
                    && !coupled.configurePulseLevelScale(chosenScale),
                "pulse balance overrode coupled-mixer calibration");
        require(independent.configurePulseLevelScale(chosenScale)
                    && !independent.configureCoupledMixer(circuit),
                "coupled mixer accepted an independent pulse scale");
        YouKnowEngine product, coupledProduct;
        ProductFidelityProfile::configureBeforePrepare(product);
        ProductFidelityProfile::configureBeforePrepare(coupledProduct, &circuit);
        require(product.pulseLevelScale() == chosenScale
                    && coupledProduct.pulseLevelScale() == 1,
                "product pulse selection or calibrated-mixer isolation failed");

        double minimumOutputRatio = 1, maximumOutputRatio = 0;
        for (int factor : { 1, 2, 4 })
            for (int note : { 24, 84 })
            {
                checkWaveNodePaths(note, factor);
                const auto reference = render(1, note, factor, 128);
                const auto scaled = render(chosenScale, note, factor, 128);
                double energy = 0, reducedEnergy = 0;
                for (std::size_t i = 0; i < reference.size(); ++i)
                {
                    energy += static_cast<double>(reference[i]) * reference[i];
                    reducedEnergy += static_cast<double>(scaled[i]) * scaled[i];
                }
                require(energy > 1e-6, "pulse fixture rendered silence");
                const double ratio = std::sqrt(reducedEnergy / energy);
                minimumOutputRatio = std::min(minimumOutputRatio, ratio);
                maximumOutputRatio = std::max(maximumOutputRatio, ratio);
                require(ratio > .82 && ratio < .94,
                        "relative pulse calibration did not reach the output");
                require(scaled == render(chosenScale, note, factor, 1),
                        "pulse balance depends on block partition");
            }
        // With pulse pinned off throughout, its changed DC is already stored
        // in C56. A relative pulse calibration must not become a saw gain.
        const auto saw = render(1, 60, 1, 128, false);
        const auto scaledSaw = render(chosenScale, 60, 1, 128, false);
        double sawError = 0;
        for (std::size_t i = 0; i < saw.size(); ++i)
            sawError = std::max(sawError, std::abs(static_cast<double>(saw[i]) - scaledSaw[i]));
        require(sawError < 2e-6, "pulse calibration changed the saw-only signal");
        std::cout << "PASS: pulse configuration, product selection, C56 priming, low/high idle tracking, "
                     "render scaling, PWM/off transitions and block invariance; output ratios "
                  << minimumOutputRatio << ".." << maximumOutputRatio
                  << ", saw-only max error " << sawError << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
