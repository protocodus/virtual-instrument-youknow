#include "DSP/YouKnowEngine.h"

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
        std::cout << "Fixed-service callback cases=" << cases
                  << " max_RES_corner_movement=" << maximumFixedMovement
                  << " min_legacy_corner_movement=" << minimumLegacyMovement
                  << " max_rate_relative_error=" << maximumRateError << '\n';
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
