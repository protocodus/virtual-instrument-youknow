#pragma once

#include "YouKnowProductFidelity.h"

namespace youknow
{
// Five-mechanism product profile, selected by ear on 2026-10-05
// (Docs/decisions.md). The original chorus recording
// supports the effective Mode-I timing. Mode II and the unmeasured voice-card
// coordinates are explicitly named schematic/component estimates, not a new
// original-card calibration. ProductFidelityProfile remains the earlier
// reference configuration for frozen comparisons and diagnostic fixtures.
// See the circuit helpers for provenance and Tools/RenderHardwareRealism.cpp
// for the frozen comparison protocol.
struct ProductHardwareRealismProfile
{
    static void configureBeforePrepare(YouKnowEngine& engine)
    {
        const auto mixer = CoupledSubMixer::evidenceCalibration();
        ProductFidelityProfile::configureBeforePrepare(engine, &mixer);
        if (!engine.configureChorusBbdTransferProfile(
                ChorusBbdTransferProfile::ServicedBiasEstimate))
            throw std::logic_error("Hardware realism requires an unprepared engine");
    }

    static void applyTo(EngineParameters& parameters) noexcept
    {
        ProductFidelityProfile::applyTo(parameters);
        parameters.chorusTimingProfile = ChorusTimingProfile::HardwareEvidence;
        parameters.enableEvidenceVcaCalibration = true;
        parameters.useOriginalCardVcfCalibration = true;
    }
};
} // namespace youknow
