#include "YouKnowFirmwareUartTrace.h"

#include <algorithm>
#include <array>

namespace youknow::FirmwareUartTrace {
namespace {

struct Transaction {
    State state;
    std::array<Event, 8> events{};
    std::size_t count = 0;
    bool valid = true;
};

std::uint8_t pinLevels(const State& state) noexcept {
    return static_cast<std::uint8_t>((state.txd ? 1 : 0) |
                                     (state.moduleRxD ? 2 : 0) |
                                     (state.midiLogic ? 4 : 0));
}

void emit(Transaction& transaction, EventKind kind, std::uint8_t value = 0) noexcept {
    const auto& state = transaction.state;
    transaction.events[transaction.count++] = {
        state.now, state.frameOrdinal, kind, value, state.bitIndex, pinLevels(state)};
}

void updatePins(Transaction& transaction) noexcept {
    auto& state = transaction.state;
    const bool moduleSelected = (state.portC & 4) != 0;
    // Roland service notes printed 6/11, IC14 74LS03: two inversions for TxD,
    // complementary PC2 enables. Inactive branch marks high. This ideal Boolean
    // reduction omits nanosecond propagation/RC and cannot model line analogs.
    // https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf
    const bool module = state.txd || !moduleSelected;
    const bool midi = state.txd || moduleSelected;
    if (state.moduleRxD != module || state.midiLogic != midi) {
        state.moduleRxD = module;
        state.midiLogic = midi;
        emit(transaction, EventKind::PinLevels, pinLevels(state));
    }
}

bool valid(const State& state) noexcept {
    if (state.now > maximumTime || state.bitIndex > 9 || state.frameOrdinal == noEvent ||
        state.moduleRxD != (state.txd || !(state.portC & 4)) ||
        state.midiLogic != (state.txd || (state.portC & 4)))
        return false;
    if (state.frameActive) {
        if (state.frameOrdinal == 0 || state.frameStart > state.now ||
            state.frameStart > maximumTime - frameStates ||
            state.now < state.frameStart + state.bitIndex * bitStates ||
            state.nextBit != state.frameStart + (state.bitIndex + 1) * bitStates ||
            state.nextBit <= state.now || state.idleLaunch != noEvent)
            return false;
        const bool expectedBit = state.bitIndex == 0 ? false :
            state.bitIndex == 9 ? true : ((state.frameByte >> (state.bitIndex - 1)) & 1) != 0;
        const auto stopCenter = state.frameStart + 9 * bitStates + bitStates / 2;
        if (state.txd != expectedBit ||
            state.stopCenterObserved != (state.now >= stopCenter))
            return false;
    } else if (state.nextBit != noEvent || !state.txd) {
        return false;
    }
    if (state.idleLaunch != noEvent &&
        (state.frameActive || !state.transmitEnabled || !state.txBufferFull ||
         state.idleLaunch <= state.now || state.idleLaunch > maximumTime))
        return false;
    if (!state.frameActive && state.transmitEnabled && state.txBufferFull &&
        state.idleLaunch == noEvent)
        return false;
    return true;
}

bool validBuffer(const EventBuffer& events) noexcept {
    return events.count <= events.capacity && (events.capacity == 0 || events.data != nullptr);
}

Status commit(State& state, EventBuffer& output, const Transaction& transaction) noexcept {
    if (output.capacity - output.count < transaction.count)
        return Status::OutputFull;
    for (std::size_t index = 0; index < transaction.count; ++index)
        output.data[output.count++] = transaction.events[index];
    state = transaction.state;
    return Status::Ok;
}

void scheduleIdle(State& state, const Configuration& configuration) noexcept {
    if (!state.frameActive && state.transmitEnabled && state.txBufferFull) {
        const auto currentPhase = state.now % bitStates;
        const auto distance = (configuration.idleBaudGridPhase + bitStates - currentPhase) %
                              bitStates;
        state.idleLaunch = state.now + (distance == 0 ? bitStates : distance);
    } else {
        state.idleLaunch = noEvent;
    }
}

void beginFrame(Transaction& transaction) noexcept {
    auto& state = transaction.state;
    if (state.frameOrdinal >= noEvent - 1) {
        transaction.valid = false;
        return;
    }
    state.frameActive = true;
    state.frameByte = state.txBuffer;
    state.txBufferFull = false;
    state.fst = true;
    state.frameOrdinal += 1;
    state.frameStart = state.now;
    state.bitIndex = 0;
    state.nextBit = state.now + bitStates;
    state.idleLaunch = noEvent;
    state.stopCenterObserved = false;
    emit(transaction, EventKind::TxBufferEmpty, state.frameByte);
    emit(transaction, EventKind::FrameBegin, state.frameByte);
    state.txd = false;
    updatePins(transaction);
    emit(transaction, EventKind::BitBegin, 0);
}

void peripheralInstant(Transaction& transaction) noexcept {
    auto& state = transaction.state;
    if (!state.frameActive) {
        beginFrame(transaction);
        return;
    }
    const auto stopCenter = state.frameStart + 9 * bitStates + bitStates / 2;
    if (!state.stopCenterObserved && state.now == stopCenter) {
        state.stopCenterObserved = true;
        // Transmitter stop-center observation only. There is no receiver phase,
        // sampling state, RXB latch, or received-byte event in this component.
        emit(transaction, EventKind::StopCenter, state.frameByte);
        return;
    }
    if (state.bitIndex == 9) {
        emit(transaction, EventKind::FrameEnd, state.frameByte);
        state.frameActive = false;
        state.nextBit = noEvent;
        if (state.transmitEnabled && state.txBufferFull)
            beginFrame(transaction);
        return;
    }
    state.bitIndex += 1;
    state.nextBit += bitStates;
    state.txd = state.bitIndex == 9 ? true :
        ((state.frameByte >> (state.bitIndex - 1)) & 1) != 0;
    updatePins(transaction);
    emit(transaction, EventKind::BitBegin, state.txd ? 1 : 0);
}

} // namespace

std::uint64_t nextEventTime(const State& state) noexcept {
    if (!state.frameActive)
        return state.idleLaunch;
    const auto stopCenter = state.frameStart + 9 * bitStates + bitStates / 2;
    return !state.stopCenterObserved ? std::min(state.nextBit, stopCenter) : state.nextBit;
}

Status advanceTo(State& state, const Configuration& configuration, std::uint64_t target,
                 EventBuffer& output) noexcept {
    if (!valid(state) || !validBuffer(output) || configuration.idleBaudGridPhase >= bitStates ||
        target > maximumTime)
        return Status::InvalidState;
    if (target < state.now)
        return Status::TimeReversal;
    while (nextEventTime(state) <= target) {
        Transaction transaction{state};
        transaction.state.now = nextEventTime(state);
        peripheralInstant(transaction);
        if (!transaction.valid)
            return Status::InvalidState;
        const auto status = commit(state, output, transaction);
        if (status != Status::Ok)
            return status;
    }
    state.now = target;
    return Status::Ok;
}

Status writeTxBuffer(State& state, const Configuration& configuration, std::uint8_t byte,
                     EventBuffer& output) noexcept {
    if (!valid(state) || !validBuffer(output) || configuration.idleBaudGridPhase >= bitStates ||
        state.now > maximumTime - frameStates - bitStates)
        return Status::InvalidState;
    Transaction transaction{state};
    if (transaction.state.txBufferFull)
        emit(transaction, EventKind::TxBufferOverwrite, transaction.state.txBuffer);
    transaction.state.txBuffer = byte;
    transaction.state.txBufferFull = true;
    emit(transaction, EventKind::TxBufferWrite, byte);
    // A write does not clear sticky FST: the actual SKIT instruction does that.
    scheduleIdle(transaction.state, configuration);
    return commit(state, output, transaction);
}

Status writePortC(State& state, std::uint8_t byte, EventBuffer& output) noexcept {
    if (!valid(state) || !validBuffer(output))
        return Status::InvalidState;
    Transaction transaction{state};
    transaction.state.portC = byte;
    emit(transaction, EventKind::PortCWrite, byte);
    updatePins(transaction);
    return commit(state, output, transaction);
}

Status setTransmitEnabled(State& state, const Configuration& configuration, bool enabled,
                          EventBuffer& output) noexcept {
    if (!valid(state) || !validBuffer(output) || configuration.idleBaudGridPhase >= bitStates ||
        state.now > maximumTime - frameStates - bitStates)
        return Status::InvalidState;
    Transaction transaction{state};
    transaction.state.transmitEnabled = enabled;
    emit(transaction, EventKind::TransmitEnable, enabled ? 1 : 0);
    // NEC printed 7-3: disabling drains the active shifter, preserves TXB, and
    // prevents TXB transferring. Reenable timing here follows the declared grid.
    scheduleIdle(transaction.state, configuration);
    return commit(state, output, transaction);
}

bool testAndClearFst(State& state) noexcept {
    const bool pending = state.fst;
    state.fst = false;
    return pending;
}

} // namespace youknow::FirmwareUartTrace
