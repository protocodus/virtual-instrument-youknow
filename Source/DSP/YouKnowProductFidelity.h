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

    // Owner-delegated evidence choice (2026-09-28): remove the product's
    // +1.34 dB pulse/saw excess against the identified #439522 isolator take.
    // Its DCOs are original, its voice cards replacements; the MKS-7's lower
    // nominal pulse/saw Vpp ratio supports the direction, not this exact value.
    // This changes the pulse leg's relative level, with no output compensation.
    static constexpr float pulseLevelScale = 0.857f;

    // One-unit EFFECTIVE output-packet covariance calibration, 2026-10-04.
    // Thornber transfer/storage theory motivates PSD 1-eta*cos(w). The
    // original chorus board of #439522 supplies seven qualified Mode-I idle
    // patches in four hash-pinned April AIFF banks. FitChorusBucketNoise.py
    // fixes the current finite-followers/Tr4 support and OwnerBlend shared
    // opposite-phase LFO; it fits eta on A1/A2 even bands only. B3/B4 odd-band
    // RMSE improves 4.288 -> 0.988 dB (two locked-eta Mode-II patches:
    // 4.146 -> 1.068 dB). Phase/gain nuisances use even bands only. Doubling
    // integration/phase resolution changes eta by0.000013; omitting one
    // training patch gives0.911864..0.925042. This is not a population bound.
    // Product r=eta,c=1 is a REPRESENTATIVE algebraic convention: PSD only
    // identifies eta=2*r*c/(1+c*c), not separate microscopic strengths or
    // transfer efficiency. Recording response and geometry remain conditional.
    // A-weighted normalization preserves the existing2.37 source scalar.
    // Capture/protocol hashes and the reproducible split are enforced by:
    // Tools/FitChorusBucketNoise.py, Tools/AnalyzeChorusIdleFloors.py.
    // https://www.lewisfrancis.com/nwio/Juno-10-test-audio-96K.zip
    // https://vtda.org/pubs/BSTJ/vol53-1974/articles/bstj53-7-1211.pdf
    static constexpr float chorusNoiseEffectiveCovariance = 0.9171323f;

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
            // C59's unity-passband load follows the same fixed service input
            // trim as the BA662 signal law (nominal 120.192k at 25 C).
            || ! engine.configureServiceDerivedVcaCoupling (true)
            // Nominal forward-active PNP current/loading in the chorus chains.
            // Independent physical-node, harmonic, switching and cost checks
            // qualify this delegated component-model choice (2026-09-29).
            || ! engine.configureChorusSupport (ChorusSupportProfile::Nominal2SA1015Nonlinear)
            || ! (coupledMixer != nullptr
                    ? engine.configureCoupledMixer (*coupledMixer)
                    : engine.configureModuleInputCouplingResistanceOhms (
                          moduleInputCouplingResistanceOhms)
                      && engine.configureOscillatorLevelScale (
                          oscillatorLevelScale)
                      && engine.configurePulseLevelScale (pulseLevelScale)))
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
        // MC5534A's unipolar saw reaches WAVE through passive resistors;
        // retain its mean until the drawn C56/C50 input coupling removes it.
        // The centred raw-reference and explicitly calibrated mixer remain
        // available for diagnostics (YouKnowEngine::sawWaveNodeOffset).
        parameters.enableSawUnipolarNodeCoupling = true;
        // Retain IC6's sourced 3 MHz/noise-gain pole's small audible-band
        // magnitude loss even when its corner is above the internal Nyquist.
        parameters.enableOutputSummerMagnitudePole = true;
        parameters.enableOutputSummerAntialias = true;
        // The delayed series wet switch also opens IC6's 39k input leg.
        // Follow its physical gate for resistor noise and noise gain.
        parameters.enableOutputSummerMuteLoading = true;
        // Original M5218L's nominal7MHz/3.6 response and2.2V/us slew on
        // PHONES only. The current/load-dependent installed limits remain unknown.
        parameters.enableHeadphoneAmplifierDynamics = true;
        // ORIGINAL M5218L's2uVrms integrated noise atRs1k,10Hz..30k:
        // conservative white-equivalent device floor, separate from Johnson.
        parameters.enableHeadphoneIntrinsicNoise = true;
        // Drawn GC1 resistors' Johnson voltage noise follows NEC's shared
        // VCA control law, before both dry and wet feeds; no device CV fit.
        parameters.enableCommonVcaControlNoise = true;
        // Named typical same-listed-part Tr4/JFET drive, not installed timing.
        parameters.enableChorusFiniteMuteDrive = true;
        parameters.enableChorusFiniteTr5Drive = true;
        parameters.enableChorusCorrelatedNoise = true;
        parameters.chorusNoiseTransferFraction = chorusNoiseEffectiveCovariance;
        parameters.chorusNoiseTransferCorrelation = 1.0f;
        // C41/R79 retains its analogue-band magnitude on the default 1x
        // grid, with the same source density and positive physical RC decay.
        parameters.enableMainNoiseMagnitudePole = true;
        // Tr22's nominal exponential junction replaces the hard knee while
        // preserving the service noise endpoint and declared VR32 setting.
        parameters.enableNoiseLevelSoftJunction = true;
        // Tr18 uses the same nominal PNP prior, with its own drawn network
        // and service-endpoint trimmer solve rather than a hard 0.6-V knee.
        parameters.enableResonanceSoftJunction = true;
        // Physical collector component of the voice OTA pairs, with current
        // and covariance normalization; installed excess spectra stay open.
        parameters.enableOtaShotNoise = true;
        parameters.enableBa662OutputMirrorNoise = true;
        parameters.enableBa662TailMirrorNoise = true;
        // Prevent the physical BA662 pair's ultrasonic harmonics/control
        // sidebands folding back on coarse output grids; gain is unchanged.
        parameters.enableVoiceVcaAntialias = true;
        // The converter holds' leakage ramp: a hundredth of an LSB per pass
        // at the sheets' typicals. The engine's reference configuration
        // keeps ideal holds for its exactness fingerprints.
        parameters.enableConverterHoldDroop = true;
        // Roland jack-board p.15: IC2b's R16 33k || C5 22p feedback
        // limits the common-VCA signal before the dry/wet split. The
        // magnitude-matched nominal pole is separate from NEC's already
        // output-referred noise specification and unknown op-amp bandwidth.
        // https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=15
        parameters.enableCommonVcaOutputPole = true;
        // Chorus Mode I at the owner's by-ear blend of the three OQ-01
        // candidates (Docs/decisions.md, 2026-09-17); the engine default
        // keeps the clone endpoints as the reference configuration.
        parameters.chorusTimingProfile = ChorusTimingProfile::OwnerBlend;
        // Complete the drawn C16/C13 drive with reciprocal C15 loading and
        // the base-junction loads before stopping/restarting the BBD clocks.
        // This uses the existing 0.6 V junction / ideal-rail priors; it is a
        // nominal circuit completion, not installed-unit timing calibration.
        // The 2026-09-28 nodal/state and bounded product-cost audits qualify
        // the selection. Raw reference renders retain the two-node path.
        parameters.enableChorusClockMuteCircuit = true;
        // Noise B and the captured-hiss target, chosen by ear on 2026-09-22
        // (OQ-16/OQ-03), with the delegated 2026-09-28 hiss-level correction.
        // YouKnowNoiseCalibration.h carries both derivations.
        parameters.mainNoiseCalibrationProfile =
            MainNoiseCalibrationProfile::CoreBandTp8;
        parameters.chorusNoiseCalibrationProfile =
            ChorusNoiseCalibrationProfile::IdleFloor439522;
    }
};
} // namespace youknow
