#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace youknow::firmwareTraceDetail {
// The decoded 4 KiB programs are immutable. Index instruction starts once at
// compile time; zero still rejects operand bytes, gaps and unsupported paths.
// Store one-based descriptor offsets rather than relocated pointer tables.
template<class Instruction, std::size_t Count, class Address>
consteval auto instructionIndex(const std::array<Instruction, Count>& program,
                                Address address) {
    static_assert(Count < 65536);
    std::array<std::uint16_t, 4096> index {};
    for (std::size_t i = 0; i < Count; ++i) {
        const auto pc = address(program[i]);
        if (pc >= index.size() || index[pc] != 0)
            throw "invalid or duplicate firmware instruction address";
        index[pc] = static_cast<std::uint16_t>(i + 1);
    }
    return index;
}
}
