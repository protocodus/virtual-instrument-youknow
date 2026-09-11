#pragma once

#include "YouKnowEngine.h"

#include <stdexcept>

namespace youknow
{
// Product selection approved on 2026-09-11 (Docs/decisions.md). Raw engine
// fixtures keep their nominal reference defaults; the plug-in, maintained
// demos and preset audit select the same two B comparisons here.
struct ProductFidelityProfile
{
    // The B audition used this datasheet comparison coordinate. It is not a
    // measurement of the installed JUNO-106 switch's on-resistance.
    static constexpr double highPassSwitchOhms = 110.0;

    // Configure a newly constructed engine exactly once. HPF configuration
    // persists across prepare()/reset(), but changing it live is unsupported.
    static void configureBeforePrepare (YouKnowEngine& engine)
    {
        if (! engine.configureHighPassSwitch (highPassSwitchOhms))
            throw std::logic_error (
                "Product fidelity must be configured before the first prepare");
    }

    // A product choice, not a stored tone parameter. Apply to every newly
    // formed snapshot so INIT, preset recall and session restore retain it.
    static void applyTo (EngineParameters& parameters) noexcept
    {
        parameters.useServiced439522VcfCalibration = true;
    }
};
} // namespace youknow
