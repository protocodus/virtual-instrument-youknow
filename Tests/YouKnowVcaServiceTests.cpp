// Roland JUNO-106 Service Notes, p.19, consecutive adjustments 5 and 6:
// bank 3, held C4, after ten-minute warmup. TP19, AFTER C59 at the hot
// end of VR27, is set to 4.8 Vp-p;
// VR27 then sets TP8 to 6 Vp-p. Exercise the actual ENV converter write,
// C59, per-card trim, thermal drive, pair shape and output conversion.
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=19
// This is a node-level service check, not a fitted external recording or a
// calibration of the separate oscillator/mixer voltage coordinate.
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"

#include <algorithm>
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
    static void serviceControl(YouKnowEngine& engine, int card,
                               std::uint16_t sustainWord = 0x3f80)
    {
        auto& voice = engine.voices_[static_cast<std::size_t>(card)];
        auto& component = engine.cards_[static_cast<std::size_t>(card)];
        // Service trims remove residual gain/offset; thermal Character stays
        // active so the ten-minute reference is exercised on every card.
        component.vcaGainError = component.vcaControlOffset = 0.0f;
        engine.refreshVoiceVcaCoupling();
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
    static double coefficient(const YouKnowEngine& engine, int card)
    {
        return engine.cards_[static_cast<std::size_t>(card)].vcaInputCouplingG;
    }
    static double input(const YouKnowEngine& engine, int card)
    {
        return engine.voices_[static_cast<std::size_t>(card)].vcaInputVolts;
    }
    static void gainError(YouKnowEngine& engine, float error)
    {
        for (auto& card : engine.cards_) card.vcaGainError = error;
        engine.refreshVoiceVcaCoupling();
    }
    static void warmup(YouKnowEngine& engine, float fraction)
    {
        engine.thermalWarmupFraction_ = fraction;
        engine.refreshVoiceCardThermalScales();
    }
    static void changeRate(YouKnowEngine& engine, double rate, int factor)
    {
        // The actual quality-rebuild function, with state preservation. Host
        // prepare() intentionally resets the instrument, so it is not used
        // to simulate a rate-preserving rebuild in this node-level test.
        engine.sampleRate_ = rate;
        engine.inverseSampleRate_ = static_cast<float>(1.0 / rate);
        engine.oversamplingRequested_ = factor;
        engine.oversamplingApplied_ = factor;
        engine.updateProcessingRate(true);
    }
};
}

namespace
{
using youknow::YouKnowEngine;
using Probe = youknow::YouKnowTestAccess;
constexpr double pi = std::numbers::pi_v<double>;
using Complex = std::complex<double>;

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
    require(result->configureServiceDerivedVcaCoupling(true), "nominal C59 rejected");
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
    // This test injects before C59, whereas Roland's TP19 is after it. The
    // expected 6-V figure is the physical measured-node target, not a fit;
    // C59's tiny 248-Hz loss and discrete extrema explain the <0.5mV error.
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

// Solve the full three-node passive network independently by complex nodal
// elimination: source--C59--TP19--(VR27+R108)--pin9--4.7k--pair input,
// with R112+VR30 source resistance at pin9 and 560 ohms at the pair input.
// This oracle never calls the engine's pole/divider helpers.
std::array<Complex, 3> physicalNetwork(double frequency, double seriesOhms)
{
    const Complex capacitor(0.0, 2.0 * pi * frequency * 1e-6);
    const double series = 1.0 / seriesOhms;
    const double hybrid = 1.0 / 4700.0;
    std::array<std::array<Complex, 4>, 3> matrix {{
        { capacitor + series, -series, 0.0, capacitor },
        { -series, series + hybrid + 1.0 / 2225000.0, -hybrid, 0.0 },
        { 0.0, -hybrid, hybrid + 1.0 / 560.0, 0.0 }
    }};
    for (int column = 0; column < 3; ++column)
    {
        const Complex diagonal = matrix[column][column];
        for (int entry = column; entry < 4; ++entry)
            matrix[column][entry] /= diagonal;
        for (int row = 0; row < 3; ++row)
        {
            if (row == column) continue;
            const Complex multiple = matrix[row][column];
            for (int entry = column; entry < 4; ++entry)
                matrix[row][entry] -= multiple * matrix[column][entry];
        }
    }
    return { matrix[0][3], matrix[1][3], matrix[2][3] };
}

double inferredResistance(const YouKnowEngine& device, int card)
{
    return 0.5 / (std::atan(Probe::coefficient(device, card))
                  * Probe::rate(device) * 1e-6);
}

void physicalDividerAndServiceTarget()
{
    // Direct current/voltage service calculation from the documented parts,
    // rather than the engine's headroom/trimDrive constants.
    constexpr double tail = (10.026514 + 0.26 - 0.62) / 32000.0;
    const double drive = std::atanh(3.0 / (47000.0 * tail));
    const double load = 1.0 / (1.0 / 5260.0 + 1.0 / 2225000.0);
    for (float character : { 0.0f, 1.0f, 2.0f })
        for (bool spatial : { false, true })
            for (float error : { -1.0f, 0.0f, 1.0f })
            {
                auto device = engine(48000, 1, character, true);
                youknow::EngineParameters parameters;
                parameters.calibration = character;
                parameters.enableSpatialThermalGradient = spatial;
                device->setParameters(parameters);
                Probe::gainError(*device, error);
                for (int card = 0; card < 6; ++card)
                {
                    const double serviceKelvin = 298.15 + character *
                        (15.0 + (spatial ? 4.0 * std::exp(-card / 2.5) : 0.0));
                    const double trim = 1.0 + 0.03 * character * error;
                    const double divider = 0.052 * serviceKelvin / 298.15
                        * drive / 2.4 * trim;
                    const double total = load * (560.0 / 5260.0) / divider;
                    const double measured = inferredResistance(*device, card);
                    near(measured, total, 0.04,
                         "C59 does not use the physical service-divider load");
                    const double vr27 = measured - 82000.0 - load;
                    require(vr27 >= 0.0 && vr27 <= 50000.0,
                            "the implied VR27 is outside its physical travel");
                    for (double frequency : { 0.1, 1.0, 20.0, 248.0, 10000.0 })
                    {
                        const auto nodes = physicalNetwork(frequency, 82000.0 + vr27);
                        const Complex s(0.0, 2.0 * pi * frequency);
                        const Complex normalized = s * total * 1e-6
                            / (1.0 + s * total * 1e-6);
                        near(std::abs(nodes[0] - normalized), 0.0, 2e-7,
                             "complex C59 transfer misses independent nodal solve");
                        near(std::abs(nodes[2] / nodes[0]), divider, 2e-9,
                             "physical pair divider is counted incorrectly");
                        const double physicalPeak = tail * 47000.0 * std::tanh(
                            2.4 * std::abs(nodes[2] / nodes[0])
                            / (0.052 * serviceKelvin / 298.15));
                        near(physicalPeak, tail * 47000.0 * std::tanh(drive * trim),
                             2e-6, "per-card divider no longer realizes the service law");
                        if (error == 0.0f)
                            near(physicalPeak, 3.0, 2e-6,
                                 "TP19's 4.8Vpp does not give TP8's 6Vpp");
                    }
                }
            }
}

void couplingDecayAndProfile()
{
    // Real ProductFidelity configuration must reach the changed render path.
    // Calling only the new comparison API would not catch a missing product
    // selection. All other product changes are upstream/downstream of this
    // node and cannot affect the capacitor response measured here.
    for (double rate : { 8000.0, 44100.0, 48000.0, 96000.0, 768000.0 })
        for (int factor : { 1, 4 })
        {
            auto product = std::make_unique<YouKnowEngine>();
            auto raw = std::make_unique<YouKnowEngine>();
            youknow::ProductFidelityProfile::configureBeforePrepare(*product);
            youknow::EngineParameters parameters;
            parameters.calibration = 0.0f;
            product->setParameters(parameters);
            raw->setParameters(parameters);
            product->prepare(rate, 128, factor);
            raw->prepare(rate, 128, factor);
            require(!product->configureServiceDerivedVcaCoupling(false),
                    "prepared circuit configuration was mutable");
            const double resistance = inferredResistance(*product, 0);
            near(resistance, 120191.797858, 0.04,
                 "product did not select nominal service-derived C59");
            near(inferredResistance(*raw, 0), 82000.0, 0.02,
                 "raw frozen C59 reference changed");
            const int frames = static_cast<int>(0.2 * Probe::rate(*product));
            for (int frame = 0; frame < frames; ++frame)
            {
                Probe::finishVolts(*product, 0, 1.0f);
                Probe::finishVolts(*raw, 0, 1.0f);
            }
            const double time = (frames - 0.5) / Probe::rate(*product);
            near(Probe::input(*product, 0), std::exp(-time / (resistance * 1e-6)),
                 5e-5, "nominal C59 step misses physical RC decay");
            near(20.0 * std::log10(Probe::input(*product, 0) / Probe::input(*raw, 0)),
                 20.0 / std::log(10.0) * time * (1.0 / 0.082 - 1.0 / (resistance * 1e-6)),
                 0.01, "200ms tail difference does not follow the physical load");
            // Config persists, while an explicit reset clears charge.
            product->reset();
            near(Probe::capacitor(*product, 0), 0.0, 0.0, "reset retained C59 charge");
            near(inferredResistance(*product, 0), resistance, 0.0,
                 "reset discarded the product circuit configuration");
        }
}

void coefficientEditsPreserveCharge()
{
    auto device = engine(48000, 1, 0.0f, true);
    Probe::gainError(*device, 0.6f);
    for (int frame = 0; frame < 4096; ++frame)
        Probe::finishVolts(*device, 0, 1.0f);
    const double charge = Probe::capacitor(*device, 0);
    const double initialG = Probe::coefficient(*device, 0);
    youknow::EngineParameters parameters;
    parameters.calibration = 1.0f;
    parameters.enableSpatialThermalGradient = false;
    device->setParameters(parameters);
    require(Probe::coefficient(*device, 0) != initialG,
            "Character edit did not update the fixed service load");
    near(Probe::capacitor(*device, 0), charge, 0.0,
         "Character edit cleared physical capacitor charge");
    const double uniformG = Probe::coefficient(*device, 0);
    parameters.enableSpatialThermalGradient = true;
    device->setParameters(parameters);
    require(Probe::coefficient(*device, 0) != uniformG,
            "gradient edit did not update the fixed service load");
    near(Probe::capacitor(*device, 0), charge, 0.0,
         "gradient edit cleared physical capacitor charge");
    const double settledG = Probe::coefficient(*device, 0);
    for (float fraction : { 0.0f, 0.25f, 0.75f, 1.0f })
    {
        Probe::warmup(*device, fraction);
        near(Probe::coefficient(*device, 0), settledG, 0.0,
             "live warmup moved the fixed VR27 trim");
        near(Probe::capacitor(*device, 0), charge, 0.0,
             "thermal update cleared physical capacitor charge");
    }
    parameters.enableVoiceVcaTemperature = false;
    device->setParameters(parameters);
    near(Probe::coefficient(*device, 0), settledG, 0.0,
         "thermal response comparison re-trimmed the input network");
    const double resistance = inferredResistance(*device, 0);
    for (const auto setting : { std::pair { 48000.0, 4 }, { 96000.0, 4 },
                                { 44100.0, 1 }, { 48000.0, 1 } })
    {
        Probe::changeRate(*device, setting.first, setting.second);
        near(Probe::rate(*device), std::min(setting.first * setting.second, 192000.0),
             0.0, "quality rebuild did not select the requested processing rate");
        near(Probe::capacitor(*device, 0), charge, 0.0,
             "rate/quality rebuild changed capacitor charge");
        near(inferredResistance(*device, 0), resistance, 0.02,
             "rate/quality rebuild changed the physical load");
    }
    // The first sample after the edit must continue the existing charge,
    // using the new pole rather than a reset/old cached coefficient.
    const double g = Probe::coefficient(*device, 0);
    Probe::finishVolts(*device, 0, 0.0f);
    near(Probe::input(*device, 0), -charge / (1.0 + g), 1e-7,
         "edited C59 did not continue its stored charge");
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
        physicalDividerAndServiceTarget();
        couplingDecayAndProfile();
        coefficientEditsPreserveCharge();
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
