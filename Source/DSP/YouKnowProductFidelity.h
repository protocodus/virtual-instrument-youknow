#pragma once

#include "YouKnowEngine.h"

#include <stdexcept>

namespace youknow
{
// Product selections: the two 2026-09-11 auditions (Docs/decisions.md), plus
// the user-requested approximate thermal clock coupling on 2026-09-14.
// Raw engine fixtures keep their nominal reference defaults. The plug-in
// and maintained product renderers select these settings centrally.
struct ProductFidelityProfile
{
    // The B audition used this datasheet comparison coordinate. It is not a
    // measurement of the installed JUNO-106 switch's on-resistance.
    static constexpr double highPassSwitchOhms = 110.0;

    // Configure a newly constructed engine exactly once. Circuit configuration
    // persists across prepare()/reset(), but changing it live is unsupported.
    static void configureBeforePrepare (YouKnowEngine& engine)
    {
        if (! engine.configureHighPassSwitch (highPassSwitchOhms)
            || ! engine.configureDcoTemperatureProxy (true, 25.0))
            throw std::logic_error (
                "Product fidelity must be configured before the first prepare");
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
    }
};
} // namespace youknow
