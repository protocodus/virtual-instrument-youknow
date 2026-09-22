#pragma once

#include "YouKnowEngine.h"

#include <stdexcept>

namespace youknow
{
// Product selections: the 2026-09-11, 09-17 and 09-22 auditions
// (Docs/decisions.md), plus the approximate thermal clock coupling and
// 2026-09-15 C56 selection.
// Raw engine fixtures keep their nominal reference defaults. The plug-in
// and maintained product renderers select these settings centrally.
struct ProductFidelityProfile
{
    // The B audition used this datasheet comparison coordinate. It is not a
    // measurement of the installed JUNO-106 switch's on-resistance.
    static constexpr double highPassSwitchOhms = 110.0;

    // Owner-delegated evidence choice, coupling B (2026-09-15). C56=10uF
    // feeds the reconstructed hybrid's 4.7k summing path in parallel with
    // its 24k+1.5k resonance-input leg: about 3.969k and 39.685ms. This is
    // the low-frequency, ideal-feedback/stiff-source reduction, not a claim
    // that WAVE source impedance or the full active input network is known.
    // JUNO-6/60 has the same topology with 10k and 47k+1.5k; retain the
    // hybrid's documented proportions rather than copying that 8.291k load.
    // https://github.com/ThomHPL/Open80017a/blob/5561ee3ea8eaa6299c9ce0c79df4df43658f9efa/Outputs/Open80017a.pdf
    // https://www.synfo.nl/servicemanuals/Roland/JUNO-6_SERVICE_NOTES.pdf#page=9
    static constexpr double moduleInputCouplingResistanceOhms =
        4700.0 * (24000.0 + 1500.0) / (4700.0 + 24000.0 + 1500.0);

    // Drive B, chosen by ear on 2026-09-22 (OQ-15): the saw, pulse and sub
    // legs at 0.738 put the nominal saw at 4.83 Vpp at TP8 against the 6 Vpp
    // VCA trim, inside the MKS-7 Service Notes' 4.8 +/- 0.5 Vpp factory
    // window for the same MC5534A/80017A voice (sawMixVolts has the
    // derivation). #439522 reads 4.88. The engine returns the oscillators'
    // loudness at its digital boundary, so this moves drive, not level.
    static constexpr float oscillatorLevelScale = 0.738f;

    // Configure a newly constructed engine exactly once. Circuit configuration
    // persists across prepare()/reset(), but changing it live is unsupported.
    // A full coupled-mixer comparison replaces the independent C56 pole and,
    // with its own calibrated source scale, the oscillator level. Configure
    // that alternative here so both mutually exclusive paths still receive
    // the same remaining product selections.
    static void configureBeforePrepare (YouKnowEngine& engine,
        const CoupledSubMixer::Calibration* coupledMixer = nullptr)
    {
        if (! engine.configureHighPassSwitch (highPassSwitchOhms)
            || ! engine.configureDcoTemperatureProxy (true, 25.0)
            || ! (coupledMixer != nullptr
                    ? engine.configureCoupledMixer (*coupledMixer)
                    : engine.configureModuleInputCouplingResistanceOhms (
                          moduleInputCouplingResistanceOhms)
                      && engine.configureOscillatorLevelScale (
                          oscillatorLevelScale)))
            throw std::logic_error (
                "Product fidelity needs valid, compatible circuits before the first prepare");
        // User-authorized approximate thermal coupling (2026-09-14): use the
        // named Murata CSA8.00MTZ shape, anchored at 8 MHz/25 C, on the shared
        // chassis temperature. This is not an installed KMFC calibration.
        // The common 3-second startup is an explicit software UX choice.
    }

    // A product choice, not a stored tone parameter. Apply to every newly
    // formed snapshot so INIT, preset recall and session restore retain it.
    static void applyTo (EngineParameters& parameters) noexcept
    {
        parameters.useServiced439522VcfCalibration = true;
        // The converter holds' leakage ramp: a hundredth of an LSB per pass
        // at the sheets' typicals. The engine's reference configuration
        // keeps ideal holds for its exactness fingerprints.
        parameters.enableConverterHoldDroop = true;
        // Chorus Mode I at the owner's by-ear blend of the three OQ-01
        // candidates (Docs/decisions.md, 2026-09-17); the engine default
        // keeps the clone endpoints as the reference configuration.
        parameters.chorusTimingProfile = ChorusTimingProfile::OwnerBlend;
        // Noise B and chorus-hiss B, chosen by ear on 2026-09-22 (OQ-16,
        // OQ-03); YouKnowNoiseCalibration.h carries both derivations.
        parameters.mainNoiseCalibrationProfile =
            MainNoiseCalibrationProfile::CoreBandTp8;
        parameters.chorusNoiseCalibrationProfile =
            ChorusNoiseCalibrationProfile::IdleFloor439522;
    }
};
} // namespace youknow
