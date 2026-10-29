#pragma once

#include <algorithm>
#include <cmath>
#include "YouKnowCompatibility.h"

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
    // D4/D5's anode-side limit on the gates: Tr4's collector plus one
    // junction. A gate sits there only while that is below its channel.
    [[nodiscard]] static double gateDriveVolts(double holdVolts,
                                               double currentGain = beta) noexcept
    {
        const double collector = std::max(emitterVolts,
            pullUpVolts - collectorOhms * std::max(currentGain, 0.0)
                          * baseCurrent(holdVolts));
        return collector + junctionVolts;
    }
    [[nodiscard]] static double gateVolts(double holdVolts,
                                          double currentGain = beta) noexcept
    {
        return std::min(0.0, gateDriveVolts(holdVolts, currentGain));
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

    // The same channel at full signal: the R72/R74 end of Tr11/Tr12, driven
    // by `sourceVolts` at TP1/TP2 and returned through 39k to IC6's virtual
    // earth. The summer's input current is the result over wetInputOhms.
    // Shichman-Hodges with the specimen's Idss/Vp above, source and drain
    // swapping with polarity: an N channel's source is its lower terminal.
    // The gates have no resistor to their channels: off D4/D5 a gate floats
    // to its source (VGS ~ 0, as conductanceRatio assumes), and while the
    // diode conducts it is pinned at `gateDrive`, so VGS follows the signal.
    // The two halves of a wave then see different channels: through the
    // mute and unmute the positive half pinches off near gateDrive + |Vp|
    // while the negative half still conducts, so the fade is uneven (half
    // open, a 1 V wave carries second harmonic near -38 dB). Conducting, the
    // gate follows whichever terminal is the source, the channel stays odd-
    // symmetric and only a -110 dB third harmonic at 1 V remains.
    // Each case is a quadratic in the R72/R74 node; the forms below avoid
    // cancellation at small signal, where the result is conductanceRatio's.
    // Gate capacitance, charge injection and leakage remain unmodelled.
    [[nodiscard]] static double channelOutputVolts(double sourceVolts,
                                                   double gateDrive) noexcept
    {
        if (!std::isfinite(sourceVolts) || !std::isfinite(gateDrive))
            return 0.0;
        const double k = idssAmps / (cutoffVolts * cutoffVolts);
        const double pinch = -cutoffVolts;
        const double g = 1.0 / wetInputOhms;
        const double saturated = idssAmps / g; // Idss into 39k, ~109 V
        if (sourceVolts < 0.0)
        {
            // Current leaves the summer: TP1/TP2 is the source end, so VGS
            // does not depend on the unknown node.
            const double v = -sourceVolts;
            const double overdrive = std::min(gateDrive - sourceVolts, 0.0) + pinch;
            if (overdrive <= 0.0)
                return 0.0;
            if (g * (v - overdrive) >= k * overdrive * overdrive)
                return -k * overdrive * overdrive / g;
            // k u^2 - (2 k c + g) u + g v = 0 in u = Vds, smaller root.
            const double b = 2.0 * k * overdrive + g;
            const double u = 2.0 * g * v
                / (b + inlineSqrt(std::max(b * b - 4.0 * k * g * v, 0.0)));
            return sourceVolts + u;
        }
        // Current enters the summer: the R72/R74 node is the source end.
        // With the gate floating there, VGS = 0 and only Vds is unknown.
        double node = 0.0;
        if (sourceVolts - pinch >= saturated)
            node = saturated;
        else
        {
            const double b = 2.0 * k * pinch + g;
            node = sourceVolts - 2.0 * g * sourceVolts
                / (b + inlineSqrt(std::max(b * b - 4.0 * k * g * sourceVolts, 0.0)));
        }
        if (gateDrive >= node)
            return node;
        // D4/D5 conduct: VGS = gateDrive - node, overdrive c0 - node.
        const double c0 = gateDrive + pinch;
        if (c0 <= 0.0)
            return 0.0;
        if (sourceVolts >= c0)
        {
            // Pinched at the TP1/TP2 end: g x = k (c0 - x)^2, root below c0.
            const double b = 2.0 * k * c0 + g;
            return 2.0 * k * c0 * c0 / (b + inlineSqrt(g * g + 4.0 * k * g * c0));
        }
        // k u^2 + (2 k (c0 - v) + g) u - g v = 0 in u = Vds, positive root.
        const double b = 2.0 * k * (c0 - sourceVolts) + g;
        return sourceVolts - 2.0 * g * sourceVolts
            / (b + inlineSqrt(b * b + 4.0 * k * g * sourceVolts));
    }
};
}
