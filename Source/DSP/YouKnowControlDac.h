#pragma once

#include <algorithm>
#include <cstdint>

namespace youknow
{
// Roland JUNO-106 Service Notes, July 31 1984, pp. 8 and 13: RA3 is a
// twelve-bit R-2R ladder, followed by the 0..+10 V IC27b branch used by the
// voice VCAs. This is the nominal span; VR34's separately trimmed standoff
// is added by the receiving circuit, not folded into a DAC reference.
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=8
//
// B-2's loadDac at 082f..083c presents EA's upper twelve bits to PB/PC.
// A stored slider reaches 127<<5 = 4064, but an envelope peak and GATE use
// 0x3fff>>2 = 4095 (05b6/073e). Both codes use the same 4096-step ladder:
// 4064 is 9.921875 V, while 4095 is 9.99755859375 V. A normalized envelope
// peak of one therefore cannot reuse the stored-slider voltage span.
// https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L909-L920
// https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L1292-L1302
struct ControlDac
{
    static constexpr std::uint16_t steps = 4096;
    static constexpr std::uint16_t maximumCode = steps - 1;
    static constexpr std::uint16_t storedMaximumCode = 127u << 5u;
    static constexpr double positiveBranchSpanVolts = 10.0;

    [[nodiscard]] static constexpr double positiveSpanVolts(std::uint16_t code) noexcept
    {
        return positiveBranchSpanVolts * std::min(code, maximumCode) / steps;
    }
};
} // namespace youknow
