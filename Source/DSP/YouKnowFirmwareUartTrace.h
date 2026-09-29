#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace youknow::FirmwareUartTrace {

// A conditional transmitter/pin model, not an RXB-ready or complete CPU emulator.
// Original NEC Stock500375 (April 1987), printed 7-1/3/4/6: separate TXB/shifter,
// empty-buffer transmit request, 8N1 LSB-first, and TxE drains an active frame.
// https://drive.google.com/file/d/0B44NKm9yPA1bNDFXZnFrdG1PdDA/view
// The original A-5 selects 12 MHz / 24 / 16: 128 original CPU states per bit.
// CPU states are 1/4 MHz; their rate is independent of the audio sample rate.
inline constexpr std::uint64_t bitStates = 128;
inline constexpr std::uint64_t frameStates = 10 * bitStates;
inline constexpr std::uint64_t noEvent = std::numeric_limits<std::uint64_t>::max();
inline constexpr std::uint64_t maximumTime = noEvent - 4096;

struct Configuration {
    // Explicit free-running baud-grid SCENARIO, not a manufacturer delay bound.
    // A new idle transfer launches at the first strictly future grid edge. A
    // buffered continuation launches at the preceding frame's end without a gap.
    // Neither the initial divide-by-16 phase nor its reset behavior is documented
    // at instruction-state resolution. A phase sweep is conditional on this map.
    std::uint8_t idleBaudGridPhase = 0;
};

enum class EventKind : std::uint8_t {
    TxBufferWrite,
    TxBufferOverwrite,
    TxBufferEmpty,
    FrameBegin,
    BitBegin,
    StopCenter,
    FrameEnd,
    PortCWrite,
    PinLevels,
    TransmitEnable
};

struct Event {
    std::uint64_t states = 0;
    std::uint64_t frameOrdinal = 0;
    EventKind kind = EventKind::TxBufferWrite;
    std::uint8_t value = 0;
    std::uint8_t bitIndex = 0;
    // Snapshot after this event: bit0 TxD, bit1 module RxD, bit2 MIDI logic line.
    // MIDI logic line is the gate output, not an optocoupler/current-loop model.
    std::uint8_t pinLevels = 7;
};

struct EventBuffer {
    Event* data = nullptr;
    std::size_t capacity = 0;
    std::size_t count = 0;
};

struct State {
    std::uint64_t now = 0;
    bool transmitEnabled = true;
    bool txBufferFull = false;
    std::uint8_t txBuffer = 0;
    bool fst = true;
    // Initial empty+enabled FST is an explicit warm-state input, not a cold-boot
    // subcycle claim. NEC printed 7-12 initializes TxE then polls FST before TXB.
    bool frameActive = false;
    std::uint8_t frameByte = 0;
    std::uint8_t bitIndex = 0; // 0=start, 1..8=data, 9=stop
    std::uint64_t frameOrdinal = 0;
    std::uint64_t frameStart = 0;
    std::uint64_t nextBit = noEvent;
    std::uint64_t idleLaunch = noEvent;
    bool stopCenterObserved = false;
    std::uint8_t portC = 0xfd;
    bool txd = true;
    bool moduleRxD = true;
    bool midiLogic = true;
};

enum class Status : std::uint8_t { Ok, OutputFull, InvalidState, TimeReversal };

// Configuration must remain fixed for a stream. Inputs/public state are checked
// before mutation. advanceTo includes peripheral events at target; perform CPU
// writes afterwards: this peripheral-before-CPU tie convention is an explicit
// scenario, not a measured within-instruction bus edge. OutputFull commits no
// part of its next peripheral instant or write. Drain events and retry the same
// operation. At most 6 events share an instant; a capacity of at least 8 is useful.
// These functions allocate nothing. No interval is advanced by a write or SKIT.
// This models the documented TxE drain/queue behavior, not every SMH side effect:
// enabling an already empty transmitter leaves FST unchanged. The manual's
// enable-then-poll example does not specify that request's assertion subcycle.
Status advanceTo(State&, const Configuration&, std::uint64_t target, EventBuffer&) noexcept;
Status writeTxBuffer(State&, const Configuration&, std::uint8_t, EventBuffer&) noexcept;
Status writePortC(State&, std::uint8_t, EventBuffer&) noexcept;
Status setTransmitEnabled(State&, const Configuration&, bool, EventBuffer&) noexcept;
bool testAndClearFst(State&) noexcept;
std::uint64_t nextEventTime(const State&) noexcept;

} // namespace youknow::FirmwareUartTrace
