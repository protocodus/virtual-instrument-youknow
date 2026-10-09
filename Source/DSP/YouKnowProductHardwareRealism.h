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
    static void configureBeforePrepare(YouKnowEngine& engine)
    {
        const auto mixer = CoupledSubMixer::evidenceCalibration();
        ProductFidelityProfile::configureBeforePrepare(engine, &mixer);
        if (!engine.configureChorusBbdTransferProfile(
                ChorusBbdTransferProfile::ServicedBiasEstimate))
            throw std::logic_error("Hardware realism requires an unprepared engine");
        // Same MN3009, measured in a Juno-60: a named insertion-gain prior,
        // separate from the Juno-106's finite surrounding filter/follower solve.
        if (!engine.configureChorusBbdInsertionGainProfile(
                ChorusBbdInsertionGainProfile::HoltersParkerJuno60Estimate))
            throw std::logic_error("Hardware realism requires an unprepared engine");
    }

    static void applyTo(EngineParameters& parameters) noexcept
    {
        ProductFidelityProfile::applyTo(parameters);
        parameters.chorusTimingProfile = ChorusTimingProfile::HardwareEvidence;
        parameters.enableEvidenceVcaCalibration = true;
        parameters.enableVoiceVcaJunctionTemperature = true;
        parameters.useBa662AResonanceOffsetEstimate = true;
        parameters.useOriginalCardVcfCalibration = true;
    }
};
} // namespace youknow
