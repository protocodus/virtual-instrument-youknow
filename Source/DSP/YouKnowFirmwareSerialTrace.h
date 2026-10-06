#pragma once
#include "YouKnowFirmwareTrace.h"
#include "YouKnowCompatibility.h"

namespace youknow {
// Resumable B-2 receive/main-loop subset. Supports voice-off80..85,
// sustain86/87, voice-on88..8D header+note, returning command headers and
// ignored data outside the parameter range. The rich Tables overload executes
// normal parameter payloads8E..A2 and unarmedA3 via TABLE/JB and command
// auto-increment. ArmedA3 stops at024B before diagnostic side effects; cold
// boot and the diagnostic07B8 path are outside this bounded implementation.
// The legacy ControlTables overload retains its prior stop before TABLE00D7.
// This does not select a product engine profile or model the A-5 assigner.
//
// ByteReady is an explicit RXB-transfer/FSR-assertion event, not frame start.
// NEC April1987 original-family manual: printed7-1/7-3/7-6 specify async8N1,
// RXB-full receive requests and overrun;9-1..9-7 establish ADC-before-RX
// priority, masking and16-state entry;12-21 specifies deferred EI acceptance.
// The exact internal receive latch subcycle/overrun retention is unresolved.
// https://drive.google.com/file/d/0B44NKm9yPA1bNDFXZnFrdG1PdDA/view
// B-2 listing/raw-byte audit and nominal instruction source are linked in
// YouKnowFirmwareTrace.h. No firmware image or coefficient ROM is embedded.
class FirmwareSerialTrace {
  public:
    // Generated model coefficients; no firmware/coefficient ROM is embedded.
    // Keep this separate from ControlTables to preserve the old trace ABI.
    struct Tables {
        FirmwareControlTrace::Tables control {};
        std::array<std::uint8_t, 128> dcoLfoDepth {};
        std::array<std::uint16_t, 128> lfoRate {}, decayRelease {};
        std::array<std::uint16_t, 8> delayFade {};
    };
    struct ByteReady {
        std::uint64_t states;
        std::uint8_t value;
    };
    enum class EventKind : std::uint8_t {
        RamByte,
        Envelope,
        Portamento,
        Inhibit,
        Converter,
        ExternalWrite,
        AdcConversion,
        AdcRequest,
        AnmWrite,
        MaskWrite,
        InterruptAcceptance,
        InterruptReturn,
        SerialReady,
        SerialRead,
        StackWrite,
        StackReset,
        PassStart,
        PortFWrite
    };
    struct Event {
        EventKind kind;
        std::uint64_t states;
        std::uint16_t pc, address, value;
        std::uint8_t card = 255, phaseBits = 0;
        std::uint8_t pa = 0xff, pb = 0, portc = 0;
    };
    struct Events {
        std::array<Event, 1024> entries{};
        std::size_t count = 0;
    };
    enum class Status : std::uint8_t {
        ReachedTarget,
        OutputFull,
        InstructionBudget,
        InvalidState,
        UnsupportedPath,
        ReceiveOverrun
    };
    struct Registers {
        unsigned a = 0, b = 0, c = 0, d = 0, e = 0, h = 0, l = 0, ea = 0, v = 0xff;
        unsigned alternateA = 0, alternateB = 0, alternateC = 0, alternateD = 0, alternateE = 0,
                 alternateH = 0, alternateL = 0, alternateEa = 0, alternateV = 0xff;
        unsigned pa = 0xff, pb = 0, portc = 0, portf = 0, pc = 0x02ec, sp = 0xffff;
        bool carry = false, skip = false;
    };
    enum class PendingKind : std::uint8_t { None, Instruction, InterruptEntry };
    struct Pending {
        PendingKind kind = PendingKind::None;
        std::uint64_t start = 0;
        unsigned remaining = 0, address = 0;
        bool skipped = false, accessDone = false;
        unsigned sampledValue = 0;
        bool sampledSkip = false;
        unsigned interruptVector = 0, returnPc = 0;
        std::uint8_t savedPsw = 0;
    };
    // Initialize at main-loop02EC, not cold reset. Both V banks remainFF;
    // MKL is implicitlyFF and MKH4/5 masks TX while enabling RX. Only the
    // control-flow/condition subset consumed by these B-2 paths is modeled.
    // The caller supplies the initial full PF latch (portf); cold boot is not
    // synthesized. PortFWrite carries address5, full byte value and source PC.
    struct State {
        FirmwareControlTrace::State control{};
        FirmwareAdcTrace::Peripheral adc{};
        Registers registers{};
        Pending pending{};
        std::uint64_t now = 0, passes = 0, serialInterrupts = 0, adcInterrupts = 0;
        unsigned ordinal = 0;
        // CY/SK are the only PSW conditions consumed by this bounded subset.
        // Other bits are retained through stack save/restore, not emulated as
        // a general instruction-set implementation.
        std::uint8_t opaquePsw = 0, rxb = 0;
        bool fsr = false, rxbFull = false;
        bool needsArbitration = true, completedInstruction = false;
    };
    struct Configuration {
        FirmwareAdcTrace::Configuration adc{};
        bool serialEnabled = true;
    };
    struct Result {
        Status status;
        std::size_t consumedBytes = 0;
        unsigned completedInstructions = 0;
    };
    // Mutates only work completed no later than target. CPU PC/register/RAM,
    // external PIT stores, PF writes and stack writes commit at instruction/entry
    // completion. Converter/Inhibit retain the existing start anchors and
    // expose actual PA/PB/PC, not only a pass-relative diagnostic ordinal.
    // Pending.kind/start/remaining expose partial instruction or16-state IRQ
    // entry progress; a zero remaining count can await event-buffer drainage.
    // ADC statesUntilConversion==0 is likewise a valid due-but-uncommitted
    // endpoint after OutputFull. No future full-pass forecast is applied.
    //
    // Deterministic same-time order: ADC completion, byte-ready input,
    // instruction/entry completion and end-access, arbitration, next instruction
    // start-access/pin events. This is an explicit endpoint convention, not a
    // measured bus subcycle or a bound on all possible mixed internal races.
    // RXB read and FSR acknowledgment are separate; a second unread byte returns
    // ReceiveOverrun without choosing an undocumented FIFO/overwrite behavior.
    //
    // Configuration and Tables must remain fixed for the stream. Inputs are
    // held bytes sampled at conversion completion; if changed between calls,
    // that is an explicit input step, not a physical acquisition-aperture model.
    // All ready timestamps must be sorted and>=state.now before any mutation.
    // Events at target are inclusive; a new instruction can start there and
    // remain pending. On OutputFull, drain Events and resume with the returned
    // State and only the unconsumed suffix of ready events. InstructionBudget
    // is also resumable. InvalidState/UnsupportedPath/ReceiveOverrun are stop
    // diagnostics; they do not roll back earlier committed work in this call.
    [[nodiscard]] static Result advanceTo(State &, const Tables &,
                                          const FirmwareAdcTrace::Inputs &, const Configuration &,
                                          Span<const ByteReady>, std::uint64_t target,
                                          Events &) noexcept;
    // Compatibility path: unchanged note/ADC scope, including UnsupportedPath
    // before00D7 for all parameter payloads. No empty table is silently used.
    [[nodiscard]] static Result advanceTo(State &, const FirmwareControlTrace::Tables &,
                                          const FirmwareAdcTrace::Inputs &, const Configuration &,
                                          Span<const ByteReady>, std::uint64_t target,
                                          Events &) noexcept;
};
} // namespace youknow
