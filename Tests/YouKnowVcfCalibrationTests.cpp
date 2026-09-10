#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowReferenceVcf.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace youknow
{
struct YouKnowTestAccess
{
    static void seedControlInterval(YouKnowEngine& engine, int slot,
                                    float cutoffCounts, float resonance)
    {
        auto& voice = engine.voices_[static_cast<std::size_t>(slot)];
        voice.active = voice.keyDown = true;
        voice.rootMidi = 60;
        voice.energy = 0;
        voice.filter.reset();
        voice.currentMidi = voice.targetMidi = 60;
        voice.cutoffCounts = voice.cutoffCountsTarget = cutoffCounts;
        voice.vcaControl = voice.vcaControlTarget = .5f;
        engine.resonanceCv_ = engine.resonanceCvTarget_ = resonance;
        engine.controlScanPhase_ = -100.0;
        engine.nextConverterWrite_ = 0;
        engine.passiveHoldEventLatch_ = {};
        engine.powerSupplyDroop_ = 0;
        engine.driftControlCountdown_ = 1000000;
        for (auto& card : engine.cards_)
            card.driftValue = 0;
        engine.updateActiveVoiceCount();
    }
    static float poleOmega(const YouKnowEngine& engine, int slot)
    {
        return engine.voices_[static_cast<std::size_t>(slot)].filterOmegaStep;
    }
    static double internalRate(const YouKnowEngine& engine)
    {
        return engine.oversampledRate_;
    }
    static float dacTarget(const YouKnowEngine& engine,
                           const EngineParameters& parameters)
    {
        return engine.voiceVcfTarget(engine.voices_[0], parameters);
    }
};
}

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

float callbackCorner(youknow::YouKnowEngine& engine,
                     const youknow::EngineParameters& parameters,
                     int slot, float resonance)
{
    engine.setParameters(parameters);
    youknow::YouKnowTestAccess::seedControlInterval(engine, slot, 6000.0f, resonance);
    float left = 0, right = 0;
    engine.process(&left, &right, 1);
    require(std::isfinite(left) && std::isfinite(right), "non-finite calibration callback");
    return youknow::YouKnowTestAccess::poleOmega(engine, slot);
}
}

int main()
{
    try
    {
        // Independent circuit invariant: p.13 places VR29/VR28 in the VCF
        // control branch, while RES CV reaches the separate feedback BA662
        // through VR26/R107/Tr18. The fixed converter voltage, trimmer and
        // integrating capacitor therefore set one pole current at every RES
        // setting. The feedback cascade itself still shifts its limit-cycle
        // frequency with amplitude; the control path cannot cancel that.
        // p.19 calibrates FREQ/WIDTH after the full-RES 4.8Vp-p adjustment.
        // https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf
        double maximumFixedMovement = 0;
        double minimumLegacyMovement = 1;
        double maximumRateError = 0;
        std::size_t cases = 0;
        for (int slot = 0; slot < 6; ++slot)
        for (float character : {0.0f, .75f, 2.0f})
        for (auto kernel : {youknow::VcfTanhMode::PolyZoned, youknow::VcfTanhMode::Exact})
        {
            double referenceHertz = 0;
            for (int rate : {8000, 48000, 192000})
            for (int factor : {1, 4})
            {
                auto engine = std::make_unique<youknow::YouKnowEngine>();
                engine->prepare(rate, 1, factor);
                youknow::EngineParameters parameters;
                parameters.calibration = character;
                parameters.vcfTanhMode = kernel;
                parameters.vcfSolverMode = kernel == youknow::VcfTanhMode::Exact
                    ? youknow::VcfSolverMode::MersonHalfSteps
                    : youknow::VcfSolverMode::Rk4Single;
                parameters.vcfFastEarlyMode = youknow::VcfFastEarlyMode::Cubic;
                parameters.sawEnabled = parameters.pulseEnabled = false;
                parameters.subLevel = parameters.noiseLevel = 0;
                // Isolate the control branch from the separate audio-load
                // supply-droop model while preserving every card trim.
                parameters.enableVcfStageOffsets = false;
                parameters.enablePulseOffWaveNodeCoupling = false;
                parameters.chorus = youknow::ChorusMode::Off;
                parameters.useFixedVcfServiceFrequencyTrim = false;
                const float legacyQuiet = callbackCorner(*engine, parameters, slot, 0.0f);
                const float legacyFull = callbackCorner(*engine, parameters, slot, 1.0f);
                require(legacyFull > legacyQuiet, "legacy comparison did not exercise live retuning");
                minimumLegacyMovement = std::min(minimumLegacyMovement,
                    static_cast<double>(legacyFull / legacyQuiet - 1.0f));
                parameters.useFixedVcfServiceFrequencyTrim = true;
                // Reuse the same engine: changing the selector must refresh
                // the cutoff memo even when every held CV stays unchanged.
                for (float resonance : {1.0f, .0f, .5f, .9f, .999f})
                {
                    const float fixed = callbackCorner(*engine, parameters, slot, resonance);
                    maximumFixedMovement = std::max(maximumFixedMovement,
                        std::abs(static_cast<double>(fixed / legacyFull - 1.0f)));
                    if (fixed != legacyFull) std::cerr << "slot=" << slot << " char=" << character << " rate=" << rate << " factor=" << factor << " resonance=" << resonance << " legacy=" << legacyFull << " fixed=" << fixed << "\n";
                    require(fixed == legacyFull,
                        "fixed FREQ/WIDTH moved with RES or changed the full-RES service calibration");
                    ++cases;
                }
                const double hertz = legacyFull * youknow::YouKnowTestAccess::internalRate(*engine)
                    / (2.0 * std::acos(-1.0));
                if (referenceHertz == 0) referenceHertz = hertz;
                maximumRateError = std::max(maximumRateError, std::abs(hertz / referenceHertz - 1));
                parameters.useFixedVcfServiceFrequencyTrim = false;
                require(callbackCorner(*engine, parameters, slot, 0.0f) == legacyQuiet,
                        "legacy retuning did not survive a profile round trip");
            }
        }
        require(minimumLegacyMovement > .05, "fixture missed the removed control-path pitch correction");
        require(maximumRateError < 2e-6, "fixed service current depends on host rate or quality rung");
        require(youknow::EngineParameters {}.useFixedVcfServiceFrequencyTrim,
                "fixed physical service trim is not the default");
        require(!youknow::EngineParameters {}.useServiced439522VcfCalibration,
                "replacement-card unit calibration became a universal default");

        // Separate circuit-coordinate oracle, evaluated in double precision:
        // converter carries precede the WIDTH multiplier, FREQ multiplies
        // current exponentially, and the finite-current limit has the same
        // fixed exponent as the nominal model. This checks wiring on both
        // audio paths, fixed physical slot identity and selector invalidation.
        using Engine = youknow::YouKnowEngine;
        const double trim = Engine::VoicedResonanceCompatibilityProfile::frequencyTrim(
            Engine::VoicedResonanceCompatibilityProfile::maximumFeedback);
        double maximumReferenceError = 0;
        for (int slot = 0; slot < 6; ++slot)
        {
            const auto& physical = youknow::serviced439522Vcf[static_cast<std::size_t>(slot)];
            require(physical.selfOscillationCeilingHertz*trim >= 64800
                    && physical.selfOscillationCeilingHertz*trim <= 72900,
                    "reference fit escaped its explicit model plausibility bounds");
            float previous = 0;
            for (int counts = -2000; counts <= 20000; counts += 4)
            {
                const float cutoff = Engine::vcfEffectiveCutoffHz(
                    static_cast<float>(counts), 4.504f, slot);
                require(std::isfinite(cutoff) && cutoff >= previous && cutoff <= 72900,
                        "reference profile is non-finite, non-monotone or exceeds its bound");
                previous = cutoff;
            }
            for (auto kernel : {youknow::VcfTanhMode::PolyZoned, youknow::VcfTanhMode::Exact})
            for (int rate : {48000, 192000})
            {
                auto engine = std::make_unique<Engine>();
                engine->prepare(rate, 1, 4);
                youknow::EngineParameters parameters;
                parameters.calibration = 0;
                parameters.sawEnabled = parameters.pulseEnabled = false;
                parameters.subLevel = parameters.noiseLevel = 0;
                parameters.enableVcfStageOffsets = false;
                parameters.enablePulseOffWaveNodeCoupling = false;
                parameters.vcfTanhMode = kernel;
                parameters.vcfFastEarlyMode = youknow::VcfFastEarlyMode::Cubic;
                parameters.vcfSolverMode = kernel == youknow::VcfTanhMode::Exact
                    ? youknow::VcfSolverMode::MersonHalfSteps
                    : youknow::VcfSolverMode::Rk4Single;
                const float nominal = callbackCorner(*engine, parameters, slot, .25f);
                parameters.useServiced439522VcfCalibration = true;
                const float selected = callbackCorner(*engine, parameters, slot, .25f);
                const double raw = physical.baseHertz*std::exp2(6000.0
                    *physical.centsPerByte/(128.0*1200.0));
                const double expectedHertz = raw*trim / std::pow(1.0
                    + std::pow(raw/physical.selfOscillationCeilingHertz, 1.7), 1.0/1.7);
                const double measuredHertz = selected*youknow::YouKnowTestAccess::internalRate(*engine)
                    /(2.0*std::acos(-1.0));
                maximumReferenceError = std::max(maximumReferenceError,
                    std::abs(measuredHertz/expectedHertz-1));
                require(selected != nominal, "reference selector did not refresh the callback coefficient");
                require(callbackCorner(*engine, parameters, slot, 1.0f) == selected,
                        "reference FREQ trim started following the RES slider");
                parameters.envDepth = parameters.keyFollow = parameters.vcfLfoDepth = 0;
                for (int code : {31,32,63,64,95,96,127})
                {
                    parameters.cutoff = static_cast<float>(code)/127.0f;
                    const double carry = ((code >= 32 ? -4.64 : 0)
                        +(code >= 64 ? 23.31 : 0)+(code >= 96 ? -4.48 : 0))*1143/1200;
                    require(std::abs(youknow::YouKnowTestAccess::dacTarget(*engine,parameters)
                        -(128.0*code+carry)) < .002,
                        "reference profile failed to put the existing carry on the physical hold target");
                }
                parameters.useServiced439522VcfCalibration = false;
                require(callbackCorner(*engine, parameters, slot, .25f) == nominal,
                        "reference profile did not restore nominal slot calibration");
            }
        }
        require(maximumReferenceError < 2e-6,
                "reference callback disagrees with independent circuit-coordinate oracle");
        for (int slot : {-5,-1,6,20})
            require(Engine::vcfEffectiveCutoffHz(6000,4.504f,slot)
                    == Engine::vcfEffectiveCutoffHz(6000,4.504f),
                    "invalid reference card does not fall back to nominal");
        std::cout << "Fixed-service callback cases=" << cases
                  << " max_RES_corner_movement=" << maximumFixedMovement
                  << " min_legacy_corner_movement=" << minimumLegacyMovement
                  << " max_rate_relative_error=" << maximumRateError
                  << " reference_callback_relative_error=" << maximumReferenceError << '\n';
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
