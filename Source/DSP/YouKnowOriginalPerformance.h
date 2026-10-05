#pragma once
#include "YouKnowFirmwareAssignerAudioBridge.h"
#include <array>
#include <span>

namespace youknow {
struct EngineParameters;
class YouKnowEngine;

// Host events are complete messages. Their timestamps name the start of a
// nominal DIN frame; we serialize bytes at 31.25 kbaud and present RXB-ready
// at frame end. This is an explicit input convention, not measured host/DIN
// latency. A-5 then executes its real foreground/allocator and sends module
// pin levels to B-2; no fitted chord stagger or random delay is introduced.
// Original A-5 00A0/00A4, 0192..0217, 0A88..0B6F:
// https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic1.txt
class OriginalPerformance {
public:
    enum class ParameterInputSource { Panel, ToneRecall, MidiReflection };
    static constexpr std::size_t inputCapacity = 4096;
    void reset(const EngineParameters&) noexcept;
    [[nodiscard]] bool parameters(const EngineParameters&, std::uint64_t now,
        ParameterInputSource source = ParameterInputSource::Panel) noexcept;
    // Explicit native gestures, independent of the current MIDI tone mirror.
    // Bits 0..15 follow tone-byte order; bits 16..24 follow range, saw,
    // pulse, PWM, VCA, envelope polarity, HPF, chorus I and chorus II.
    void panelParameters(const EngineParameters&, std::uint32_t fields) noexcept;
    // Local keys use the actual 61-contact matrix, distinct from received DIN.
    // The source note retains its contact until release across host transpose
    // edits. False means the requested pitch needs the legacy DIN adapter.
    [[nodiscard]] bool keyboardNoteOn(int sourceNote, int midiPitch) noexcept;
    // Off returns the retained fallback wire pitch, -1 if no local owner,
    // or -2 when the native contact release was handled here.
    [[nodiscard]] int keyboardNoteOff(int sourceNote) noexcept;
    void clearKeyboardNotes() noexcept;
    [[nodiscard]] bool message(std::span<const std::uint8_t>, std::uint64_t now) noexcept;
    [[nodiscard]] bool advance(YouKnowEngine&, std::uint64_t target) noexcept;
    void keyMode(unsigned mode) noexcept;
    [[nodiscard]] const FirmwareAssignerAudioBridge::State& state() const noexcept { return state_; }
    [[nodiscard]] std::size_t pending() const noexcept { return count_; }
private:
    FirmwareAssignerAudioBridge::State state_ {};
    FirmwareAssignerIo::Inputs inputs_ {};
    FirmwareAssignerScheduler::Configuration configuration_ {};
    FirmwareAssignerScheduler::Tables tables_ {};
    std::array<FirmwareAssignerScheduler::InputEvent, inputCapacity> queue_ {};
    std::array<std::int8_t, inputCapacity> queueChannel_ {};
    std::array<std::uint8_t, 18> tone_ {};
    // -1 absent;0..60 physical contact;128..255 legacy DIN fallback pitch.
    std::array<std::int16_t, 128> keyboardContacts_ {};
    std::size_t head_ = 0, count_ = 0;
    std::uint64_t nextRx_ = 0;
    unsigned wantedMode_ = 0;
    bool modeContact_ = false, modeObserved_ = false;
    std::uint64_t modeObservedPass_ = 0;
    bool switchContact_ = false, switchRelease_ = false, switchObserved_ = false;
    bool switchTargetPending_ = false;
    std::uint8_t switchTarget_ = 0;
    std::uint8_t switchTargetMask_ = 0;
    std::uint64_t switchObservedPass_ = 0;
    void beginSwitchContact() noexcept;
    void serviceSwitchContact(bool scanned) noexcept;
    void queueSwitchTarget(std::uint8_t next, std::uint8_t mask) noexcept;
    void serviceModeContact(bool scanned) noexcept;
};
}
