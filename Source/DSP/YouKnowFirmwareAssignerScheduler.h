#pragma once
#include "YouKnowFirmwareUartTrace.h"
#include "YouKnowFirmwareAssignerIo.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace youknow {
// Original A-5 normal foreground, receive parser, allocator, ADC and sender.
// A warm-state comparison executor; cold boot, tape and diagnostic paths stop.
// This is independent of the earlier bounded FirmwareAssignerTrace API.
class FirmwareAssignerScheduler {
  public:
    enum class EventKind : std::uint8_t {
        RamWrite, StackWrite, PatchWrite, MaskWrite, TxBufferWrite, PortCWrite,
        PortBWrite, PortFWrite, MuxWrite, AnmWrite, AdcConversion, ReceiveReady,
        ReceiveRead, InterruptAcceptance, InterruptReturn, ForegroundPass
    };
    struct Event {
        EventKind kind;
        std::uint64_t states;
        std::uint16_t pc, address, value;
    };
    struct Events {
        std::array<Event, 1024> entries{};
        std::size_t count = 0;
    };
    enum class InputKind : std::uint8_t { ByteReady, ReceiveError };
    struct InputEvent {
        std::uint64_t states;
        std::uint8_t value;
        InputKind kind = InputKind::ByteReady;
    };
    // Original data-table values are explicit inputs, not an embedded ROM.
    // The bit tables and slider-address permutation are mathematical board data.
    struct Tables {
        std::array<std::uint8_t, 16> channelDisplay{}; //0040..004F
        std::array<std::uint8_t, 16> digitDisplay{};   //0050..005F
        std::array<std::uint8_t, 25> transposeDisplay{}; //0060..0078
    };
    struct Registers {
        unsigned a = 0, b = 0, c = 0, d = 0, e = 0, h = 0, l = 0, ea = 0, v = 0xff;
        unsigned alternateA = 0, alternateB = 0, alternateC = 0, alternateD = 0,
                 alternateE = 0, alternateH = 0, alternateL = 0, alternateEa = 0,
                 alternateV = 0xff;
        unsigned pc = 0x0111, sp = 0xffff;
        bool carry = false, skip = false, halfCarry = false, zero = false;
        bool l0 = false, l1 = false;
    };
    enum class PendingKind : std::uint8_t { None, Instruction, InterruptEntry };
    struct Pending {
        PendingKind kind = PendingKind::None;
        std::uint64_t start = 0;
        unsigned remaining = 0, address = 0, returnPc = 0;
        std::uint8_t savedPsw = 0;
        bool skipped = false;
    };
    struct State {
        // Caller supplies a coherent warm snapshot. Zero RAM is not cold boot.
        std::array<std::uint8_t, 256> ram{};
        std::array<std::uint8_t, 2048> patchRam{}; //2000..27FF, mutable bus RAM
        bool patchRamAvailable = false;
        Registers registers{};
        Pending pending{};
        FirmwareAssignerIo::Peripheral io{};
        std::uint64_t now = 0, foregroundPasses = 0;
        unsigned mkh = 4, eiDeferred = 0;
        bool interruptEnabled = true, fsr = false, receiveError = false;
        bool rxBufferFull = false;
        std::uint8_t rxBuffer = 0;
    };
    struct Configuration {
        FirmwareUartTrace::Configuration uart{};
        FirmwareAssignerIo::AnmWritePhase anmWritePhase =
            FirmwareAssignerIo::AnmWritePhase::RestartConversion;
    };
    enum class Status : std::uint8_t {
        ReachedTarget, OutputFull, InstructionBudget, InvalidState,
        UnsupportedPath, UnavailableMemory, ReceiveOverrun, PeripheralError
    };
    struct Result {
        Status status;
        std::size_t consumedInputs = 0;
        unsigned completedInstructions = 0;
    };
    // Inclusive absolute original 4MHz-state target; CPU and UART now agree.
    // Partial instructions and16-state entries survive arbitrary chunking.
    // Config/tables are immutable across continuations; input pins may change at
    // call boundaries (no analog acquisition-aperture claim). ByteReady is an
    // explicit RXB-ready scenario, not a pin UART/latch timing model. A new byte
    // while RXB remains unread stops with ReceiveOverrun before choosing which
    // byte real hardware retains. ReceiveError sets/clears ER from value!=0.
    //
    // Equal-state convention: UART events, ADC completion, supplied RX events,
    // instruction completion/access, IRQ arbitration, next instruction. ADC
    // samples at conversion completion. These endpoint choices are numerical
    // scenarios, not claims of subinstruction access timing or all race bounds.
    // ADC has priority over shared RX/TX, MKL=FF fixes paired INTEIN masked.
    // Both V banks must remainFF; MKH0..2 vary. Original HC/Z/CY,SK and MVI/LXI
    // overlays are modeled. Ordinary instructions run continuously: no invented
    // idle instructions, mainloop period or interrupt padding is supplied.
    //
    // All CPU writes commit atomically at completion; OutputFull retains a due
    // zero-remaining instruction/ADC instant. Consume only consumedInputs and
    // drain both event buffers before resuming. Other failures retain the last
    // committed frontier. UnsupportedPath stops before a non-covered instruction;
    // UnavailableMemory stops a supported instruction needing absent patch RAM.
    // InstructionBudget is a resumable100000-instruction per-call safety bound.
    [[nodiscard]] static Result advanceTo(
        State &, FirmwareUartTrace::State &, const Configuration &, const Tables &,
        const FirmwareAssignerIo::Inputs &, std::span<const InputEvent>,
        std::uint64_t target, Events &, FirmwareUartTrace::EventBuffer &) noexcept;
};
} // namespace youknow
