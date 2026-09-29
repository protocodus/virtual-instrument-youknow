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

// Standalone, serial-quiet B-2 pass with the free-running four-channel ADC.
// This is a comparison capability; it does not select an engine timing profile.
// Eight raw input bytes are held constant throughout run(). No input acquisition
// aperture, serial wire timing, host-to-ADC inverse map or analog settling is
// inferred. The caller supplies initial RAM and peripheral phase explicitly.
//
// NEC April 1987 uPD7810/11 user manual (also covers H/CMOS variants), printed
// 8-2/8-3: 192 states/conversion, four-channel repeated scan even while masked;
// 9-6/9-7: instruction-boundary acceptance and 16-state automatic entry;
// 12-21: EI defers until one following instruction; 12-52: RETI restores PC/PSW.
// https://drive.google.com/file/d/0B44NKm9yPA1bNDFXZnFrdG1PdDA/view
// Handler semantics are original descriptors from the hash-pinned B-2 listing
// linked above: vector 0020, handler 0070..008D. No firmware image is embedded.
class FirmwareAdcTrace {
public:
    static constexpr unsigned conversionStates = 192;
    static constexpr unsigned interruptEntryStates = 16;

    // The manual does not identify the ANM write's precise conversion substate.
    // These are explicit numerical scenarios, not measured variants or proven
    // exhaustive bounds. RestartConversion is a nominal restart convention.
    enum class AnmWritePhase : std::uint8_t {
        RestartConversion,
        PreserveConversionBoundary,
        PreserveCompleteScanPhase
    };
    // IRQ recognition occurs at an instruction boundary, but the source does
    // not identify SKIT/ANM/CR/MKH access substates. These policies place all
    // such accesses at one selected endpoint, without claiming that correlated
    // endpoint choices bound every possible internal race. CPU RAM commits and
    // architectural RETI completion retain their existing completion timestamps.
    enum class PeripheralAccessBoundary : std::uint8_t {
        InstructionStart,
        InstructionCompletion
    };
    struct Configuration {
        AnmWritePhase anmWritePhase = AnmWritePhase::RestartConversion;
        PeripheralAccessBoundary accessBoundary =
            PeripheralAccessBoundary::InstructionCompletion;
        // Diagnostic disabled-acceptance control; ADC conversions still run.
        bool interrupts = true;
    };
    struct Inputs {
        std::array<std::uint8_t, 8> raw {};
    };
    struct Peripheral {
        std::array<std::uint8_t, 4> conversion {};
        std::uint64_t elapsedStates = 0;
        std::uint16_t statesUntilConversion = conversionStates;
        std::uint8_t channel = 0;
        std::uint8_t anm = 0;
        // Quiet B-2 uses MKH4/5: serial TX masked, ADC enabled/disabled.
        // MKL is implicitly fixed at its reset value FF, as in B-2; its bit7
        // masks paired INTEIN, allowing automatic FAD clear on vectoring.
        std::uint8_t mkh = 4;
        // At an API boundary, 0 means eligible; 1 means execute one instruction
        // before acceptance. Normally preserve the returned value unchanged.
        std::uint8_t eiDeferred = 0;
        bool request = false;
        bool interruptsEnabled = true;
    };
    struct State {
        FirmwareControlTrace::State control {};
        Peripheral peripheral {};
    };
    enum class EventKind : std::uint8_t {
        Conversion, Request, Acceptance, AnmWrite, MaskWrite, Return
    };
    struct Event {
        EventKind kind;
        // Absolute CPU-state coordinate, including automatic entry/ISR time.
        std::uint64_t states;
        // Instruction source for ANM/Mask/Return; interrupted next PC for
        // Acceptance; FFFF for autonomous Conversion/Request.
        std::uint16_t pc;
        std::uint8_t channel, value;
    };
    struct Result {
        // Main-loop RAM/converter event times are relative to run() entry;
        // peripheral event times above use the persistent absolute coordinate.
        FirmwareControlTrace::Result control {};
        Peripheral peripheral {};
        std::array<Event, 512> events {};
        std::size_t count = 0;
        std::uint32_t acceptedInterrupts = 0;
        bool peripheralValid = true;
    };
    // State counts are CPU timing states, independent of host sample rate.
    // Entry is a main-loop boundary before 02EC, not a cold-reset simulation.
    // An eligible initial pending request is serviced before the main pass,
    // then that pass still executes.
    // A pending request at the final main-loop boundary is likewise arbitrated
    // before returning. Thus a recorded pass can contain 0/1/2 ADC services.
    //
    // For continuation, after BOTH result.control.valid and peripheralValid,
    // pass State{result.control.finalState,result.peripheral} to the next call.
    // State.control.adcComplete is ignored as input; Peripheral.request is the
    // authoritative FAD latch and is mirrored to that legacy field on return.
    // This complete-pass API does not commit partial-pass CPU/peripheral state
    // at a serial note restart, nor is it a general uPD7810 emulator.
    [[nodiscard]] static Result run(const State&, const FirmwareControlTrace::Tables&,
                                    const Inputs&, const Configuration&) noexcept;
};
}
