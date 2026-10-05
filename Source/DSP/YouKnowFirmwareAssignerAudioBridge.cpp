#include "YouKnowFirmwareAssignerAudioBridge.h"
#include <algorithm>

namespace youknow {
namespace {
using Bridge = FirmwareAssignerAudioBridge;
using Sender = Bridge::Sender;
namespace Decoder = FirmwareSerialPinDecoder;
namespace Uart = FirmwareUartTrace;
namespace Io = FirmwareAssignerIo;

bool sameInputs(const Io::Inputs& left, const Io::Inputs& right) noexcept {
    return left.keyboard == right.keyboard && left.panel == right.panel
        && left.panelAdc == right.panelAdc && left.directAdc == right.directAdc
        && left.pcInputBits == right.pcInputBits;
}

bool resumable(Sender::Status status) noexcept {
    return status == Sender::Status::ReachedTarget
        || status == Sender::Status::OutputFull
        || status == Sender::Status::InstructionBudget;
}

bool valid(const Bridge::State& state) noexcept {
    if (state.assigner.now != state.uart.now || state.assigner.now > Uart::maximumTime
        || !Decoder::valid(state.receiver) || state.receiver.now > state.assigner.now
        || state.wireCount > state.pendingWire.size() || state.wireCursor > state.wireCount
        || (!state.batchPending && (state.wireCount != 0 || state.wireCursor != 0))
        || (state.completedSourceValid && state.completedSourceThrough > state.assigner.now)
        || static_cast<unsigned>(state.batchStatus)
            > static_cast<unsigned>(Sender::Status::PeripheralError))
        return false;
    auto previous = state.receiver.now;
    for (auto i = state.wireCursor; i < state.wireCount; ++i) {
        const auto& event = state.pendingWire[i];
        // Non-pin metadata may precede receiver.now after a retry has advanced
        // toward a later pin. Only the next unconsumed pin constrains the decoder.
        if (event.states > state.assigner.now)
            return false;
        if (event.kind == Uart::EventKind::PinLevels) {
            if (event.states < previous)
                return false;
            previous = event.states;
        }
    }
    return true;
}

// The decoder's type deliberately does not depend on B-2 or Engine. One local
// byte lets its transactional operation stop exactly at downstream backpressure.
// If an operation spans two completed frames, retry it with another one-byte
// slot while the caller still has room; do not consume its pin prematurely.
template<class Operation>
Decoder::Status decode(Bridge::State& state, Bridge::Output& output,
                       Operation operation) noexcept {
    for (;;) {
        Decoder::ByteReady byte{};
        const bool hasSpace = output.count < output.bytes.size();
        Decoder::Output decoded{
            std::span<Decoder::ByteReady>(&byte, hasSpace ? 1u : 0u), 0};
        const auto result = operation(state.receiver, decoded);
        if (decoded.count != 0)
            output.bytes[output.count++] = byte;
        if (result.status != Decoder::Status::OutputFull || !hasSpace
            || output.count == output.bytes.size())
            return result.status;
    }
}
} // namespace

bool FirmwareAssignerAudioBridge::reset(State& state, const Sender::State& warmAssigner,
                                       const Uart::State& warmUart) noexcept {
    if (warmAssigner.now != 0 || warmUart.now != 0 || warmUart.frameActive
        || warmUart.txBufferFull || warmUart.nextBit != Uart::noEvent
        || warmUart.idleLaunch != Uart::noEvent || !warmUart.txd
        || !warmUart.moduleRxD || !warmUart.midiLogic)
        return false;
    // With an idle, empty transmitter this performs no events and its baud
    // phase is irrelevant. Reuse the peripheral's structural validation rather
    // than accepting an invalid ordinal/bit index for a successful reset.
    auto checkedUart = warmUart;
    Uart::EventBuffer noEvents;
    if (Uart::advanceTo(checkedUart, {}, 0, noEvents) != Uart::Status::Ok)
        return false;
    State fresh;
    fresh.assigner = warmAssigner;
    fresh.uart = warmUart;
    state = fresh;
    return true;
}

FirmwareAssignerAudioBridge::Result FirmwareAssignerAudioBridge::advanceTo(
    State& state, const Sender::Configuration& configuration, const Sender::Tables& tables,
    const Io::Inputs& inputs, std::span<const Sender::InputEvent> incoming,
    std::uint64_t target, Output& output) noexcept {
    if (!valid(state))
        return {Status::InvalidState};
    if (target < state.assigner.now || target > Uart::maximumTime
        || output.count > output.bytes.size()
        || (!output.bytes.empty() && output.bytes.data() == nullptr)
        || incoming.size() > maximumInputEvents
        || (!incoming.empty() && incoming.data() == nullptr)
        || !Io::valid(inputs) || !Io::valid(configuration.anmWritePhase)
        || configuration.uart.idleBaudGridPhase >= Uart::bitStates
        || (state.heldInputsValid && !sameInputs(inputs, state.heldInputs)))
        return {Status::InvalidArgument};
    auto previous = state.assigner.now;
    for (const auto& event : incoming) {
        if (event.states < previous || event.states > Uart::maximumTime
            || static_cast<unsigned>(event.kind) > 1u
            || (state.completedSourceValid && event.states <= state.completedSourceThrough))
            return {Status::InvalidArgument};
        previous = event.states;
    }

    Result result;
    const auto finish = [&](Status status, Decoder::Status decoder =
            Decoder::Status::ReachedTarget) {
        result.status = status;
        result.sourceStatus = state.batchStatus;
        result.decoderStatus = decoder;
        return result;
    };
    unsigned senderCalls = 0;
    for (;;) {
        if (state.batchPending) {
            while (state.wireCursor < state.wireCount) {
                const auto& event = state.pendingWire[state.wireCursor];
                if (event.kind == Uart::EventKind::PinLevels) {
                    const Decoder::PinLevel pin{event.states, (event.pinLevels & 2u) != 0};
                    const auto status = decode(state, output, [&](auto& receiver, auto& decoded) {
                        return Decoder::applyPin(receiver, pin, decoded);
                    });
                    if (status == Decoder::Status::OutputFull)
                        return finish(Status::OutputFull, status);
                    if (status != Decoder::Status::ReachedTarget)
                        return finish(Status::DecodeError, status);
                }
                ++state.wireCursor;
            }
            // OutputFull may leave the UART bit event committed while a CPU
            // PortC write at this same state is still pending. Keep that instant
            // open until the scheduler has really completed it. Advancing the
            // next source slice will also flush older samples before newer pins.
            const auto boundary = state.batchStatus == Sender::Status::ReachedTarget
                ? Decoder::Boundary::Through : Decoder::Boundary::Before;
            const auto status = decode(state, output, [&](auto& receiver, auto& decoded) {
                return Decoder::advanceTo(receiver, state.assigner.now, boundary, decoded);
            });
            if (status == Decoder::Status::OutputFull)
                return finish(Status::OutputFull, status);
            if (status != Decoder::Status::ReachedTarget)
                return finish(Status::DecodeError, status);
            state.batchPending = false;
            state.wireCount = state.wireCursor = 0;
            if (!resumable(state.batchStatus))
                return finish(Status::SourceStopped);
        }
        if (state.completedSourceValid && state.completedSourceThrough == target
            && state.receiver.now == target && state.receiver.instantClosed) {
            state.heldInputsValid = false;
            return finish(Status::ReachedTarget);
        }
        if (senderCalls == maximumSenderCalls)
            return finish(Status::WorkLimit);

        if (!state.heldInputsValid) {
            state.heldInputs = inputs;
            state.heldInputsValid = true;
        }
        const auto next = state.assigner.now
            + std::min(statesPerSlice, target - state.assigner.now);
        Sender::Events discarded;
        Uart::EventBuffer wire{state.pendingWire.data(), state.pendingWire.size(), 0};
        const auto sender = Sender::advanceTo(
            state.assigner, state.uart, configuration, tables, inputs,
            incoming.subspan(result.consumedInputs), next, discarded, wire);
        // The configured contact owner can change its physical pins at exact
        // CPU instruction endpoints. Retain its resulting input image across
        // output-pressure retries, never a host-polling endpoint.
        if (configuration.inputService != nullptr) state.heldInputs = inputs;
        ++senderCalls;
        result.consumedInputs += sender.consumedInputs;
        state.wireCount = wire.count;
        state.wireCursor = 0;
        state.batchStatus = sender.status;
        state.batchPending = true;
        if (sender.status == Sender::Status::ReachedTarget) {
            state.completedSourceValid = true;
            state.completedSourceThrough = state.assigner.now;
        }
    }
}
} // namespace youknow
