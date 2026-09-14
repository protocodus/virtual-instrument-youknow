#pragma once
#include <array>
#include <cstdint>

namespace youknow {
// An executable nominal no-interrupt control pass. Inputs are explicit CPU RAM
// and coefficient tables supplied by the existing engine laws. No oscillator
// ROM image, interrupt latency, pin propagation or random timing is embedded.
// RAM events commit at instruction completion, when an interrupt may observe
// them; paired bytes from one word store are atomic at this boundary. Converter
// edges use instruction-start T, matching the existing PIT scheduling policy.
// The installed chip's within-instruction pin timing remains unmeasured.
// B-2 source: https://github.com/ErroneousBosh/j106roms/blob/
// 26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt
// NMOS timing: https://datasheet4u.com/pdf/298676/UPD7810.pdf#page=17
// Shift-carry ISA corroboration (not NMOS timing):
// https://datasheets.chipdb.org/NEC/uPD78C1x/uPD78C10A.pdf#page=32
class FirmwareControlTrace {
public:
    struct Tables {
        std::array<std::uint8_t,128> portamento {};
        std::array<std::uint16_t,128> attack {};
        std::array<std::uint16_t,104> pitchCv {}, pitchDivider {};
    };
    struct State { std::array<std::uint8_t,256> ram {}; bool adcComplete=false; };
    enum class EventKind : std::uint8_t { Envelope, Portamento, Inhibit, Converter, RamByte };
    struct Event {
        EventKind kind;
        std::uint32_t states;
        std::uint16_t address, value;
        std::uint8_t card, phaseBits;
    };
    struct Result {
        State finalState {};
        std::array<Event,512> events {};
        std::size_t count=0;
        std::uint32_t states=0;
        std::uint16_t stoppedAt=0;
        bool valid=false;
    };
    [[nodiscard]] static Result run(const State&,const Tables&) noexcept;
};
}
