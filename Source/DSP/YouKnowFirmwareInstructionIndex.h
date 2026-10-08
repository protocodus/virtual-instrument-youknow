#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace youknow::firmwareTraceDetail {
// The decoded 4 KiB programs are immutable. Index instruction starts once at
// compile time; zero still rejects operand bytes, gaps and unsupported paths.
// Store one-based descriptor offsets rather than relocated pointer tables.
struct InstructionIndex {
    std::array<std::uint16_t, 4096> offsets {};
    bool valid = true;
    constexpr std::size_t size() const noexcept { return offsets.size(); }
    constexpr std::uint16_t operator[](std::size_t pc) const noexcept { return offsets[pc]; }
};

template<class Instruction, std::size_t Count, class Address>
constexpr auto instructionIndex(const std::array<Instruction, Count>& program,
                                Address address) noexcept {
    static_assert(Count < 65536);
    InstructionIndex index;
    for (std::size_t i = 0; i < Count; ++i) {
        const auto pc = address(program[i]);
        if (pc >= index.size() || index[pc] != 0) {
            // Every production caller asserts this flag on its constexpr
            // result. Keep invalid programs a compile error in C++17 hosts
            // too, without introducing an exception path into embedded DSP.
            index.valid = false;
            return index;
        }
        index.offsets[pc] = static_cast<std::uint16_t>(i + 1);
    }
    return index;
}
}
