// Primary-semantic A-5 input tests: Roland service printed6/11 gives matrix
// enables and PF6/7 analog mux; NEC Stock500375 printed8-2/8-3 gives192-state
// conversions and continuous four-channel scan. No raw ROM or data files needed.
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf
// https://drive.google.com/file/d/0B44NKm9yPA1bNDFXZnFrdG1PdDA/view
// The caller supplies an independent hardware-state clock. Completion sampling
// and each ANM restart choice are declared scenarios, not aperture measurements.
#include "../Source/DSP/YouKnowFirmwareAssignerIo.h"

#include <cstdlib>
#include <iostream>

namespace Io = youknow::FirmwareAssignerIo;
unsigned assertions = 0;
bool same(const Io::Peripheral& a, const Io::Peripheral& b) {
    return a.muxLatch == b.muxLatch && a.portF == b.portF && a.portB == b.portB
        && a.anm == b.anm && a.conversion == b.conversion && a.channel == b.channel
        && a.statesUntilConversion == b.statesUntilConversion && a.request == b.request;
}
void check(bool condition, const char* message) {
    ++assertions;
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

int main() {
    Io::Inputs inputs;
    for (unsigned column = 0; column < 8; ++column) {
        inputs.keyboard[column] = static_cast<std::uint8_t>(1u << column);
        inputs.panel[column] = column >= 2 ? static_cast<std::uint8_t>(0x81u ^ column) : 0;
    }
    for (unsigned column = 0; column < 8; ++column) {
        check(Io::readPa(inputs, 0xe8 + column) == (1u << column), "keyboard scan route");
        check(Io::readPa(inputs, 0xe0 + column) == (1u << column), "bit3 is not an enable");
        check(Io::readPa(inputs, 0xf8 + column) == 0, "both banks inhibited");
        check(Io::matrixSelectionSupported(0xe8 + column), "keyboard selection supported");
        check(Io::matrixSelectionSupported(0xd8 + column), "panel selection supported");
        check(!Io::matrixSelectionSupported(0xc8 + column), "dual bank is not qualified");
    }
    for (unsigned column = 2; column < 8; ++column)
        check(Io::readPa(inputs, 0xd8 + column) == (0x81u ^ column), "panel scan route");
    for (unsigned latch = 0; latch < 256; ++latch) {
        for (unsigned physical = 0; physical < 4; ++physical) {
            inputs.pcInputBits = static_cast<std::uint8_t>(physical << 3);
            const auto result = Io::readPc(inputs, latch);
            check((result & 0x18) == (physical << 3), "PC physical input levels");
            check((result & 0xe7) == (latch & 0xe7), "PC unrelated latch bits preserved");
        }
    }
    inputs.pcInputBits = 8;
    for (unsigned index = 0; index < 16; ++index)
        inputs.panelAdc[index] = static_cast<std::uint8_t>(0x40 + index);
    inputs.directAdc = {0xa0, 0xb1, 0xc2, 0xd3};
    for (unsigned pf = 0; pf < 256; ++pf) {
        for (unsigned channel = 0; channel < 4; ++channel) {
            check(Io::analogFor(inputs, pf, 0, channel) == (0x40 + (pf / 64) * 4 + channel),
                  "external mux bank follows PF6/7");
            check(Io::analogFor(inputs, pf, 8, channel) == inputs.directAdc[channel],
                  "direct ADC ignores external mux");
        }
    }
    for (unsigned countdown = 0; countdown <= 192; ++countdown) {
        for (unsigned channel = 0; channel < 4; ++channel) {
            for (auto phase : {Io::AnmWritePhase::RestartConversion,
                               Io::AnmWritePhase::PreserveConversionBoundary,
                               Io::AnmWritePhase::PreserveCompleteScanPhase}) {
                Io::Peripheral state;
                state.channel = channel;
                state.statesUntilConversion = countdown;
                state.conversion = {1, 2, 3, 4};
                state.request = true;
                check(Io::writeAnm(state, 0, phase), "valid ANM write");
                check(state.channel == (phase == Io::AnmWritePhase::PreserveCompleteScanPhase
                    ? channel : 0), "declared ANM scan-index policy");
                check(state.statesUntilConversion == (phase == Io::AnmWritePhase::RestartConversion
                    ? 192 : countdown), "declared ANM conversion-phase policy");
                check(state.conversion == std::array<std::uint8_t, 4>{1, 2, 3, 4}
                    && state.request, "ANM retains prior CR values and sticky request");
            }
        }
    }
    Io::Peripheral state;
    state.anm = 0;
    for (unsigned conversion = 0; conversion < 40; ++conversion) {
        state.portF = static_cast<std::uint8_t>((conversion / 4 % 4) << 6);
        state.statesUntilConversion = 0;
        Io::Conversion event;
        check(Io::completeConversion(state, inputs, event), "due conversion commits");
        check(event.channel == conversion % 4, "CR round robin");
        check(event.value == 0x40 + conversion % 16, "completion observes current PF");
        check(event.inputIndex == conversion % 16, "source ledger identifies actual input");
        check(event.scanComplete == (conversion % 4 == 3), "request after fourth result");
        check(state.statesUntilConversion == 192, "conversion duration is hardware192");
        if (conversion >= 3)
            check(state.request, "masked request remains sticky while results update");
    }
    auto before = state;
    check(!Io::writeAnm(state, 0x10, Io::AnmWritePhase::RestartConversion), "unsupported ANM rejects");
    check(!Io::writeAnm(state, 0, static_cast<Io::AnmWritePhase>(99)), "invalid phase rejects");
    check(state.anm == before.anm && state.channel == before.channel
        && state.statesUntilConversion == before.statesUntilConversion, "invalid write is atomic");
    Io::Conversion event;
    check(!Io::completeConversion(state, inputs, event), "not-yet-due conversion rejects");
    for (unsigned invalid = 0; invalid < 5; ++invalid) {
        Io::Peripheral bad;
        auto badInputs = inputs;
        bad.statesUntilConversion = 0;
        if (invalid == 0) bad.anm = 0x10;
        if (invalid == 1) bad.channel = 4;
        if (invalid == 2) bad.statesUntilConversion = 193;
        if (invalid == 3) badInputs.pcInputBits = 0x20;
        if (invalid == 4) badInputs.panel[0] = 1;
        const auto snapshot = bad;
        Io::Conversion rejectedEvent {3, 19, 177, true};
        check(!Io::completeConversion(bad, badInputs, rejectedEvent), "invalid conversion rejects");
        check(same(bad, snapshot), "invalid conversion does not mutate peripheral");
        check(rejectedEvent.channel == 3 && rejectedEvent.inputIndex == 19
            && rejectedEvent.value == 177 && rejectedEvent.scanComplete,
            "invalid conversion does not mutate event");
        if (invalid < 3) {
            check(!Io::writeAnm(bad, 0, Io::AnmWritePhase::RestartConversion),
                  "ANM write rejects invalid incoming peripheral");
            check(same(bad, snapshot), "rejected ANM leaves every field unchanged");
        }
    }
    // An independently timed caller owns the countdown. A conversion every192
    // states and a request every768 states must continue when FAD is uncleared.
    // Vary initial phase across all192 states, rather than choosing a quiet one.
    for (unsigned initial = 1; initial <= 192; ++initial) {
        Io::Peripheral continuous;
        continuous.statesUntilConversion = initial;
        unsigned conversions = 0;
        for (unsigned elapsed = 1; elapsed <= 1600; ++elapsed) {
            --continuous.statesUntilConversion;
            if (continuous.statesUntilConversion != 0)
                continue;
            Io::Conversion completed;
            check(Io::completeConversion(continuous, inputs, completed), "continuous completion");
            check(elapsed == initial + conversions * 192, "independent ADC deadline");
            check(completed.channel == conversions % 4, "continuous CR order");
            ++conversions;
            check(completed.scanComplete == (conversions % 4 == 0), "four-result INTAD cadence");
            check(continuous.request == (conversions >= 4), "continuous sticky FAD with no acceptance");
        }
    }
    std::cout << "PASS " << assertions << " I/O mapping and peripheral-transition assertions\n";
}
