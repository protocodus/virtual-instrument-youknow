#pragma once

#include "YouKnowFirmwareAssignerScheduler.h"
#include "YouKnowFirmwareSerialPinDecoder.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace youknow {

// Incremental original A-5 foreground -> UART module pin -> ideal B-2 receiver.
// The scheduler executes actual instructions; there is no synthetic foreground
// delay or precomputed song. The receiver observes PinLevels only, never sender
// byte/frame metadata. Its frame-end RXB-ready convention and the transmitter's
// baud phase are explicit comparison scenarios, not measured latch subcycles.
// This class has no Engine dependency and does not change the product MIDI path.
class FirmwareAssignerAudioBridge {
  public:
    using Sender = FirmwareAssignerScheduler;
    using ByteReady = FirmwareSerialPinDecoder::ByteReady;
    struct Output {
        std::span<ByteReady> bytes;
        std::size_t count = 0;
    };
    static constexpr std::size_t wireCapacity = 64;
    static constexpr std::size_t maximumInputEvents = 1024;
    static constexpr unsigned maximumSenderCalls = 64;
    static constexpr std::uint64_t statesPerSlice = 4096;

    struct State {
        Sender::State assigner{};
        FirmwareUartTrace::State uart{};
        FirmwareSerialPinDecoder::State receiver{};
        // Retained wire events can precede assigner.now under output pressure.
        // Callers must not edit scratch fields or replace individual clocks.
        std::array<FirmwareUartTrace::Event, wireCapacity> pendingWire{};
        std::size_t wireCount = 0, wireCursor = 0;
        Sender::Status batchStatus = Sender::Status::ReachedTarget;
        bool batchPending = false;
        // CPU has already consumed the inclusive endpoint of this source slice.
        bool completedSourceValid = false;
        std::uint64_t completedSourceThrough = 0;
        // Held physical inputs may change only after a public ReachedTarget.
        FirmwareAssignerIo::Inputs heldInputs{};
        bool heldInputsValid = false;
    };
    enum class Status : std::uint8_t {
        ReachedTarget, OutputFull, WorkLimit, InvalidArgument, InvalidState,
        SourceStopped, DecodeError
    };
    struct Result {
        Status status = Status::ReachedTarget;
        std::size_t consumedInputs = 0;
        Sender::Status sourceStatus = Sender::Status::ReachedTarget;
        FirmwareSerialPinDecoder::Status decoderStatus =
            FirmwareSerialPinDecoder::Status::ReachedTarget;
    };

    // Reset accepts an explicit time-zero warm CPU and idle-high, empty UART.
    // It clears every partial receiver frame and retained wire event. It does
    // not synthesize a cold boot or validate the caller's musical patch. Full
    // CPU/configuration validation remains the scheduler's responsibility.
    // Failure leaves destination unchanged. Resetting an attached audio stream
    // also requires resetting its B-2 Engine; sample-rate changes need no reset.
    [[nodiscard]] static bool reset(State&, const Sender::State& warmAssigner,
                                   const FirmwareUartTrace::State& warmUart) noexcept;

    // Advance to an inclusive absolute 4MHz CPU-state target. Configuration and
    // tables must remain immutable. Physical inputs may change only after this
    // API reports ReachedTarget; all incomplete retries use the same held inputs.
    // At most maximumInputEvents sorted RX events are accepted per call. Consume
    // exactly Result::consumedInputs before retrying. Future inputs stay caller-
    // owned. Once an inclusive source boundary is completed, new RX events at
    // or before it are late; time zero is accepted only before the first advance.
    //
    // Drain returned bytes into the B-2 streaming queue BEFORE rendering audio
    // through target. OutputFull and WorkLimit retain all partially decoded
    // frames, same-time pin events and source progress; repeat with drained output
    // and the unconsumed RX suffix. A5/UART may be ahead of receiver under pressure.
    // ReachedTarget means all three are through target. No wall-clock deadlines
    // are implied by the bounded 64*4096-state work quota.
    //
    // Pin changes at an instant precede receiver sampling. A sender OutputFull
    // or InstructionBudget can leave another same-time CPU pin write pending;
    // only a completed sender slice allows Through sampling at its boundary.
    // Earlier pin events are always drained before new source work. SourceStopped
    // exposes the unchanged scheduler frontier (e.g. unavailable patch RAM), not
    // invented continuation. DecodeError requires explicit receiver/stream reset.
    // Invalid argument rejection changes neither state nor output count; other
    // diagnostics can retain preceding committed work/bytes. This API allocates
    // nothing, and has no audio sample-rate or oversampling state.
    [[nodiscard]] static Result advanceTo(
        State&, const Sender::Configuration&, const Sender::Tables&,
        const FirmwareAssignerIo::Inputs&, std::span<const Sender::InputEvent>,
        std::uint64_t target, Output&) noexcept;
};

} // namespace youknow
