#pragma once

#include <array>
#include <cstdint>

namespace youknow::FirmwareAssignerIo {

// Original Roland service notes, printed pp. 6 and 11: IC8/IC9 select the
// keyboard/panel contact columns, IC11/12 invert the shared returns into PA,
// and PF6/7 select the two external 4052s feeding AN0..3.
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf
// Inputs are held physical-contact/ADC-code scenarios. They are not host patch
// values, MIDI notes, switch bounce, analog acquisition, or a cold-boot image.
struct Inputs {
    std::array<std::uint8_t, 8> keyboard {};
    // Normal foreground scans columns 2..7; columns 0/1 are not wired controls.
    std::array<std::uint8_t, 8> panel {};
    // PF group 0..3, then CR0..3. See panelParameterAddress below.
    std::array<std::uint8_t, 16> panelAdc {};
    // AN4: bender magnitude; AN5: LFO sensitivity; AN6: bender polarity;
    // AN7: LFO trigger (high means released). These remain explicit raw codes.
    std::array<std::uint8_t, 4> directAdc {0, 0, 0, 255};
    // PC3 sustain input and PC4 memory protect; released sustain is high.
    // Cassette PC5 is outside this normal-operation input contract.
    std::uint8_t pcInputBits = 0x08;
};

// Original A-5 ROM table 0030..003F, independently byte-checked against its ROM.
// This gives the patch-RAM destination, not an ADC voltage-to-code calibration.
// https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic1.txt
inline constexpr std::array<std::uint8_t, 16> panelParameterAddress {
    0x9c, 0x97, 0x94, 0x90, 0x9d, 0x9a, 0x95, 0x93,
    0x9e, 0x98, 0x96, 0x91, 0x9b, 0x99, 0x9f, 0x92
};

// NEC Stock500375, April 1987, printed 8-2/8-3: FR=0 at 12 MHz means 192
// original CPU timing states per conversion; scan mode converts four channels
// continuously and asserts INTAD after CR3, whether or not software accepts it.
// https://drive.google.com/file/d/0B44NKm9yPA1bNDFXZnFrdG1PdDA/view
inline constexpr std::uint16_t conversionStates = 192;

// The exact conversion substate of an ANM write is not specified by that source.
// These retain the established B-2 scenarios; they are not measured variants or
// proven exhaustive bounds on every possible internal race.
enum class AnmWritePhase : std::uint8_t {
    RestartConversion,
    PreserveConversionBoundary,
    PreserveCompleteScanPhase
};

struct Peripheral {
    std::uint8_t muxLatch = 0xff;
    std::uint8_t portF = 0;
    std::uint8_t portB = 0;
    std::uint8_t anm = 0x08;
    std::array<std::uint8_t, 4> conversion {};
    std::uint8_t channel = 0;
    // Zero denotes a due conversion. The scheduler must service that instant
    // before a same-time CPU access, including after an output-buffer retry.
    std::uint16_t statesUntilConversion = conversionStates;
    bool request = false;
};

struct Conversion {
    std::uint8_t channel = 0;
    // 0..15 panelAdc, 16..19 directAdc; useful for an independent event ledger.
    std::uint8_t inputIndex = 0;
    std::uint8_t value = 0;
    bool scanComplete = false;
};

constexpr bool valid(AnmWritePhase phase) noexcept {
    return phase == AnmWritePhase::RestartConversion
        || phase == AnmWritePhase::PreserveConversionBoundary
        || phase == AnmWritePhase::PreserveCompleteScanPhase;
}

constexpr bool valid(const Peripheral& state) noexcept {
    return (state.anm == 0 || state.anm == 0x08)
        && state.channel < 4 && state.statesUntilConversion <= conversionStates;
}

constexpr bool valid(const Inputs& inputs) noexcept {
    return (inputs.pcInputBits & ~0x18u) == 0
        && inputs.panel[0] == 0 && inputs.panel[1] == 0;
}

// IC8/IC9 G1 is tied high and G2B low. The active-low G2A enables are latch
// bits 4/5; bit 3 is NOT a decoder enable. Normal code selects only one bank.
// Simultaneously selecting both banks is outside the qualified contact model.
constexpr bool matrixSelectionSupported(std::uint8_t muxLatch) noexcept {
    return (muxLatch & 0x30u) != 0;
}

constexpr std::uint8_t readPa(const Inputs& inputs, std::uint8_t muxLatch) noexcept {
    const unsigned column = muxLatch & 7u;
    return static_cast<std::uint8_t>(((muxLatch & 0x10u) ? 0 : inputs.keyboard[column])
        | ((muxLatch & 0x20u) ? 0 : inputs.panel[column]));
}

constexpr std::uint8_t readPc(const Inputs& inputs, std::uint8_t outputLatch) noexcept {
    return static_cast<std::uint8_t>((outputLatch & ~0x18u) | (inputs.pcInputBits & 0x18u));
}

// Precondition: ANM is 00/08 and channel < 4, checked by the scheduler. Sampling
// the selected code at completion is an explicit model convention, not a claim
// about the internal sample aperture or the external 4052 settling response.
constexpr std::uint8_t analogFor(const Inputs& inputs, std::uint8_t portF,
                                 std::uint8_t anm, std::uint8_t channel) noexcept {
    return (anm & 0x08u) ? inputs.directAdc[channel]
        : inputs.panelAdc[static_cast<unsigned>(portF >> 6u) * 4u + channel];
}

// Pure peripheral transition. The scheduler owns absolute time, event capacity,
// masks, instruction completion, and request clearing on acceptance/SKIT. It
// must reserve its event slot before calling; failure here changes no state.
inline bool completeConversion(Peripheral& state, const Inputs& inputs,
                               Conversion& event) noexcept {
    if (!valid(state) || !valid(inputs) || state.statesUntilConversion != 0)
        return false;
    event.channel = state.channel;
    event.inputIndex = static_cast<std::uint8_t>((state.anm & 0x08u)
        ? 16u + state.channel : (state.portF >> 6u) * 4u + state.channel);
    event.value = analogFor(inputs, state.portF, state.anm, state.channel);
    state.conversion[state.channel] = event.value;
    state.channel = static_cast<std::uint8_t>((state.channel + 1u) & 3u);
    state.statesUntilConversion = conversionStates;
    event.scanComplete = state.channel == 0;
    if (event.scanComplete)
        state.request = true;
    return true;
}

inline bool writeAnm(Peripheral& state, std::uint8_t value, AnmWritePhase phase) noexcept {
    if (!valid(state) || !valid(phase) || (value != 0 && value != 0x08))
        return false;
    state.anm = value;
    if (phase != AnmWritePhase::PreserveCompleteScanPhase)
        state.channel = 0;
    if (phase == AnmWritePhase::RestartConversion)
        state.statesUntilConversion = conversionStates;
    return true;
}

} // namespace youknow::FirmwareAssignerIo
