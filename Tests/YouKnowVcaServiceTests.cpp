// Roland JUNO-106 Service Notes, p.19, consecutive adjustments 5 and 6:
// bank 3, held C4, after ten-minute warmup. TP19 is set to 4.8 Vp-p;
// VR27 then sets TP8 to 6 Vp-p. Exercise the actual ENV converter write,
// C59, per-card trim, thermal drive, pair shape and output conversion.
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=19
// This is a node-level service check, not a fitted external recording or a
// calibration of the separate oscillator/mixer voltage coordinate.
#include "DSP/YouKnowEngine.h"

#include <algorithm>
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
    static void serviceControl(YouKnowEngine& engine, int card,
                               std::uint16_t sustainWord = 0x3f80)
    {
        auto& voice = engine.voices_[static_cast<std::size_t>(card)];
        auto& component = engine.cards_[static_cast<std::size_t>(card)];
        // Service trims remove residual gain/offset; thermal Character stays
        // active so the ten-minute reference is exercised on every card.
        component.vcaGainError = component.vcaControlOffset = 0.0f;
        voice.active = voice.keyDown = true;
        voice.sustained = false;
        voice.envelope.level = sustainWord;
        voice.envelope.gate = voice.envelope.running = true;
        voice.envelope.phase = true;
        voice.envelope.tick(1, 0, sustainWord, 0);
        engine.performConverterWrite(
            { YouKnowEngine::ConverterDestination::VoiceVca, card },
            engine.activeParameters_);
        // The held note has settled before its waveform is measured.
        voice.vcaControl = voice.vcaControlTarget;
        engine.updateVoiceAudio(voice, engine.activeParameters_);
    }
    static double control(const YouKnowEngine& engine, int card)
    {
        return engine.voices_[static_cast<std::size_t>(card)].vcaControl;
    }
    static double rate(const YouKnowEngine& engine)
    {
        return engine.oversampledRate_;
    }
    static double finishVolts(YouKnowEngine& engine, int card, float volts)
    {
        return engine.finishVoiceFilter(
            engine.voices_[static_cast<std::size_t>(card)], volts)
            * YouKnowEngine::internalVoltsPerUnit;
    }
    static double capacitor(const YouKnowEngine& engine, int card)
    {
        return engine.voices_[static_cast<std::size_t>(card)]
            .vcaInputCoupling.state;
    }
    static double thermalDrive(const YouKnowEngine& engine, int card)
    {
        return engine.voiceVcaThermalDriveScale(engine.activeParameters_, card);
    }
};
}

namespace
{
using youknow::YouKnowEngine;
using Probe = youknow::YouKnowTestAccess;
constexpr double pi = std::numbers::pi_v<double>;

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
std::unique_ptr<YouKnowEngine> engine(double rate, int factor,
                                    float character, bool correction)
{
    auto result = std::make_unique<YouKnowEngine>();
    require(result->configureThermalStart(true), "settled thermal start rejected");
    youknow::EngineParameters parameters;
    parameters.calibration = character;
    parameters.vcaMode = youknow::VcaMode::Envelope;
    parameters.enableVoiceVcaServiceGain = correction;
    result->setParameters(parameters);
    result->prepare(rate, 128, factor);
    return result;
}

struct Measurement
{
    double correctedPeakToPeak {};
    double legacyPeakToPeak {};
    double sampleScaleError {};
};
Measurement serviceSine(double hostRate, int factor, float character, int card)
{
    auto corrected = engine(hostRate, factor, character, true);
    auto legacy = engine(hostRate, factor, character, false);
    Probe::serviceControl(*corrected, card);
    Probe::serviceControl(*legacy, card);
    near(Probe::control(*corrected, card), 4064.0 / 4095.0, 1e-7,
         "bank-3 maximum sustain did not reach physical code4064");
    near(Probe::thermalDrive(*corrected, card), 1.0, 0.0,
         "settled card changed its fixed service input trim");

    const double rate = Probe::rate(*corrected);
    const int count = static_cast<int>(2 * rate);
    double correctedLow = 1e9, correctedHigh = -1e9;
    double legacyLow = 1e9, legacyHigh = -1e9, scaleError = 0;
    const double scale = YouKnowEngine::VoiceVcaSignalLaw::serviceGain();
    for (int sample = 0; sample < count; ++sample)
    {
        const float input = static_cast<float>(
            2.4 * std::sin(2.0 * pi * 248.0 * sample / rate));
        const double after = Probe::finishVolts(*corrected, card, input);
        const double before = Probe::finishVolts(*legacy, card, input);
        scaleError = std::max(scaleError, std::abs(after - scale * before));
        if (sample < static_cast<int>(rate)) continue;
        correctedLow = std::min(correctedLow, after);
        correctedHigh = std::max(correctedHigh, after);
        legacyLow = std::min(legacyLow, before);
        legacyHigh = std::max(legacyHigh, before);
    }
    require(Probe::capacitor(*corrected, card) == Probe::capacitor(*legacy, card),
            "fixed service gain changed C59 capacitor charge");
    const Measurement result { correctedHigh - correctedLow,
                               legacyHigh - legacyLow, scaleError };
    // The expected 6-V figure is Roland's measured-node target, independent
    // of the implementation's gain formula. C59's finite loss at 248 Hz and
    // discrete waveform extrema leave less than 0.5 mV of discrepancy.
    near(result.correctedPeakToPeak, 6.0, 0.0005,
         "the actual VCA path missed Roland's 6-Vp-p service target");
    near(result.legacyPeakToPeak, 4.69076, 0.0005,
         "the comparison switch did not preserve the old service shortfall");
    require(result.sampleScaleError < 1e-6,
            "service correction changed the pair's normalized timbre");
    return result;
}

void lowerControlsKeepTheirRelativeLaw()
{
    auto corrected = engine(48000, 1, 0, true);
    auto legacy = engine(48000, 1, 0, false);
    const double scale = YouKnowEngine::VoiceVcaSignalLaw::serviceGain();
    for (std::uint16_t word : { 0u, 256u, 1024u, 8192u, 16256u, 16383u })
    {
        Probe::serviceControl(*corrected, 0, word);
        Probe::serviceControl(*legacy, 0, word);
        for (int sample = 0; sample < 1000; ++sample)
        {
            const float input = static_cast<float>(
                6.8 * std::sin(2.0 * pi * 997.0 * sample / 48000.0));
            const double after = Probe::finishVolts(*corrected, 0, input);
            const double before = Probe::finishVolts(*legacy, 0, input);
            near(after, before * scale, 2e-6,
                 "fixed service gain reshaped the envelope or voice saturation");
        }
    }
}

std::vector<float> renderOutput(bool corrected, bool strong)
{
    auto result = std::make_unique<YouKnowEngine>();
    youknow::EngineParameters parameters;
    parameters.calibration = 0.0f;
    parameters.enableVoiceVcaServiceGain = corrected;
    parameters.sawEnabled = true;
    parameters.pulseEnabled = strong;
    parameters.pwmSource = youknow::PwmSource::Manual;
    parameters.pwmDepth = 0.37f;
    parameters.subLevel = strong ? 0.5f : 0.0f;
    parameters.noiseLevel = parameters.envDepth = parameters.keyFollow = 0.0f;
    parameters.resonance = 0.0f;
    parameters.cutoff = 1.0f;
    parameters.attack = parameters.release = 0.0f;
    parameters.decay = parameters.sustain = 1.0f;
    parameters.vcaLevel = strong ? 1.0f : 0.2f;
    parameters.volume = 1.0f;
    parameters.highPass = youknow::HighPassMode::One;
    parameters.chorus = youknow::ChorusMode::Off;
    result->setParameters(parameters);
    result->prepare(48000, 128, 4);
    if (strong)
        for (int note : { 36, 43, 48, 52, 55, 60 })
            result->noteOn(note, 1.0f);
    else
        result->noteOn(60, 1.0f);
    constexpr int count = 24000 + 4096;
    std::vector<float> left(count), right(count);
    for (int start = 0; start < count; start += 128)
        result->process(left.data() + start, right.data() + start,
                        std::min(128, count - start));
    return left;
}

void finalNormalizationPreservesDriveAndHeadroom()
{
    for (bool strong : { false, true })
    {
        const auto corrected = renderOutput(true, strong);
        const auto legacy = renderOutput(false, strong);
        double correctedPower = 0.0, legacyPower = 0.0, differencePower = 0.0;
        double correctedPeak = 0.0, legacyPeak = 0.0;
        for (std::size_t frame = 24000; frame < corrected.size(); ++frame)
        {
            const double after = corrected[frame];
            const double before = legacy[frame];
            correctedPower += after * after;
            legacyPower += before * before;
            differencePower += (after - before) * (after - before);
            correctedPeak = std::max(correctedPeak, std::abs(after));
            legacyPeak = std::max(legacyPeak, std::abs(before));
        }
        require(legacyPower > 1e-5, "digital-boundary fixture was silent");
        if (!strong)
            near(10.0 * std::log10(correctedPower / legacyPower), 0.0, 0.005,
                 "service calibration added global gain to a quiet dry voice");
        else
        {
            require(legacyPeak < 1.0,
                    "the fixed six-note reference already exceeds full scale");
            require(correctedPeak < 1.0,
                    "service calibration introduced avoidable digital overload");
            // This fixture reaches output-stage compression. Moving the
            // reciprocal trim ahead of that stage would almost cancel the
            // circuit correction and fail this check: internal physical
            // drive must change even when ordinary output loudness does not.
            require(differencePower / legacyPower > 1e-6,
                    "output normalization cancelled the physical drive correction");
            std::cout << "Fixed six-note digital peaks: " << legacyPeak
                      << " -> " << correctedPeak << '\n';
        }
    }
}
}

int main()
{
    try
    {
        require(youknow::EngineParameters {}.enableVoiceVcaServiceGain,
                "service voltage gain must be enabled by default");
        for (double rate : { 48000.0, 96000.0 })
            for (int factor : { 1, 4 })
                serviceSine(rate, factor, 0.0f, 0);
        Measurement reference;
        for (int card = 0; card < 6; ++card)
            reference = serviceSine(48000, 4, 1.0f, card);
        lowerControlsKeepTheirRelativeLaw();
        finalNormalizationPreservesDriveAndHeadroom();
        std::cout << "Voice VCA service checks passed: "
                  << reference.legacyPeakToPeak << " -> "
                  << reference.correctedPeakToPeak << " Vp-p, "
                  << 20.0 * std::log10(reference.correctedPeakToPeak
                                      / reference.legacyPeakToPeak)
                  << " dB fixed gain\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
