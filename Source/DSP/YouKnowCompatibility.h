#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>

#if __cplusplus >= 202002L || (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L)
#include <bit>
#include <span>
#endif

namespace youknow
{
// Shared DSP is also consumed by C++17 hosts. Keep C++20 clients' span API,
// and supply only the dynamic view operations used by the firmware paths on
// older toolchains. The view never owns or allocates its backing storage.
#if defined(__cpp_lib_span) && __cpp_lib_span >= 202002L
template <typename T>
using Span = std::span<T>;
#else
template <typename T>
class Span
{
public:
    constexpr Span() noexcept = default;
    constexpr Span(T* data, std::size_t count) noexcept : data_(data), size_(count) {}

    template <typename U, std::size_t N,
              std::enable_if_t<std::is_convertible_v<U (*)[], T (*)[]>, int> = 0>
    constexpr Span(U (&data)[N]) noexcept : data_(data), size_(N) {}

    template <typename Container,
              typename U = std::remove_pointer_t<decltype(std::declval<Container&>().data())>,
              std::enable_if_t<std::is_convertible_v<
                  U (*)[], T (*)[]>, int> = 0>
    constexpr Span(Container& data) noexcept : data_(data.data()), size_(data.size()) {}

    template <typename Container,
              typename U = std::remove_pointer_t<decltype(std::declval<const Container&>().data())>,
              std::enable_if_t<std::is_convertible_v<
                  U (*)[], T (*)[]>, int> = 0>
    constexpr Span(const Container& data) noexcept : data_(data.data()), size_(data.size()) {}

    constexpr T* data() const noexcept { return data_; }
    constexpr std::size_t size() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }
    constexpr T& operator[](std::size_t index) const noexcept { return data_[index]; }
    constexpr T* begin() const noexcept { return data_; }
    constexpr T* end() const noexcept { return size_ == 0 ? data_ : data_ + size_; }
    constexpr Span first(std::size_t count) const noexcept { return { data_, count }; }
    constexpr Span subspan(std::size_t offset, std::size_t count) const noexcept
    { return { offset == 0 ? data_ : data_ + offset, count }; }
    constexpr Span subspan(std::size_t offset) const noexcept
    { return subspan(offset, size_ - offset); }

private:
    T* data_ = nullptr;
    std::size_t size_ = 0;
};
#endif

// Exactly the correctly rounded float/double constants used by std::numbers.
namespace numbers
{
template <typename T>
inline constexpr T pi_v = static_cast<T>(3.141592653589793238462643383279502884L);
inline constexpr double pi = pi_v<double>;
}

#if defined(__cpp_lib_bit_cast) && __cpp_lib_bit_cast >= 201806L
template <typename To, typename From>
constexpr To bitCast(const From& value) noexcept
{
    return std::bit_cast<To>(value);
}
#else
template <typename To, typename From>
To bitCast(const From& value) noexcept
{
    static_assert(sizeof(To) == sizeof(From));
    static_assert(std::is_trivially_copyable_v<To> && std::is_trivially_copyable_v<From>);
    To result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
#endif
} // namespace youknow
