// Independent B-2 quiet-board ADC/IRQ contract. Timing states are transcribed
// from original NEC NMOS7810/11 tables pp4-91/92/95/99/100, not inferred from
// this implementation. NEC Stock500375 (April1987) pp8-2/3,9-6/7/8,12-21
// supplies192states/conversion,16-state entry and one-instruction EI deferral.
// The manual explicitly covers the installed NMOS part (ppii,1-1/2).
// https://datasheets.chipdb.org/NEC/781x/uPD7810.pdf
// https://drive.google.com/file/d/0B44NKm9yPA1bNDFXZnFrdG1PdDA/view
// B-2 opcode/RAM source, immutable revision:
// https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt
// Tests bracket unmeasured ANM/bus-access substates; they do not promote any
// convention to measured hardware timing or change the shipping profile.
// Compile this TU standalone, without linking YouKnowDSP: the implementation
// include permits private PC/PSW instruction fixtures without test-only API.
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include "../Source/DSP/YouKnowFirmwareTrace.cpp"
namespace
{
using T = youknow::FirmwareControlTrace;
using A = youknow::FirmwareAdcTrace;
using K = A::EventKind;
std::map<std::string, unsigned> failureCounts;
int failures = 0;
unsigned assertions = 0, phaseRuns = 0, irqProbes = 0;
void check(bool yes, const char *message)
{
    ++assertions;
    if (!yes)
        ++failureCounts[message];
    if (!yes && failures++ < 24)
        std::cerr << message << '\n';
}
void word(T::State &s, unsigned off, unsigned v)
{
    s.ram[off] = v & 255;
    s.ram[off + 1] = (v >> 8) & 255;
}
T::Tables tables()
{
    T::Tables t;
    t.attack.fill(0x4000);
    for (unsigned i = 0; i < 128; ++i)
        t.portamento[i] = i;
    for (unsigned i = 0; i < 104; ++i)
    {
        t.pitchCv[i] = 32 + i * 12;
        t.pitchDivider[i] = 60000 - i * 500;
    }
    return t;
}
T::State base()
{
    T::State s;
    s.ram[0x37] = 6;
    s.ram[0x1e] = 0x40;
    for (unsigned i = 0; i < 6; ++i)
    {
        s.ram[9 + i] = 60;
        word(s, 0x71 + 2 * i, 60 * 256);
    }
    return s;
}
unsigned processRaw(unsigned raw)
{
    unsigned x = raw > 4 ? raw - 4 : 0;
    return x < 235 ? x : std::min(255u, 2 * x - 235);
}
// Test-only instruction stepping permits arbitrary initial PC/PSW states. The
// expected facts are from NEC/ROM, not a second copy of the IRQ algorithm.
struct Probe
{
    T::Tables table = tables();
    A::Inputs input{{132, 31, 0, 196, 42, 127, 8, 93}};
    A::Configuration config{};
    A::Result result{};
    youknow::Cpu cpu;
    Probe() : cpu(base(), table)
    {
        cpu.adc = &result;
        cpu.adcInputs = &input;
        cpu.adcConfiguration = &config;
        result.peripheral.conversion = {132, 31, 0, 196};
    }
    void step()
    {
        const auto &i = youknow::firmwareTraceDetail::program[youknow::addressMap[cpu.pc]];
        const unsigned d = cpu.skip ? i.skippedStates : i.states;
        const bool completion =
            config.accessBoundary == A::PeripheralAccessBoundary::InstructionCompletion;
        if (completion || cpu.skip)
            cpu.advanceAdc(d);
        if (cpu.skip)
        {
            cpu.result.states += d;
            cpu.pc += i.bytes;
            cpu.skip = false;
        }
        else
        {
            cpu.execute(i);
            if (!completion)
                cpu.advanceAdc(d);
            cpu.result.states += d;
        }
        if (cpu.returnEventPending)
        {
            cpu.adcEvent(K::Return, result.peripheral.anm);
            cpu.returnEventPending = false;
        }
        cpu.interruptBoundary();
    }
};
void hardwareProbe()
{
    for (bool completion : {false, true})
        for (unsigned psw = 0; psw < 4; ++psw)
        {
            Probe p;
            p.config.accessBoundary = completion
                                          ? A::PeripheralAccessBoundary::InstructionCompletion
                                          : A::PeripheralAccessBoundary::InstructionStart;
            p.cpu.pc = 0x777;
            p.cpu.carry = psw & 1;
            p.cpu.skip = psw & 2;
            p.cpu.a = 39;
            p.cpu.ea = 0xfedc;
            p.cpu.bc(0x1234);
            p.cpu.de(0x5678);
            p.cpu.hl(0x9abc);
            p.cpu.stackSize = 2;
            p.cpu.stack[0] = 0x456;
            p.cpu.stack[1] = 0x123;
            p.result.peripheral.request = true;
            p.cpu.interruptBoundary();
            check(p.cpu.pc == 0x20 && p.cpu.result.states == 16,
                  "automatic entry must cost16 states and vector0020");
            check(!p.cpu.skip && !p.result.peripheral.request &&
                      !p.result.peripheral.interruptsEnabled,
                  "accepted IRQ clears request/IE and isolates savedSK");
            for (unsigned guard = 0; p.cpu.inInterrupt && guard < 25; ++guard)
                p.step();
            check(!p.cpu.error && !p.cpu.inInterrupt && p.cpu.pc == 0x777,
                  "IRQ must return to exact interrupted PC");
            check(p.cpu.result.states == 188, "complete source vector+ISR must cost188states");
            check(p.cpu.carry == bool(psw & 1) && p.cpu.skip == bool(psw & 2),
                  "RETI must restore CY and SK for every combination");
            check(p.cpu.a == 39 && p.cpu.ea == 0xfedc && p.cpu.bc() == 0x1234 &&
                      p.cpu.de() == 0x5678 && p.cpu.hl() == 0x9abc && p.cpu.stackSize == 2 &&
                      p.cpu.stack[0] == 0x456 && p.cpu.stack[1] == 0x123,
                  "EXA/EXX and RETI preserve interrupted registers and call stack");
            constexpr std::array<unsigned, 5> expectedTime{81, 98, 115, 132, 149},
                expectedPc{0x7a, 0x7d, 0x80, 0x83, 0x86}, expectedData{0, 132, 31, 0, 196};
            unsigned count = 0;
            for (unsigned i = 0; i < p.cpu.result.count; ++i)
            {
                const auto &e = p.cpu.result.events[i];
                if (e.kind == T::EventKind::RamByte)
                {
                    check(count < 5, "ISR writes exactly five RAM bytes");
                    if (count < 5)
                    {
                        check(e.states == expectedTime[count] && e.address == expectedPc[count] &&
                                  e.card == 0x5c + count && e.value == expectedData[count],
                              "ISR RAM store differs from independent opcode/timing ledger");
                    }
                    ++count;
                }
            }
            check(count == 5 && p.result.peripheral.anm == 8 && p.result.peripheral.mkh == 5,
                  "old bank copied, next bank selected, ADC masked");
            ++irqProbes;
        }
    // Mask/request behavior independent of the long main loop.
    {
        Probe p;
        p.result.peripheral.mkh = 5;
        p.result.peripheral.request = true;
        p.cpu.interruptBoundary();
        check(!p.cpu.inInterrupt && p.result.peripheral.request,
              "mask inhibits acceptance without clearing FAD");
        p.cpu.pc = 0x6b3;
        p.step();
        check(!p.result.peripheral.interruptsEnabled, "DI disables maskable IRQs");
        p.step();
        check(!p.result.peripheral.request && p.cpu.skip, "SKIT tests and clears pending FAD");
        p.step();
        check(p.cpu.pc == 0x6b7 && !p.cpu.skip, "SKIT skips exactly following NOP");
        p.step();
        p.result.peripheral.request = true;
        p.step();
        check(p.cpu.pc == 0x6bb && !p.cpu.inInterrupt, "EI cannot accept until next instruction");
        p.step();
        check(p.cpu.inInterrupt && p.cpu.interruptPc == 0x737,
              "EI following JRE completes before acceptance");
    }
    // Accepted interrupt after an instruction setting SK cannot consume the skip.
    {
        Probe p;
        p.cpu.pc = 0x707;
        p.cpu.a = 5;
        p.cpu.c = 1;
        p.result.peripheral.request = true;
        p.step();
        check(p.cpu.inInterrupt && p.cpu.interruptSkip,
              "conditional-skip flag saved at actual instruction boundary");
        for (unsigned n = 0; p.cpu.inInterrupt && n < 25; ++n)
            p.step();
        check(p.cpu.pc == 0x709 && p.cpu.skip, "RETI restores still-pending mainline skip");
        p.step();
        check(p.cpu.pc == 0x70a && p.cpu.a == 5,
              "restored SK skips original MOV A,C, not vector instruction");
    }
}
void callAndEntryCases()
{
    // Source CALF at0749 pushes074B then executes loadDac. Interrupt withSK set
    // inside that helper must return to the helper and later to the main caller.
    {
        Probe p;
        p.cpu.pc = 0x749;
        p.cpu.a = 5;
        p.cpu.ea = 0x1234;
        p.step();
        check(p.cpu.pc == 0x82f && p.cpu.stackSize == 1 && p.cpu.stack[0] == 0x74b,
              "real CALF helper fixture establishes return address");
        p.cpu.skip = true;
        p.cpu.carry = true;
        p.result.peripheral.request = true;
        p.cpu.interruptBoundary();
        for (unsigned n = 0; p.cpu.inInterrupt && n < 25; ++n)
            p.step();
        check(!p.cpu.error && p.cpu.pc == 0x82f && p.cpu.stackSize == 1 && p.cpu.skip &&
                  p.cpu.carry,
              "ADC within CALF restores pendingSK and call frame");
        p.step();
        check(p.cpu.pc == 0x832 && !p.cpu.skip,
              "restoredSK consumed by original helper instruction");
        for (unsigned n = 0; p.cpu.pc != 0x74b && n < 12; ++n)
            p.step();
        check(!p.cpu.error && p.cpu.pc == 0x74b && p.cpu.stackSize == 0,
              "loadDac returns to exact caller after ADC");
    }
    auto t = tables();
    A::Inputs in{{132, 31, 0, 196, 42, 127, 8, 93}};
    for (unsigned deferred = 0; deferred < 2; ++deferred)
    {
        A::State s;
        s.control = base();
        s.peripheral.request = true;
        s.peripheral.eiDeferred = deferred;
        s.peripheral.conversion = {132, 31, 0, 196};
        auto r = A::run(s, t, in, {});
        check(r.control.valid && r.peripheralValid,
              "initial pending IRQ still runs a complete23-converter pass");
        auto first = std::find_if(r.events.begin(), r.events.begin() + r.count,
                                  [](const auto &e) { return e.kind == K::Acceptance; });
        check(first != r.events.begin() + r.count, "initial pending request accepted");
        if (first != r.events.begin() + r.count)
            check(first->states == (deferred ? 10u : 0u) && first->pc == (deferred ? 0x2ee : 0x2ec),
                  "entry boundary does not consume deferredEI; eligibleIRQ precedes02EC");
    }
    // Equal original bank inputs let a fixed reference consume exactly the same
    // data while injected ISR timing alone may split the NOISE helper.
    for (unsigned phase = 0; phase < 768; ++phase)
    {
        A::State s;
        s.control = base();
        s.peripheral.channel = phase / 192;
        s.peripheral.statesUntilConversion = 192 - phase % 192;
        s.peripheral.conversion = {132, 31, 0, 196};
        for (unsigned j = 0; j < 4; ++j)
        {
            s.control.ram[0x5d + j] = in.raw[j];
            s.control.ram[0x80 + j] = in.raw[j];
            s.control.ram[0x88 + j] = processRaw(in.raw[j]);
        }
        auto nominal = T::run(s.control, t);
        auto adc = A::run(s, t, in, {});
        std::array<unsigned, 23> a{}, b{};
        unsigned na = 0, nb = 0;
        for (unsigned j = 0; j < nominal.count; ++j)
            if (nominal.events[j].kind == T::EventKind::Converter)
                a[na++] = nominal.events[j].value;
        for (unsigned j = 0; j < adc.control.count; ++j)
            if (adc.control.events[j].kind == T::EventKind::Converter)
                b[nb++] = adc.control.events[j].value;
        check(na == 23 && nb == 23 && a == b,
              "compatible held bank preserves all23 converter codes");
    }
}
void standaloneAdcClock()
{
    for (unsigned bank : {0u, 8u})
        for (unsigned phase = 0; phase < 768; ++phase)
        {
            Probe p;
            p.result.peripheral.anm = bank;
            p.config.interrupts = false;
            p.result.peripheral.channel = phase / 192;
            p.result.peripheral.statesUntilConversion = 192 - phase % 192;
            p.cpu.advanceAdc(1536);
            check(p.result.peripheral.channel == phase / 192 &&
                      p.result.peripheral.statesUntilConversion == 192 - phase % 192,
                  "free ADC phase preserved over two scan periods");
            unsigned conversions = 0, requests = 0;
            for (unsigned j = 0; j < p.result.count; ++j)
            {
                const auto &e = p.result.events[j];
                if (e.kind == K::Conversion)
                {
                    check(e.states == 192 - phase % 192 + 192 * conversions,
                          "independent192state converter cadence");
                    check(e.channel == (bank ? 4u : 0u) + (phase / 192 + conversions) % 4 &&
                              e.value == p.input.raw[e.channel],
                          "sequential converter channel/rawvalue");
                    ++conversions;
                }
                if (e.kind == K::Request)
                {
                    check(e.states == 768 - phase + 768 * requests,
                          "FAD generated only after allfour results");
                    ++requests;
                }
            }
            check(conversions == 8 && requests == 2 && p.result.peripheral.request,
                  "masked/disabled ADC still runs and raises sticky FAD");
        }
    // Completion exactly at SKIT is bracketed explicitly by bus access policy.
    for (bool completion : {false, true})
    {
        Probe p;
        p.config.accessBoundary = completion ? A::PeripheralAccessBoundary::InstructionCompletion
                                             : A::PeripheralAccessBoundary::InstructionStart;
        p.cpu.pc = 0x6b4;
        p.result.peripheral.interruptsEnabled = false;
        p.result.peripheral.channel = 3;
        p.result.peripheral.statesUntilConversion = 8;
        p.step();
        check(p.cpu.skip == completion && p.result.peripheral.request != completion,
              "SKIT/ADC coincident edge follows explicit start/completion policy");
    }
}
void completePhaseSweep()
{
    const auto t = tables();
    A::Inputs input{{132, 31, 0, 196, 42, 127, 8, 93}};
    unsigned leastIrqs = 99, mostIrqs = 0, minStates = ~0u, maxStates = 0, extendedNoiseGaps = 0;
    for (auto policy :
         {A::AnmWritePhase::RestartConversion, A::AnmWritePhase::PreserveConversionBoundary,
          A::AnmWritePhase::PreserveCompleteScanPhase})
        for (auto access : {A::PeripheralAccessBoundary::InstructionStart,
                            A::PeripheralAccessBoundary::InstructionCompletion})
            for (unsigned initialBank : {0u, 8u})
                for (unsigned phase = 0; phase < 768; ++phase)
                {
                    A::Configuration cfg;
                    cfg.anmWritePhase = policy;
                    cfg.accessBoundary = access;
                    A::State s;
                    s.control = base();
                    s.control.ram[0x5c] = initialBank;
                    s.peripheral.anm = initialBank;
                    s.peripheral.channel = phase / 192;
                    s.peripheral.statesUntilConversion = 192 - phase % 192;
                    for (unsigned j = 0; j < 4; ++j)
                        s.peripheral.conversion[j] = input.raw[(initialBank ? 4 : 0) + j];
                    for (unsigned bank = 0; bank < 2; ++bank)
                        for (unsigned j = 0; j < 4; ++j)
                        {
                            s.control.ram[0x80 + bank * 4 + j] = input.raw[bank * 4 + j];
                            s.control.ram[0x88 + bank * 4 + j] =
                                processRaw(input.raw[bank * 4 + j]);
                        }
                    for (unsigned j = 0; j < 4; ++j)
                        s.control.ram[0x5d + j] = input.raw[(initialBank ? 4 : 0) + j];
                    unsigned previousBank = initialBank ^ 8, totalAccepts = 0;
                    for (unsigned pass = 0; pass < 4; ++pass)
                    {
                        const auto start = s.peripheral.elapsedStates;
                        auto r = A::run(s, t, input, cfg);
                        check(r.control.valid && r.peripheralValid,
                              "all phase/policy passes must terminate valid");
                        check(r.peripheral.elapsedStates - start == r.control.states,
                              "peripheral advances during every CPUstate inclIRQ");
                        leastIrqs = std::min(leastIrqs, r.acceptedInterrupts);
                        mostIrqs = std::max(mostIrqs, r.acceptedInterrupts);
                        minStates = std::min(minStates, r.control.states);
                        maxStates = std::max(maxStates, r.control.states);
                        std::array<std::uint64_t, 4> accepts{};
                        std::array<unsigned, 4> banks{};
                        unsigned na = 0, nr = 0;
                        for (unsigned e = 0; e < r.count; ++e)
                        {
                            const auto &v = r.events[e];
                            if (v.kind == K::Acceptance)
                            {
                                check(!((v.pc >= 0x465 && v.pc <= 0x471) || v.pc == 0x4c8 ||
                                        v.pc == 0x4c9),
                                      "no ADC acceptance inside protected PIT reset/count stores");
                                check(na < 4, "bounded interrupts perpass");
                                if (na < 4)
                                {
                                    accepts[na] = v.states;
                                    banks[na] = v.value;
                                }
                                check(v.value == (previousBank ^ 8),
                                      "accepted ADC banks must alternate persistently");
                                previousBank = v.value;
                                ++na;
                                ++totalAccepts;
                            }
                            if (v.kind == K::Return)
                            {
                                check(nr < na && v.states - accepts[nr] == 188,
                                      "run API IRQ duration matches independent188state ledger");
                                ++nr;
                            }
                        }
                        check(na == nr && na == r.acceptedInterrupts,
                              "all ADC entries have one full return");
                        for (unsigned j = 0; j < na; ++j)
                        {
                            unsigned found = 0;
                            for (unsigned e = 0; e < r.control.count; ++e)
                            {
                                const auto &v = r.control.events[e];
                                if (v.kind != T::EventKind::RamByte || v.address < 0x7a ||
                                    v.address > 0x86)
                                    continue;
                                const auto abs = start + v.states;
                                if (abs < accepts[j] || abs > accepts[j] + 188)
                                    continue;
                                const unsigned k = v.card - 0x5c;
                                constexpr std::array<unsigned, 5> offs{81, 98, 115, 132, 149};
                                check(k < 5, "ISR RAM destination range");
                                if (k < 5)
                                {
                                    check(abs - accepts[j] == offs[k],
                                          "phase sweep ISR fixed store offsets");
                                    const unsigned expected =
                                        k ? input.raw[(banks[j] ? 4 : 0) + k - 1] : banks[j];
                                    check(v.value == expected,
                                          "old-bank tag and CR payload must agree");
                                }
                                ++found;
                            }
                            check(found == 5, "phase sweep ISR writes allfive bytes");
                        }
                        unsigned cv = 0, inh = 0;
                        std::array<unsigned, 23> inhibits{};
                        for (unsigned e = 0; e < r.control.count; ++e)
                        {
                            const auto &v = r.control.events[e];
                            if (v.kind == T::EventKind::Inhibit && inh < 23)
                                inhibits[inh++] = v.states;
                            if (v.kind == T::EventKind::Converter)
                            {
                                check(cv < 23 && v.card == cv,
                                      "23 ordered physical converter events retained");
                                if (cv < 23 && inh > cv)
                                {
                                    unsigned gap = v.states - inhibits[cv];
                                    check(gap >= 75 && (gap - 75) % 188 == 0,
                                          "converter aperture extends only by whole accepted ISR");
                                    if (cv == 22 && gap > 75)
                                        ++extendedNoiseGaps;
                                }
                                ++cv;
                            }
                        }
                        check(cv == 23 && inh == 23,
                              "exact complete converter/inhibit train count");
                        s.control = r.control.finalState;
                        s.peripheral = r.peripheral;
                        ++phaseRuns;
                    }
                    check(totalAccepts >= 4 && totalAccepts <= 5,
                          "one accepted service perrearm plus initialphase service");
                }
    std::cout << "phase_passes=" << phaseRuns << " interrupt_count_per_pass=" << leastIrqs << ".."
              << mostIrqs << " duration_states=" << minStates << ".." << maxStates
              << " extended_noise_gaps=" << extendedNoiseGaps << '\n';
    check(extendedNoiseGaps > 0, "phase sweep exercises ADC extension inside NOISE loadDac");
}
void invalidInputCases()
{
    const auto t = tables();
    const A::Inputs in{{132, 31, 0, 196, 42, 127, 8, 93}};
    A::State baseState;
    baseState.control = base();
    const auto reject = [&](const A::State &state, const A::Configuration &cfg)
    {
        auto r = A::run(state, t, in, cfg);
        check(!r.peripheralValid && !r.control.valid,
              "invalid peripheral/config rejected without validcontroltrace");
    };
    for (unsigned value : {0u, 193u})
    {
        auto s = baseState;
        s.peripheral.statesUntilConversion = value;
        reject(s, {});
    }
    {
        auto s = baseState;
        s.peripheral.channel = 4;
        reject(s, {});
    }
    for (unsigned value : {1u, 4u, 16u, 255u})
    {
        auto s = baseState;
        s.peripheral.anm = value;
        reject(s, {});
    }
    for (unsigned value : {0u, 1u, 6u, 255u})
    {
        auto s = baseState;
        s.peripheral.mkh = value;
        reject(s, {});
    }
    {
        auto s = baseState;
        s.peripheral.eiDeferred = 2;
        reject(s, {});
    }
    {
        A::Configuration c;
        c.anmWritePhase = static_cast<A::AnmWritePhase>(3);
        reject(baseState, c);
    }
    {
        A::Configuration c;
        c.accessBoundary = static_cast<A::PeripheralAccessBoundary>(2);
        reject(baseState, c);
    }
    for (unsigned mask : {4u, 5u})
    {
        auto s = baseState;
        s.peripheral.mkh = mask;
        s.peripheral.request = true;
        auto r = A::run(s, t, in, {});
        check(r.peripheralValid && r.control.valid, "supported B-2 MKH4/5 startup accepted");
    }
}
void hysteresisReference()
{
    auto t = tables();
    for (unsigned raw = 0; raw < 256; ++raw)
        for (unsigned previous : {0u, 1u, 4u, 5u, raw ? raw - 1 : 0u, raw, std::min(255u, raw + 1),
                                  239u, 240u, 249u, 255u})
            for (unsigned bank = 0; bank < 2; ++bank)
            {
                auto s = base();
                s.ram[0x5c] = bank * 8;
                for (unsigned j = 0; j < 4; ++j)
                {
                    s.ram[0x5d + j] = raw;
                    s.ram[0x80 + bank * 4 + j] = previous;
                }
                auto r = T::run(s, t);
                check(r.valid, "hysteresis witness pass valid");
                const unsigned last = std::abs(int(raw) - int(previous)) > 1 ? raw : previous;
                for (unsigned j = 0; j < 4; ++j)
                {
                    check(r.finalState.ram[0x80 + bank * 4 + j] == last,
                          "original raw-byte deadband independentoracle");
                    check(r.finalState.ram[0x88 + bank * 4 + j] == processRaw(last),
                          "original endpoint stretch independentoracle");
                }
            }
}
}
int main()
{
    hardwareProbe();
    callAndEntryCases();
    standaloneAdcClock();
    completePhaseSweep();
    invalidInputCases();
    hysteresisReference();
    std::cout << "direct_irq_probes=" << irqProbes << " assertions=" << assertions
              << " failures=" << failures << '\n';
    for (const auto &[text, n] : failureCounts)
        std::cerr << n << " x " << text << '\n';
    return failures ? 1 : 0;
}
