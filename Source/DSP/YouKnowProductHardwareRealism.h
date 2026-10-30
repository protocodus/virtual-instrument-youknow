#pragma once

#include "YouKnowProductFidelity.h"

namespace youknow
{
// Product profile: the first five mechanisms were selected by ear on
// 2026-10-05 (Docs/decisions.md). Subsequent component estimates are enabled
// by the owner's implementation/default instruction, not a listening verdict.
// The original chorus recording
// supports the effective Mode-I timing. Mode II and the unmeasured voice-card
// coordinates are explicitly named schematic/component estimates, not a new
// original-card calibration. ProductFidelityProfile remains the earlier
// reference configuration for frozen comparisons and diagnostic fixtures.
// See the circuit helpers for component provenance and Docs/decisions.md
// for the accepted comparison and its limits.
struct ProductHardwareRealismProfile
{
#if !defined(YOUKNOW_EMBEDDED_TARGET)
    static void configureBeforePrepare(YouKnowEngine& engine)
    {
        if (!tryConfigureBeforePrepare(engine))
            throw std::logic_error("Hardware realism requires an unprepared engine");
    }
#endif

    [[nodiscard]] static bool tryConfigureBeforePrepare(YouKnowEngine& engine) noexcept
    {
        const auto mixer = CoupledSubMixer::evidenceCalibration();
        if (!ProductFidelityProfile::tryConfigureBeforePrepare(engine, &mixer))
            return false;
        if (!engine.configureChorusBbdTransferProfile(
                ChorusBbdTransferProfile::ServicedBiasEstimate))
            return false;
        // Same MN3009, measured in a Juno-60: a named insertion-gain prior,
        // separate from the Juno-106's finite surrounding filter/follower solve.
        if (!engine.configureChorusBbdInsertionGainProfile(
                ChorusBbdInsertionGainProfile::HoltersParkerJuno60Estimate))
            return false;
        return true;
    }

    static void applyTo(EngineParameters& parameters) noexcept
    {
        ProductFidelityProfile::applyTo(parameters);
        parameters.chorusTimingProfile = ChorusTimingProfile::HardwareEvidence;
        parameters.enableEvidenceVcaCalibration = true;
        parameters.enableVoiceVcaJunctionTemperature = true;
        parameters.useBa662AResonanceOffsetEstimate = true;
        parameters.useOriginalCardVcfCalibration = true;
        // 2026-10-15 numerical corrections to these same selected circuits:
        // preserve the signed reconstructed Tr19 edge, evaluate chorus
        // support on the existing IC6 FIR grid, and move the VCF/C59 inside
        // the existing voice-VCA FIR boundary. No hardware value is refitted.
        // Independent circuit/transport references qualify the implementation;
        // the fresh auditions are not a new by-ear calibration verdict.
        parameters.enableCoupledMixerBandlimitedSwitch = true;
        parameters.enableChorusRateIsland = true;
        parameters.enableVoiceFilterRateIsland = true;
        // 2026-10-16 CPU pass: the island's collector-noise covariance is
        // factored once per internal sample (see the parameter's note).
        parameters.enableVoiceIslandFrameShotCovariance = true;
        // 2026-10-22: reconstruct the already selected circuits' continuous
        // controls inside their existing numerical intervals. The C61 cutoff
        // settling, C58/Tr20 current and Tr11/12 wet-switch loading retain
        // their physical values, endpoints, gain laws and reported latency.
        // Independent continuous/fine-step references qualify these numerical
        // corrections; they are not additional hardware calibration values.
        parameters.enableVcfSettlingReconstruction = true;
        parameters.enableVoiceVcaControlReconstruction = true;
        parameters.enableContinuousChorusMuteLoading = true;
        // 2026-10-28 component tolerances and laws from the p. 15 drawing
        // and the p. 12 parts legend (Docs/decisions.md). Unit Character
        // zero removes every tolerance draw.
        parameters.enableChorusClockSourceSpread = true;
        parameters.enableChorusInverterSpread = true;
        parameters.enableChorusLargeSignalWetSwitch = true;
        parameters.enableOutputSummerLegSpread = true;
        parameters.enableHighPassBoostSpread = true;
        // 2026-10-28: Tr19's storage time as measured on the identified
        // unit's original sub switch, in place of the borrowed part class.
        parameters.useMeasuredSubStorageSkew = true;
        // 2026-10-30: the chorus hiss as #439522's original board produces
        // it across the factory-bank captures (Docs/decisions.md).
        parameters.enableChorusClockDependentHiss = true;
        parameters.useMeasuredChorusModeHissLevels = true;
    }
};
} // namespace youknow
