#include "YouKnowFirmwareAssignerScheduler.h"
#include "YouKnowFirmwareAssignerSchedulerProgram.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace youknow {
// Original A-5 listing and byte-audit provenance:
// https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic1.txt
// ROM SHA256 d43cce5578ee2f16b27c8b06bff30743e3e2dffc796d033811e565d5d578c52e.
// 1753 semantic descriptors:0020..002A,0111..026D,029B..07F8,0800..0D61.
// Cold boot, diagnostic entry026E and cassette entry0D62 are explicit frontiers.
// NEC Stock500375 (April1987),3-4 flags,11-8 overlays,7-4 TXB-empty FST,
// 9-5/7 IRQ acknowledgement/16-state entry,12-21 deferred EI:
// https://drive.google.com/file/d/0B44NKm9yPA1bNDFXZnFrdG1PdDA/view
namespace {
using Trace = FirmwareAssignerScheduler;
namespace Uart = FirmwareUartTrace;
namespace Io = FirmwareAssignerIo;
using firmwareAssignerSchedulerDetail::Instruction;
using firmwareAssignerSchedulerDetail::Op;
const Instruction *find(unsigned pc) noexcept {
    const auto &p = firmwareAssignerSchedulerDetail::program;
    const auto it = std::lower_bound(p.begin(), p.end(), pc,
        [](const auto &i, unsigned address) { return i.pc < address; });
    return it != p.end() && it->pc == pc ? &*it : nullptr;
}
unsigned pair(unsigned high, unsigned low) noexcept { return high * 256 + low; }
void pairSet(unsigned &high, unsigned &low, unsigned value) noexcept {
    high = (value >> 8) & 255; low = value & 255;
}
std::uint8_t psw(const Trace::Registers &r) noexcept {
    return static_cast<std::uint8_t>((r.carry ? 1 : 0) | (r.l0 ? 4 : 0) |
        (r.l1 ? 8 : 0) | (r.halfCarry ? 16 : 0) | (r.skip ? 32 : 0) |
        (r.zero ? 64 : 0));
}
void restorePsw(Trace::Registers &r, unsigned p) noexcept {
    r.carry = (p & 1) != 0; r.l0 = (p & 4) != 0; r.l1 = (p & 8) != 0;
    r.halfCarry = (p & 16) != 0; r.skip = (p & 32) != 0; r.zero = (p & 64) != 0;
}
struct Completion {
    Trace::State state;
    const Trace::Configuration &configuration;
    const Trace::Tables &tables;
    const Io::Inputs &inputs;
    std::array<Trace::Event, 8> events{};
    unsigned count = 0;
    Trace::Status error = Trace::Status::ReachedTarget;
    bool txWrite = false, pcWrite = false;
    unsigned peripheralValue = 0;
    Completion(const Trace::State &s, const Trace::Configuration &c,
               const Trace::Tables &t, const Io::Inputs &in)
        : state(s), configuration(c), tables(t), inputs(in) {}
    void emit(Trace::EventKind kind, unsigned address, unsigned value) noexcept {
        if (count == events.size()) { error = Trace::Status::InvalidState; return; }
        events[count++] = {kind, state.now,
            static_cast<std::uint16_t>(state.pending.address),
            static_cast<std::uint16_t>(address), static_cast<std::uint16_t>(value)};
    }
    unsigned read(unsigned address) noexcept {
        if (address >= 0xff00 && address <= 0xffff) return state.ram[address & 255];
        if (address >= 0x2000 && address < 0x2800) {
            if (!state.patchRamAvailable) { error = Trace::Status::UnavailableMemory; return 0; }
            return state.patchRam[address - 0x2000];
        }
        if (address >= 8 && address < 16) return 1u << (address - 8);
        if (address >= 16 && address < 24) return 255u ^ (1u << (address - 16));
        if (address >= 0x30 && address < 0x40) return Io::panelParameterAddress[address - 0x30];
        if (address >= 0x40 && address < 0x50) return tables.channelDisplay[address - 0x40];
        if (address >= 0x50 && address < 0x60) return tables.digitDisplay[address - 0x50];
        if (address >= 0x60 && address < 0x79) return tables.transposeDisplay[address - 0x60];
        error = Trace::Status::UnavailableMemory;
        return 0;
    }
    void write(unsigned address, unsigned value, bool stack = false) noexcept {
        value &= 255;
        if (address >= 0xff00 && address <= 0xffff) {
            state.ram[address & 255] = static_cast<std::uint8_t>(value);
            emit(stack ? Trace::EventKind::StackWrite : Trace::EventKind::RamWrite,
                 address, value);
        } else if (!stack && address == 0x1fff) {
            if (!Io::matrixSelectionSupported(static_cast<std::uint8_t>(value))) {
                error = Trace::Status::UnsupportedPath; return;
            }
            state.io.muxLatch = static_cast<std::uint8_t>(value);
            emit(Trace::EventKind::MuxWrite, address, value);
        } else if (!stack && address >= 0x2000 && address < 0x2800) {
            if (!state.patchRamAvailable) { error = Trace::Status::UnavailableMemory; return; }
            state.patchRam[address - 0x2000] = static_cast<std::uint8_t>(value);
            emit(Trace::EventKind::PatchWrite, address, value);
        } else error = Trace::Status::UnavailableMemory;
    }
    void pushByte(unsigned value) noexcept {
        auto &sp = state.registers.sp;
        if (sp <= 0xff00 || sp > 0xffff) { error = Trace::Status::InvalidState; return; }
        write(--sp, value, true);
    }
    unsigned popByte() noexcept {
        auto &sp = state.registers.sp;
        if (sp < 0xff00 || sp >= 0xffff) { error = Trace::Status::InvalidState; return 0; }
        return read(sp++);
    }
    void push(unsigned value) noexcept { pushByte(value >> 8); pushByte(value); }
    unsigned pop() noexcept { const auto lo = popByte(); return lo + 256 * popByte(); }
    unsigned add(unsigned left, unsigned right) noexcept {
        auto &r = state.registers;
        const auto sum = left + right;
        r.carry = sum > 255; r.halfCarry = (left & 15) + (right & 15) > 15;
        r.zero = (sum & 255) == 0;
        return sum & 255;
    }
    unsigned subtract(unsigned left, unsigned right, unsigned borrow = 0) noexcept {
        auto &r = state.registers;
        const auto value = (left - right - borrow) & 255;
        r.carry = left < right + borrow;
        r.halfCarry = (left & 15) < (right & 15) + borrow;
        r.zero = value == 0;
        return value;
    }
    unsigned logic(unsigned value) noexcept {
        state.registers.zero = (value & 255) == 0; return value & 255;
    }
    void test(unsigned value, bool on) noexcept {
        logic(value); state.registers.skip = on ? value != 0 : value == 0;
    }
    unsigned increment(unsigned value, bool down = false) noexcept {
        const bool carry = state.registers.carry;
        const auto result = down ? subtract(value, 1) : add(value, 1);
        state.registers.skip = state.registers.carry;
        state.registers.carry = carry;
        return result;
    }
    void compare(unsigned left, unsigned right, char kind) noexcept {
        subtract(left, right, kind == '>' ? 1 : 0);
        auto &r = state.registers;
        r.skip = kind == '=' ? left == right : kind == '!' ? left != right :
                 kind == '<' ? left < right : left > right;
    }
    void writePf(unsigned value) noexcept {
        state.io.portF = static_cast<std::uint8_t>(value);
        emit(Trace::EventKind::PortFWrite, 5, value);
    }
    void execute(const Instruction &i, const Uart::State &uart) noexcept {
        auto &r = state.registers;
        const auto x = i.argument, y = i.second;
        r.pc += i.bytes;
        r.skip = false;
        // Different instruction groups break overlay sequences; SK suppresses
        // instruction effects but does not invent a new overlay flag.
        if (i.op != Op::MVI_A_xx) r.l1 = false;
        if (i.op != Op::LXI_H_w) r.l0 = false;
        if (state.pending.skipped) return;
        switch (i.op) {
        case Op::NOP: break;
        case Op::ADDNC_A_C: r.a = add(r.a, r.c); r.skip = !r.carry; break;
        case Op::ADDW_wa: r.a = add(r.a, read(0xff00 + x)); break;
        case Op::ADI_A_xx: r.a = add(r.a, x); break;
        case Op::ADI_E_xx: r.e = add(r.e, x); break;
        case Op::ADI_PF_xx: writePf(add(state.io.portF, x)); break;
        case Op::SUBNBX_H: r.a = subtract(r.a, read(pair(r.h, r.l))); r.skip = !r.carry; break;
        case Op::SUBW_wa: r.a = subtract(r.a, read(0xff00 + x)); break;
        case Op::SUI_A_xx: r.a = subtract(r.a, x); break;
        case Op::SUINB_A_xx: r.a = subtract(r.a, x); r.skip = !r.carry; break;
        case Op::ANAW_wa: r.a = logic(r.a & read(0xff00 + x)); break;
        case Op::ANA_A_C: r.a = logic(r.a & r.c); break;
        case Op::ANI_A_xx: r.a = logic(r.a & x); break;
        case Op::ANIW_wa_xx: write(0xff00 + x, logic(read(0xff00 + x) & y)); break;
        case Op::ANI_MKH_xx:
            state.mkh = logic(state.mkh & x); emit(Trace::EventKind::MaskWrite, 0, state.mkh); break;
        case Op::ORI_MKH_xx:
            state.mkh = logic(state.mkh | x); emit(Trace::EventKind::MaskWrite, 0, state.mkh); break;
        case Op::ORAW_wa: r.a = logic(r.a | read(0xff00 + x)); break;
        case Op::ORAX_D: r.a = logic(r.a | read(pair(r.d, r.e))); break;
        case Op::ORAX_H: r.a = logic(r.a | read(pair(r.h, r.l))); break;
        case Op::ORA_A_B: r.a = logic(r.a | r.b); break;
        case Op::ORA_A_C: r.a = logic(r.a | r.c); break;
        case Op::ORI_A_xx: r.a = logic(r.a | x); break;
        case Op::ORIW_wa_xx: write(0xff00 + x, logic(read(0xff00 + x) | y)); break;
        case Op::XRAW_wa: r.a = logic(r.a ^ read(0xff00 + x)); break;
        case Op::XRAX_H: r.a = logic(r.a ^ read(pair(r.h, r.l))); break;
        case Op::XRA_A_C: r.a = logic(r.a ^ r.c); break;
        case Op::XRI_A_xx: r.a = logic(r.a ^ x); break;
        case Op::BIT_0_wa: case Op::BIT_1_wa: case Op::BIT_2_wa: case Op::BIT_3_wa:
        case Op::BIT_4_wa: case Op::BIT_5_wa: case Op::BIT_6_wa: case Op::BIT_7_wa:
            r.skip = (read(0xff00 + x) & (1u << (static_cast<unsigned>(i.op) -
                static_cast<unsigned>(Op::BIT_0_wa)))) != 0; break;
        case Op::ONAW_wa: test(r.a & read(0xff00 + x), true); break;
        case Op::ONIW_wa_xx: test(read(0xff00 + x) & y, true); break;
        case Op::OFFIW_wa_xx: test(read(0xff00 + x) & y, false); break;
        case Op::OFFI_A_xx: test(r.a & x, false); break;
        case Op::OFFI_MKH_xx: test(state.mkh & x, false); break;
        case Op::ONI_A_xx: test(r.a & x, true); break;
        case Op::ONI_B_xx: test(r.b & x, true); break;
        case Op::ONI_ANM_xx: test(state.io.anm & x, true); break;
        case Op::ONI_PC_xx: test(Io::readPc(inputs, uart.portC) & x, true); break;
        case Op::EQAW_wa: compare(r.a, read(0xff00 + x), '='); break;
        case Op::EQIW_wa_xx: compare(read(0xff00 + x), y, '='); break;
        case Op::EQI_A_xx: compare(r.a, x, '='); break;
        case Op::EQI_B_xx: compare(r.b, x, '='); break;
        case Op::EQI_C_xx: compare(r.c, x, '='); break;
        case Op::NEAW_wa: compare(r.a, read(0xff00 + x), '!'); break;
        case Op::NEIW_wa_xx: compare(read(0xff00 + x), y, '!'); break;
        case Op::NEA_A_C: compare(r.a, r.c, '!'); break;
        case Op::NEI_A_xx: compare(r.a, x, '!'); break;
        case Op::NEI_C_xx: compare(r.c, x, '!'); break;
        case Op::LTI_A_xx: compare(r.a, x, '<'); break;
        case Op::LTI_B_xx: compare(r.b, x, '<'); break;
        case Op::LTIW_wa_xx: compare(read(0xff00 + x), y, '<'); break;
        case Op::GTI_A_xx: compare(r.a, x, '>'); break;
        case Op::GTI_B_xx: compare(r.b, x, '>'); break;
        case Op::GTIW_wa_xx: compare(read(0xff00 + x), y, '>'); break;
        case Op::INRW_wa: write(0xff00 + x, increment(read(0xff00 + x))); break;
        case Op::DCRW_wa: write(0xff00 + x, increment(read(0xff00 + x), true)); break;
        case Op::INR_A: r.a = increment(r.a); break;
        case Op::INR_B: r.b = increment(r.b); break;
        case Op::INR_C: r.c = increment(r.c); break;
        case Op::DCR_A: r.a = increment(r.a, true); break;
        case Op::DCR_B: r.b = increment(r.b, true); break;
        case Op::DCR_C: r.c = increment(r.c, true); break;
        case Op::INX_DE: pairSet(r.d, r.e, pair(r.d, r.e) + 1); break;
        case Op::INX_HL: pairSet(r.h, r.l, pair(r.h, r.l) + 1); break;
        case Op::DCX_DE: pairSet(r.d, r.e, pair(r.d, r.e) - 1); break;
        case Op::DCX_HL: pairSet(r.h, r.l, pair(r.h, r.l) - 1); break;
        case Op::NEGA: r.a = (0u - r.a) & 255; break;
        case Op::SLR_A: case Op::SLRC_A:
            r.carry = (r.a & 1) != 0; r.a >>= 1;
            if (i.op == Op::SLRC_A) r.skip = r.carry; break;
        case Op::SLR_B: r.carry = (r.b & 1) != 0; r.b >>= 1; break;
        case Op::SLRC_C: r.carry = (r.c & 1) != 0; r.c >>= 1; r.skip = r.carry; break;
        case Op::SLL_A: r.carry = (r.a & 128) != 0; r.a = (r.a << 1) & 255; break;
        case Op::SLLC_C:
            r.carry = (r.c & 128) != 0; r.c = (r.c << 1) & 255; r.skip = r.carry; break;
        case Op::RLR_C: {
            const bool carry = r.carry;
            r.carry = (r.c & 1) != 0; r.c = (r.c >> 1) | (carry ? 128 : 0);
        } break;
        case Op::DSLL_EA: r.carry = (r.ea & 32768) != 0; r.ea = (r.ea << 1) & 65535; break;
        case Op::DSLR_EA: r.carry = (r.ea & 1) != 0; r.ea >>= 1; break;
        case Op::CLC: r.carry = false; break;
        case Op::STC: r.carry = true; break;
        case Op::SK_HC: r.skip = r.halfCarry; break;
        case Op::SK_Z: r.skip = r.zero; break;
        case Op::SKIT_FSR: r.skip = state.fsr; state.fsr = false; break;
        case Op::SKIT_FST: r.skip = uart.fst; break;
        case Op::SKNIT_ER: r.skip = !state.receiveError; state.receiveError = false; break;
        case Op::CALF: case Op::CALL_w: push(r.pc); r.pc = x; break;
        case Op::JMP_w: case Op::JR: case Op::JRE: r.pc = x; break;
        case Op::RET: r.pc = pop(); break;
        case Op::RETI: {
            r.pc = pop(); const auto saved = popByte(); restorePsw(r, saved);
            emit(Trace::EventKind::InterruptReturn, r.pc, saved);
        } break;
        case Op::EI: state.interruptEnabled = true; state.eiDeferred = 2; break;
        case Op::EXA:
            std::swap(r.a, r.alternateA); std::swap(r.ea, r.alternateEa);
            std::swap(r.v, r.alternateV); break;
        case Op::EXX:
            std::swap(r.b, r.alternateB); std::swap(r.c, r.alternateC);
            std::swap(r.d, r.alternateD); std::swap(r.e, r.alternateE);
            std::swap(r.h, r.alternateH); std::swap(r.l, r.alternateL); break;
        case Op::PUSH_BC: push(pair(r.b, r.c)); break;
        case Op::PUSH_DE: push(pair(r.d, r.e)); break;
        case Op::PUSH_HL: push(pair(r.h, r.l)); break;
        case Op::POP_BC: { const auto v = pop(); pairSet(r.b, r.c, v); } break;
        case Op::POP_DE: { const auto v = pop(); pairSet(r.d, r.e, v); } break;
        case Op::POP_HL: { const auto v = pop(); pairSet(r.h, r.l, v); } break;
        case Op::LXI_D_w: pairSet(r.d, r.e, x); break;
        case Op::LXI_EA_s: r.ea = x; break;
        case Op::LXI_H_w:
            if (!r.l0) { pairSet(r.h, r.l, x); r.l0 = true; } break;
        case Op::MVI_A_xx: if (!r.l1) { r.a = x; r.l1 = true; } break;
        case Op::MVI_B_xx: r.b = x; break;
        case Op::MVI_C_xx: r.c = x; break;
        case Op::MVI_D_xx: r.d = x; break;
        case Op::MVI_E_xx: r.e = x; break;
        case Op::MVI_H_xx: r.h = x; break;
        case Op::MVIW_wa_xx: write(0xff00 + x, y); break;
        case Op::MVI_PF_xx: writePf(x); break;
        case Op::MVI_ANM_xx:
            if (!Io::writeAnm(state.io, static_cast<std::uint8_t>(x), configuration.anmWritePhase))
                error = Trace::Status::UnsupportedPath;
            emit(Trace::EventKind::AnmWrite, 8, x); break;
        case Op::DMOV_HL_EA: pairSet(r.h, r.l, r.ea); break;
        case Op::MOV_A_B: r.a = r.b; break;
        case Op::MOV_A_C: r.a = r.c; break;
        case Op::MOV_A_D: r.a = r.d; break;
        case Op::MOV_A_E: r.a = r.e; break;
        case Op::MOV_A_L: r.a = r.l; break;
        case Op::MOV_A_EAH: r.a = r.ea >> 8; break;
        case Op::MOV_A_EAL: r.a = r.ea & 255; break;
        case Op::MOV_B_A: r.b = r.a; break;
        case Op::MOV_C_A: r.c = r.a; break;
        case Op::MOV_E_A: r.e = r.a; break;
        case Op::MOV_L_A: r.l = r.a; break;
        case Op::MOV_EAH_A: r.ea = (r.ea & 255) | (r.a << 8); break;
        case Op::MOV_EAL_A: r.ea = (r.ea & 65280) | r.a; break;
        case Op::MOV_A_PA: r.a = Io::readPa(inputs, state.io.muxLatch); break;
        case Op::MOV_A_PC: r.a = Io::readPc(inputs, uart.portC); break;
        case Op::MOV_A_CR0: case Op::MOV_A_CR1: case Op::MOV_A_CR2: case Op::MOV_A_CR3:
            r.a = state.io.conversion[static_cast<unsigned>(i.op) - static_cast<unsigned>(Op::MOV_A_CR0)]; break;
        case Op::MOV_A_RXB:
            r.a = state.rxBuffer; state.rxBufferFull = false;
            emit(Trace::EventKind::ReceiveRead, 0xd9, r.a); break;
        case Op::MOV_PC_A:
            pcWrite = true; peripheralValue = r.a;
            emit(Trace::EventKind::PortCWrite, 2, r.a); break;
        case Op::MOV_PB_A:
            state.io.portB = static_cast<std::uint8_t>(r.a);
            emit(Trace::EventKind::PortBWrite, 1, r.a); break;
        case Op::MOV_TXB_A:
            txWrite = true; peripheralValue = r.a;
            emit(Trace::EventKind::TxBufferWrite, 0xd8, r.a); break;
        case Op::LDAW_wa: r.a = read(0xff00 + x); break;
        case Op::LDAX_D: r.a = read(pair(r.d, r.e)); break;
        case Op::LDAX_Dp: r.a = read(pair(r.d, r.e)); pairSet(r.d, r.e, pair(r.d, r.e) + 1); break;
        case Op::LDAX_H: r.a = read(pair(r.h, r.l)); break;
        case Op::LDAX_Hp: r.a = read(pair(r.h, r.l)); pairSet(r.h, r.l, pair(r.h, r.l) + 1); break;
        case Op::LDAX_Hm: r.a = read(pair(r.h, r.l)); pairSet(r.h, r.l, pair(r.h, r.l) - 1); break;
        case Op::LDAX_H_A: r.a = read((pair(r.h, r.l) + r.a) & 65535); break;
        case Op::LDAX_H_B: r.a = read((pair(r.h, r.l) + r.b) & 65535); break;
        case Op::LDAX_H_xx: r.a = read((pair(r.h, r.l) + x) & 65535); break;
        case Op::STAW_wa: write(0xff00 + x, r.a); break;
        case Op::STAX_D: write(pair(r.d, r.e), r.a); break;
        case Op::STAX_Dp: write(pair(r.d, r.e), r.a); pairSet(r.d, r.e, pair(r.d, r.e) + 1); break;
        case Op::STAX_H: write(pair(r.h, r.l), r.a); break;
        case Op::STAX_Hp: write(pair(r.h, r.l), r.a); pairSet(r.h, r.l, pair(r.h, r.l) + 1); break;
        case Op::STAX_H_B: write((pair(r.h, r.l) + r.b) & 65535, r.a); break;
        case Op::STAX_H_xx: write((pair(r.h, r.l) + x) & 65535, r.a); break;
        }
        if (r.pc == 0x0111 && (i.op == Op::JMP_w || i.op == Op::JR || i.op == Op::JRE)) {
            ++state.foregroundPasses;
            emit(Trace::EventKind::ForegroundPass, r.pc, 0);
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
        r.alternateEa > 65535 || r.pc > 65535 || r.sp < 0xff00 || r.sp > 0xffff ||
        s.mkh > 7 || s.eiDeferred > 1 || s.ram[0xc1] >= 48 || s.ram[0xc2] >= 48 ||
        !Io::valid(s.io) || !Io::matrixSelectionSupported(s.io.muxLatch)) return false;
    const auto &p = s.pending;
    if (p.start > s.now) return false;
    if (p.kind == Trace::PendingKind::None) return p.remaining == 0;
    unsigned duration = 0;
    if (p.kind == Trace::PendingKind::Instruction) {
        const auto *i = find(p.address);
        if (!i || r.pc != p.address) return false;
        duration = p.skipped ? i->skipped : i->states;
    } else if (p.kind == Trace::PendingKind::InterruptEntry) {
        if ((p.address != 0x20 && p.address != 0x28) || p.returnPc > 65535 ||
            (p.savedPsw & 0x82) != 0) return false;
        duration = 16;
    } else return false;
    return p.remaining <= duration && s.now - p.start == duration - p.remaining;
}
Trace::Status uartStatus(Uart::Status status) noexcept {
    return status == Uart::Status::OutputFull ? Trace::Status::OutputFull
                                            : Trace::Status::PeripheralError;
}
} // namespace

FirmwareAssignerScheduler::Result FirmwareAssignerScheduler::advanceTo(
    State &state, Uart::State &uart, const Configuration &configuration,
    const Tables &tables, const Io::Inputs &inputs, Span<const InputEvent> inputEvents,
    std::uint64_t target, Events &events, Uart::EventBuffer &uartEvents) noexcept {
    Result result{Status::ReachedTarget};
    if (!valid(state) || !Io::valid(inputs) || !Io::valid(configuration.anmWritePhase) ||
        state.now != uart.now || target < state.now || target > Uart::maximumTime ||
        events.count > events.entries.size()) return {Status::InvalidState};
    std::uint64_t previous = state.now;
    for (const auto &event : inputEvents) {
        if (event.states < previous || event.states > Uart::maximumTime ||
            static_cast<unsigned>(event.kind) > 1) return {Status::InvalidState};
        previous = event.states;
    }
    const auto finish = [&](Status status) { result.status = status; return result; };
    for (;;) {
        const auto peripheral = Uart::advanceTo(uart, configuration.uart, state.now, uartEvents);
        if (peripheral != Uart::Status::Ok) return finish(uartStatus(peripheral));
        if (state.io.statesUntilConversion == 0) {
            if (events.count == events.entries.size()) return finish(Status::OutputFull);
            Io::Conversion conversion;
            if (!Io::completeConversion(state.io, inputs, conversion)) return finish(Status::InvalidState);
            events.entries[events.count++] = {EventKind::AdcConversion, state.now,
                static_cast<std::uint16_t>(state.registers.pc), conversion.inputIndex, conversion.value};
        }
        while (result.consumedInputs < inputEvents.size() &&
               inputEvents[result.consumedInputs].states == state.now) {
            const auto &input = inputEvents[result.consumedInputs];
            if (input.kind == InputKind::ByteReady && state.rxBufferFull)
                return finish(Status::ReceiveOverrun);
            if (events.count == events.entries.size()) return finish(Status::OutputFull);
            if (input.kind == InputKind::ByteReady) {
                state.rxBuffer = input.value; state.rxBufferFull = true; state.fsr = true;
            } else state.receiveError = input.value != 0;
            ++result.consumedInputs;
            events.entries[events.count++] = {EventKind::ReceiveReady, state.now,
                static_cast<std::uint16_t>(state.registers.pc),
                static_cast<std::uint16_t>(input.kind), input.value};
        }
        if (state.pending.kind != PendingKind::None && state.pending.remaining == 0) {
            Completion commit(state, configuration, tables, inputs);
            const auto pending = state.pending;
            if (pending.kind == PendingKind::InterruptEntry) {
                commit.pushByte(pending.savedPsw); commit.push(pending.returnPc);
                auto &r = commit.state.registers;
                r.pc = pending.address; r.skip = false; r.l0 = false; r.l1 = false;
            } else {
                const auto *instruction = find(pending.address);
                if (!instruction) return finish(Status::InvalidState);
                commit.execute(*instruction, uart);
            }
            if (commit.error != Status::ReachedTarget) return finish(commit.error);
            if (events.entries.size() - events.count < commit.count) return finish(Status::OutputFull);
            Uart::Status writeStatus = Uart::Status::Ok;
            if (commit.txWrite)
                writeStatus = Uart::writeTxBuffer(uart, configuration.uart,
                    static_cast<std::uint8_t>(commit.peripheralValue), uartEvents);
            else if (commit.pcWrite)
                writeStatus = Uart::writePortC(uart,
                    static_cast<std::uint8_t>(commit.peripheralValue), uartEvents);
            if (writeStatus != Uart::Status::Ok) return finish(uartStatus(writeStatus));
            if (pending.kind == PendingKind::Instruction && !pending.skipped &&
                find(pending.address)->op == Op::SKIT_FST) (void)Uart::testAndClearFst(uart);
            state = commit.state;
            for (unsigned i = 0; i < commit.count; ++i) events.entries[events.count++] = commit.events[i];
            state.pending = {};
            if (pending.kind == PendingKind::Instruction) {
                ++result.completedInstructions;
                if (state.eiDeferred != 0) --state.eiDeferred;
            }
            if (configuration.inputService != nullptr)
                for (unsigned i = 0; i < commit.count; ++i) {
                    const auto& event = commit.events[i];
                    if (event.kind == EventKind::ForegroundPass
                        || (event.kind == EventKind::RamWrite
                            && (event.address == 0xffa6 || event.address == 0xffa8))) {
                        configuration.inputService(configuration.inputServiceContext, state, event);
                        break;
                    }
                }
        }
        if (state.pending.kind == PendingKind::None) {
            unsigned vector = 0;
            const bool receive = state.fsr && (state.mkh & 2) == 0;
            const bool transmit = uart.fst && (state.mkh & 4) == 0;
            if (state.interruptEnabled && state.eiDeferred == 0) {
                if (state.io.request && (state.mkh & 1) == 0) vector = 0x20;
                else if (receive || transmit) vector = 0x28;
            }
            if (vector != 0) {
                if (events.count == events.entries.size()) return finish(Status::OutputFull);
                const auto &r = state.registers;
                const auto saved = psw(r);
                events.entries[events.count++] = {EventKind::InterruptAcceptance, state.now,
                    static_cast<std::uint16_t>(r.pc), static_cast<std::uint16_t>(vector), saved};
                state.pending = {PendingKind::InterruptEntry, state.now, 16, vector, r.pc, saved, false};
                state.interruptEnabled = false;
                if (vector == 0x20) state.io.request = false; //paired INTEIN masked by MKL=FF
                else if ((state.mkh & 4) != 0) state.fsr = false;
                else if ((state.mkh & 2) != 0) (void)Uart::testAndClearFst(uart);
            } else {
                const auto *instruction = find(state.registers.pc);
                if (!instruction) return finish(Status::UnsupportedPath);
                if (result.completedInstructions >= 100000) return finish(Status::InstructionBudget);
                const bool skipped = state.registers.skip;
                state.pending = {PendingKind::Instruction, state.now,
                    skipped ? instruction->skipped : instruction->states, instruction->pc, 0, 0, skipped};
            }
        }
        if (state.now == target) return finish(Status::ReachedTarget);
        auto next = std::min(target, state.now + state.pending.remaining);
        next = std::min(next, state.now + state.io.statesUntilConversion);
        if (result.consumedInputs < inputEvents.size())
            next = std::min(next, inputEvents[result.consumedInputs].states);
        const auto before = state.now;
        const auto peripheralAdvance = Uart::advanceTo(uart, configuration.uart, next, uartEvents);
        const auto elapsed = uart.now - before;
        state.now = uart.now;
        state.pending.remaining -= static_cast<unsigned>(elapsed);
        state.io.statesUntilConversion -= static_cast<std::uint16_t>(elapsed);
        if (peripheralAdvance != Uart::Status::Ok) return finish(uartStatus(peripheralAdvance));
    }
}
} // namespace youknow
