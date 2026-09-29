#include "YouKnowFirmwareAssignerTrace.h"
#include "YouKnowFirmwareAssignerProgram.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace youknow {
// Original A-5 provenance: pinned ic1 listing at
// https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic1.txt
// Raw image SHA256 d43cce5578ee2f16b27c8b06bff30743e3e2dffc796d033811e565d5d578c52e.
// The90 semantic descriptors cover0020..002A,0595..059F,07AF..07F8,
// 0837..0842 and09E8..0A0C. The bytes/timings were independently audited.
// Producer entry->slot store134, MKH mask158, publication168, unmask188,
// RET228 original CPU states. The changed-route loop has48 polls:1530states
// from07C7 to07CF,1537including07C5,1579through07D7 PC completion.
// NEC Stock500375, April1987, printed7-4 and9-5/7: TXB-empty FST, grouped
// flag clearing,16-state entry. Printed12-21 defers EI by one instruction.
// https://drive.google.com/file/d/0B44NKm9yPA1bNDFXZnFrdG1PdDA/view
namespace {
using Trace = FirmwareAssignerTrace;
namespace Uart = FirmwareUartTrace;
using firmwareAssignerDetail::Instruction;
using firmwareAssignerDetail::Op;
const Instruction *find(unsigned pc) noexcept {
    const auto &p = firmwareAssignerDetail::program;
    const auto it = std::lower_bound(p.begin(), p.end(), pc,
        [](const auto &i, unsigned address) { return i.pc < address; });
    return it != p.end() && it->pc == pc ? &*it : nullptr;
}
struct Completion {
    Trace::State state;
    std::array<Trace::Event, 8> events{};
    unsigned count = 0;
    bool valid = true, txWrite = false, pcWrite = false;
    unsigned peripheralValue = 0;
    explicit Completion(const Trace::State &s) : state(s) {}
    void emit(Trace::EventKind kind, unsigned address, unsigned value) noexcept {
        if (count == events.size()) { valid = false; return; }
        events[count++] = {kind, state.now,
            static_cast<std::uint16_t>(state.pending.address),
            static_cast<std::uint16_t>(address), static_cast<std::uint16_t>(value)};
    }
    unsigned read(unsigned address) noexcept {
        if (address < 0xff00 || address > 0xffff) { valid = false; return 0; }
        return state.ram[address & 255];
    }
    void write(unsigned address, unsigned value, bool stack = false) noexcept {
        if (address < 0xff00 || address > 0xffff) { valid = false; return; }
        state.ram[address & 255] = static_cast<std::uint8_t>(value);
        emit(stack ? Trace::EventKind::StackWrite : Trace::EventKind::RamWrite,
             address, value & 255);
    }
    void pushByte(unsigned value) noexcept {
        auto &sp = state.registers.sp;
        if (sp <= 0xff00 || sp > 0xffff) { valid = false; return; }
        write(--sp, value, true);
    }
    unsigned popByte() noexcept {
        auto &sp = state.registers.sp;
        if (sp < 0xff00 || sp >= 0xffff) { valid = false; return 0; }
        return read(sp++);
    }
    void push(unsigned value) noexcept { pushByte(value >> 8); pushByte(value); }
    unsigned pop() noexcept { const auto lo = popByte(); return lo + 256 * popByte(); }
    void execute(const Instruction &i, const Uart::State &uart) noexcept {
        auto &r = state.registers;
        const auto x = i.argument;
        r.pc += i.bytes;
        r.skip = false;
        if (state.pending.skipped) return;
        switch (i.op) {
        case Op::ANI_MKH_xx:
            state.mkh &= x; emit(Trace::EventKind::MaskWrite, 0, state.mkh); break;
        case Op::ORI_MKH_xx:
            state.mkh |= x; emit(Trace::EventKind::MaskWrite, 0, state.mkh); break;
        case Op::CALF: push(r.pc); r.pc = x; break;
        case Op::DCX_HL: {
            const unsigned value = (r.h * 256 + r.l - 1) & 65535;
            r.h = value >> 8; r.l = value & 255;
        } break;
        case Op::EI: state.interruptEnabled = true; state.eiDeferred = 2; break;
        case Op::EQAW_wa:
            r.carry = r.a < read(0xff00 + x);
            r.skip = r.a == read(0xff00 + x); break;
        case Op::NEAW_wa:
            r.carry = r.a < read(0xff00 + x);
            r.skip = r.a != read(0xff00 + x); break;
        case Op::NEI_A_xx: r.carry = r.a < x; r.skip = r.a != x; break;
        case Op::LTI_A_xx: r.carry = r.a < x; r.skip = r.a < x; break;
        case Op::EXA:
            std::swap(r.a, r.alternateA); std::swap(r.ea, r.alternateEa);
            std::swap(r.v, r.alternateV); break;
        case Op::EXX:
            std::swap(r.b, r.alternateB); std::swap(r.c, r.alternateC);
            std::swap(r.d, r.alternateD); std::swap(r.e, r.alternateE);
            std::swap(r.h, r.alternateH); std::swap(r.l, r.alternateL); break;
        case Op::INRW_wa: {
            const auto value = (read(0xff00 + x) + 1) & 255;
            write(0xff00 + x, value); r.skip = value == 0;
        } break;
        case Op::INR_A: r.a = (r.a + 1) & 255; r.skip = r.a == 0; break;
        case Op::JMP_w: case Op::JR: case Op::JRE: r.pc = x; break;
        case Op::LDAW_wa: r.a = read(0xff00 + x); break;
        case Op::LDAX_Hp: {
            const auto hl = r.h * 256 + r.l;
            r.a = read(hl); const auto value = (hl + 1) & 65535;
            r.h = value >> 8; r.l = value & 255;
        } break;
        case Op::LXI_H_w: r.h = x >> 8; r.l = x & 255; break;
        case Op::MOV_A_B: r.a = r.b; break;
        case Op::MOV_A_C: r.a = r.c; break;
        case Op::MOV_A_E: r.a = r.e; break;
        case Op::MOV_A_L: r.a = r.l; break;
        case Op::MOV_A_PC: r.a = uart.portC; break;
        case Op::MOV_B_A: r.b = r.a; break;
        case Op::MOV_C_A: r.c = r.a; break;
        case Op::MOV_E_A: r.e = r.a; break;
        case Op::MOV_L_A: r.l = r.a; break;
        case Op::MOV_PC_A:
            pcWrite = true; peripheralValue = r.a;
            emit(Trace::EventKind::PortCWrite, 2, r.a); break;
        case Op::MOV_TXB_A:
            txWrite = true; peripheralValue = r.a;
            emit(Trace::EventKind::TxBufferWrite, 0xd8, r.a); break;
        case Op::MVI_A_xx: r.a = x; break;
        case Op::MVI_H_xx: r.h = x; break;
        case Op::OFFI_MKH_xx: r.skip = (state.mkh & x) == 0; break;
        case Op::ONI_A_xx: r.skip = (r.a & x) != 0; break;
        case Op::PUSH_BC: push(r.b * 256 + r.c); break;
        case Op::PUSH_DE: push(r.d * 256 + r.e); break;
        case Op::PUSH_HL: push(r.h * 256 + r.l); break;
        case Op::POP_BC: { const auto v = pop(); r.b = v >> 8; r.c = v & 255; } break;
        case Op::POP_DE: { const auto v = pop(); r.d = v >> 8; r.e = v & 255; } break;
        case Op::POP_HL: { const auto v = pop(); r.h = v >> 8; r.l = v & 255; } break;
        case Op::RET:
            r.pc = pop(); state.foregroundBoundary = find(r.pc) == nullptr;
            if (state.foregroundBoundary) emit(Trace::EventKind::ForegroundReturn, r.pc, 0);
            break;
        case Op::RETI: {
            r.pc = pop(); const auto psw = popByte();
            r.carry = (psw & 1) != 0; r.skip = (psw & 32) != 0;
            state.opaquePsw = static_cast<std::uint8_t>(psw & 0x5c);
            state.foregroundBoundary = find(r.pc) == nullptr;
            emit(Trace::EventKind::InterruptReturn, r.pc, psw);
        } break;
        case Op::SKIT_FSR: r.skip = state.fsr; state.fsr = false; break;
        case Op::SKIT_FST: r.skip = uart.fst; break; // acknowledge atomically after staging
        case Op::STAW_wa: write(0xff00 + x, r.a); break;
        case Op::STAX_H_B: write((r.h * 256 + r.l + r.b) & 65535, r.a); break;
        case Op::XRA_A_C: r.a ^= r.c; break;
        }
    }
};
bool valid(const Trace::State &s) noexcept {
    const auto &r = s.registers;
    const unsigned bytes[] = {r.a,r.b,r.c,r.d,r.e,r.h,r.l,r.v,r.alternateA,
        r.alternateB,r.alternateC,r.alternateD,r.alternateE,r.alternateH,
        r.alternateL,r.alternateV};
    for (auto b : bytes) if (b > 255) return false;
    if (r.v != 0xff || r.alternateV != 0xff || r.ea > 65535 ||
        r.alternateEa > 65535 || r.pc > 65535 ||
        r.sp < 0xff00 || r.sp > 0xffff || s.mkh > 7 || s.eiDeferred > 1 ||
        s.ram[0xc1] >= 48 || s.ram[0xc2] >= 48 || (s.opaquePsw & ~0x5c) != 0)
        return false;
    const auto &p = s.pending;
    if (p.start > s.now) return false;
    if (p.kind == Trace::PendingKind::None) return p.remaining == 0;
    unsigned duration = 0;
    if (p.kind == Trace::PendingKind::Instruction) {
        const auto *i = find(p.address);
        if (!i || r.pc != p.address) return false;
        duration = p.skipped ? i->skipped : i->states;
    } else if (p.kind == Trace::PendingKind::InterruptEntry) {
        if ((p.address != 0x20 && p.address != 0x28) || p.returnPc > 65535) return false;
        duration = 16;
    } else return false;
    return p.remaining <= duration && s.now - p.start == duration - p.remaining;
}
Trace::Status uartStatus(Uart::Status status) noexcept {
    return status == Uart::Status::OutputFull ? Trace::Status::OutputFull
                                            : Trace::Status::PeripheralError;
}
} // namespace

FirmwareAssignerTrace::Result FirmwareAssignerTrace::advanceTo(
    State &state, Uart::State &uart, const Uart::Configuration &configuration,
    std::span<const Request> requests, std::uint64_t target, Events &events,
    Uart::EventBuffer &uartEvents) noexcept {
    Result result{Status::ReachedTarget};
    if (!valid(state) || state.now != uart.now || target < state.now ||
        target > Uart::maximumTime ||
        events.count > events.entries.size()) return {Status::InvalidState};
    std::uint64_t previous = state.now;
    for (const auto &r : requests) {
        if (r.states < previous || r.states > Uart::maximumTime ||
            static_cast<unsigned>(r.kind) > 1)
            return {Status::InvalidState};
        previous = r.states;
    }
    const auto finish = [&](Status status) { result.status = status; return result; };
    for (;;) {
        const auto peripheral = Uart::advanceTo(uart, configuration, state.now, uartEvents);
        if (peripheral != Uart::Status::Ok) return finish(uartStatus(peripheral));
        while (result.consumedRequests < requests.size() &&
               requests[result.consumedRequests].states == state.now) {
            if (events.count == events.entries.size()) return finish(Status::OutputFull);
            const auto &request = requests[result.consumedRequests++];
            if (request.kind == RequestKind::Receive) state.fsr = true;
            else state.fad = true;
            events.entries[events.count++] = {EventKind::Request, state.now,
                static_cast<std::uint16_t>(state.registers.pc),
                static_cast<std::uint16_t>(request.kind), 1};
        }
        if (state.pending.kind != PendingKind::None && state.pending.remaining == 0) {
            Completion commit(state);
            const auto pending = state.pending;
            if (pending.kind == PendingKind::InterruptEntry) {
                commit.pushByte(pending.savedPsw);
                commit.push(pending.returnPc);
                commit.state.registers.pc = pending.address;
                commit.state.registers.skip = false;
                commit.state.opaquePsw &= static_cast<std::uint8_t>(~0x0c);
            } else {
                const auto *instruction = find(pending.address);
                if (!instruction) return finish(Status::InvalidState);
                commit.execute(*instruction, uart);
            }
            if (!commit.valid) return finish(Status::InvalidState);
            if (events.entries.size() - events.count < commit.count)
                return finish(Status::OutputFull);
            Uart::Status writeStatus = Uart::Status::Ok;
            if (commit.txWrite)
                writeStatus = Uart::writeTxBuffer(uart, configuration,
                    static_cast<std::uint8_t>(commit.peripheralValue), uartEvents);
            else if (commit.pcWrite)
                writeStatus = Uart::writePortC(uart,
                    static_cast<std::uint8_t>(commit.peripheralValue), uartEvents);
            if (writeStatus != Uart::Status::Ok) return finish(uartStatus(writeStatus));
            if (pending.kind == PendingKind::Instruction && !pending.skipped &&
                find(pending.address)->op == Op::SKIT_FST)
                (void)Uart::testAndClearFst(uart);
            state = commit.state;
            for (unsigned i = 0; i < commit.count; ++i)
                events.entries[events.count++] = commit.events[i];
            state.pending = {};
            if (pending.kind == PendingKind::Instruction) {
                ++result.completedInstructions;
                if (state.eiDeferred != 0) --state.eiDeferred;
            }
        }
        if (state.pending.kind == PendingKind::None) {
            unsigned vector = 0;
            const bool receive = state.fsr && (state.mkh & 2) == 0;
            const bool transmit = uart.fst && (state.mkh & 4) == 0;
            if (state.interruptEnabled && state.eiDeferred == 0) {
                if (state.fad && (state.mkh & 1) == 0) vector = 0x20;
                else if (receive || transmit) vector = 0x28;
            }
            if (vector != 0) {
                if (events.count == events.entries.size()) return finish(Status::OutputFull);
                const auto &r = state.registers;
                const auto psw = static_cast<std::uint8_t>(state.opaquePsw |
                    (r.carry ? 1 : 0) | (r.skip ? 32 : 0));
                events.entries[events.count++] = {EventKind::InterruptAcceptance, state.now,
                    static_cast<std::uint16_t>(r.pc), static_cast<std::uint16_t>(vector), psw};
                state.pending = {PendingKind::InterruptEntry, state.now, 16, vector,
                                 r.pc, psw, false};
                state.interruptEnabled = false;
                state.foregroundBoundary = false;
                if (vector == 0x20) state.fad = false; // paired INTEIN masked by MKL=FF
                else if ((state.mkh & 4) != 0) state.fsr = false;
                else if ((state.mkh & 2) != 0) (void)Uart::testAndClearFst(uart);
            } else if (state.foregroundBoundary) {
                return finish(Status::AwaitingForeground);
            } else {
                const auto *instruction = find(state.registers.pc);
                if (!instruction) return finish(Status::UnsupportedPath);
                if (result.completedInstructions >= 100000)
                    return finish(Status::InstructionBudget);
                const bool skipped = state.registers.skip;
                state.pending = {PendingKind::Instruction, state.now,
                    skipped ? instruction->skipped : instruction->states,
                    instruction->pc, 0, 0, skipped};
            }
        }
        if (state.now == target) return finish(Status::ReachedTarget);
        const auto remaining = static_cast<std::uint64_t>(state.pending.remaining);
        const auto room = std::numeric_limits<std::uint64_t>::max() - state.now;
        if (remaining > room) return finish(Status::InvalidState);
        auto next = std::min(target, state.now + remaining);
        if (result.consumedRequests < requests.size())
            next = std::min(next, requests[result.consumedRequests].states);
        const auto before = state.now;
        const auto peripheralAdvance = Uart::advanceTo(uart, configuration, next, uartEvents);
        const auto elapsed = uart.now - before;
        state.now = uart.now;
        state.pending.remaining -= static_cast<unsigned>(elapsed);
        if (peripheralAdvance != Uart::Status::Ok)
            return finish(uartStatus(peripheralAdvance));
    }
}
} // namespace youknow
