#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace youknow
{
namespace finite_detail
{
// C++17 object-representation copy through character types is permitted by
// the aliasing rules. A fixed byte loop avoids the out-of-line memcpy that
// -fno-builtin can retain for a library bit-cast fallback. Optimizers reduce
// it to a register transfer; neither classification nor the copy performs
// floating arithmetic, including on signaling NaNs.
template <typename Bits, typename Value>
[[nodiscard]] inline Bits representation(const Value& value) noexcept
{
    static_assert(sizeof(Bits)==sizeof(Value));
    Bits result {};
    auto* destination=reinterpret_cast<unsigned char*>(&result);
    const auto* source=reinterpret_cast<const unsigned char*>(&value);
    for(std::size_t i=0;i<sizeof(Bits);++i) destination[i]=source[i];
    return result;
}
}

// Keep exactly std::isfinite's finite/nonfinite decision. SDK 5's scalar
// runtime otherwise calls a full out-of-line fpclassify routine at each
// guarded VCF control node. IEEE binary32/binary64 encode infinities and all
// NaNs with the all-ones exponent. Sign, payload, zeros and subnormals do not
// affect this predicate. No input value, fallback or safety guard is changed.
[[nodiscard]] inline bool finiteValue(float value) noexcept
{
    if constexpr(sizeof(float)==sizeof(std::uint32_t)
        && std::numeric_limits<float>::is_iec559
        && std::numeric_limits<float>::radix==2
        && std::numeric_limits<float>::digits==24)
        return (finite_detail::representation<std::uint32_t>(value)&0x7f800000u)!=0x7f800000u;
    else return std::isfinite(value);
}

[[nodiscard]] inline bool finiteValue(double value) noexcept
{
    if constexpr(sizeof(double)==sizeof(std::uint64_t)
        && std::numeric_limits<double>::is_iec559
        && std::numeric_limits<double>::radix==2
        && std::numeric_limits<double>::digits==53)
        return (finite_detail::representation<std::uint64_t>(value)&0x7ff0000000000000ull)
            !=0x7ff0000000000000ull;
    else return std::isfinite(value);
}
} // namespace youknow
