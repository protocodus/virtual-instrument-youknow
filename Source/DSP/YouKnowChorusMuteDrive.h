#pragma once

#include <algorithm>
#include <cmath>

namespace youknow
{
// Roland JUNO-106 jack board p.15: Tr4 emitter -15 V, R42=39k across
// its base/emitter, R49=560k from C13, R43=100k collector pull-up to +15 V.
// D4/D5 let that collector pull the SERIES Tr11/12 gates negative; otherwise
// the gates approach their signal sources (zero DC after C28/C25).
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=15
// The p.4 NPN inventory permits 2SC1815-GR/Y or 2SC1740-R/Q or 2SC2603-F/E;
// Tr4 is not individually type-labelled. beta150 is the 2SC1815 typical-curve
// specimen at 25C, VCE6V, Ic~0.1..1mA; NOT an installed/GR-grade measurement.
// Its ratio hFE(.1mA)/hFE(2mA)=.95 supports this low-current extrapolation.
// https://media.digikey.com/pdf/Data%20Sheets/Toshiba%20PDFs/2SC1815.pdf#page=2
// Keep the established 0.6 V junction and ideal saturated collector priors;
// beta, Vbe/Early effect, diode drops and gate leakage remain unit uncertainty.
struct ChorusMuteDrive
{
    static constexpr double beta = 150.0;
    static constexpr double junctionVolts = 0.6;
    static constexpr double emitterVolts = -15.0;
    static constexpr double pullUpVolts = 15.0;
    static constexpr double baseSeriesOhms = 560000.0;
    static constexpr double baseBleedOhms = 39000.0;
    static constexpr double collectorOhms = 100000.0;
    static constexpr double wetInputOhms = 39000.0;

    // Same-part 2SK30A typical characteristic specimen: Idss~2.8mA and
    // Vp~-1.8V, read approximately from the printed p.563 curves at25C.
    // These fall in the permitted Y/GR overlap, but are NOT identified card
    // parameters. The family spans Vp=-.4..-5V; cutoff is not set by its grade.
    // https://amptone.pl/templates/images/files/4686/1710237661-2sk30a-toshiba-6465.pdf#page=2
    static constexpr double idssAmps = 2.8e-3;
    static constexpr double cutoffVolts = -1.8;

    [[nodiscard]] static double baseCurrent(double holdVolts) noexcept
    {
        if (!std::isfinite(holdVolts)) return 0.0;
        return std::max((holdVolts - emitterVolts - junctionVolts) / baseSeriesOhms
                        - junctionVolts / baseBleedOhms, 0.0);
    }
    [[nodiscard]] static double gateVolts(double holdVolts,
                                          double currentGain = beta) noexcept
    {
        const double collector = std::max(emitterVolts,
            pullUpVolts - collectorOhms * std::max(currentGain, 0.0)
                          * baseCurrent(holdVolts));
        return std::min(0.0, collector + junctionVolts);
    }
    [[nodiscard]] static double conductanceRatio(double gate) noexcept
    {
        // Small-Vds Shockley channel: Gds=2*Idss/|Vp|*(1-Vgs/Vp).
        // Only its incremental ohmic resistance is represented: signal-
        // dependent JFET distortion, gate capacitance and charge injection
        // remain uncalibrated. Typical on Ron~321ohm, against R72/74=39k.
        const double channel = 2.0 * idssAmps / -cutoffVolts
            * std::clamp(1.0 - gate / cutoffVolts, 0.0, 1.0);
        return wetInputOhms * channel / (1.0 + wetInputOhms * channel);
    }
};
}
