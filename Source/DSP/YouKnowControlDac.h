#pragma once

#include <algorithm>
#include <cstdint>

namespace youknow
{
namespace controlDacDetail
{
// r is the VR34 segment from -15 V to its wiper; the other path to ground
// is (10k-r)+R126 18k. At TP4=0 the inverting input is a virtual ground,
// so the +0.26 V TP7 trim requires Vw=-0.26*R127/R135=-12.22 V. Solve
// wiper KCL including R127, rather than treating the loaded pot as ideal.
[[nodiscard]] constexpr double biasWiperResistanceOhms() noexcept
{
    constexpr double wiperVolts = -0.26 * 470000.0 / 10000.0;
    double low = 0.0, high = 10000.0;
    for (int iteration = 0; iteration < 50; ++iteration)
    {
        const double r = (low + high) * 0.5;
        const double current = (wiperVolts + 15.0) / r
            + wiperVolts / (28000.0 - r) + wiperVolts / 470000.0;
        if (current > 0.0)
            low = r;
        else
            high = r;
    }
    return (low + high) * 0.5;
}
} // namespace controlDacDetail

// Roland JUNO-106 Service Notes, July 31 1984, pp. 8 and 13: RA3 is a
// twelve-bit R-2R ladder, followed by the positive IC27b branch used by the
// voice VCAs, resonance and noise. Page 8 labels its span approximately
// 0..+10 V. The exact nominal resistor network is derived below; VR34's
// trimmed standoff is added by the receiving circuit, not to DAC reference.
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=8
//
// B-2's loadDac at 082f..083c presents EA's upper twelve bits to PB/PC.
// A stored slider reaches 127<<5 = 4064, but an envelope peak and GATE use
// 0x3fff>>2 = 4095 (05b6/073e). Both codes use the same 4096-step ladder:
// a normalized envelope peak of one therefore cannot reuse the stored-slider
// voltage span. Before the bias-network gain correction, these endpoints
// were 9.921875 V and 9.99755859375 V respectively.
// https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L909-L920
// https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L1292-L1302
struct ControlDac
{
    static constexpr std::uint16_t steps = 4096;
    static constexpr std::uint16_t maximumCode = steps - 1;
    static constexpr std::uint16_t storedMaximumCode = 127u << 5u;
    // Page 13: IC27b noninverting input is TP4; feedback R135=10k,
    // R136=10k to ground, R127=470k to the VR34 10k wiper. R126=18k
    // lies above VR34, whose bottom is -15 V. With p.18's TP7 trim fixed
    // at +0.26 V for DAC zero, VR34's segment r=4990.302718 ohm and
    // Rth=r||(28k-r)=4100.905532 ohm. Its small-signal loading makes
    // IC27b gain 1+R135/R136+R135/(R127+Rth)=2.021092556, not 2.
    // All values are nominal drawn resistances and the existing trim
    // midpoint; no resistor tolerance or transistor characteristic is fitted.
    // https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=13
    static constexpr double biasWiperResistanceOhms =
        controlDacDetail::biasWiperResistanceOhms();
    static constexpr double biasTheveninOhms = biasWiperResistanceOhms
        * (28000.0 - biasWiperResistanceOhms) / 28000.0;
    static constexpr double positiveBufferGain = 1.0 + 10000.0 / 10000.0
        + 10000.0 / (470000.0 + biasTheveninOhms);
    static constexpr double positiveBranchSpanVolts = 5.0 * positiveBufferGain;

    [[nodiscard]] static constexpr double positiveSpanVolts(std::uint16_t code) noexcept
    {
        return positiveBranchSpanVolts * std::min(code, maximumCode) / steps;
    }
};
} // namespace youknow
