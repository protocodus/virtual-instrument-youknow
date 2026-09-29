#include "YouKnowFirmwareSerialPinDecoder.h"

namespace youknow::FirmwareSerialPinDecoder {
namespace {

std::uint64_t offset(std::uint8_t index) noexcept {
    return index == 10 ? frameStates : bitStates / 2 + bitStates * index;
}

bool validOutput(const Output& output) noexcept {
    return output.count <= output.bytes.size()
        && (output.bytes.empty() || output.bytes.data() != nullptr);
}

void startFrame(State& state, std::uint64_t time) noexcept {
    state.active = true;
    state.frameStart = time;
    state.nextSample = time + bitStates / 2;
    state.sampleIndex = 0;
    state.byte = 0;
    state.pendingStart = false;
}

// Caller stages State and restores output.count on non-backpressure errors.
Status advance(State& state, std::uint64_t target, Boundary boundary,
               Output& output) noexcept {
    while (state.active && (state.nextSample < target
        || (boundary == Boundary::Through && state.nextSample == target))) {
        const auto time = state.nextSample;
        if (state.sampleIndex == 10 && output.count == output.bytes.size()) {
            state.now = time;
            state.instantClosed = false;
            return Status::OutputFull;
        }
        state.now = time;
        state.instantClosed = false;
        if (state.sampleIndex == 0 && state.moduleLevel)
            return Status::InvalidStart;
        if (state.sampleIndex == 9 && !state.moduleLevel)
            return Status::FramingError;
        if (state.sampleIndex >= 1 && state.sampleIndex <= 8 && state.moduleLevel)
            state.byte |= static_cast<std::uint8_t>(1u << (state.sampleIndex - 1));
        if (state.sampleIndex == 10) {
            output.bytes[output.count++] = {time, state.byte};
            if (state.pendingStart) {
                startFrame(state, time);
            } else {
                state.active = false;
                state.frameStart = 0;
                state.nextSample = noEvent;
                state.sampleIndex = 0;
                state.byte = 0;
            }
        } else {
            ++state.sampleIndex;
            state.nextSample = state.frameStart + offset(state.sampleIndex);
        }
    }
    if (target > state.now) {
        state.now = target;
        state.instantClosed = boundary == Boundary::Through;
    } else if (boundary == Boundary::Through) {
        state.instantClosed = true;
    }
    return Status::ReachedTarget;
}

Result finish(State& destination, const State& staged, Status status,
              Output& output, std::size_t originalCount) noexcept {
    if (status == Status::ReachedTarget || status == Status::OutputFull)
        destination = staged;
    else
        output.count = originalCount;
    return {status, destination.now};
}

} // namespace

bool valid(const State& state) noexcept {
    if (state.now > maximumTime)
        return false;
    if (!state.active)
        return state.frameStart == 0 && state.nextSample == noEvent
            && state.sampleIndex == 0 && state.byte == 0 && !state.pendingStart;
    if (state.sampleIndex > 10 || state.frameStart > maximumTime - frameStates
        || state.now < state.frameStart
        || state.nextSample != state.frameStart + offset(state.sampleIndex)
        || state.now > state.nextSample
        || (state.now == state.nextSample && state.instantClosed))
        return false;
    const unsigned bitsRead = state.sampleIndex <= 1 ? 0
        : (state.sampleIndex <= 8 ? state.sampleIndex - 1 : 8);
    if ((static_cast<unsigned>(state.byte) >> bitsRead) != 0)
        return false;
    return !state.pendingStart || (state.sampleIndex == 10
        && state.now == state.nextSample && !state.instantClosed);
}

Result advanceTo(State& state, std::uint64_t target, Boundary boundary,
                 Output& output) noexcept {
    if (!valid(state))
        return {Status::InvalidState, state.now};
    if (!validOutput(output) || target > maximumTime
        || (boundary != Boundary::Before && boundary != Boundary::Through))
        return {Status::InvalidArgument, state.now};
    if (target < state.now)
        return {Status::TimeReversal, state.now};
    State staged = state;
    const auto originalCount = output.count;
    return finish(state, staged, advance(staged, target, boundary, output),
                  output, originalCount);
}

Result applyPin(State& state, PinLevel pin, Output& output) noexcept {
    if (!valid(state))
        return {Status::InvalidState, state.now};
    if (!validOutput(output) || pin.states > maximumTime)
        return {Status::InvalidArgument, state.now};
    if (pin.states < state.now || (pin.states == state.now && state.instantClosed))
        return {Status::TimeReversal, state.now};
    State staged = state;
    const auto originalCount = output.count;
    auto status = advance(staged, pin.states, Boundary::Before, output);
    if (status == Status::ReachedTarget) {
        const bool falling = staged.moduleLevel && !pin.level;
        if (falling && !staged.active) {
            if (pin.states > maximumTime - frameStates)
                status = Status::InvalidArgument;
            else
                startFrame(staged, pin.states);
        } else if (falling && staged.sampleIndex == 10) {
            // One full stop bit is required by this declared fixed-rate frame.
            if (pin.states != staged.frameStart + frameStates)
                status = Status::FramingError;
            else if (pin.states > maximumTime - frameStates)
                status = Status::InvalidArgument;
            else
                staged.pendingStart = true;
        }
        if (status == Status::ReachedTarget)
            staged.moduleLevel = pin.level;
    }
    return finish(state, staged, status, output, originalCount);
}

} // namespace youknow::FirmwareSerialPinDecoder
