#include "YouKnowFirmwareSerialTrace.h"
#include "YouKnowFirmwareSerialProgram.h"
#include "YouKnowFirmwareProgram.h"
#include "YouKnowFirmwareInstructionIndex.h"
#include <algorithm>
#include <limits>

namespace youknow {
namespace {
using firmwareTraceDetail::Instruction;
using firmwareTraceDetail::Op;
using Stream = FirmwareSerialTrace;
struct StreamCpu {
    // Only completed entries are read, and completeInstruction resets count
    // before each instruction. Do not clear the unused 512-event audit buffer
    // on every audio interval; it is not persistent emulated machine state.
    struct Scratch {
        FirmwareControlTrace::State finalState;
        std::array<FirmwareControlTrace::Event, 512> events;
        std::size_t count = 0;
        std::uint32_t states = 0;
    } result;
    const FirmwareControlTrace::Tables &tables;
    const Stream::Tables *parameterTables = nullptr;
    unsigned a = 0, b = 0, c = 0, d = 0, e = 0, h = 0, l = 0, ea = 0, pa = 0xff, pb = 0, portc = 0, portf = 0;
    unsigned pc = 0x02ec, executingAddress = 0, storeStates = 0;
    bool carry = false, skip = false, error = false;
    unsigned ordinal = 0;
    unsigned sp = 0xffff, v = 0xff, alternateV = 0xff;
    struct Write {
        unsigned address, value;
    };
    std::array<Write, 8> externalWrites{}, stackWrites{};
    unsigned externalCount = 0, stackCount = 0;
    void pushByte(unsigned value) {
        if (sp <= 0xff00 || sp > 0xffff || stackCount == stackWrites.size()) {
            error = true;
            return;
        }
        --sp;
        result.finalState.ram[sp & 255] = value & 255;
        stackWrites[stackCount++] = {sp, value & 255};
    }
    unsigned popByte() {
        if (sp < 0xff00 || sp >= 0xffff) {
            error = true;
            return 0;
        }
        return result.finalState.ram[(sp++) & 255];
    }
    unsigned alternateA = 0, alternateEa = 0, alternateB = 0, alternateC = 0, alternateD = 0,
             alternateE = 0, alternateH = 0, alternateL = 0;
    explicit StreamCpu(const FirmwareControlTrace::State &state,
                       const FirmwareControlTrace::Tables &lookup,
                       const Stream::Tables *parameters = nullptr)
        : tables(lookup), parameterTables(parameters) {
        result.finalState = state;
    }
    unsigned bc() const { return b * 256 + c; }
    unsigned de() const { return d * 256 + e; }
    unsigned hl() const { return h * 256 + l; }
    void bc(unsigned v) {
        b = (v >> 8) & 255;
        c = v & 255;
    }
    void de(unsigned v) {
        d = (v >> 8) & 255;
        e = v & 255;
    }
    void hl(unsigned v) {
        h = (v >> 8) & 255;
        l = v & 255;
    }
    void push(unsigned value) {
        pushByte(value >> 8);
        pushByte(value);
    }
    unsigned pop() {
        unsigned low = popByte();
        return low + 256 * popByte();
    }
    unsigned read(unsigned address) {
        if (address >= 0xff00 && address <= 0xffff)
            return result.finalState.ram[address & 255];
        // Raw B-2 bytes verify the listing's inverted-table address is a typo:
        // 0008/9 contain EI/RETI; six masks begin at000A, not0008.
        if (address >= 0x0a && address <= 0x0f)
            return (~(1u << (address - 0x0a))) & 255;
        if (address >= 0x12 && address <= 0x17)
            return 1u << (address - 0x12);
        // Semantic target addresses from the bounded normal-command dispatch.
        // TABLE still computes its byte address; JB separately consumes BC.
        if (address >= 0x00da && address < 0x0106) {
            constexpr std::array<unsigned, 22> handlers {
                0x014f, 0x0184, 0x0198, 0x0200, 0x023a, 0x01ab,
                0x01c8, 0x01cc, 0x01d0, 0x01af, 0x01b3, 0x01b7,
                0x01d4, 0x01e2, 0x01ed, 0x01c4, 0x01e9, 0x01c0,
                0x0223, 0x0227, 0x01a7, 0x0246
            };
            const unsigned offset = address - 0x00da;
            return (handlers[offset / 2] >> (8 * (offset & 1))) & 255;
        }
        if (parameterTables != nullptr) {
            if (address >= 0x0a80 && address < 0x0b00)
                return parameterTables->dcoLfoDepth[address - 0x0a80];
            if (address >= 0x0b30 && address < 0x0b40) {
                const unsigned offset = address - 0x0b30;
                return (parameterTables->delayFade[offset / 2] >> (8 * (offset & 1))) & 255;
            }
            if (address >= 0x0c60 && address < 0x0d60) {
                const unsigned offset = address - 0x0c60;
                return (parameterTables->lfoRate[offset / 2] >> (8 * (offset & 1))) & 255;
            }
            if (address >= 0x0d60 && address < 0x0e60) {
                const unsigned offset = address - 0x0d60;
                return (parameterTables->decayRelease[offset / 2] >> (8 * (offset & 1))) & 255;
            }
        }
        if (address >= 0x0a00 && address < 0x0a80)
            return tables.portamento[address - 0x0a00];
        if (address >= 0x0b50 && address < 0x0b56) {
            constexpr std::array<unsigned, 6> map{0x20, 0x21, 0x22, 0x10, 0x11, 0x12};
            return map[address - 0x0b50];
        }
        if (address >= 0x0b56 && address < 0x0b5c)
            return 0x36 + 0x40 * ((address - 0x0b56) % 3);
        if (address >= 0x0b60 && address < 0x0c60) {
            unsigned offset = address - 0x0b60;
            return (tables.attack[offset / 2] >> (8 * (offset & 1))) & 255;
        }
        if (address >= 0x0e60 && address < 0x0f30) {
            unsigned offset = address - 0x0e60;
            return (tables.pitchCv[offset / 2] >> (8 * (offset & 1))) & 255;
        }
        if (address >= 0x0f30 && address < 0x1000) {
            unsigned offset = address - 0x0f30;
            return (tables.pitchDivider[offset / 2] >> (8 * (offset & 1))) & 255;
        }
        error = true;
        return 0;
    }
    unsigned word(unsigned address) { return read(address) + 256 * read((address + 1) & 65535); }
    void write(unsigned address, unsigned value) {
        if (address >= 0xff00 && address <= 0xffff) {
            result.finalState.ram[address & 255] = static_cast<std::uint8_t>(value);
            emit(FirmwareControlTrace::EventKind::RamByte, executingAddress, value & 255,
                 address & 255);
        } else if (address == 0x3000 || (address >= 0x1000 && address <= 0x2300)) {
            if (externalCount == externalWrites.size())
                error = true;
            else
                externalWrites[externalCount++] = {address, value & 255};
        } else
            error = true;
    }
    void emit(FirmwareControlTrace::EventKind kind, unsigned address, unsigned value,
              unsigned card = 255, unsigned bits = 0) {
        if (result.count == result.events.size()) {
            error = true;
            return;
        }
        const bool ramStore = kind == FirmwareControlTrace::EventKind::RamByte ||
                              kind == FirmwareControlTrace::EventKind::Envelope ||
                              kind == FirmwareControlTrace::EventKind::Portamento;
        result.events[result.count++] = {kind,
                                         ramStore ? storeStates : result.states,
                                         static_cast<std::uint16_t>(address),
                                         static_cast<std::uint16_t>(value),
                                         static_cast<std::uint8_t>(card),
                                         static_cast<std::uint8_t>(bits)};
    }
    void storeWord(unsigned address, unsigned value, unsigned instruction) {
        write(address, value);
        write((address + 1) & 65535, value >> 8);
        if (instruction == 0x0590) {
            unsigned card = (address - 0xff27) / 2;
            unsigned bits = ((read(0xff07) >> card) & 1) | (((read(0xff08) >> card) & 1) << 1) |
                            (((read(0xff33) >> card) & 1) << 2);
            emit(FirmwareControlTrace::EventKind::Envelope, instruction, value, card, bits);
        } else if (instruction == 0x03ec)
            emit(FirmwareControlTrace::EventKind::Portamento, instruction, value,
                 (address - 0xff71) / 2);
    }
    unsigned add(unsigned x, unsigned y, unsigned mask) {
        unsigned v = x + y;
        carry = v > mask;
        return v & mask;
    }
    unsigned sub(unsigned x, unsigned y, unsigned mask) {
        carry = x < y;
        return (x - y) & mask;
    }
    // NEC April1987 printed12-23/27/35/40: comparisons update CY
    // from subtraction borrow; GT tests lhs-(rhs+1), without wrapping rhs.
    void execute(const Instruction &i) {
        const unsigned x = i.argument, y = i.second, address = i.address;
        executingAddress = address;
        storeStates = result.states + i.states;
        pc += i.bytes;
        switch (i.op) {
        case Op::ADDNCW_wa:
            a = add(a, read(0xff00 + x), 255);
            skip = !carry;
            break;
        case Op::ADDNC_A_B:
            a = add(a, b, 255);
            skip = !carry;
            break;
        case Op::ADINC_A_xx:
            a = add(a, x, 255);
            skip = !carry;
            break;
        case Op::ADI_A_xx:
            a = add(a, x, 255);
            break;
        case Op::ANAW_wa:
            a &= read(0xff00 + x);
            break;
        case Op::ANIW_wa_xx:
            write(0xff00 + x, read(0xff00 + x) & y);
            break;
        case Op::ANI_A_xx:
            a &= x;
            break;
        case Op::ANI_PA_xx:
            pa &= x;
            emit(FirmwareControlTrace::EventKind::Converter, address,
                 ((pb & 63) * 256 + portc) >> 2, ordinal++);
            break;
        case Op::BIT_0_wa:
            skip = (read(0xff00 + x) & 1) != 0;
            break;
        case Op::BIT_1_wa:
            skip = (read(0xff00 + x) & 2) != 0;
            break;
        case Op::BIT_2_wa:
            skip = (read(0xff00 + x) & 4) != 0;
            break;
        case Op::BIT_3_wa:
            skip = (read(0xff00 + x) & 8) != 0;
            break;
        case Op::BIT_5_wa:
            skip = (read(0xff00 + x) & 32) != 0;
            break;
        case Op::BIT_6_wa:
            skip = (read(0xff00 + x) & 64) != 0;
            break;
        case Op::BIT_7_wa:
            skip = (read(0xff00 + x) & 128) != 0;
            break;
        case Op::CALF:
            push(pc);
            pc = x;
            break;
        case Op::CLC:
            carry = false;
            break;
        case Op::DADDNC_EA_BC:
            ea = add(ea, bc(), 65535);
            skip = !carry;
            break;
        case Op::DADD_EA_BC:
            ea = add(ea, bc(), 65535);
            break;
        case Op::DCR_A:
            a = (a - 1) & 255;
            skip = a == 255;
            break;
        case Op::DCR_C:
            c = (c - 1) & 255;
            skip = c == 255;
            break;
        case Op::DGT_EA_BC:
            carry = ea <= bc();
            skip = ea > bc();
            break;
        case Op::DI:
            error = true;
            break;
        case Op::DLT_EA_BC:
            carry = ea < bc();
            skip = ea < bc();
            break;
        case Op::DMOV_BC_EA:
            bc(ea);
            break;
        case Op::DMOV_EA_BC:
            ea = bc();
            break;
        case Op::DMOV_HL_EA:
            hl(ea);
            break;
        case Op::DNE_EA_BC:
            carry = ea < bc();
            skip = ea != bc();
            break;
        case Op::DSLL_EA:
            carry = (ea & 32768) != 0;
            ea = (ea << 1) & 65535;
            break;
        case Op::DSLR_EA:
            carry = (ea & 1) != 0;
            ea >>= 1;
            break;
        case Op::DSUBNB_EA_BC:
            ea = sub(ea, bc(), 65535);
            skip = !carry;
            break;
        case Op::DSUB_EA_BC:
            ea = sub(ea, bc(), 65535);
            break;
        case Op::EADD_EA_A:
            ea = add(ea, a, 65535);
            break;
        case Op::EADD_EA_C:
            ea = add(ea, c, 65535);
            break;
        case Op::EI:
            error = true;
            break;
        case Op::EQAW_wa:
            carry = a < read(0xff00 + x);
            skip = a == read(0xff00 + x);
            break;
        case Op::EQIW_wa_xx:
            carry = read(0xff00 + x) < y;
            skip = read(0xff00 + x) == y;
            break;
        case Op::EQI_A_xx:
            carry = a < x;
            skip = a == x;
            break;
        case Op::ESUB_EA_A:
            ea = sub(ea, a, 65535);
            break;
        case Op::GTAW_wa:
            carry = a <= read(0xff00 + x);
            skip = a > read(0xff00 + x);
            break;
        case Op::GTA_A_C:
            carry = a <= c;
            skip = a > c;
            break;
        case Op::GTI_A_xx:
            carry = a <= x;
            skip = a > x;
            break;
        case Op::INRW_wa: {
            unsigned v = (read(0xff00 + x) + 1) & 255;
            write(0xff00 + x, v);
            skip = v == 0;
        } break;
        case Op::INX_DE:
            de(de() + 1);
            break;
        case Op::INX_HL:
            hl(hl() + 1);
            break;
        case Op::JMP_w:
            pc = x;
            break;
        case Op::JR:
            pc = x;
            break;
        case Op::JRE:
            pc = x;
            break;
        case Op::LBCD_w:
            bc(word(x));
            break;
        case Op::LDAW_wa:
            a = read(0xff00 + x);
            break;
        case Op::LDAX_D:
            a = read((de()) & 65535);
            break;
        case Op::LDAX_H:
            a = read((hl()) & 65535);
            break;
        case Op::LDAX_H_A:
            a = read((hl() + a) & 65535);
            break;
        case Op::LDAX_H_B:
            a = read((hl() + b) & 65535);
            break;
        case Op::LDAX_Hp:
            a = read((hl()) & 65535);
            hl(hl() + 1);
            break;
        case Op::LDEAX_D:
            ea = word((de()) & 65535);
            break;
        case Op::LDEAX_D_xx:
            ea = word((de() + x) & 65535);
            break;
        case Op::LDEAX_Dp:
            ea = word((de()) & 65535);
            de(de() + 2);
            break;
        case Op::LDEAX_H:
            ea = word((hl()) & 65535);
            break;
        case Op::LDEAX_H_A:
            ea = word((hl() + a) & 65535);
            break;
        case Op::LDEAX_H_xx:
            ea = word((hl() + x) & 65535);
            break;
        case Op::LDEAX_Hp:
            ea = word((hl()) & 65535);
            hl(hl() + 2);
            break;
        case Op::LTI_A_xx:
            carry = a < x;
            skip = a < x;
            break;
        case Op::LTI_B_xx:
            carry = b < x;
            skip = b < x;
            break;
        case Op::LXI_B_w:
            bc(x);
            break;
        case Op::LXI_D_w:
            de(x);
            break;
        case Op::LXI_EA_s:
            ea = x;
            break;
        case Op::LXI_H_w:
            hl(x);
            break;
        case Op::MOV_A_B:
            a = b;
            break;
        case Op::MOV_A_C:
            a = c;
            break;
        case Op::MOV_A_D:
            a = d;
            break;
        case Op::MOV_A_EAH:
            a = (ea >> 8);
            break;
        case Op::MOV_A_EAL:
            a = (ea & 255);
            break;
        case Op::MOV_A_H:
            a = h;
            break;
        case Op::MOV_A_L:
            a = l;
            break;
        case Op::MOV_B_A:
            b = a;
            break;
        case Op::MOV_C_A:
            c = a;
            break;
        case Op::MOV_D_A:
            d = a;
            break;
        case Op::MOV_H_A:
            h = a;
            break;
        case Op::MOV_L_A:
            l = a;
            break;
        case Op::MOV_PA_A:
            pa = a;
            break;
        case Op::MOV_PB_A:
            pb = a;
            break;
        case Op::MOV_PC_A:
            portc = a;
            break;
        case Op::MOV_w_A:
            write(x, a);
            break;
        case Op::MUL_B:
            ea = a * b;
            break;
        case Op::MUL_C:
            ea = a * c;
            break;
        case Op::MVIW_wa_xx:
            write(0xff00 + x, y);
            break;
        case Op::MVI_A_xx:
            a = x;
            break;
        case Op::MVI_B_xx:
            b = x;
            break;
        case Op::MVI_C_xx:
            c = x;
            break;
        case Op::MVI_H_xx:
            h = x;
            break;
        case Op::MVI_L_xx:
            l = x;
            break;
        case Op::MVI_MKH_xx:
            error = true;
            break;
        case Op::NEGA:
            a = (-a) & 255;
            break;
        case Op::NEI_A_xx:
            carry = a < x;
            skip = a != x;
            break;
        case Op::NOP:
            break;
        case Op::OFFAW_wa:
            skip = (a & read(0xff00 + x)) == 0;
            break;
        case Op::OFFIW_wa_xx:
            skip = (read(0xff00 + x) & y) == 0;
            break;
        case Op::OFFI_A_xx:
            skip = (a & x) == 0;
            break;
        case Op::ONAW_wa:
            skip = (a & read(0xff00 + x)) != 0;
            break;
        case Op::ONIW_wa_xx:
            skip = (read(0xff00 + x) & y) != 0;
            break;
        case Op::ONI_A_xx:
            skip = (a & x) != 0;
            break;
        case Op::ORAW_wa:
            a |= read(0xff00 + x);
            break;
        case Op::ORIW_wa_xx:
            write(0xff00 + x, read(0xff00 + x) | y);
            break;
        case Op::ORI_A_xx:
            a |= x;
            break;
        case Op::ORI_PA_xx:
            emit(FirmwareControlTrace::EventKind::Inhibit, address, pa | x, ordinal);
            pa |= x;
            break;
        case Op::POP_BC:
            bc(pop());
            break;
        case Op::POP_DE:
            de(pop());
            break;
        case Op::POP_EA:
            ea = pop();
            break;
        case Op::POP_HL:
            hl(pop());
            break;
        case Op::PUSH_BC:
            push(bc());
            break;
        case Op::PUSH_DE:
            push(de());
            break;
        case Op::PUSH_EA:
            push(ea);
            break;
        case Op::PUSH_HL:
            push(hl());
            break;
        case Op::RET:
            pc = pop();
            break;
        case Op::RLL_A: {
            const bool old = carry;
            carry = (a & 128) != 0;
            a = ((a << 1) | old) & 255;
        } break;
        case Op::SBCD_w:
            storeWord(x, bc(), address);
            break;
        case Op::SKIT_FAD:
            error = true;
            break;
        case Op::SLL_A:
            carry = (a & 128) != 0;
            a = (a << 1) & 255;
            break;
        case Op::SLR_A:
            carry = (a & 1) != 0;
            a >>= 1;
            break;
        case Op::STAW_wa:
            write(0xff00 + x, a);
            break;
        case Op::STAX_D:
            write((de()) & 65535, a);
            break;
        case Op::STAX_D_xx:
            write((de() + x) & 65535, a);
            break;
        case Op::STAX_H:
            write((hl()) & 65535, a);
            break;
        case Op::STEAX_D:
            storeWord((de()) & 65535, ea, address);
            break;
        case Op::STEAX_D_xx:
            storeWord((de() + x) & 65535, ea, address);
            break;
        case Op::STEAX_Dp:
            storeWord((de()) & 65535, ea, address);
            de(de() + 2);
            break;
        case Op::STEAX_H:
            storeWord((hl()) & 65535, ea, address);
            break;
        case Op::STEAX_Hp:
            storeWord((hl()) & 65535, ea, address);
            hl(hl() + 2);
            break;
        case Op::SUBNBX_D:
            a = sub(a, read(de()), 255);
            skip = !carry;
            break;
        case Op::SUB_A_B:
            a = sub(a, b, 255);
            break;
        case Op::SUINB_A_xx:
            a = sub(a, x, 255);
            skip = !carry;
            break;
        case Op::SUI_A_xx:
            a = sub(a, x, 255);
            break;
        case Op::XRAW_wa:
            a ^= read(0xff00 + x);
            break;
        case Op::XRI_A_xx:
            a ^= x;
            break;
        case Op::EXA:
            std::swap(a, alternateA);
            std::swap(ea, alternateEa);
            break;
        case Op::EXX:
            std::swap(b, alternateB);
            std::swap(c, alternateC);
            std::swap(d, alternateD);
            std::swap(e, alternateE);
            std::swap(h, alternateH);
            std::swap(l, alternateL);
            break;
        case Op::MOV_A_ANM:
            error = true;
            break;
        case Op::XRI_ANM_xx:
            error = true;
            break;
        case Op::MOV_A_CR:
            error = true;
            break;
        case Op::STAX_Hp:
            write(hl(), a);
            hl(hl() + 1);
            break;
        case Op::RETI:
            error = true;
            break;
        }
    }
};
using Extra = firmwareSerialTraceDetail::Extra;
using Decoded = firmwareSerialTraceDetail::Instruction;
const Decoded *serialInstruction(unsigned address) {
    constexpr auto& program = firmwareSerialTraceDetail::program;
    static constexpr auto index = firmwareTraceDetail::instructionIndex(
        program, [](const Decoded& i) { return i.instruction.address; });
    const auto offset = address < index.size() ? index[address] : 0;
    return offset != 0 ? &program[offset - 1] : nullptr;
}
const Instruction *mainInstruction(unsigned address) {
    constexpr auto& program = firmwareTraceDetail::program;
    static constexpr auto index = firmwareTraceDetail::instructionIndex(
        program, [](const Instruction& i) { return i.address; });
    const auto offset = address < index.size() ? index[address] : 0;
    return offset != 0 ? &program[offset - 1] : nullptr;
}
#define REGISTERS(X)                                                                               \
    X(a)                                                                                           \
    X(b) X(c) X(d) X(e) X(h) X(l) X(ea) X(v) X(alternateA) X(alternateB) X(alternateC)             \
        X(alternateD) X(alternateE) X(alternateH) X(alternateL) X(alternateEa) X(alternateV) X(pa) \
            X(pb) X(portc) X(portf) X(pc) X(sp) X(carry) X(skip)
struct Runner {
    Stream::State &state;
    const FirmwareAdcTrace::Inputs &inputs;
    const Stream::Configuration &config;
    Stream::Events &output;
    StreamCpu cpu;
    Stream::Result result{Stream::Status::ReachedTarget};
    Runner(Stream::State &s, const FirmwareControlTrace::Tables &t,
           const FirmwareAdcTrace::Inputs &in, const Stream::Configuration &c, Stream::Events &out,
           const Stream::Tables *parameters = nullptr)
        : state(s), inputs(in), config(c), output(out), cpu(s.control, t, parameters) {
#define LOAD(name) cpu.name = s.registers.name;
        REGISTERS(LOAD)
#undef LOAD
        cpu.ordinal = s.ordinal;
    }
    Stream::Result finish(Stream::Status status) {
#define SAVE(name) state.registers.name = cpu.name;
        REGISTERS(SAVE)
#undef SAVE
        state.ordinal = cpu.ordinal;
        state.control = cpu.result.finalState;
        state.control.adcComplete = state.adc.request;
        result.status = status;
        return result;
    }
    void emit(Stream::EventKind kind, unsigned address = 0, unsigned value = 0, unsigned card = 255,
              unsigned bits = 0, unsigned pc = ~0u) {
        if (output.count == output.entries.size()) {
            cpu.error = true;
            return;
        }
        output.entries[output.count++] = {
            kind,
            state.now,
            static_cast<std::uint16_t>(pc == ~0u ? state.pending.address : pc),
            static_cast<std::uint16_t>(address),
            static_cast<std::uint16_t>(value),
            static_cast<std::uint8_t>(card),
            static_cast<std::uint8_t>(bits),
            static_cast<std::uint8_t>(cpu.pa),
            static_cast<std::uint8_t>(cpu.pb),
            static_cast<std::uint8_t>(cpu.portc)};
    }
    std::uint8_t psw() const {
        return (state.opaquePsw & ~0x21) | (cpu.carry ? 1 : 0) | (cpu.skip ? 0x20 : 0);
    }
    void restorePsw(unsigned value) {
        state.opaquePsw = value & ~0x21;
        cpu.carry = value & 1;
        cpu.skip = value & 0x20;
    }
    Decoded decoded() const {
        if (auto *serial = serialInstruction(state.pending.address))
            return *serial;
        if (auto *normal = mainInstruction(state.pending.address))
            return {*normal};
        return {{static_cast<std::uint16_t>(state.pending.address), Op::NOP, 0, 0, 0, 0, 0}};
    }
    bool peripheralOperation(const Decoded &d) const {
        const auto op = d.instruction.op;
        return d.extra == Extra::MOV_A_RXB || op == Op::MOV_A_ANM || op == Op::MOV_A_CR ||
               op == Op::XRI_ANM_xx || op == Op::MVI_MKH_xx || op == Op::SKIT_FAD;
    }
    void access(const Decoded &d) {
        auto &pending = state.pending;
        auto &adc = state.adc;
        const auto &i = d.instruction;
        if (!peripheralOperation(d) || pending.accessDone)
            return;
        pending.accessDone = true;
        if (d.extra == Extra::MOV_A_RXB) {
            pending.sampledValue = state.rxb;
            state.rxbFull = false;
            emit(Stream::EventKind::SerialRead, 0, state.rxb);
        } else if (i.op == Op::MOV_A_ANM)
            pending.sampledValue = adc.anm;
        else if (i.op == Op::MOV_A_CR)
            pending.sampledValue = adc.conversion[i.argument];
        else if (i.op == Op::SKIT_FAD) {
            pending.sampledSkip = adc.request;
            adc.request = false;
        } else if (i.op == Op::MVI_MKH_xx) {
            adc.mkh = i.argument;
            emit(Stream::EventKind::MaskWrite, 0, adc.mkh);
        } else if (i.op == Op::XRI_ANM_xx) {
            adc.anm ^= i.argument;
            if (config.adc.anmWritePhase !=
                FirmwareAdcTrace::AnmWritePhase::PreserveCompleteScanPhase)
                adc.channel = 0;
            if (config.adc.anmWritePhase == FirmwareAdcTrace::AnmWritePhase::RestartConversion)
                adc.statesUntilConversion = FirmwareAdcTrace::conversionStates;
            emit(Stream::EventKind::AnmWrite, 0, adc.anm);
        }
    }
    void stackEvents() {
        for (unsigned i = 0; i < cpu.stackCount; ++i)
            emit(Stream::EventKind::StackWrite, cpu.stackWrites[i].address,
                 cpu.stackWrites[i].value);
        cpu.stackCount = 0;
    }
    void completeInstruction() {
        const auto d = decoded();
        const auto &i = d.instruction;
        auto &pending = state.pending;
        cpu.result.count = 0;
        cpu.externalCount = 0;
        cpu.stackCount = 0;
        cpu.result.states = 0;
        cpu.executingAddress = i.address;
        if (pending.skipped) {
            cpu.pc += i.bytes;
            cpu.skip = false;
        } else {
            access(d);
            bool manually = true;
            cpu.pc += i.bytes;
            if (d.extra == Extra::MOV_A_RXB)
                cpu.a = pending.sampledValue;
            else if (d.extra == Extra::MOV_E_A)
                cpu.e = cpu.a;
            else if (d.extra == Extra::MOV_A_E)
                cpu.a = cpu.e;
            else if (d.extra == Extra::TABLE) {
                // NEC April1987 printed12-67: C=[opcodePC+3+A],
                // B=[opcodePC+4+A]. PC has advanced by2 here; CY survives.
                cpu.bc(cpu.word((cpu.pc + cpu.a + 1) & 65535));
                cpu.skip = false;
            } else if (d.extra == Extra::JB) {
                // Printed12-30: indirect branch through unchanged BC.
                cpu.pc = cpu.bc();
                cpu.skip = false;
            } else if (d.extra == Extra::ORA_A_B)
                cpu.a |= cpu.b;
            else if (d.extra == Extra::OFFI_D_xx)
                cpu.skip = (cpu.d & i.argument) == 0;
            else if (d.extra == Extra::MVI_PF_xx) {
                cpu.portf = i.argument;
                emit(Stream::EventKind::PortFWrite, 5, cpu.portf, 255, 0, i.address);
            } else if (d.extra == Extra::MOV_EAH_A)
                cpu.ea = (cpu.ea & 255) | (cpu.a << 8);
            else if (d.extra == Extra::DMOV_DE_EA)
                cpu.de(cpu.ea);
            else if (d.extra == Extra::STC)
                cpu.carry = true;
            else if (d.extra == Extra::NEA_A_D) {
                cpu.carry = cpu.a < cpu.d;
                cpu.skip = cpu.a != cpu.d;
            } else if (d.extra == Extra::STAX_H_B)
                cpu.write((cpu.hl() + cpu.b) & 65535, cpu.a);
            else if (d.extra == Extra::LXI_S_w) {
                cpu.sp = i.argument;
                emit(Stream::EventKind::StackReset, 0, cpu.sp);
            } else if (i.op == Op::MOV_A_ANM || i.op == Op::MOV_A_CR)
                cpu.a = pending.sampledValue;
            else if (i.op == Op::SKIT_FAD)
                cpu.skip = pending.sampledSkip;
            else if (i.op == Op::XRI_ANM_xx || i.op == Op::MVI_MKH_xx) {
            } else if (i.op == Op::DI) {
                state.adc.interruptsEnabled = false;
                state.adc.eiDeferred = 0;
            } else if (i.op == Op::EI) {
                state.adc.interruptsEnabled = true;
                state.adc.eiDeferred = 2;
            } else if (i.op == Op::RETI) {
                unsigned lo = cpu.popByte(), hi = cpu.popByte(), flags = cpu.popByte();
                cpu.pc = lo + 256 * hi;
                restorePsw(flags);
                emit(Stream::EventKind::InterruptReturn, 0, cpu.pc);
            } else
                manually = false;
            if (!manually) {
                cpu.pc -= i.bytes;
                cpu.execute(i);
                if (i.op == Op::EXA)
                    std::swap(cpu.v, cpu.alternateV);
            }
            for (std::size_t j = 0; j < cpu.result.count; ++j) {
                const auto &e = cpu.result.events[j];
                if (e.kind == FirmwareControlTrace::EventKind::Converter ||
                    e.kind == FirmwareControlTrace::EventKind::Inhibit)
                    continue;
                Stream::EventKind kind = Stream::EventKind::RamByte;
                if (e.kind == FirmwareControlTrace::EventKind::Envelope)
                    kind = Stream::EventKind::Envelope;
                else if (e.kind == FirmwareControlTrace::EventKind::Portamento)
                    kind = Stream::EventKind::Portamento;
                emit(kind,
                     e.kind == FirmwareControlTrace::EventKind::RamByte ? 0xff00 + e.card
                                                                        : e.address,
                     e.value, e.card, e.phaseBits, i.address);
            }
            for (unsigned j = 0; j < cpu.externalCount; ++j)
                emit(Stream::EventKind::ExternalWrite, cpu.externalWrites[j].address,
                     cpu.externalWrites[j].value, 255, 0, i.address);
            stackEvents();
        }
        pending.kind = Stream::PendingKind::None;
        state.needsArbitration = true;
        state.completedInstruction = true;
        ++result.completedInstructions;
    }
    void completeEntry() {
        const auto pending = state.pending;
        cpu.stackCount = 0;
        cpu.pushByte(pending.savedPsw);
        cpu.pushByte(pending.returnPc >> 8);
        cpu.pushByte(pending.returnPc);
        stackEvents();
        cpu.pc = pending.interruptVector;
        cpu.skip = false;
        state.opaquePsw &= ~0x0c;
        // Carry is preserved on entry; only SK/overlay conditions are isolated.
        state.pending.kind = Stream::PendingKind::None;
        state.needsArbitration = false;
    }
    void arbitrate() {
        if (!state.needsArbitration)
            return;
        state.needsArbitration = false;
        auto &adc = state.adc;
        if (state.completedInstruction && adc.eiDeferred)
            --adc.eiDeferred;
        state.completedInstruction = false;
        if (adc.eiDeferred || !adc.interruptsEnabled)
            return;
        const bool ad = config.adc.interrupts && adc.request && !(adc.mkh & 1);
        const bool serial = config.serialEnabled && state.fsr && !(adc.mkh & 2);
        if (!ad && !serial)
            return;
        auto &pending = state.pending;
        pending = {};
        pending.kind = Stream::PendingKind::InterruptEntry;
        pending.start = state.now;
        pending.remaining = FirmwareAdcTrace::interruptEntryStates;
        pending.returnPc = cpu.pc;
        pending.interruptVector = ad ? 0x20 : 0x28;
        pending.savedPsw = psw();
        pending.address = cpu.pc;
        if (ad) {
            adc.request = false;
            ++state.adcInterrupts;
        } else {
            state.fsr = false;
            ++state.serialInterrupts;
        }
        adc.interruptsEnabled = false;
        emit(Stream::EventKind::InterruptAcceptance, pending.interruptVector, ad ? 0 : 1, 255, 0,
             cpu.pc);
    }
    bool beginInstruction() {
        // Preserve the original overload's bounded parameter refusal. The
        // rich overload reaches024B only for armed A3; that diagnostic tail
        // has no descriptor and stops before FF01 or any diagnostic output.
        if (cpu.pc == 0x00d7 && cpu.parameterTables == nullptr)
            return false;
        Decoded d;
        if (auto *serial = serialInstruction(cpu.pc))
            d = *serial;
        else if (auto *normal = mainInstruction(cpu.pc))
            d = {*normal};
        else
            return false;
        auto &pending = state.pending;
        pending = {};
        pending.kind = Stream::PendingKind::Instruction;
        pending.start = state.now;
        pending.address = cpu.pc;
        pending.skipped = cpu.skip;
        pending.remaining = cpu.skip ? d.instruction.skippedStates : d.instruction.states;
        if (cpu.pc == 0x02ec) {
            cpu.ordinal = 0;
            ++state.passes;
            emit(Stream::EventKind::PassStart, 0, state.passes);
        }
        if (!pending.skipped) {
            if (config.adc.accessBoundary ==
                FirmwareAdcTrace::PeripheralAccessBoundary::InstructionStart)
                access(d);
            if (d.instruction.op == Op::ANI_PA_xx) {
                const unsigned saved = cpu.pa;
                cpu.pa &= d.instruction.argument;
                emit(Stream::EventKind::Converter, cpu.pa & 15,
                     ((cpu.pb & 63) * 256 + cpu.portc) >> 2, cpu.ordinal);
                cpu.pa = saved;
            } else if (d.instruction.op == Op::ORI_PA_xx) {
                const unsigned saved = cpu.pa;
                cpu.pa |= d.instruction.argument;
                emit(Stream::EventKind::Inhibit, cpu.pa & 15, cpu.pa, cpu.ordinal);
                cpu.pa = saved;
            }
        }
        return true;
    }
    void advanceTime(unsigned duration) {
        state.now += duration;
        state.adc.elapsedStates = state.now;
        state.adc.statesUntilConversion -= duration;
        state.pending.remaining -= duration;
    }
    void adcDue() {
        auto &adc = state.adc;
        if (adc.statesUntilConversion)
            return;
        const unsigned channel = ((adc.anm & 8) ? 4 : 0) + adc.channel;
        adc.conversion[adc.channel] = inputs.raw[channel];
        emit(Stream::EventKind::AdcConversion, channel, adc.conversion[adc.channel], 255, 0,
             0xffff);
        adc.channel = (adc.channel + 1) & 3;
        adc.statesUntilConversion = FirmwareAdcTrace::conversionStates;
        if (!adc.channel) {
            adc.request = true;
            emit(Stream::EventKind::AdcRequest, 0, adc.anm, 255, 0, 0xffff);
        }
    }
};
#undef REGISTERS
Stream::Result advanceToImpl(Stream::State &state, const FirmwareControlTrace::Tables &tables,
                             const Stream::Tables *parameterTables,
                             const FirmwareAdcTrace::Inputs &inputs,
                             const Stream::Configuration &configuration,
                             std::span<const Stream::ByteReady> bytes,
                             std::uint64_t target, Stream::Events &events) noexcept {
    using Status = Stream::Status;
    using PendingKind = Stream::PendingKind;
    using EventKind = Stream::EventKind;
    const auto &r = state.registers;
    const std::array<unsigned, 23> byteRegisters{r.a,          r.b,          r.c,
                                                 r.d,          r.e,          r.h,
                                                 r.l,          r.v,          r.alternateA,
                                                 r.alternateB, r.alternateC, r.alternateD,
                                                 r.alternateE, r.alternateH, r.alternateL,
                                                 r.alternateV, r.pa,         r.pb,
                                                 r.portc,      r.portf,      state.rxb,    state.opaquePsw,
                                                 state.adc.mkh};
    if (std::any_of(byteRegisters.begin(), byteRegisters.end(),
                    [](unsigned value) { return value > 255; }) ||
        r.ea > 65535 || r.alternateEa > 65535 || r.pc > 65535 ||
        static_cast<unsigned>(configuration.adc.anmWritePhase) > 2 ||
        static_cast<unsigned>(configuration.adc.accessBoundary) > 1 ||
        static_cast<unsigned>(state.pending.kind) > 2 || state.adc.eiDeferred > 1 ||
        target < state.now || state.adc.elapsedStates != state.now ||
        state.adc.statesUntilConversion > FirmwareAdcTrace::conversionStates ||
        state.adc.channel > 3 || (state.adc.anm & ~8) ||
        (state.adc.mkh != 4 && state.adc.mkh != 5) || state.registers.sp < 0xff00 ||
        state.registers.sp > 0xffff || state.registers.v != 0xff ||
        state.registers.alternateV != 0xff || events.count > events.entries.size())
        return {Status::InvalidState};
    const auto &pending = state.pending;
    unsigned duration = 0;
    if (pending.kind == PendingKind::None) {
        if (pending.remaining)
            return {Status::InvalidState};
    } else if (pending.kind == PendingKind::InterruptEntry) {
        duration = FirmwareAdcTrace::interruptEntryStates;
        if ((pending.interruptVector != 0x20 && pending.interruptVector != 0x28) ||
            pending.returnPc != r.pc)
            return {Status::InvalidState};
    } else {
        const Instruction *instruction = nullptr;
        if (const auto *serial = serialInstruction(pending.address))
            instruction = &serial->instruction;
        else
            instruction = mainInstruction(pending.address);
        if (!instruction || pending.address != r.pc || pending.skipped != r.skip)
            return {Status::InvalidState};
        duration = pending.skipped ? instruction->skippedStates : instruction->states;
    }
    if (pending.kind != PendingKind::None &&
        (pending.remaining > duration || pending.start > state.now ||
         state.now - pending.start != duration - pending.remaining))
        return {Status::InvalidState};
    std::uint64_t previous = state.now;
    for (const auto &byte : bytes) {
        if (byte.states < previous)
            return {Status::InvalidState};
        previous = byte.states;
    }
    Runner run(state, tables, inputs, configuration, events, parameterTables);
    for (;;) {
        // Reserve enough for an instruction's atomic completion, one ADC
        // boundary and one received byte. Overrun aborts before a second byte.
        if (events.entries.size() - events.count < 24)
            return run.finish(Status::OutputFull);
        run.adcDue();
        while (run.result.consumedBytes < bytes.size() &&
               bytes[run.result.consumedBytes].states == state.now) {
            if (state.rxbFull)
                return run.finish(Status::ReceiveOverrun);
            state.rxb = bytes[run.result.consumedBytes++].value;
            state.rxbFull = true;
            state.fsr = true;
            run.emit(EventKind::SerialReady, 0, state.rxb, 255, 0, 0xffff);
        }
        if (state.pending.kind != PendingKind::None && state.pending.remaining == 0) {
            if (state.pending.kind == PendingKind::InterruptEntry)
                run.completeEntry();
            else
                run.completeInstruction();
            if (run.cpu.error)
                return run.finish(Status::InvalidState);
        }
        if (state.pending.kind == PendingKind::None) {
            run.arbitrate();
            if (state.pending.kind == PendingKind::None && !run.beginInstruction())
                return run.finish(Status::UnsupportedPath);
        }
        if (state.now == target)
            return run.finish(Status::ReachedTarget);
        if (run.result.completedInstructions >= 16384)
            return run.finish(Status::InstructionBudget);
        auto delta = std::min<std::uint64_t>(
            target - state.now,
            std::min<unsigned>(state.pending.remaining, state.adc.statesUntilConversion));
        if (run.result.consumedBytes < bytes.size())
            delta = std::min(delta, bytes[run.result.consumedBytes].states - state.now);
        if (delta == 0)
            return run.finish(Status::InvalidState);
        run.advanceTime(static_cast<unsigned>(delta));
    }
}
} // namespace

FirmwareSerialTrace::Result FirmwareSerialTrace::advanceTo(
    State &state, const Tables &tables, const FirmwareAdcTrace::Inputs &inputs,
    const Configuration &configuration, std::span<const ByteReady> bytes,
    std::uint64_t target, Events &events) noexcept {
    return advanceToImpl(state, tables.control, &tables, inputs, configuration, bytes, target, events);
}

FirmwareSerialTrace::Result FirmwareSerialTrace::advanceTo(
    State &state, const FirmwareControlTrace::Tables &tables,
    const FirmwareAdcTrace::Inputs &inputs, const Configuration &configuration,
    std::span<const ByteReady> bytes, std::uint64_t target, Events &events) noexcept {
    return advanceToImpl(state, tables, nullptr, inputs, configuration, bytes, target, events);
}
} // namespace youknow
