// Independent B-2 serial/ADC/main-loop contract; synthetic coefficient tables.
// No firmware image, external listing, private oracle or downloaded input is
// needed to run this test. Expected branches and completion timestamps were
// independently derived from the original B-2 bytes and NEC NMOS tables.
// Original NEC uPD7810/11 shortsheet pp4-91..4-100; Stock500375 (April1987)
// pp7-1..7-6,9-5..9-7,12-21: RXB/FSR semantics,192state ADC conversions,
// 16state automatic IRQ entry, original instruction timing/borrow flags and
// EI deferral through one following instruction. Three12MHz clocks/state.
// https://datasheets.chipdb.org/NEC/781x/uPD7810.pdf
// https://drive.google.com/file/d/0B44NKm9yPA1bNDFXZnFrdG1PdDA/view
// https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt
// The listing's inverted-mask label0008 is a known typo: raw B-2 masks are
//000A..000F (0008/09=EI/RETI); SUI76 is correct. Maintained audit tooling can
// verify the external B-2 dump identity separately; no ROM data lives here.
// ByteReady starts at completed RXB transfer, not a UART falling edge or host
// MIDI arrival. Initial phases and access/ANM conventions remain explicit.
// The burst matrix is1280 offsets x3 ANM x2 access with correlated ADC phases,
// not a Cartesian proof of all CPU/ADC/UART phases. Unsupported payloads and
// receive collisions must fail explicitly. This does not select a product
// profile, emulate A-5 scheduling or claim the RX stop-latch subcycle is known.
// An optional YOUKNOW_SERIAL_LEDGER path exports the nominal synthetic scene;
// default test runs create no files.
#include "../Source/DSP/YouKnowFirmwareSerialTrace.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <span>
#include <string>
#include <tuple>
#include <vector>
using S = youknow::FirmwareSerialTrace;
using T = youknow::FirmwareControlTrace;
using A = youknow::FirmwareAdcTrace;
using K = S::EventKind;
unsigned assertions = 0, failures = 0, phaseRuns = 0;
std::map<std::string, unsigned> failureKinds;
void check(bool b, const char *m)
{
    ++assertions;
    if (!b)
    {
        ++failureKinds[m];
        if (failures++ < 16)
            std::cerr << m << '\n';
    }
}
T::Tables tables()
{
    T::Tables t;
    t.attack.fill(0x4000);
    for (unsigned i = 0; i < 128; ++i)
        t.portamento[i] = i;
    for (unsigned i = 0; i < 104; ++i)
    {
        t.pitchCv[i] = 32 + 12 * i;
        t.pitchDivider[i] = 60000 - 500 * i;
    }
    return t;
}
void word(T::State &s, unsigned a, unsigned v)
{
    s.ram[a] = v & 255;
    s.ram[a + 1] = v >> 8;
}
S::State base()
{
    S::State s;
    s.control.ram[0x37] = 6;
    s.control.ram[0x1e] = 0x40;
    for (unsigned i = 0; i < 6; ++i)
    {
        s.control.ram[9 + i] = 60;
        word(s.control, 0x71 + 2 * i, 60 * 256);
    }
    return s;
}
const T::Tables table = tables();
const A::Inputs inputs{{132, 31, 0, 196, 42, 127, 8, 93}};
using E = std::tuple<unsigned, std::uint64_t, unsigned, unsigned, unsigned, unsigned, unsigned,
                     unsigned, unsigned, unsigned>;
E normalized(const S::Event &e)
{
    return {unsigned(e.kind), e.states,    e.pc, e.address, e.value,
            e.card,           e.phaseBits, e.pa, e.pb,      e.portc};
}
struct Run
{
    S::State state;
    S::Status status = S::Status::ReachedTarget;
    std::vector<S::Event> events;
    std::size_t consumed = 0;
};
Run advance(S::State s, std::span<const S::ByteReady> bytes, std::uint64_t target,
            S::Configuration cfg = {}, unsigned chunk = 0, unsigned drainAt = 0)
{
    Run out;
    out.state = s;
    S::Events e;
    unsigned guard = 0;
    for (;;)
    {
        auto end = chunk ? std::min(target, out.state.now + chunk) : target;
        if (drainAt)
            e.count = drainAt;
        auto start = e.count;
        auto r = S::advanceTo(out.state, table, inputs, cfg, bytes.subspan(out.consumed), end, e);
        out.consumed += r.consumedBytes;
        out.status = r.status;
        out.events.insert(out.events.end(), e.entries.begin() + start, e.entries.begin() + e.count);
        e.count = 0;
        if (r.status != S::Status::ReachedTarget && r.status != S::Status::OutputFull &&
            r.status != S::Status::InstructionBudget)
            break;
        if (r.status == S::Status::ReachedTarget && out.state.now == target)
            break;
        if (++guard > 2000000)
        {
            check(false, "bounded progress");
            break;
        }
    }
    return out;
}
std::vector<E> normalized(const std::vector<S::Event> &v)
{
    std::vector<E> o;
    for (const auto &e : v)
        o.push_back(normalized(e));
    return o;
}
std::vector<S::Event> select(const Run &r, K k)
{
    std::vector<S::Event> v;
    for (const auto &e : r.events)
        if (e.kind == k)
            v.push_back(e);
    return v;
}
void checkWrite(const Run &r, unsigned address, unsigned value, unsigned time)
{
    auto rows = select(r, K::RamByte);
    check(std::any_of(rows.begin(), rows.end(), [&](const auto &e)
                      { return e.address == address && e.value == value && e.states == time; }),
          "independent RAM write ledger");
}
void pathLedger()
{
    S::Configuration c;
    c.adc.interrupts = false;
    for (unsigned mode = 0; mode < 2; ++mode)
    {
        c.adc.accessBoundary = static_cast<A::PeripheralAccessBoundary>(mode);
        for (unsigned card = 0; card < 6; ++card)
        {
            unsigned bit = 1u << card;
            for (unsigned hold = 0; hold < 2; ++hold)
            {
                S::State s;
                s.control.ram[0x10] = s.control.ram[0x33] = 63;
                s.control.ram[0x1e] = hold;
                std::array<S::ByteReady, 1> b{{{0, static_cast<std::uint8_t>(0x80 + card)}}};
                unsigned total = hold ? 189 : 227;
                auto r = advance(s, b, total, c);
                check(r.status == S::Status::ReachedTarget, "off path target");
                check(r.state.registers.pc == 0x2ec && r.state.registers.sp == 0xffff,
                      "off destructive restart");
                checkWrite(r, 0xff36, 0x80 + card, 68);
                checkWrite(r, 0xff10, 63 ^ bit, 142);
                if (!hold)
                    checkWrite(r, 0xff33, 63 ^ bit, 193);
                check(r.state.control.ram[0x33] == (hold ? 63 : 63 ^ bit), "off HOLD masks");
                check(select(r, K::InterruptReturn).empty(), "restart has no synthetic RETI");
                auto read = select(r, K::SerialRead);
                check(read.size() == 1 && read[0].states == (mode ? 44 : 34),
                      "read access bracket34/44");
            }
            for (unsigned same = 0; same < 2; ++same)
                for (unsigned running = 0; running < 2; ++running)
                    for (unsigned phase = 0; phase < 2; ++phase)
                    {
                        S::State s;
                        s.control.ram[0x36] = 0x88 + card;
                        s.control.ram[9 + card] = same ? 60 : 59;
                        s.control.ram[0x11] = running * bit;
                        s.control.ram[0x33] = phase * bit;
                        unsigned total = !same ? (!running ? 358
                                                  : phase  ? 377
                                                           : 342)
                                               : (!running ? 307
                                                  : phase  ? 372
                                                           : 337);
                        std::array<S::ByteReady, 1> b{{{0, 60}}};
                        auto r = advance(s, b, total, c);
                        check(r.status == S::Status::ReachedTarget && r.state.registers.pc == 0x2ec,
                              "on independent path duration");
                        checkWrite(r, 0xff10, bit, 177);
                        checkWrite(r, 0xff07, bit, 205);
                        if (!same)
                            checkWrite(r, 0xff09 + card, 60, 272);
                        check(r.state.control.ram[0] == (!same && !running ? bit : 0),
                              "same pitch reset policy");
                        check(r.state.control.ram[0x33] == (running ? 0 : phase * bit),
                              "on release phase policy");
                        check(select(r, K::StackReset).size() == 1 &&
                                  select(r, K::InterruptReturn).empty(),
                              "on stack abandonment");
                    }
        }
        for (unsigned byte : {0x86u, 0x87u, 0x88u, 0x8eu, 0xa3u, 0xa4u, 0xffu})
        {
            S::State s;
            s.control.ram[0x1e] = 1;
            unsigned total = byte == 0x86 ? 164 : byte == 0x87 ? 170 : 110;
            std::array<S::ByteReady, 1> b{{{0, static_cast<std::uint8_t>(byte)}}};
            auto r = advance(s, b, total, c);
            check(r.status == S::Status::ReachedTarget, "command target");
            auto ret = select(r, K::InterruptReturn);
            check(ret.size() == 1 && ret[0].states == total, "command independent RETI ledger");
            checkWrite(r, 0xff36, byte, 68);
            if (byte == 0x86 || byte == 0x87)
                checkWrite(r, 0xff1e, byte == 0x86 ? 1 : 0, byte == 0x86 ? 129 : 135);
        }
        for (unsigned command = 0x8e; command <= 0xa3; ++command)
        {
            S::State s;
            s.control.ram[0x36] = command;
            std::array<S::ByteReady, 1> b{{{0, 63}}};
            auto r = advance(s, b, 450, c);
            check(r.status == S::Status::UnsupportedPath && r.state.registers.pc == 0xd7,
                  "parameter path fails explicitly at unimplemented TABLE");
        }
    }
}
void ignoredAndCarry()
{
    S::Configuration c;
    c.adc.interrupts = false;
    for (unsigned command : {0x80u, 0x81u, 0x82u, 0x83u, 0x84u, 0x85u, 0x86u, 0x87u, 0xa4u})
        for (unsigned value : {0u, 1u, 63u, 127u})
        {
            S::State s;
            s.control.ram[0x36] = command;
            std::array<S::ByteReady, 1> b{{{0, static_cast<std::uint8_t>(value)}}};
            unsigned total = command < 0x88 ? 117 : 131;
            auto r = advance(s, b, total, c);
            auto ret = select(r, K::InterruptReturn);
            check(r.status == S::Status::ReachedTarget && ret.size() == 1 && ret[0].states == total,
                  "ignored data exact source duration");
            check(select(r, K::RamByte).empty(), "ignored data cannot mutate controls");
        }
    struct Compare
    {
        unsigned pc, operand, kind, states;
    };
    for (auto op : std::array<Compare, 5>{{{0x128, 83, 0, 8},
                                           {0x98, 0x88, 1, 7},
                                           {0xc7, 0x87, 2, 7},
                                           {0xb7, 0x86, 3, 7},
                                           {0x6e9, 0, 4, 7}}})
        for (unsigned a = 0; a < 256; ++a)
            for (bool old : {false, true})
            {
                auto s = base();
                s.adc.interruptsEnabled = false;
                s.registers.pc = op.pc;
                s.registers.a = a;
                s.registers.d = op.operand;
                s.registers.carry = old;
                auto r = advance(s, {}, op.states, c);
                bool skip = op.kind == 0   ? a != op.operand
                            : op.kind == 1 ? a < op.operand
                            : op.kind == 2 ? a > op.operand
                            : op.kind == 3 ? a == op.operand
                                           : a != op.operand;
                bool carry = op.kind == 2 ? a < (op.operand + 1) : a < op.operand;
                check(r.status == S::Status::ReachedTarget && r.state.registers.a == a &&
                          r.state.registers.d == op.operand,
                      "comparison preserves operands");
                check(r.state.registers.skip == skip && r.state.registers.carry == carry,
                      "NEC comparison subtract-borrow CY and exact SK");
            }
}
void widerComparisons()
{
    S::Configuration c;
    c.adc.interrupts = false;
    for (unsigned left : {0u, 1u, 255u, 256u, 65534u, 65535u})
        for (unsigned right : {0u, 1u, 255u, 256u, 65534u, 65535u})
            for (unsigned pc : {0x3f5u, 0x3f8u, 0x403u})
            {
                auto s = base();
                s.adc.interruptsEnabled = false;
                s.registers.pc = pc;
                s.registers.ea = left;
                s.registers.b = right >> 8;
                s.registers.c = right & 255;
                s.registers.carry = true;
                auto r = advance(s, {}, 11, c);
                bool skip = pc == 0x3f5 ? left != right : pc == 0x3f8 ? left > right : left < right;
                bool carry = pc == 0x3f8 ? left < (right + 1) : left < right;
                check(r.status == S::Status::ReachedTarget && r.state.registers.skip == skip &&
                          r.state.registers.carry == carry,
                      "16bit comparison borrow must not wrap atFFFF");
                check(r.state.registers.ea == left && r.state.registers.b == right >> 8 &&
                          r.state.registers.c == (right & 255),
                      "16bit comparison preserves operands");
            }
    for (unsigned pc : {0x707u, 0x728u})
        for (unsigned a : {0u, 1u, 254u, 255u})
            for (unsigned operand : {0u, 1u, 254u, 255u})
            {
                auto s = base();
                s.adc.interruptsEnabled = false;
                s.registers.pc = pc;
                s.registers.a = a;
                s.registers.c = operand;
                s.control.ram[0x7e] = operand;
                auto r = advance(s, {}, pc == 0x707 ? 8 : 14, c);
                check(r.state.registers.skip == (a > operand) &&
                          r.state.registers.carry == (a < (operand + 1)),
                      "8bit greater-than borrow must not wrap atFF");
            }
}
std::vector<S::ByteReady> burst(unsigned first);
void distinctPitchBurst()
{
    auto b = burst(0);
    for (unsigned i = 0; i < 6; ++i)
        b[2 * i + 1].value = 61 + i;
    auto r = advance(base(), b, 35000);
    check(r.status == S::Status::ReachedTarget && r.consumed == 12,
          "distinct pitch routing scene completes");
    for (unsigned i = 0; i < 6; ++i)
        check(r.state.control.ram[9 + i] == 61 + i,
              "literal board pitch maps to addressed card only");
}
void lifecycle()
{
    S::Configuration c;
    c.adc.interrupts = false;
    // A real prior call has pushed return address030A. An IRQ over a skipped
    // helper instruction must restore that frame, both register banks and CY/SK.
    for (unsigned flags = 0; flags < 4; ++flags)
    {
        auto s = base();
        s.registers.pc = 0x082f;
        s.registers.sp = 0xfffd;
        s.control.ram[0xfd] = 0x0a;
        s.control.ram[0xfe] = 3;
        s.registers.carry = flags & 1;
        s.registers.skip = flags & 2;
        s.registers.a = 39;
        s.registers.ea = 0xabcd;
        s.registers.b = 0x12;
        s.registers.c = 0x34;
        s.registers.alternateA = 77;
        s.registers.alternateEa = 0x4567;
        s.registers.alternateB = 0x89;
        s.registers.alternateC = 0xab;
        std::array<S::ByteReady, 1> b{{{0, 0x88}}};
        auto r = advance(s, b, 110, c);
        check(r.status == S::Status::ReachedTarget && r.state.registers.pc == 0x82f &&
                  r.state.registers.sp == 0xfffd,
              "serial RETI preserves live CALF stack");
        check(r.state.registers.carry == bool(flags & 1) &&
                  r.state.registers.skip == bool(flags & 2),
              "serial RETI restores CY/SK");
        check(r.state.registers.a == 39 && r.state.registers.ea == 0xabcd &&
                  r.state.registers.b == 0x12 && r.state.registers.c == 0x34,
              "serial restores caller registers");
        check(r.state.control.ram[0xfd] == 0x0a && r.state.control.ram[0xfe] == 3,
              "return frame preserved");
        if (flags & 2)
        {
            auto next = advance(r.state, {}, 121, c);
            check(next.state.registers.pc == 0x832 && next.state.registers.pa == 0xff,
                  "restored SK consumes skipped helper without side effect");
        }
    }
    // Tie and pending serial during ADC: actual ISR is188 states with EI;RETI.
    for (unsigned when : {0u, 16u, 64u, 171u, 180u, 187u})
    {
        auto s = base();
        s.adc.request = true;
        std::array<S::ByteReady, 1> b{{{when, 0x88}}};
        auto r = advance(s, b, 298);
        auto accepted = select(r, K::InterruptAcceptance);
        check(accepted.size() == 2 && accepted[0].address == 0x20 && accepted[0].states == 0 &&
                  accepted[1].address == 0x28 && accepted[1].states == 188,
              "ADC wins and serial waits for complete188state ISR");
    }
    // FSR clear is not RXB consumption. Collision before the actual read rejects.
    for (unsigned mode = 0; mode < 2; ++mode)
    {
        c.adc.accessBoundary = static_cast<A::PeripheralAccessBoundary>(mode);
        for (unsigned when : {0u, 15u, 16u, 33u})
        {
            std::array<S::ByteReady, 2> b{{{0, 0x88}, {when, 60}}};
            auto r = advance(base(), b, 100, c);
            check(r.status == S::Status::ReceiveOverrun && r.state.now == when && r.consumed == 1,
                  "unread RXB collision after FSR clear");
        }
        std::array<S::ByteReady, 2> b{{{0, 0x88}, {45, 60}}};
        auto r = advance(base(), b, 500, c);
        check(r.status == S::Status::ReachedTarget && r.state.serialInterrupts == 2,
              "postread second byte remains pending without overwrite");
    }
    // Software restart retains changed bank identity and defers pending ADC
    // until the LDAW following02EB EI finishes, not merely the jump destination.
    auto s = base();
    s.registers.a = 91;
    s.registers.alternateA = 47;
    s.adc.request = true;
    s.adc.mkh = 5;
    std::array<S::ByteReady, 1> b{{{0, 0x80}}};
    auto r = advance(s, b, 227, c);
    check(r.state.registers.alternateA == 91 && r.state.registers.a != 91,
          "restart must not silently exchange back");
    r.state.adc.mkh = 4;
    r.state.needsArbitration = true;
    c.adc.interrupts = true;
    auto next = advance(r.state, {}, 238, c);
    auto acc = select(next, K::InterruptAcceptance);
    check(acc.size() == 1 && acc[0].states == 237, "restart EI one-following-instruction deferral");
}
bool sameState(const S::State &a, const S::State &b)
{
    const auto &x = a.registers;
    const auto &y = b.registers;
    return a.control.ram == b.control.ram && a.control.adcComplete == b.control.adcComplete &&
           a.now == b.now && a.rxb == b.rxb && a.ordinal == b.ordinal &&
           a.registers.portf == b.registers.portf &&
           a.adc.elapsedStates == b.adc.elapsedStates && a.adc.conversion == b.adc.conversion &&
           a.adc.channel == b.adc.channel && a.adc.anm == b.adc.anm &&
           a.adc.request == b.adc.request &&
           a.adc.statesUntilConversion == b.adc.statesUntilConversion &&
           a.serialInterrupts == b.serialInterrupts && a.adcInterrupts == b.adcInterrupts &&
           a.passes == b.passes && a.fsr == b.fsr && a.rxbFull == b.rxbFull &&
           a.pending.start == b.pending.start && a.pending.returnPc == b.pending.returnPc &&
           a.pending.savedPsw == b.pending.savedPsw &&
           a.pending.interruptVector == b.pending.interruptVector &&
           a.pending.sampledSkip == b.pending.sampledSkip &&
           a.pending.skipped == b.pending.skipped && a.pending.remaining == b.pending.remaining &&
           a.pending.kind == b.pending.kind && a.pending.address == b.pending.address &&
           a.pending.accessDone == b.pending.accessDone &&
           a.pending.sampledValue == b.pending.sampledValue &&
           a.needsArbitration == b.needsArbitration &&
           a.completedInstruction == b.completedInstruction &&
           a.adc.interruptsEnabled == b.adc.interruptsEnabled &&
           a.adc.eiDeferred == b.adc.eiDeferred && a.adc.mkh == b.adc.mkh &&
           a.opaquePsw == b.opaquePsw &&
           std::tie(x.a, x.b, x.c, x.d, x.e, x.h, x.l, x.ea, x.v, x.alternateA, x.alternateB,
                    x.alternateC, x.alternateD, x.alternateE, x.alternateH, x.alternateL,
                    x.alternateEa, x.alternateV, x.pc, x.sp, x.carry, x.skip, x.pa, x.pb,
                    x.portc) == std::tie(y.a, y.b, y.c, y.d, y.e, y.h, y.l, y.ea, y.v, y.alternateA,
                                         y.alternateB, y.alternateC, y.alternateD, y.alternateE,
                                         y.alternateH, y.alternateL, y.alternateEa, y.alternateV,
                                         y.pc, y.sp, y.carry, y.skip, y.pa, y.pb, y.portc);
}
std::vector<S::ByteReady> burst(unsigned first = 0)
{
    std::vector<S::ByteReady> b;
    for (unsigned i = 0; i < 6; ++i)
    {
        b.push_back({first + 2560 * i, static_cast<std::uint8_t>(0x88 + i)});
        b.push_back({first + 2560 * i + 1280, static_cast<std::uint8_t>(61)});
    }
    return b;
}
void chunksAndDrain()
{
    {
        auto s = base();
        s.adc.statesUntilConversion = 1;
        auto all = advance(s, {}, 1);
        auto drained = advance(s, {}, 1, {}, 0, 1000);
        check(drained.status == S::Status::ReachedTarget && sameState(all.state, drained.state) &&
                  normalized(all.events) == normalized(drained.events),
              "OutputFull exact target1 drains before declaring completion");
    }
    for (unsigned phase = 0; phase < 3; ++phase)
        for (unsigned mode = 0; mode < 2; ++mode)
        {
            S::Configuration c;
            c.adc.anmWritePhase = static_cast<A::AnmWritePhase>(phase);
            c.adc.accessBoundary = static_cast<A::PeripheralAccessBoundary>(mode);
            auto b = burst(187);
            auto s = base();
            s.adc.statesUntilConversion = 1;
            auto all = advance(s, b, 35000, c);
            check(all.status == S::Status::ReachedTarget, "whole burst completes");
            for (unsigned chunk : {1u, 7u, 16u, 31u, 83u, 1280u, 4096u})
            {
                auto part = advance(s, b, 35000, c, chunk);
                check(part.status == all.status && part.consumed == all.consumed &&
                          sameState(part.state, all.state),
                      "arbitrary chunk complete state parity");
                check(normalized(part.events) == normalized(all.events),
                      "arbitrary chunk full ledger parity");
            }
            auto drained = advance(s, b, 35000, c, 0, 1000);
            check(drained.status == all.status && sameState(drained.state, all.state),
                  "OutputFull drain state parity");
            check(normalized(drained.events) == normalized(all.events),
                  "OutputFull drain event parity");
        }
    for (unsigned arrival : {0u, 1u, 15u, 16u, 17u, 33u, 34u, 43u, 44u})
    {
        auto s = base();
        s.adc.statesUntilConversion = 16;
        std::array<S::ByteReady, 1> b{{{arrival, 0x88}}};
        auto whole = advance(s, b, 500);
        auto drain = advance(s, b, 500, {}, 0, 1000);
        check(sameState(whole.state, drain.state) &&
                  normalized(whole.events) == normalized(drain.events),
              "atomic completion drain boundaries");
    }
}
void burstMatrix()
{
    std::uint64_t maxLatency = 0;
    unsigned maxPhase = 0, maxPolicy = 0, consumed = 0;
    for (unsigned policy = 0; policy < 3; ++policy)
        for (unsigned mode = 0; mode < 2; ++mode)
            for (unsigned phase = 0; phase < 1280; ++phase)
            {
                S::Configuration c;
                c.adc.anmWritePhase = static_cast<A::AnmWritePhase>(policy);
                c.adc.accessBoundary = static_cast<A::PeripheralAccessBoundary>(mode);
                auto s = base();
                s.adc.anm = (phase & 1) ? 8 : 0;
                s.adc.channel = (phase / 192) % 4;
                s.adc.statesUntilConversion = 192 - phase % 192;
                auto b = burst(phase);
                auto r = advance(s, b, phase + 35000, c);
                ++phaseRuns;
                check(r.status == S::Status::ReachedTarget && r.consumed == 12 &&
                          r.state.serialInterrupts == 12,
                      "1280state wire-compatible burst has no overrun");
                auto reads = select(r, K::SerialRead);
                check(reads.size() == 12, "all12 burst bytes consumed once");
                for (unsigned i = 0; i < reads.size() && i < b.size(); ++i)
                {
                    check(reads[i].value == b[i].value && reads[i].states >= b[i].states,
                          "ordered literal RX byte stream");
                    auto dt = reads[i].states - b[i].states;
                    if (dt > maxLatency)
                    {
                        maxLatency = dt;
                        maxPhase = phase;
                        maxPolicy = policy * 2 + mode;
                    }
                    ++consumed;
                }
                for (unsigned i = 0; i < 6; ++i)
                    check(r.state.control.ram[9 + i] == 61, "six literal board pitches delivered");
            }
    std::cout << "burst_matrix runs=" << phaseRuns << " reads=" << consumed
              << " max_ready_to_read=" << maxLatency << " states phase=" << maxPhase
              << " policy=" << maxPolicy << '\n';
}
#include <fstream>
void pitWitness()
{
    auto b = burst();
    auto r = advance(base(), b, 35000);
    check(r.status == S::Status::ReachedTarget, "nominal ledger witness completes");
    std::ofstream out;
    if (const char *path = std::getenv("YOUKNOW_SERIAL_LEDGER"))
        out.open(path);
    out << "kind,states,pc,address,value,card,pa,pb,portc\n";
    for (const auto &e : r.events)
        if (e.kind == K::ExternalWrite || e.kind == K::Converter || e.kind == K::StackReset ||
            e.kind == K::InterruptAcceptance || e.kind == K::SerialRead)
            out << unsigned(e.kind) << ',' << e.states << ',' << e.pc << ',' << e.address << ','
                << e.value << ',' << unsigned(e.card) << ',' << unsigned(e.pa) << ','
                << unsigned(e.pb) << ',' << unsigned(e.portc) << '\n';
    auto warm = base();
    warm.control.ram[0x10] = 63;
    warm.control.ram[0x11] = 63;
    warm.control.ram[0] = 63;
    auto quiet = advance(warm, {}, 20000);
    auto ext = select(quiet, K::ExternalWrite);
    bool witnessed = false;
    for (const auto &e : ext)
        if (e.pc == 0x046f)
        {
            std::array<S::ByteReady, 1> one{{{e.states, 0x80}}};
            auto cut = advance(warm, one, e.states + 500);
            auto cv = select(cut, K::Converter);
            auto reset = select(cut, K::StackReset);
            auto physical = select(cut, K::ExternalWrite);
            bool msb = std::any_of(physical.begin(), physical.end(),
                                   [&](const auto &p)
                                   {
                                       return p.pc == 0x046f && p.states == e.states &&
                                              p.value == e.value && p.address == e.address;
                                   });
            bool cvAfter =
                std::any_of(cv.begin(), cv.end(), [&](const auto &p)
                            { return p.states > e.states && p.card >= 3 && p.card <= 8; });
            if (msb && !cvAfter && !reset.empty())
            {
                witnessed = true;
                std::cout << "PIT_without_matching_DAC witness countMSB=" << e.states
                          << " restart=" << reset[0].states << " address=" << e.address << '\n';
                break;
            }
        }
    check(witnessed, "independent PIT count must survive abandoned matching DAC computation");
}
void invalidInputs()
{
    auto invalid =
        [](S::State s, S::Configuration c, std::span<const S::ByteReady> b, std::uint64_t target)
    {
        S::Events e;
        auto r = S::advanceTo(s, table, inputs, c, b, target, e);
        check(r.status == S::Status::InvalidState && r.consumedBytes == 0 && e.count == 0,
              "invalid input rejected before events");
    };
    for (unsigned bad : {193u, 65535u})
    {
        auto s = base();
        s.adc.statesUntilConversion = bad;
        invalid(s, {}, {}, 1);
    }
    {
        auto s = base();
        s.adc.channel = 4;
        invalid(s, {}, {}, 1);
    }
    {
        auto s = base();
        s.adc.anm = 4;
        invalid(s, {}, {}, 1);
    }
    {
        auto s = base();
        s.adc.mkh = 0;
        invalid(s, {}, {}, 1);
    }
    {
        auto s = base();
        s.registers.a = 256;
        invalid(s, {}, {}, 1);
    }
    {
        auto s = base();
        s.registers.sp = 0xfefe;
        invalid(s, {}, {}, 1);
    }
    {
        auto s = base();
        s.now = 1;
        s.adc.elapsedStates = 1;
        invalid(s, {}, {}, 0);
    }
    {
        auto s = base();
        s.now = 1;
        invalid(s, {}, {}, 2);
    }
    {
        S::Configuration c;
        c.adc.anmWritePhase = static_cast<A::AnmWritePhase>(3);
        invalid(base(), c, {}, 1);
    }
    {
        S::Configuration c;
        c.adc.accessBoundary = static_cast<A::PeripheralAccessBoundary>(2);
        invalid(base(), c, {}, 1);
    }
    {
        std::array<S::ByteReady, 2> b{{{2, 0x88}, {1, 60}}};
        invalid(base(), {}, b, 3);
    }
}

// Compact normal-parameter fixtures use synthetic coefficients, never the
// original coefficient ROM. Store PCs/times/values were independently derived
// from the pinned B-2 bytes with those synthetic memory regions. Timing starts
// at RX interrupt acceptance and includes its 16-state entry. The 40 cases
// cover all 22 dispatch targets plus switch, bend and lookup boundaries.
namespace normalParameters
{
using Store = std::tuple<unsigned, unsigned, unsigned, unsigned>; // PC, state, address, value
struct Fixture
{
    unsigned command, value, duration;
    std::vector<Store> writes, portWrites;
};
S::Tables coefficients()
{
    S::Tables result;
    result.control = table;
    for (unsigned i = 0; i < 128; ++i)
    {
        result.control.attack[i] = 0x4000 + 3 * i;
        result.lfoRate[i] = 0x2100 + 5 * i;
        result.decayRelease[i] = 0x3300 + 7 * i;
        result.dcoLfoDepth[i] = (17 + 3 * i) & 255;
    }
    for (unsigned i = 0; i < 8; ++i)
        result.delayFade[i] = 0x4400 + 13 * i;
    return result;
}
S::State initial(unsigned command)
{
    S::State state;
    state.control.ram[0x36] = command;
    state.control.ram[0x1e] = 0xa5;
    state.control.ram[0x37] = 6;
    state.control.ram[0x46] = 255;
    state.registers.a = 77;
    state.registers.b = 21;
    state.registers.d = 201;
    state.registers.ea = 4123;
    state.registers.portf = 0x55;
    return state;
}
Run execute(S::State state, const S::Tables &lookup, std::span<const S::ByteReady> bytes,
            unsigned target, unsigned chunk = 10000, bool forceDrain = false)
{
    Run result;
    result.state = state;
    S::Configuration config;
    config.adc.interrupts = false;
    for (unsigned iteration = 0; iteration < 10001; ++iteration)
    {
        S::Events events;
        events.count = forceDrain ? 1001 : 0;
        const auto first = events.count;
        const auto end = std::min<std::uint64_t>(target, result.state.now + chunk);
        const auto step = S::advanceTo(result.state, lookup, inputs, config,
                                      bytes.subspan(result.consumed), end, events);
        if (forceDrain)
            check(step.status == S::Status::OutputFull && result.state.now == state.now
                      && step.consumedBytes == 0 && events.count == first,
                  "rich overload full output buffer is resumable before any mutation");
        forceDrain = false;
        result.consumed += step.consumedBytes;
        result.status = step.status;
        for (auto index = first; index < events.count; ++index)
            check(events.entries[index].states <= end, "parameter events never commit in future");
        result.events.insert(result.events.end(), events.entries.begin() + first,
                             events.entries.begin() + events.count);
        if (step.status != S::Status::ReachedTarget && step.status != S::Status::OutputFull)
            return result;
        if (step.status == S::Status::ReachedTarget && result.state.now == target)
        {
            check(result.consumed == bytes.size(), "all parameter bytes consumed");
            return result;
        }
    }
    check(false, "bounded normal-parameter continuation");
    return result;
}
std::vector<Store> stores(const Run &run, K kind)
{
    std::vector<Store> result;
    for (const auto &event : run.events)
        if (event.kind == kind)
            result.emplace_back(event.pc, unsigned(event.states), event.address, event.value);
    return result;
}
void contract()
{
    const auto lookup = coefficients();
    const std::vector<Fixture> fixtures{
        {0x8e, 37, 403,
         {{0x0150, 167, 0xff7f, 0x25}, {0x0164, 248, 0xff46, 0x0c},
          {0x0166, 267, 0xff1e, 0xe5}, {0x0106, 374, 0xff36, 0x8f}},
         {{0x017B, 338, 0x0005, 0x40}}},
        {0x8f, 37, 283,
         {{0x0185, 167, 0xff37, 0x25}, {0x0194, 228, 0xff46, 0xff}, {0x0106, 254, 0xff36, 0x90}},
         {}},
        {0x90, 37, 274,
         {{0x01A3, 219, 0xff4b, 0xb9}, {0x01A3, 219, 0xff4c, 0x21}, {0x0106, 245, 0xff36, 0x91}},
         {}},
        {0x91, 37, 363,
         {{0x0211, 242, 0xff6c, 0x1a}, {0x0211, 242, 0xff6d, 0x44},
          {0x021E, 308, 0xff58, 0x6f}, {0x021E, 308, 0xff59, 0x40}, {0x0106, 334, 0xff36, 0x92}},
         {}},
        {0x92, 37, 252,
         {{0x0242, 197, 0xff49, 0x80}, {0x0106, 223, 0xff36, 0x93}},
         {}},
        {0x93, 37, 247,
         {{0x01BD, 192, 0xff47, 0x4a}, {0x0106, 218, 0xff36, 0x94}},
         {}},
        {0x94, 37, 268,
         {{0x01DE, 213, 0xff39, 0x80}, {0x01DE, 213, 0xff3a, 0x12}, {0x0106, 239, 0xff36, 0x95}},
         {}},
        {0x95, 37, 268,
         {{0x01DE, 213, 0xff3d, 0x80}, {0x01DE, 213, 0xff3e, 0x12}, {0x0106, 239, 0xff36, 0x96}},
         {}},
        {0x96, 37, 268,
         {{0x01DE, 213, 0xff3f, 0x80}, {0x01DE, 213, 0xff40, 0x12}, {0x0106, 239, 0xff36, 0x97}},
         {}},
        {0x97, 37, 247,
         {{0x01BD, 192, 0xff41, 0x4a}, {0x0106, 218, 0xff36, 0x98}},
         {}},
        {0x98, 37, 247,
         {{0x01BD, 192, 0xff48, 0x4a}, {0x0106, 218, 0xff36, 0x99}},
         {}},
        {0x99, 37, 237,
         {{0x01BD, 182, 0xff42, 0x4a}, {0x0106, 208, 0xff36, 0x9a}},
         {}},
        {0x9a, 37, 258,
         {{0x01DE, 203, 0xff43, 0x80}, {0x01DE, 203, 0xff44, 0x12}, {0x0106, 229, 0xff36, 0x9b}},
         {}},
        {0x9b, 37, 229,
         {{0x01E6, 174, 0xff45, 0x25}, {0x0106, 200, 0xff36, 0x9c}},
         {}},
        {0x9c, 37, 283,
         {{0x01FB, 228, 0xff21, 0x03}, {0x01FB, 228, 0xff22, 0x34}, {0x0106, 254, 0xff36, 0x9d}},
         {}},
        {0x9d, 37, 268,
         {{0x01DE, 213, 0xff23, 0x80}, {0x01DE, 213, 0xff24, 0x12}, {0x0106, 239, 0xff36, 0x9e}},
         {}},
        {0x9e, 37, 293,
         {{0x01FB, 238, 0xff25, 0x03}, {0x01FB, 238, 0xff26, 0x34}, {0x0106, 264, 0xff36, 0x9f}},
         {}},
        {0x9f, 37, 268,
         {{0x01DE, 213, 0xff3b, 0x80}, {0x01DE, 213, 0xff3c, 0x12}, {0x0106, 239, 0xff36, 0xa0}},
         {}},
        {0xa0, 37, 291,
         {{0x0223, 172, 0xff1e, 0xa5}, {0x0232, 223, 0xff62, 0x4b},
          {0x0234, 236, 0xff05, 0x01}, {0x0106, 262, 0xff36, 0xa1}},
         {}},
        {0xa1, 37, 281,
         {{0x0227, 172, 0xff1e, 0xb5}, {0x0232, 213, 0xff62, 0x4b},
          {0x0234, 226, 0xff05, 0x01}, {0x0106, 252, 0xff36, 0xa2}},
         {}},
        {0xa2, 37, 247,
         {{0x01BD, 192, 0xff63, 0x4a}, {0x0106, 218, 0xff36, 0xa3}},
         {}},
        {0xa3, 37, 198,
         {},
         {}},
        {0x90, 0, 274,
         {{0x01A3, 219, 0xff4b, 0x00}, {0x01A3, 219, 0xff4c, 0x21}, {0x0106, 245, 0xff36, 0x91}},
         {}},
        {0x90, 127, 274,
         {{0x01A3, 219, 0xff4b, 0x7b}, {0x01A3, 219, 0xff4c, 0x23}, {0x0106, 245, 0xff36, 0x91}},
         {}},
        {0x91, 0, 363,
         {{0x0211, 242, 0xff6c, 0x00}, {0x0211, 242, 0xff6d, 0x44},
          {0x021E, 308, 0xff58, 0x00}, {0x021E, 308, 0xff59, 0x40}, {0x0106, 334, 0xff36, 0x92}},
         {}},
        {0x91, 127, 363,
         {{0x0211, 242, 0xff6c, 0x5b}, {0x0211, 242, 0xff6d, 0x44},
          {0x021E, 308, 0xff58, 0x7d}, {0x021E, 308, 0xff59, 0x41}, {0x0106, 334, 0xff36, 0x92}},
         {}},
        {0x92, 0, 252,
         {{0x0242, 197, 0xff49, 0x11}, {0x0106, 223, 0xff36, 0x93}},
         {}},
        {0x92, 127, 252,
         {{0x0242, 197, 0xff49, 0x8e}, {0x0106, 223, 0xff36, 0x93}},
         {}},
        {0x9c, 0, 283,
         {{0x01FB, 228, 0xff21, 0x00}, {0x01FB, 228, 0xff22, 0x33}, {0x0106, 254, 0xff36, 0x9d}},
         {}},
        {0x9c, 127, 283,
         {{0x01FB, 228, 0xff21, 0x79}, {0x01FB, 228, 0xff22, 0x36}, {0x0106, 254, 0xff36, 0x9d}},
         {}},
        {0x9e, 0, 293,
         {{0x01FB, 238, 0xff25, 0x00}, {0x01FB, 238, 0xff26, 0x33}, {0x0106, 264, 0xff36, 0x9f}},
         {}},
        {0x9e, 127, 293,
         {{0x01FB, 238, 0xff25, 0x79}, {0x01FB, 238, 0xff26, 0x36}, {0x0106, 264, 0xff36, 0x9f}},
         {}},
        {0x8e, 0, 382,
         {{0x0150, 167, 0xff7f, 0x00}, {0x0164, 248, 0xff46, 0x0d},
          {0x0166, 267, 0xff1e, 0xe5}, {0x0106, 353, 0xff36, 0x8f}},
         {{0x0173, 317, 0x0005, 0xc0}}},
        {0x8e, 6, 399,
         {{0x0150, 167, 0xff7f, 0x06}, {0x0164, 248, 0xff46, 0x0d},
          {0x0166, 267, 0xff1e, 0xe5}, {0x0106, 370, 0xff36, 0x8f}},
         {{0x017F, 344, 0x0005, 0x00}}},
        {0x8e, 127, 408,
         {{0x0150, 167, 0xff7f, 0x7f}, {0x0164, 248, 0xff46, 0x1e},
          {0x0166, 267, 0xff1e, 0xe5}, {0x016C, 297, 0xff1e, 0xa5}, {0x0106, 379, 0xff36, 0x8f}},
         {{0x017F, 353, 0x0005, 0x00}}},
        {0x8f, 24, 283,
         {{0x0185, 167, 0xff37, 0x18}, {0x0194, 228, 0xff46, 0xf3}, {0x0106, 254, 0xff36, 0x90}},
         {}},
        {0x91, 15, 363,
         {{0x0211, 242, 0xff6c, 0x00}, {0x0211, 242, 0xff6d, 0x44},
          {0x021E, 308, 0xff58, 0x2d}, {0x021E, 308, 0xff59, 0x40}, {0x0106, 334, 0xff36, 0x92}},
         {}},
        {0x91, 16, 363,
         {{0x0211, 242, 0xff6c, 0x0d}, {0x0211, 242, 0xff6d, 0x44},
          {0x021E, 308, 0xff58, 0x30}, {0x021E, 308, 0xff59, 0x40}, {0x0106, 334, 0xff36, 0x92}},
         {}},
        {0xa0, 0, 281,
         {{0x0223, 172, 0xff1e, 0xa5}, {0x0232, 213, 0xff62, 0x00},
          {0x0234, 226, 0xff05, 0x01}, {0x0106, 252, 0xff36, 0xa1}},
         {}},
        {0xa1, 0, 271,
         {{0x0227, 172, 0xff1e, 0xb5}, {0x0232, 203, 0xff62, 0x00},
          {0x0234, 216, 0xff05, 0x01}, {0x0106, 242, 0xff36, 0xa2}},
         {}},
    };
    for (const auto &fixture : fixtures)
        for (unsigned flags : {0u, 3u})
        {
            auto state = initial(fixture.command);
            state.registers.carry = (flags & 1) != 0;
            state.registers.skip = (flags & 2) != 0;
            const std::array<S::ByteReady, 1> bytes{{{0, std::uint8_t(fixture.value)}}};
            const auto whole = execute(state, lookup, bytes, fixture.duration);
            check(whole.status == S::Status::ReachedTarget,
                  "all normal parameter dispatch targets execute");
            check(stores(whole, K::RamByte) == fixture.writes,
                  "independent normal-parameter RAM ledger");
            check(stores(whole, K::PortFWrite) == fixture.portWrites,
                  "independent PF port source value and completion ledger");
            auto expected = state.control.ram;
            for (const auto &[pc, time, address, value] : fixture.writes)
            {
                (void) pc;
                (void) time;
                expected[address & 255] = value;
            }
            // Interrupt stack uses FFFC..FFFE; FFFF is unchanged RAM.
            expected[0xfc] = 0xec;
            expected[0xfd] = 2;
            expected[0xfe] = (flags & 1 ? 1 : 0) | (flags & 2 ? 32 : 0);
            check(whole.state.control.ram == expected, "no unlisted parameter RAM side effects");
            const auto returns = select(whole, K::InterruptReturn);
            check(returns.size() == 1 && returns[0].states == fixture.duration,
                  "parameter handler returns at independently derived state");
            const auto &registers = whole.state.registers;
            check(registers.pc == 0x02ec && registers.sp == 0xffff
                      && registers.a == 77 && registers.b == 21 && registers.d == 201
                      && registers.ea == 4123 && registers.carry == state.registers.carry
                      && registers.skip == state.registers.skip,
                  "normal parameter restores interrupted register bank and CY/SK");
            const auto expectedPort = fixture.portWrites.empty()
                ? 0x55 : std::get<3>(fixture.portWrites.back());
            check(registers.portf == expectedPort, "PF latch persists outside swapped register bank");
            const auto split = execute(state, lookup, bytes, fixture.duration, 1, true);
            check(sameState(whole.state, split.state)
                      && normalized(whole.events) == normalized(split.events),
                  "parameter continuation and output drainage preserve exact state and ledger");

            S::Configuration config;
            config.adc.interrupts = false;
            S::Events events;
            auto legacy = state;
            const auto refused = S::advanceTo(legacy, lookup.control, inputs, config, bytes,
                                               fixture.duration, events);
            check(refused.status == S::Status::UnsupportedPath && legacy.now == 132
                      && legacy.registers.pc == 0x00d7 && legacy.registers.portf == 0x55,
                  "legacy table overload retains explicit stop before TABLE dispatch");
        }

    // Two returning payloads with no new header prove the actual FF36
    // auto-increment, not independently initialized command fixtures alone.
    auto state = initial(0x8e);
    const std::array<S::ByteReady, 2> bytes{{{0, 0}, {382, 24}}};
    const auto returned = execute(state, lookup, bytes, 665, 7, true);
    check(returned.status == S::Status::ReachedTarget
              && returned.state.control.ram[0x36] == 0x90
              && returned.state.control.ram[0x37] == 24
              && returned.state.control.ram[0x46] == 1,
          "live switch payloads auto-increment through TABLE dispatch");
    check(select(returned, K::ExternalWrite).empty(),
          "switch RAM write is not an early IC40 bus write");
    const auto beforeLatch = execute(returned.state, lookup, {}, 691, 1);
    check(select(beforeLatch, K::ExternalWrite).empty(),
          "IC40 bus store does not commit before its completion");
    const auto latched = execute(beforeLatch.state, lookup, {}, 692, 1);
    const auto latch = stores(latched, K::ExternalWrite);
    check(latch == std::vector<Store>{{0x02ee, 692, 0x3000, 1}},
          "IC40 receives staged RAM only at completed external store");

    const std::array<S::ByteReady, 1> rangeByte{{{0, 0}}};
    const auto beforePort = execute(initial(0x8e), lookup, rangeByte, 316);
    check(beforePort.state.registers.portf == 0x55
              && select(beforePort, K::PortFWrite).empty(),
          "MVI PF preserves old latch until completion");
    const auto port = execute(beforePort.state, lookup, {}, 317);
    check(port.state.registers.portf == 0xc0
              && stores(port, K::PortFWrite) == std::vector<Store>{{0x0173, 317, 5, 0xc0}},
          "MVI PF completion changes exact full-byte latch");

    auto armed = initial(0xa3);
    armed.control.ram[0x37] |= 0x40;
    armed.control.ram[0x01] = 0x5a;
    const std::array<S::ByteReady, 1> diagnosticByte{{{0, 37}}};
    const auto stopped = execute(armed, lookup, diagnosticByte, 400, 1, true);
    check(stopped.status == S::Status::UnsupportedPath && stopped.state.now == 173
              && stopped.state.registers.pc == 0x024b
              && stopped.state.control.ram[0x01] == 0x5a
              && stopped.state.control.ram[0x36] == 0xa3,
          "armed A3 stops before original diagnostic side effects");
    check(select(stopped, K::RamByte).empty() && select(stopped, K::PortFWrite).empty()
              && select(stopped, K::ExternalWrite).empty()
              && select(stopped, K::InterruptReturn).empty(),
          "unsupported diagnostic path produces no fabricated store or return");
}
} // namespace normalParameters

int main()
{
    normalParameters::contract();
    pathLedger();
    ignoredAndCarry();
    widerComparisons();
    lifecycle();
    chunksAndDrain();
    pitWitness();
    distinctPitchBurst();
    invalidInputs();
    burstMatrix();
    std::cout << "assertions=" << assertions << " failures=" << failures << '\n';
    for (const auto &[s, n] : failureKinds)
        std::cerr << n << " " << s << '\n';
    return failures ? 1 : 0;
}
