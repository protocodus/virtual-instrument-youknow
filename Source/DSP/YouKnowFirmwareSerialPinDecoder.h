#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include "YouKnowCompatibility.h"

namespace youknow::FirmwareSerialPinDecoder {

// Conditional ideal 8N1 receiver, observing ONLY the module RxD pin. Original
// NEC Stock500375 (April 1987), printed 7-6 (PDF59), explicitly validates a low
// start at half a bit after detection, then samples each bit from that center.
// A-5/B-2 configuration gives 128 CPU states/bit. Pin-edge detection is idealized:
// no undocumented input synchronizer latency is inferred from the manual.
// https://drive.google.com/file/d/0B44NKm9yPA1bNDFXZnFrdG1PdDA/view
// Frame-END delivery below is an explicit comparison convention, not a sourced
// RXB/FSR latch subcycle. There is no clock drift, aperture, framing recovery,
// optocoupler, byte metadata, or hidden sender frame information in this model.
inline constexpr std::uint64_t bitStates = 128;
inline constexpr std::uint64_t frameStates = 1280;
inline constexpr std::uint64_t noEvent = std::numeric_limits<std::uint64_t>::max();
inline constexpr std::uint64_t maximumTime = noEvent - 4096;

struct PinLevel {
    std::uint64_t states = 0;
    bool level = true;
};

struct ByteReady {
    std::uint64_t states = 0;
    std::uint8_t value = 0;
};

struct Output {
    Span<ByteReady> bytes;
    std::size_t count = 0;
};

struct State {
    std::uint64_t now = 0;
    bool moduleLevel = true;
    bool instantClosed = false;
    bool active = false;
    std::uint64_t frameStart = 0;
    std::uint64_t nextSample = noEvent;
    // Next event: 0 start center; 1..8 data; 9 stop center; 10 frame end.
    std::uint8_t sampleIndex = 0;
    std::uint8_t byte = 0;
    // A contiguous frame's start edge may precede prior frame-end delivery at
    // the same instant. Retain it until that delivery succeeds under backpressure.
    bool pendingStart = false;
};

enum class Boundary : std::uint8_t { Before, Through };
enum class Status : std::uint8_t {
    ReachedTarget,
    OutputFull,
    InvalidState,
    InvalidArgument,
    TimeReversal,
    InvalidStart,
    FramingError
};

struct Result {
    Status status = Status::ReachedTarget;
    std::uint64_t states = 0;
};

// applyPin advances timed events STRICTLY BEFORE the pin timestamp, then applies
// its level. Ordered equal-time pin changes are permitted. advanceTo(Through)
// samples only after every pin change at the target is known; it closes that
// instant, rejecting late pins. Before leaves the target open (but cannot reopen
// a previously closed instant). A scheduler stopped partway through one instant
// must use Before, not Through. This pin-before-sample tie order is a scenario.
//
// OutputFull commits earlier progress/output, but leaves the next byte pending;
// retry the same operation with drained output. For applyPin, OutputFull means
// the requested pin has NOT been consumed. Every other error leaves State and
// output count unchanged for that call. Errors in a frame are terminal until the
// caller explicitly resets/replaces its state. There are no allocations.
Result applyPin(State&, PinLevel, Output&) noexcept;
Result advanceTo(State&, std::uint64_t target, Boundary, Output&) noexcept;
bool valid(const State&) noexcept;

} // namespace youknow::FirmwareSerialPinDecoder
