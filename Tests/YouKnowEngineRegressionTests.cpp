// Engine regressions first written for the Reason Rack adapter, which keeps
// none of its own: the Rack compiles this engine directly, so these run here
// against the C++17 YouKnowDSPCxx17 build it uses.
#include "DSP/YouKnowEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace youknow
{
struct YouKnowTestAccess
{
    static float chorusWetGain(const Chorus& chorus) noexcept
    {
        return chorus.wetGain_;
    }

    static void setThermalWarmupSeconds(YouKnowEngine& engine,
                                        double seconds) noexcept
    {
        engine.thermalWarmupSeconds_ = seconds;
        engine.thermalWarmupFraction_ = 1.0f - std::exp(
            -static_cast<float>(seconds)
            / static_cast<float>(YouKnowEngine::thermalWarmupTimeConstantSeconds));
    }
};
} // namespace youknow

namespace
{
using namespace youknow;
constexpr int blockSize = 64;
constexpr double pi = 3.1415926535897932384626433832795;
struct Render { std::vector<float> left, right; };
void expect(bool valid, const std::string& message)
{
    if (!valid) { std::fprintf(stderr, "%s\n", message.c_str()); std::exit(1); }
}
void expectNear(double actual, double expected, double tolerance,
                const std::string& message)
{
    expect(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
           message + " (actual=" + std::to_string(actual)
               + ", expected=" + std::to_string(expected) + ")");
}
Render renderExact(YouKnowEngine& engine, int samples)
{
    Render result;
    result.left.assign(static_cast<std::size_t>(samples), 0.0f);
    result.right.assign(static_cast<std::size_t>(samples), 0.0f);
    for (int offset = 0; offset < samples; offset += blockSize)
    {
        const int count = std::min(blockSize, samples - offset);
        engine.process(result.left.data() + offset,
                       result.right.data() + offset, count);
    }
    return result;
}

EngineParameters plainPatch()
{
    EngineParameters parameters;
    parameters.sawEnabled = true;
    parameters.pulseEnabled = false;
    parameters.subLevel = 0.0f;
    parameters.noiseLevel = 0.0f;
    parameters.highPass = HighPassMode::One;
    parameters.cutoff = 1.0f;
    parameters.resonance = 0.0f;
    parameters.envDepth = 0.0f;
    parameters.keyFollow = 0.0f;
    parameters.attack = 0.0f;
    parameters.decay = 1.0f;
    parameters.sustain = 1.0f;
    parameters.release = 0.0f;
    // The stored LEVEL slider is a shared post-sum trim. Byte 99 is the nearest
    // stored setting to 0 dB in the nominal jack-board/NEC solve (+0.073 dB);
    // full travel adds 4.71 dB and changes the downstream chorus drive.
    parameters.vcaLevel = 99.0f / 127.0f;
    parameters.volume = 1.0f;
    parameters.chorus = ChorusMode::Off;
    // The plain reference patch carries no character at all: no component
    // spread, no drift, and none of the inherent circuit non-linearities --
    // all of it answers to this one control.
    parameters.calibration = 0.0f;
    return parameters;
}

void testOutputJackCoefficientsTrackTheAnalogCornerAtEveryHostRate()
{
    // The matched one-pole (OutputJackLowPass) replaces the former
    // exponential blend. Adapted from the upstream OutputJack suite: at every
    // supported host rate and wiper position the discretization must be
    // stable, finite, and within 0.35 dB of the continuous RC magnitude up to
    // min(20 kHz, 0.45 fs), where the former design lost most of the roll-off.
    const auto corner = [](double volume) {
        // Independent p.15 component reduction, not the production helper.
        constexpr double load = 41300.0 * 101000.0 / (41300.0 + 101000.0);
        const double upper = 1500.0 + 10000.0 * (1.0 - volume);
        const double lower = 10000.0 * volume;
        const double shunt = lower * load / (lower + load);
        return 1.0 / (2.0 * pi * 1e-9 * (2200.0 + upper * shunt / (upper + shunt)));
    };
    double worst = 0.0;
    for (const double rate : { 8000.0, 11025.0, 22050.0, 32000.0, 44100.0,
                               48000.0, 64000.0, 88200.0, 96000.0, 192000.0,
                               384000.0, 768000.0 })
        for (const double position : { 0.0, 0.25, 0.5, 0.75, 1.0 })
        {
            const double pole = corner(position);
            const auto coefficients = OutputJackLowPass::coefficients(pole, rate);
            expect(std::isfinite(coefficients.a1)
                       && std::isfinite(coefficients.correction)
                       && std::abs(coefficients.a1) < 1.0,
                   "jack coefficients are nonfinite or unstable at "
                       + std::to_string(rate) + " Hz");
            expectNear(YouKnowEngine::outputJackCornerHz(static_cast<float>(position)),
                       pole, 1.0e-3 * pole,
                       "the production jack corner departs from the p.15 reduction");
            OutputJackLowPass filter;
            std::array<float, 256> impulse {};
            for (std::size_t index = 0; index < impulse.size(); ++index)
            {
                impulse[index] = filter.process(index == 0 ? 1.0f : 0.0f, coefficients);
                expect(std::isfinite(impulse[index]), "jack impulse is nonfinite");
            }
            for (int bin = 0; bin <= 160; ++bin)
            {
                const double frequency = std::min(20000.0, 0.45 * rate) * bin / 160.0;
                const auto z = std::polar(1.0, -2.0 * pi * frequency / rate);
                std::complex<double> response {}, power { 1.0, 0.0 };
                for (const float value : impulse)
                {
                    response += static_cast<double>(value) * power;
                    power *= z;
                }
                const double analog =
                    1.0 / std::sqrt(1.0 + (frequency / pole) * (frequency / pole));
                const double error = std::abs(
                    20.0 * std::log10(std::abs(response) / analog));
                expect(std::isfinite(error), "jack frequency response is nonfinite");
                worst = std::max(worst, error);
            }
        }
    expect(worst <= 0.35,
           "the matched jack pole departs from the analog RC by "
               + std::to_string(worst) + " dB");

    // A constant input must stay exactly constant through the correction
    // term, whatever coefficients VOLUME hands it.
    OutputJackLowPass filter;
    const auto coefficients = OutputJackLowPass::coefficients(corner(0.5), 48000.0);
    float last = 0.0f;
    for (int index = 0; index < 4096; ++index)
        last = filter.process(0.25f, coefficients);
    expect(last == 0.25f, "a held input drifted through the jack pole");
}

void testVoiceVcaServiceGainIsFrozenBitExactly()
{
    // Rack freezes VoiceVcaSignalLaw::serviceGain() so chip code carries no
    // guarded static; the source expression must still reproduce it exactly.
    using Signal = YouKnowEngine::VoiceVcaSignalLaw;
    using Control = YouKnowEngine::VoiceVcaControlLaw;
    const float expected = Signal::trimOutputPeakVolts
        / (Signal::shape(Signal::trimFilterPeakVolts)
           * Control::gain(4064.0f / 4095.0f));
    std::uint32_t frozenBits {}, expectedBits {};
    const float frozen = Signal::serviceGain();
    std::memcpy(&frozenBits, &frozen, sizeof(frozenBits));
    std::memcpy(&expectedBits, &expected, sizeof(expectedBits));
    expect(frozenBits == expectedBits,
           "the frozen VCA service gain differs from the source expression");
    expectNear(frozen, 1.2790, 5.0e-4,
               "the service gain is not the documented +2.138 dB");
}

void testWarmOutputIsIndependentOfBlockPartition()
{
    // Jack-board temperature belongs to the converter pass. Sampling it at
    // callback entry made identical event streams depend on Reason's splits.
    for (const int factor : { 1, 2, 4 })
    {
        for (const double warmup : { 0.0, 450.0, 900.0 })
        {
            YouKnowEngine regular;
            YouKnowEngine partitioned;
            auto parameters = plainPatch();
            parameters.calibration = 1.5f;
            parameters.vcaLevel = 0.1f;
            parameters.noiseLevel = 0.2f;
            for (auto* engine : { &regular, &partitioned })
            {
                engine->prepare(48000.0, blockSize, factor);
                engine->setParameters(parameters);
                YouKnowTestAccess::setThermalWarmupSeconds(*engine, warmup);
                engine->noteOn(60, 1.0f);
            }
            constexpr int samples = 4096;
            const auto reference = renderExact(regular, samples);
            Render result;
            result.left.resize(samples);
            result.right.resize(samples);
            constexpr std::array<int, 7> splits { 1, 17, 63, 2, 31, 64, 7 };
            int offset = 0;
            std::size_t index = 0;
            while (offset < samples)
            {
                const int count = std::min(splits[index++ % splits.size()],
                                           samples - offset);
                partitioned.process(result.left.data() + offset,
                                     result.right.data() + offset, count);
                offset += count;
            }
            expect(reference.left == result.left && reference.right == result.right,
                   "warm output changed with callback partition at factor "
                       + std::to_string(factor) + ", warmup "
                       + std::to_string(warmup));
        }
    }
}

void testChorusSettlesWithoutHostDenormalPolicy()
{
    for (const float rate : { 48000.0f, 192000.0f })
    {
        for (const bool muteDrive : { false, true })
        {
            Chorus chorus;
            chorus.prepare(rate);
            float left {}, right {};
            const auto run = [&](ChorusMode mode, float seconds) {
                for (int sample = 0; sample < static_cast<int>(rate * seconds);
                     ++sample)
                    chorus.process(0.0f, mode, 0.0f, left, right,
                                   1.0f, true, muteDrive, false);
            };
            run(ChorusMode::One, 0.02f);
            expect(!chorus.processBypassedWhenSettled(0.0f, left, right),
                   "chorus bypass engaged while wet return was open");
            run(ChorusMode::Off, 1.0f);
            expect(YouKnowTestAccess::chorusWetGain(chorus) == 0.0f
                       && chorus.processBypassedWhenSettled(0.0f, left, right),
                   "chorus did not settle to exact zero without host FTZ");
        }
    }
}
} // namespace

int main()
{
    testOutputJackCoefficientsTrackTheAnalogCornerAtEveryHostRate();
    testVoiceVcaServiceGainIsFrozenBitExactly();
    testWarmOutputIsIndependentOfBlockPartition();
    testChorusSettlesWithoutHostDenormalPolicy();
    std::puts("engine regressions passed (4 groups)");
}
