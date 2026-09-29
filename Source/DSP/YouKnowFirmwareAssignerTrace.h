#pragma once
#include "YouKnowFirmwareUartTrace.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace youknow {
// Original A-5 sender subgraph, not a complete keyboard/MIDI assigner.
// Execute09E8 producer,0837 FD/command/value caller,0028/0595 serial dispatch,
// 07AF consumer and0020..0027 short ADC counter vector. Full ADC054B and RX05A0
// stop before their first instruction. No firmware image is embedded.
class FirmwareAssignerTrace {
  public:
    enum class EventKind : std::uint8_t {
        RamWrite, StackWrite, MaskWrite, TxBufferWrite, PortCWrite,
        InterruptAcceptance, InterruptReturn, ForegroundReturn, Request
    };
    struct Event {
        EventKind kind;
        std::uint64_t states;
        std::uint16_t pc, address, value;
    };
    struct Events {
        std::array<Event, 512> entries{};
        std::size_t count = 0;
    };
    enum class RequestKind : std::uint8_t { Receive, Adc };
    struct Request { std::uint64_t states; RequestKind kind; };
    struct Registers {
        unsigned a = 0, b = 0, c = 0, d = 0, e = 0, h = 0, l = 0, ea = 0, v = 0xff;
        unsigned alternateA = 0, alternateB = 0, alternateC = 0, alternateD = 0,
                 alternateE = 0, alternateH = 0, alternateL = 0, alternateEa = 0,
                 alternateV = 0xff;
        unsigned pc = 0x09e8, sp = 0xfffd;
        bool carry = false, skip = false;
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
        // Procedure-entry snapshot: seed its real caller return address at SP.
        // Default zero RAM is not a synthesized original cold boot.
        std::array<std::uint8_t, 256> ram{};
        Registers registers{};
        Pending pending{};
        std::uint64_t now = 0;
        unsigned mkh = 4, eiDeferred = 0;
        // Only CY/SK are consumed by this subgraph. Other PSW bits remain opaque
        // through IRQ saves/restores; this is not a full flag/CPU emulator.
        std::uint8_t opaquePsw = 0;
        bool interruptEnabled = true, fsr = false, fad = false;
        bool foregroundBoundary = false;
    };
    enum class Status : std::uint8_t {
        ReachedTarget, OutputFull, InstructionBudget, InvalidState,
        UnsupportedPath, AwaitingForeground, PeripheralError
    };
    struct Result {
        Status status;
        std::size_t consumedRequests = 0;
        unsigned completedInstructions = 0;
    };
    // UART and CPU must enter at identical now. Configuration remains fixed.
    // SMH enable-edge request/reset behavior is not synthesized; initial FST and
    // UART clock phase are explicit warm-state/scenario inputs, not cold boot.
    // target is an inclusive absolute count of original4MHz CPU states. Partial
    // instructions and16-state entries retain their original start/remaining.
    // ReachedTarget includes all effects due at target. InstructionBudget is a
    // resumable100000-completion limit per call. InvalidState/UnsupportedPath/
    // PeripheralError stop at the current committed frontier without rollback.
    // Incoming requests latch FSR/FAD; they do not execute unsupported handlers.
    // Both V banks areFF. MKL=FF is implicit (paired non-ADC source disabled);
    // only MKH0..2 vary.
    // All data/register/port/stack effects commit at instruction/entry completion.
    // Same-state order: UART events, external requests, CPU completion, interrupt
    // arbitration, next instruction. This is a declared endpoint convention.
    //
    // An outer RET to an unsupported foreground PC yields AwaitingForeground,
    // after any immediately eligible IRQ. No idle instructions or fixed IRQ delay
    // are inserted. To continue, supply an actual next supported CPU snapshot;
    // keep any non-original foreground-boundary scenario explicitly labelled.
    // OutputFull is resumable including zero-remaining due completions. Consume
    // only the returned request prefix; drain both event buffers before resuming.
    [[nodiscard]] static Result advanceTo(
        State &, FirmwareUartTrace::State &, const FirmwareUartTrace::Configuration &,
        std::span<const Request>, std::uint64_t target, Events &,
        FirmwareUartTrace::EventBuffer &) noexcept;
};
} // namespace youknow
