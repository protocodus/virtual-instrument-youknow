// Conditional Toshiba 2SA1015 prior, not an original-card measurement.
// The independent oracle solves absolute Tr20 junction voltage and C58 KCL.
// Roland module p.13 supplies R106=10k, R105=22k, C58=.1uF; service
// pp.18-19 supply the .26-V standoff and TP19=4.8Vp-p -> TP8=6Vp-p.
// The nominal 25C junction point is the same named specimen as Tr18/Tr22.
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=13
// https://media.digikey.com/PDF/Data%20Sheets/Toshiba%20PDFs/2SA1015.pdf#page=2
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowEvidenceVca.h"
#include "DSP/YouKnowVcaControl.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace youknow
{
struct YouKnowTestAccess
{
    static const VcaControlCircuit& evidenceCircuit()
    {
        return YouKnowEngine::voiceVcaControlCircuit(true);
    }
    static double control(const YouKnowEngine& engine, int slot)
    {
        return engine.voices_[static_cast<std::size_t>(slot)].vcaControl;
    }
    static float gain(const YouKnowEngine& engine, int slot)
    {
        return engine.voices_[static_cast<std::size_t>(slot)].vcaGain;
    }
    static std::pair<float, float> writeEnvelopeWord(
        YouKnowEngine& engine, std::uint16_t word, VcaMode mode,
        bool gateOpen = true)
    {
        auto& voice = engine.voices_[0];
        voice.keyDown = gateOpen;
        voice.sustained = false;
        voice.envelope.level = word;
        voice.envelope.gate = voice.envelope.running = true;
        voice.envelope.phase = true;
        voice.envelope.tick(1u, 0u, word, 0u);
        voice.envelope.running = gateOpen;
        auto parameters = engine.activeParameters_;
        parameters.vcaMode = mode;
        engine.performConverterWrite(
            {YouKnowEngine::ConverterDestination::VoiceVca, 0}, parameters);
        voice.vcaControl = voice.vcaControlTarget;
        engine.updateVoiceAudio(voice, parameters);
        return {voice.vcaControlTarget, voice.vcaGain};
    }
    static double seedFractionalWrite(YouKnowEngine& engine, int slot,
                                      double requestedPosition)
    {
        for (int index = 0; index < YouKnowEngine::hardwareVoices; ++index)
        {
            auto& voice = engine.voices_[static_cast<std::size_t>(index)];
            voice.vcaControl = .009;
            voice.vcaControlTarget = .006f;
            voice.envelope.value = .8f;
        }
        const auto& writes = YouKnowEngine::converterWriteOrder();
        const auto found = std::find_if(writes.begin(), writes.end(), [slot](const auto& write) {
            return write.destination == YouKnowEngine::ConverterDestination::VoiceVca
                && write.voice == slot;
        });
        if (found == writes.end())
            throw std::runtime_error("voice VCA write missing from converter schedule");
        const auto ordinal = static_cast<std::size_t>(found - writes.begin());
        const double delta = YouKnowEngine::controlScanHz / engine.oversampledRate_;
        const double event = engine.converterEventPhases_[ordinal];
        engine.controlScanPhase_ = event - requestedPosition * delta;
        engine.nextConverterWrite_ = ordinal;
        engine.passiveHoldEventLatch_ = {};
        engine.assignmentRescanPending_ = false;
        engine.assignmentRescanPassArmed_ = false;
        return std::clamp((event - engine.controlScanPhase_) / delta, 0.0, 1.0);
    }
    static float target(const YouKnowEngine& engine, int slot)
    {
        return engine.voices_[static_cast<std::size_t>(slot)].vcaControlTarget;
    }
    static void changeEnvelope(YouKnowEngine& engine, int slot)
    {
        engine.voices_[static_cast<std::size_t>(slot)].envelope.value = .3f;
    }
    static void settleControl(YouKnowEngine& engine, float control)
    {
        auto& voice = engine.voices_[0];
        voice.active = voice.keyDown = true;
        voice.vcaControl = voice.vcaControlTarget = control;
        engine.updateVoiceAudio(voice, engine.activeParameters_);
    }
    static double finishVolts(YouKnowEngine& engine, float volts)
    {
        return engine.finishVoiceFilter(engine.voices_[0], volts)
            * YouKnowEngine::internalVoltsPerUnit;
    }
    static void seedVcfControl(YouKnowEngine& engine, int slot, float counts,
                               float resonance)
    {
        auto& voice = engine.voices_[static_cast<std::size_t>(slot)];
        voice.active = voice.keyDown = true;
        voice.cutoffCounts = voice.cutoffCountsTarget = counts;
        voice.cutoffChainCounts = -1.0e30f;
        voice.vcaControl = voice.vcaControlTarget = .5;
        engine.resonanceCv_ = engine.resonanceCvTarget_ = resonance;
        engine.powerSupplyDroop_ = engine.railRippleVolts_ = 0;
    }
    static std::pair<double, float> updateVcfControl(YouKnowEngine& engine, int slot)
    {
        auto& voice = engine.voices_[static_cast<std::size_t>(slot)];
        engine.updateVoiceAudio(voice, engine.activeParameters_);
        return {voice.filterOmegaStep * engine.oversampledRate_
                    / (2.0 * std::numbers::pi), voice.feedback};
    }
    static bool cutoffCacheInvalid(const YouKnowEngine& engine, int slot)
    {
        return engine.voices_[static_cast<std::size_t>(slot)].cutoffChainCounts == -1.0e30f;
    }
};
} // namespace youknow

namespace
{
using Engine = youknow::YouKnowEngine;
using Calibration = youknow::EvidenceVcaCalibration;
using Probe = youknow::YouKnowTestAccess;

// Keep independent physical values here rather than reusing the model's
// derived knee, gain table, current solver or differential-coordinate solve.
constexpr long double vt = 1.380649e-23L * 298.15L / 1.602176634e-19L;
constexpr long double beta = 200.0L;
constexpr long double referenceVbe = .61L;
constexpr long double referenceIc = .0006509655627366167L;
constexpr long double referenceIe = referenceIc * (1.0L + 1.0L / beta);
constexpr long double standoff = .26L;
constexpr long double positiveBufferGain = 2.0210925562118116L;
constexpr long double idealSpan = 5.0L * positiveBufferGain * 4095.0L / 4096.0L;
// Engine CV is explicitly float; retain that representable span in the
// voltage oracle, while separately checking its binary-weighted value.
constexpr long double span = static_cast<long double>(static_cast<float>(idealSpan));

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

// Newton in junction VOLTAGE, not the model's log-current coordinate:
// Vbe + R*Ie(Vbe) = applied voltage. The .61V initial point is above every
// root in this fixture, so this convex equation converges from above.
long double emitterCurrent(long double applied, long double resistance)
{
    long double junction = referenceVbe;
    for (int iteration = 0; iteration < 80; ++iteration)
    {
        const long double current = referenceIe * std::exp((junction - referenceVbe) / vt);
        const long double residual = junction + resistance * current - applied;
        const long double step = residual / (1.0L + resistance * current / vt);
        junction -= step;
        if (std::abs(step) < 1.0e-19L) break;
    }
    return referenceIe * std::exp((junction - referenceVbe) / vt);
}

long double settledCurrent(long double control)
{
    return emitterCurrent(standoff + span * control, 32000.0L);
}

long double equilibrium(long double control)
{
    return standoff + span * control - 10000.0L * settledCurrent(control);
}

long double advanceOracle(long double node, long double target, long double dt)
{
    const auto derivative = [target](long double voltage) {
        return ((standoff + span * target - voltage) / 10000.0L
                - emitterCurrent(voltage, 22000.0L)) / .1e-6L;
    };
    const auto a = derivative(node);
    const auto b = derivative(node + dt * a / 2);
    const auto c = derivative(node + dt * b / 2);
    const auto d = derivative(node + dt * c);
    return node + dt * (a + 2*b + 2*c + d) / 6;
}

long double advanceFineOracle(long double node, long double target, long double seconds)
{
    for (int substep = 0; substep < 64; ++substep)
        node = advanceOracle(node, target, seconds / 64);
    return node;
}

double capacitorError(const youknow::VcaControlCircuit& circuit,
                       double control, long double reference)
{
    return std::abs(static_cast<double>(standoff
        + span * circuit.capacitorCoordinate(control) - reference));
}

double circuitErrorBudget(int rate)
{
    return .003 * std::pow(8000.0 / rate, 2) + 5e-6;
}

std::unique_ptr<Engine> engine(bool evidence, double rate = 48000)
{
    auto result = std::make_unique<Engine>();
    require(result->configureThermalStart(true), "settled thermal start rejected");
    require(result->configureServiceDerivedVcaCoupling(true), "service C59 rejected");
    youknow::EngineParameters parameters;
    parameters.calibration = parameters.velocityDepth = 0;
    parameters.vcaMode = youknow::VcaMode::Envelope;
    parameters.enableEvidenceVcaCalibration = evidence;
    parameters.enableVoiceVcaAntialias = false;
    result->setParameters(parameters);
    result->prepare(rate, 1, 1);
    return result;
}

void processOne(Engine& engine)
{
    float left = 0, right = 0;
    engine.process(&left, &right, 1);
    require(std::isfinite(left) && std::isfinite(right),
            "VCA callback fixture produced non-finite audio");
}

void testPhysicalCurrentAndDcGain(const Calibration& calibration)
{
    near(calibration.spanVolts(), static_cast<double>(idealSpan), 1e-6,
         "evidence calibration lost the independently solved ENV rail span");
    near(Calibration::thermalVolts, static_cast<double>(vt), 1e-17,
         "evidence junction did not use kT/q at 25C");
    const long double peak = settledCurrent(1);
    near(calibration.referenceTailAmps(), static_cast<double>(peak * beta / (beta + 1)),
         1e-15, "BA662 tail was not fed by Tr20 collector current");
    require(calibration.gain(1) == 1, "ENV peak must retain exact unity gain");
    require(calibration.gain(0) == 0 && calibration.gain(.001f) == 0,
            "declared zero-current policy changed");
    require(calibration.gain(std::numeric_limits<float>::quiet_NaN()) == 0,
            "non-finite CV was not made silent");

    double maximumGainDb = 0, maximumCurrentError = 0;
    float previous = 0;
    // Physical DAC words do not align with the 4096-step interpolation grid.
    // Cover all 12-bit codes, especially the low-current junction transition.
    for (unsigned code = 0; code <= 4095; ++code)
    {
        const float control = static_cast<float>(code) / 4095.0f;
        const long double current = settledCurrent(control);
        maximumCurrentError = std::max(maximumCurrentError,
            std::abs(calibration.emitterAmps(control) - static_cast<double>(current)));
        near(calibration.collectorAmps(control), static_cast<double>(current * beta / (beta + 1)),
             1e-15, "finite-beta collector conversion misses the independent oracle");
        const float gain = calibration.gain(control);
        require(gain >= previous, "evidence CV law is not monotone");
        previous = gain;
        if (control > .001f)
            maximumGainDb = std::max(maximumGainDb, std::abs(20.0 * std::log10(
                static_cast<double>(gain) / static_cast<double>(current / peak))));
        else
            require(gain == 0, "DAC zero policy differs from its declared deadband");
    }
    require(maximumCurrentError < 1e-15,
            "Tr20 current differs from the absolute-voltage junction oracle");
    require(maximumGainDb < .012,
            "evidence gain table exceeded its quiet-current interpolation budget");

    const long double turnOn = referenceVbe - vt * std::log(referenceIe * 32000.0L / vt)
        - standoff;
    near(calibration.turnOnVolts(), static_cast<double>(turnOn), 1e-14,
         "C58's junction knee does not follow the named transistor prior");
    std::cout << "4096 evidence DAC codes: maximum gain error " << maximumGainDb
              << "dB; full collector current " << calibration.referenceTailAmps() * 1e6
              << "uA; derived knee " << calibration.turnOnVolts() * 1e3 << "mV\n";
}

void testServiceTrimAndQuietDifference(const Calibration& calibration)
{
    const double serviceControl = 4064.0 / 4095.0;
    const long double serviceIc = settledCurrent(serviceControl) * beta / (beta + 1);
    const long double expectedHeadroom = 2.4L / std::atanh(3.0L / (47000.0L * serviceIc));
    near(calibration.headroomVolts(), static_cast<double>(expectedHeadroom), 1e-11,
         "VR27 input drive missed the independently solved service current");
    const double serviceOutput = calibration.serviceGain()
        * calibration.gain(4064.0f / 4095.0f) * calibration.headroomVolts()
        * std::tanh(2.4 / calibration.headroomVolts());
    near(2 * serviceOutput, 6.0, 1e-12,
         "4.8-Vp-p service input did not produce the 6-Vp-p output anchor");
    // Hold the full-scale normalization fixed while exercising an audible
    // late-decay region. This is the consequence of a named junction prior,
    // not a value fitted solely to make the candidate sound different.
    for (float control : {.014f, .015f, .02f})
    {
        const double deltaDb = 20.0 * std::log10(calibration.gain(control)
            / Engine::VoiceVcaControlLaw::gain(control));
        require(deltaDb < -4,
                "named transistor prior no longer changes the quiet tail appreciably");
        std::cout << "CV " << control << ": conditional quiet-tail change " << deltaDb << "dB\n";
    }
    require(Engine::VoiceVcaControlLaw::gain(1) == calibration.gain(1),
            "quiet-tail comparison changed the normalized peak");
}

void testC58(const youknow::VcaControlCircuit& circuit)
{
    for (double control : {0.0, .004, .014, .02, .1, .5, 1.0})
    {
        require(circuit.advance(control, control, 1.0/48000) == control,
                "settled evidence C58 control changed");
        require(capacitorError(circuit, control, equilibrium(control)) < 5e-6,
                "evidence C58 equilibrium misses its absolute-voltage oracle");
    }
    constexpr std::array targets {.02, .006, .0, .1, 1.0, .018, .0};
    for (int rate : {8000, 44100, 48000, 96000, 192000})
    {
        double control = 0;
        long double reference = equilibrium(0);
        double maximumError = 0;
        for (double target : targets)
            for (int frame = 0; frame < rate / 500; ++frame)
            {
                const double next = circuit.advance(control, target, 1.0 / rate);
                require(next >= std::min(control, target) && next <= std::max(control, target),
                        "passive evidence C58 overshot its target");
                control = next;
                reference = advanceFineOracle(reference, target, 1.0L / rate);
                maximumError = std::max(maximumError, capacitorError(circuit, control, reference));
            }
        require(maximumError < circuitErrorBudget(rate),
                "evidence C58 transition exceeded the independent circuit budget");
        std::cout << rate << "Hz evidence C58: maximum voltage error " << maximumError * 1e6
                  << "uV\n";
    }
}

void testActualEnvelopeAndServiceAudio(const Calibration& calibration)
{
    auto candidate = engine(true);
    auto compatibility = engine(false);
    for (std::uint16_t word : {0u, 256u, 1024u, 8192u, 16256u, 16383u})
    {
        const auto [control, gain] = Probe::writeEnvelopeWord(*candidate, word, youknow::VcaMode::Envelope);
        near(control, static_cast<double>(word >> 2u) / 4095.0, 1e-7,
             "candidate changed the firmware ENV word-to-DAC conversion");
        near(gain, calibration.gain(control), 1e-7,
             "production audio did not consume the candidate VCA gain table");
        const auto [oldControl, oldGain] = Probe::writeEnvelopeWord(*compatibility, word,
                                                                 youknow::VcaMode::Envelope);
        near(oldControl, control, 0, "candidate changed the envelope CV coordinate");
        near(oldGain, Engine::VoiceVcaControlLaw::gain(oldControl), 1e-7,
             "compatibility switch lost its legacy junction");
    }
    const auto [gate, gateGain] = Probe::writeEnvelopeWord(*candidate, 0, youknow::VcaMode::Gate);
    require(gate == 1 && gateGain == 1, "candidate changed the full GATE endpoint");
    const auto [off, offGain] = Probe::writeEnvelopeWord(*candidate, 0x3fffu,
                                                      youknow::VcaMode::Gate, false);
    require(off == 0 && offGain == 0, "candidate changed the closed GATE endpoint");

    // Node audio checks the actual pair/trim/coupling implementation rather
    // than only the helper's algebra. Inject before C59, settle its charge,
    // then measure the 248Hz service sine and normalized quiet gain.
    for (float control : {4064.0f / 4095.0f, .015f})
    {
        Probe::settleControl(*candidate, control);
        Probe::settleControl(*compatibility, control);
        double low = 1e9, high = -1e9, oldLow = 1e9, oldHigh = -1e9;
        for (int sample = 0; sample < 96000; ++sample)
        {
            const float input = static_cast<float>(2.4 * std::sin(
                2.0 * std::numbers::pi * 248.0 * sample / 48000.0));
            const double output = Probe::finishVolts(*candidate, input);
            const double oldOutput = Probe::finishVolts(*compatibility, input);
            if (sample < 48000) continue;
            low = std::min(low, output);
            high = std::max(high, output);
            oldLow = std::min(oldLow, oldOutput);
            oldHigh = std::max(oldHigh, oldOutput);
        }
        const double peakToPeak = high - low;
        if (control > .9f)
        {
            near(peakToPeak, 6.0, .0005, "actual candidate path missed the service sine anchor");
            near(oldHigh - oldLow, 6.0, .0005, "legacy comparison lost its service normalization");
        }
        else
        {
            const double measuredDb = 20.0 * std::log10(peakToPeak / (oldHigh - oldLow));
            const double expectedDb = 20.0 * std::log10(
                calibration.gain(control) / calibration.gain(4064.0f / 4095.0f)
                * Engine::VoiceVcaControlLaw::gain(4064.0f / 4095.0f)
                / Engine::VoiceVcaControlLaw::gain(control));
            near(measuredDb, expectedDb, .002,
                 "actual quiet audio missed the independently normalized tail change");
            require(measuredDb < -4, "quiet node audio did not preserve the audible junction change");
            std::cout << "Actual 248Hz quiet VCA audio: " << measuredDb << "dB candidate/compatibility\n";
        }
    }
}

void testFractionalConverterCallbacks(const youknow::VcaControlCircuit& circuit,
                                      const Calibration& calibration)
{
    for (int rate : {8000, 48000, 192000})
    {
        double maximumError = 0;
        for (int slot = 0; slot < Engine::hardwareVoices; ++slot)
            for (double requested : {0.0, .001, .17, .5, .89, .999, 1.0})
            {
                auto candidate = engine(true, rate);
                const double position = Probe::seedFractionalWrite(*candidate, slot, requested);
                const long double dt = 1.0L / rate;
                auto reference = advanceFineOracle(equilibrium(.009), .006f, dt * position);
                reference = advanceFineOracle(reference, .8f, dt * (1 - position));
                processOne(*candidate);
                maximumError = std::max(maximumError,
                    capacitorError(circuit, Probe::control(*candidate, slot), reference));
                near(Probe::gain(*candidate, slot),
                     calibration.gain(static_cast<float>(Probe::control(*candidate, slot))), 1e-7,
                     "fractional callback consumed stale candidate VCA gain");
                const auto otherReference = advanceFineOracle(equilibrium(.009), .006f, dt);
                for (int other = 0; other < Engine::hardwareVoices; ++other)
                    if (other != slot)
                        require(capacitorError(circuit, Probe::control(*candidate, other),
                                               otherReference) < 5e-6,
                                "fractional candidate CV write reached a different card");
                Probe::changeEnvelope(*candidate, slot);
                processOne(*candidate);
                reference = advanceFineOracle(reference, .8f, dt);
                require(Probe::target(*candidate, slot) == .8f,
                        "right-edge candidate CV write lost its captured payload");
                maximumError = std::max(maximumError,
                    capacitorError(circuit, Probe::control(*candidate, slot), reference));
            }
        require(maximumError < circuitErrorBudget(rate),
                "actual fractional candidate callbacks miss continuous C58 KCL");
        std::cout << rate << "Hz actual candidate converter callbacks: maximum C58 error "
                  << maximumError * 1e6 << "uV\n";
    }
}

void testOriginalCardVcfSelection()
{
    // The original-card selector chooses the existing nominal circuit/service
    // coordinates instead of transplanting #439522's replacement-card fit.
    // This checks that choice and its cache refresh, not a new original-unit
    // frequency measurement. RES remains the same separately modeled Tr18.
    auto instrument = engine(false, 192000);
    // Start the physical audio timeline before injecting held converter
    // values. Pre-audio setParameters calls are restore snapshots and
    // intentionally re-prime every hold from the panel, including RES.
    // Selector changes below must exercise live cache invalidation instead.
    processOne(*instrument);
    youknow::EngineParameters parameters;
    parameters.calibration = parameters.velocityDepth = 0;
    parameters.useServiced439522VcfCalibration = true;
    parameters.enableResonanceSoftJunction = true;
    parameters.useFixedVcfServiceFrequencyTrim = true;
    const float fullFeedback = Engine::VoicedResonanceCompatibilityProfile::maximumFeedback;
    double maximumSelectionError = 0;
    for (int slot = 0; slot < Engine::hardwareVoices; ++slot)
        for (float counts : {6272.0f, 8556.0f, 12000.0f})
            for (unsigned byte : {0u, 16u, 32u, 64u, 96u, 127u})
            {
                parameters.useOriginalCardVcfCalibration = false;
                parameters.enableEvidenceVcaCalibration = false;
                instrument->setParameters(parameters);
                Probe::seedVcfControl(*instrument, slot, counts, static_cast<float>(byte) / 127.0f);
                const auto [replacementHz, replacementFeedback] = Probe::updateVcfControl(*instrument, slot);
                const double expectedReplacement = Engine::vcfEffectiveCutoffHz(counts, fullFeedback, slot);
                maximumSelectionError = std::max(maximumSelectionError,
                    std::abs(replacementHz / expectedReplacement - 1));
                require(!Probe::cutoffCacheInvalid(*instrument, slot),
                        "replacement VCF coefficient did not populate its cache");

                // Deliberately keep the held counts and feedback unchanged.
                // The selector itself must invalidate the cached coefficient.
                parameters.useOriginalCardVcfCalibration = true;
                instrument->setParameters(parameters);
                require(Probe::cutoffCacheInvalid(*instrument, slot),
                        "live original-card selector retained the replacement coefficient");
                const auto [originalHz, originalFeedback] = Probe::updateVcfControl(*instrument, slot);
                const double expectedOriginal = Engine::vcfEffectiveCutoffHz(counts, fullFeedback, -1);
                maximumSelectionError = std::max(maximumSelectionError,
                    std::abs(originalHz / expectedOriginal - 1));
                require(std::abs(originalHz / replacementHz - 1) > 1e-5,
                        "original-card selector did not exercise a different cutoff coordinate");
                require(originalFeedback == replacementFeedback,
                        "original-card cutoff selector changed the RES junction curve");

                parameters.enableEvidenceVcaCalibration = true;
                instrument->setParameters(parameters);
                const auto [withVcaHz, withVcaFeedback] = Probe::updateVcfControl(*instrument, slot);
                near(withVcaHz, originalHz, 0,
                     "voice VCA calibration changed the original-card cutoff");
                require(withVcaFeedback == originalFeedback,
                        "voice VCA calibration changed the RES junction curve");

                parameters.useOriginalCardVcfCalibration = false;
                instrument->setParameters(parameters);
                require(Probe::cutoffCacheInvalid(*instrument, slot),
                        "live replacement-card selector failed to refresh the cutoff cache");
                const auto [roundTripHz, roundTripFeedback] = Probe::updateVcfControl(*instrument, slot);
                near(roundTripHz, replacementHz, 0, "VCF selector round trip lost its replacement calibration");
                require(roundTripFeedback == replacementFeedback,
                        "VCF selector round trip changed resonance feedback");
            }
    require(maximumSelectionError < 2e-6,
            "actual VCF selector misses its nominal/replacement calibration coordinates");

    // Divide out the pole's fixed service-condition correction to inspect
    // its predicted full-RES oscillation coordinate. C6's 8558-count sum is
    // quantized to 8556 at the physical DAC, a documented 2.10-cent shift;
    // compare with that quantized 992-Hz target rather than hiding the loss.
    const double trim = Engine::VoicedResonanceCompatibilityProfile::frequencyTrim(fullFeedback);
    const double low = Engine::vcfEffectiveCutoffHz(6272, fullFeedback, -1) / trim;
    const double high = Engine::vcfEffectiveCutoffHz(8556, fullFeedback, -1) / trim;
    const double quantizedWidthTarget = 992.0 * std::exp2(-2.0 / 1143.0);
    require(std::abs(1200.0 * std::log2(low / 248.0)) < 1,
            "nominal original-card FREQ coordinate missed the 248-Hz service anchor");
    require(std::abs(1200.0 * std::log2(high / quantizedWidthTarget)) < 1,
            "nominal original-card WIDTH coordinate missed its quantized service anchor");
    std::cout << "Original-card VCF selection: maximum relative coefficient error "
              << maximumSelectionError << "; predicted service coordinates "
              << low << "Hz / " << high << "Hz\n";
}
} // namespace

int main()
{
    try
    {
        const Calibration calibration(Engine::VoiceVcaControlLaw::controlFullScaleVolts);
        const auto& circuit = Probe::evidenceCircuit();
        testPhysicalCurrentAndDcGain(calibration);
        testServiceTrimAndQuietDifference(calibration);
        testC58(circuit);
        testActualEnvelopeAndServiceAudio(calibration);
        testFractionalConverterCallbacks(circuit, calibration);
        testOriginalCardVcfSelection();
        std::cout << "conditional evidence VCA calibration tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
