#include "DSP/YouKnowEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace youknow
{
struct YouKnowTestAccess
{
    static float trim(const YouKnowEngine& engine)
    {
        return engine.activeParameters_.outputTrimDb;
    }

    static std::array<double, 11> circuitState(const YouKnowEngine& engine)
    {
        return { engine.glidedVolume_, engine.outputSlewStateLeft_,
            engine.outputSlewStateRight_, engine.outputBandwidthStateLeft_,
            engine.outputBandwidthStateRight_, engine.voices_[0].vcaInputVolts,
            engine.getDisplayEnvelope(), engine.getDisplayLfo(),
            engine.getDisplayTemperatureC(), engine.getDisplayRailDroopVolts(),
            static_cast<double>(engine.getDisplayVoiceMask()) };
    }
};
}

namespace
{
using namespace youknow;
constexpr int segmentFrames = 4096;
constexpr int totalFrames = 4 * segmentFrames;

void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

EngineParameters parametersFor(int route)
{
    EngineParameters parameters;
    parameters.chorus = ChorusMode::One;
    parameters.sustain = 1.0f;
    parameters.vcfTanhMode = VcfTanhMode::PolyZoned;
    parameters.vcfSolverMode = VcfSolverMode::Rk4Single;
    if (route == 1)
    {
        parameters.outputSelector = OutputNetwork::Selector::Low;
        parameters.outputLoadOhms = 10000.0f;
        parameters.outputCapacitancePf = 500.0f;
    }
    else if (route == 2)
    {
        parameters.outputRoute = HeadphoneOutput::Route::Headphones;
        parameters.enableHeadphoneAmplifierDynamics = true;
        parameters.enableHeadphoneIntrinsicNoise = true;
    }
    return parameters;
}

struct Take
{
    std::vector<float> left, right;
    std::array<double, 11> circuit;
};

Take render(int route, int block, std::optional<float> trim = std::nullopt,
            bool automate = false, int factor = 1)
{
    auto engine = std::make_unique<YouKnowEngine>();
    auto parameters = parametersFor(route);
    if (trim) parameters.outputTrimDb = *trim;
    engine->setParameters(parameters);
    engine->prepare(48000.0, 256, factor);
    for (const int note : { 48, 55, 60 }) engine->noteOn(note, 1.0f);
    Take result { std::vector<float>(totalFrames),
                  std::vector<float>(totalFrames), {} };
    for (int segment = 0; segment < 4; ++segment)
    {
        if (automate)
        {
            parameters.outputTrimDb = std::array { 0.0f, 12.0f, -12.0f, 0.0f }[segment];
            engine->setParameters(parameters);
        }
        for (int frame = segment * segmentFrames;
             frame < (segment + 1) * segmentFrames;)
        {
            const int count = std::min(block, (segment + 1) * segmentFrames - frame);
            engine->process(result.left.data() + frame, result.right.data() + frame, count);
            frame += count;
        }
    }
    result.circuit = YouKnowTestAccess::circuitState(*engine);
    return result;
}

void stationaryTrimPreservesEveryCircuit()
{
    require(EngineParameters {}.outputTrimDb == 0.0f, "trim default is not unity");
    for (const int route : { 0, 1, 2 })
    {
        const auto reference = render(route, 127);
        const auto explicitZero = render(route, 127, 0.0f);
        require(reference.left == explicitZero.left && reference.right == explicitZero.right,
                "explicit 0 dB changes the default output");
        for (const float trim : { -18.0f, 24.0f })
        {
            const auto calibrated = render(route, 127, trim);
            const float gain = static_cast<float>(std::pow(10.0, trim / 20.0));
            double power = 0.0, stereoDifference = 0.0;
            for (int frame = 0; frame < totalFrames; ++frame)
            {
                // Exact multiplication of the delivered sample is stronger
                // than RMS equality: any upstream saturation, loading, noise
                // or channel-balance change fails this on either route.
                require(calibrated.left[frame] == reference.left[frame] * gain
                            && calibrated.right[frame] == reference.right[frame] * gain,
                        "trim changed the signal beyond its final linear gain");
                power += reference.left[frame] * reference.left[frame]
                       + reference.right[frame] * reference.right[frame];
                stereoDifference += std::abs(reference.left[frame] - reference.right[frame]);
            }
            require(power > 1.0e-5 && stereoDifference > 1.0e-4,
                    "trim test needs an audible stereo fixture");
            require(reference.circuit == calibrated.circuit,
                    "digital trim changed physical circuit state or diagnostics");
        }
    }
}

void automationIsSmoothAndBlockInvariant()
{
    const auto reference = render(2, 127, 0.0f);
    const auto single = render(2, 1, 0.0f, true);
    const auto blocked = render(2, 127, 0.0f, true);
    require(single.left == blocked.left && single.right == blocked.right,
            "trim automation depends on block boundaries");
    require(reference.circuit == single.circuit,
            "trim automation disturbed internal circuitry or telemetry");
    constexpr std::array<double, 4> targets { 1.0, 3.9810717055349722,
                                             0.251188643150958, 1.0 };
    double previousGain = 1.0;
    for (int frame = 0; frame < totalFrames; ++frame)
    {
        require(std::isfinite(single.left[frame]) && std::isfinite(single.right[frame]),
                "trim automation produced nonfinite output");
        // Infer the gain from the actual audio; do not test the smoother by
        // repeating its update equation. Use whichever channel is stronger.
        const bool useLeft = std::abs(reference.left[frame]) >= std::abs(reference.right[frame]);
        const float before = useLeft ? reference.left[frame] : reference.right[frame];
        const float after = useLeft ? single.left[frame] : single.right[frame];
        if (std::abs(before) < 1.0e-8f) continue;
        const double gain = static_cast<double>(after) / before;
        const int segment = frame / segmentFrames;
        const double target = targets[segment];
        require(gain >= 0.25 && gain <= 4.0, "trim automation overshot its endpoints");
        require(std::abs(gain - previousGain) < 0.02,
                "trim automation stepped abruptly");
        if (frame % segmentFrames > 1)
            require(std::abs(gain - target) <= std::abs(previousGain - target) + 1.0e-6,
                    "trim glide did not converge monotonically");
        if (frame % segmentFrames == segmentFrames - 1)
            require(std::abs(gain - target) < 1.0e-5,
                    "trim did not settle after a parameter change");
        previousGain = gain;
    }
    // The trim is applied on the host grid even when the circuit is oversampled.
    const auto highQualityReference = render(0, 127, 0.0f, false, 4);
    const auto highQualityTrim = render(0, 127, -12.0f, false, 4);
    const float gain = static_cast<float>(std::pow(10.0, -12.0 / 20.0));
    for (int frame = 0; frame < totalFrames; ++frame)
        require(highQualityTrim.left[frame] == highQualityReference.left[frame] * gain
                    && highQualityTrim.right[frame] == highQualityReference.right[frame] * gain,
                "oversampling moved trim ahead of a physical stage");
}

void hostileInputAndReset()
{
    auto engine = std::make_unique<YouKnowEngine>();
    auto reference = std::make_unique<YouKnowEngine>();
    auto parameters = parametersFor(0);
    engine->prepare(48000.0, 32, 1);
    reference->prepare(48000.0, 32, 1);
    const std::array<std::array<float, 2>, 5> cases {{
        { std::numeric_limits<float>::quiet_NaN(), 0.0f },
        { std::numeric_limits<float>::infinity(), 0.0f },
        { -std::numeric_limits<float>::infinity(), 0.0f },
        { 1000.0f, 48.0f }, { -1000.0f, -48.0f }
    }};
    for (const auto& input : cases)
    {
        parameters.outputTrimDb = input[0];
        engine->setParameters(parameters);
        auto referenceParameters = parameters;
        referenceParameters.outputTrimDb = 0.0f;
        reference->setParameters(referenceParameters);
        require(YouKnowTestAccess::trim(*engine) == input[1],
                "trim did not sanitize its dB range");
        std::array<float, 32> left {}, right {}, referenceLeft {}, referenceRight {};
        engine->process(left.data(), right.data(), 32);
        reference->process(referenceLeft.data(), referenceRight.data(), 32);
        for (std::size_t frame = 0; frame < left.size(); ++frame)
            require(std::isfinite(left[frame]) && std::isfinite(right[frame]),
                    "invalid trim escaped into the audio");
    }
    // reset() primes the latest saved trim on the first real interval; invalid
    // calls cannot start a glide or retain the preceding gain.
    parameters.outputTrimDb = -12.0f;
    engine->setParameters(parameters);
    engine->reset();
    reference->reset();
    engine->process(nullptr, nullptr, 32);
    engine->noteOn(48, 1.0f);
    reference->noteOn(48, 1.0f);
    std::array<float, 1024> left {}, right {}, referenceLeft {}, referenceRight {};
    engine->process(left.data(), right.data(), 0);
    engine->process(left.data(), right.data(), 1024);
    reference->process(referenceLeft.data(), referenceRight.data(), 1024);
    const float gain = static_cast<float>(std::pow(10.0, -12.0 / 20.0));
    for (std::size_t frame = 0; frame < left.size(); ++frame)
        require(left[frame] == referenceLeft[frame] * gain
                    && right[frame] == referenceRight[frame] * gain,
                "reset retained trim smoothing history");
}
}

int main()
{
    try
    {
        stationaryTrimPreservesEveryCircuit();
        automationIsSmoothAndBlockInvariant();
        hostileInputAndReset();
        std::cout << "PASS: output trim is linear on both routes, preserves circuitry, "
                     "glides independently of blocks, and sanitizes/restores safely\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
