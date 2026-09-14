#include "YouKnowFirmwareTrace.h"
#include "YouKnowFirmwareProgram.h"
#include <algorithm>

namespace youknow {
namespace {
using firmwareTraceDetail::Instruction;
using firmwareTraceDetail::Op;
constexpr auto addressMap=[] {
    std::array<std::int16_t,0x851> map {};
    map.fill(-1);
    for(std::size_t i=0;i<firmwareTraceDetail::program.size();++i)
        map[firmwareTraceDetail::program[i].address]=static_cast<std::int16_t>(i);
    return map;
}();
struct Cpu {
    FirmwareControlTrace::Result result;
    const FirmwareControlTrace::Tables& tables;
    unsigned a=0,b=0,c=0,d=0,e=0,h=0,l=0,ea=0,pa=0xff,pb=0,portc=0;
    unsigned pc=0x02ec,executingAddress=0,storeStates=0;
    bool carry=false,skip=false,error=false;
    std::array<unsigned,64> stack {};
    unsigned stackSize=0,ordinal=0;
    explicit Cpu(const FirmwareControlTrace::State& state,
                 const FirmwareControlTrace::Tables& lookup):tables(lookup) {result.finalState=state;}
    unsigned bc()const{return b*256+c;}
    unsigned de()const{return d*256+e;}
    unsigned hl()const{return h*256+l;}
    void bc(unsigned v){b=(v>>8)&255;c=v&255;}
    void de(unsigned v){d=(v>>8)&255;e=v&255;}
    void hl(unsigned v){h=(v>>8)&255;l=v&255;}
    void push(unsigned v){if(stackSize==stack.size())error=true;else stack[stackSize++]=v;}
    unsigned pop(){if(!stackSize){error=true;return 0;}return stack[--stackSize];}
    unsigned read(unsigned address) {
        if(address>=0xff00&&address<=0xffff)return result.finalState.ram[address&255];
        if(address>=0x0a00&&address<0x0a80)return tables.portamento[address-0x0a00];
        if(address>=0x0b50&&address<0x0b56) {
            constexpr std::array<unsigned,6> map{0x20,0x21,0x22,0x10,0x11,0x12};
            return map[address-0x0b50];
        }
        if(address>=0x0b56&&address<0x0b5c)return 0x36+0x40*((address-0x0b56)%3);
        if(address>=0x0b60&&address<0x0c60) {
            unsigned offset=address-0x0b60;return (tables.attack[offset/2]>>(8*(offset&1)))&255;
        }
        if(address>=0x0e60&&address<0x0f30) {
            unsigned offset=address-0x0e60;return (tables.pitchCv[offset/2]>>(8*(offset&1)))&255;
        }
        if(address>=0x0f30&&address<0x1000) {
            unsigned offset=address-0x0f30;return (tables.pitchDivider[offset/2]>>(8*(offset&1)))&255;
        }
        error=true;return 0;
    }
    unsigned word(unsigned address){return read(address)+256*read((address+1)&65535);}
    void write(unsigned address,unsigned value) {
        if(address>=0xff00&&address<=0xffff) {
            result.finalState.ram[address&255]=static_cast<std::uint8_t>(value);
            emit(FirmwareControlTrace::EventKind::RamByte,executingAddress,value&255,address&255);
        }
        else if(address!=0x3000 && (address<0x1000||address>0x2300))error=true;
    }
    void emit(FirmwareControlTrace::EventKind kind,unsigned address,unsigned value,
              unsigned card=255,unsigned bits=0) {
        if(result.count==result.events.size()){error=true;return;}
        const bool ramStore = kind == FirmwareControlTrace::EventKind::RamByte
            || kind == FirmwareControlTrace::EventKind::Envelope
            || kind == FirmwareControlTrace::EventKind::Portamento;
        result.events[result.count++]={kind,ramStore ? storeStates : result.states,static_cast<std::uint16_t>(address),
            static_cast<std::uint16_t>(value),static_cast<std::uint8_t>(card),static_cast<std::uint8_t>(bits)};
    }
    void storeWord(unsigned address,unsigned value,unsigned instruction) {
        write(address,value);write((address+1)&65535,value>>8);
        if(instruction==0x0590) {
            unsigned card=(address-0xff27)/2;
            unsigned bits=((read(0xff07)>>card)&1)|(((read(0xff08)>>card)&1)<<1)
                         |(((read(0xff33)>>card)&1)<<2);
            emit(FirmwareControlTrace::EventKind::Envelope,instruction,value,card,bits);
        } else if(instruction==0x03ec)emit(FirmwareControlTrace::EventKind::Portamento,instruction,value,(address-0xff71)/2);
    }
    unsigned add(unsigned x,unsigned y,unsigned mask){unsigned v=x+y;carry=v>mask;return v&mask;}
    unsigned sub(unsigned x,unsigned y,unsigned mask){carry=x<y;return (x-y)&mask;}
    void execute(const Instruction& i) {
        const unsigned x=i.argument,y=i.second,address=i.address;
        executingAddress=address;
        storeStates=result.states+i.states;
        pc+=i.bytes;
        switch(i.op) {
        case Op::ADDNCW_wa: a=add(a,read(0xff00+x),255);skip=!carry; break;
        case Op::ADDNC_A_B: a=add(a,b,255);skip=!carry; break;
        case Op::ADINC_A_xx: a=add(a,x,255);skip=!carry; break;
        case Op::ADI_A_xx: a=add(a,x,255); break;
        case Op::ANAW_wa: a&=read(0xff00+x); break;
        case Op::ANIW_wa_xx: write(0xff00+x,read(0xff00+x)&y); break;
        case Op::ANI_A_xx: a&=x; break;
        case Op::ANI_PA_xx: pa&=x;emit(FirmwareControlTrace::EventKind::Converter,address,((pb&63)*256+portc)>>2,ordinal++); break;
        case Op::BIT_0_wa: skip=(read(0xff00+x)&1)!=0; break;
        case Op::BIT_1_wa: skip=(read(0xff00+x)&2)!=0; break;
        case Op::BIT_2_wa: skip=(read(0xff00+x)&4)!=0; break;
        case Op::BIT_3_wa: skip=(read(0xff00+x)&8)!=0; break;
        case Op::BIT_5_wa: skip=(read(0xff00+x)&32)!=0; break;
        case Op::BIT_6_wa: skip=(read(0xff00+x)&64)!=0; break;
        case Op::BIT_7_wa: skip=(read(0xff00+x)&128)!=0; break;
        case Op::CALF: push(pc);pc=x; break;
        case Op::CLC: carry=false; break;
        case Op::DADDNC_EA_BC: ea=add(ea,bc(),65535);skip=!carry; break;
        case Op::DADD_EA_BC: ea=add(ea,bc(),65535); break;
        case Op::DCR_A: a=(a-1)&255;skip=a==255; break;
        case Op::DCR_C: c=(c-1)&255;skip=c==255; break;
        case Op::DGT_EA_BC: skip=ea>bc(); break;
        case Op::DI:  break;
        case Op::DLT_EA_BC: skip=ea<bc(); break;
        case Op::DMOV_BC_EA: bc(ea); break;
        case Op::DMOV_EA_BC: ea=bc(); break;
        case Op::DMOV_HL_EA: hl(ea); break;
        case Op::DNE_EA_BC: skip=ea!=bc(); break;
        case Op::DSLL_EA: carry=(ea&32768)!=0;ea=(ea<<1)&65535; break;
        case Op::DSLR_EA: carry=(ea&1)!=0;ea>>=1; break;
        case Op::DSUBNB_EA_BC: ea=sub(ea,bc(),65535);skip=!carry; break;
        case Op::DSUB_EA_BC: ea=sub(ea,bc(),65535); break;
        case Op::EADD_EA_A: ea=add(ea,a,65535); break;
        case Op::EADD_EA_C: ea=add(ea,c,65535); break;
        case Op::EI:  break;
        case Op::EQAW_wa: skip=(a==read(0xff00+x)); break;
        case Op::EQIW_wa_xx: skip=(read(0xff00+x)==y); break;
        case Op::EQI_A_xx: skip=(a==x); break;
        case Op::ESUB_EA_A: ea=sub(ea,a,65535); break;
        case Op::GTAW_wa: skip=(a>read(0xff00+x)); break;
        case Op::GTA_A_C: skip=a>c; break;
        case Op::GTI_A_xx: skip=(a>x); break;
        case Op::INRW_wa: {unsigned v=(read(0xff00+x)+1)&255;write(0xff00+x,v);skip=v==0;} break;
        case Op::INX_DE: de(de()+1); break;
        case Op::INX_HL: hl(hl()+1); break;
        case Op::JMP_w: pc=x; break;
        case Op::JR: pc=x; break;
        case Op::JRE: pc=x; break;
        case Op::LBCD_w: bc(word(x)); break;
        case Op::LDAW_wa: a=read(0xff00+x); break;
        case Op::LDAX_D: a=read((de())&65535); break;
        case Op::LDAX_H: a=read((hl())&65535); break;
        case Op::LDAX_H_A: a=read((hl()+a)&65535); break;
        case Op::LDAX_H_B: a=read((hl()+b)&65535); break;
        case Op::LDAX_Hp: a=read((hl())&65535);hl(hl()+1); break;
        case Op::LDEAX_D: ea=word((de())&65535); break;
        case Op::LDEAX_D_xx: ea=word((de()+x)&65535); break;
        case Op::LDEAX_Dp: ea=word((de())&65535);de(de()+2); break;
        case Op::LDEAX_H: ea=word((hl())&65535); break;
        case Op::LDEAX_H_A: ea=word((hl()+a)&65535); break;
        case Op::LDEAX_H_xx: ea=word((hl()+x)&65535); break;
        case Op::LDEAX_Hp: ea=word((hl())&65535);hl(hl()+2); break;
        case Op::LTI_A_xx: skip=(a<x); break;
        case Op::LTI_B_xx: skip=(b<x); break;
        case Op::LXI_B_w: bc(x); break;
        case Op::LXI_D_w: de(x); break;
        case Op::LXI_EA_s: ea=x; break;
        case Op::LXI_H_w: hl(x); break;
        case Op::MOV_A_B: a=b; break;
        case Op::MOV_A_C: a=c; break;
        case Op::MOV_A_D: a=d; break;
        case Op::MOV_A_EAH: a=(ea>>8); break;
        case Op::MOV_A_EAL: a=(ea&255); break;
        case Op::MOV_A_H: a=h; break;
        case Op::MOV_A_L: a=l; break;
        case Op::MOV_B_A: b=a; break;
        case Op::MOV_C_A: c=a; break;
        case Op::MOV_D_A: d=a; break;
        case Op::MOV_H_A: h=a; break;
        case Op::MOV_L_A: l=a; break;
        case Op::MOV_PA_A: pa=a; break;
        case Op::MOV_PB_A: pb=a; break;
        case Op::MOV_PC_A: portc=a; break;
        case Op::MOV_w_A: write(x,a); break;
        case Op::MUL_B: ea=a*b; break;
        case Op::MUL_C: ea=a*c; break;
        case Op::MVIW_wa_xx: write(0xff00+x,y); break;
        case Op::MVI_A_xx: a=x; break;
        case Op::MVI_B_xx: b=x; break;
        case Op::MVI_C_xx: c=x; break;
        case Op::MVI_H_xx: h=x; break;
        case Op::MVI_L_xx: l=x; break;
        case Op::MVI_MKH_xx:  break;
        case Op::NEGA: a=(-a)&255; break;
        case Op::NEI_A_xx: skip=(a!=x); break;
        case Op::NOP:  break;
        case Op::OFFAW_wa: skip=(a&read(0xff00+x))==0; break;
        case Op::OFFIW_wa_xx: skip=(read(0xff00+x)&y)==0; break;
        case Op::OFFI_A_xx: skip=(a&x)==0; break;
        case Op::ONAW_wa: skip=(a&read(0xff00+x))!=0; break;
        case Op::ONIW_wa_xx: skip=(read(0xff00+x)&y)!=0; break;
        case Op::ONI_A_xx: skip=(a&x)!=0; break;
        case Op::ORAW_wa: a|=read(0xff00+x); break;
        case Op::ORIW_wa_xx: write(0xff00+x,read(0xff00+x)|y); break;
        case Op::ORI_A_xx: a|=x; break;
        case Op::ORI_PA_xx: emit(FirmwareControlTrace::EventKind::Inhibit,address,pa|x,ordinal);pa|=x; break;
        case Op::POP_BC: bc(pop()); break;
        case Op::POP_DE: de(pop()); break;
        case Op::POP_EA: ea=pop(); break;
        case Op::POP_HL: hl(pop()); break;
        case Op::PUSH_BC: push(bc()); break;
        case Op::PUSH_DE: push(de()); break;
        case Op::PUSH_EA: push(ea); break;
        case Op::PUSH_HL: push(hl()); break;
        case Op::RET: pc=pop(); break;
        case Op::RLL_A: {const bool old=carry;carry=(a&128)!=0;a=((a<<1)|old)&255;} break;
        case Op::SBCD_w: storeWord(x,bc(),address); break;
        case Op::SKIT_FAD: skip=result.finalState.adcComplete;result.finalState.adcComplete=false; break;
        case Op::SLL_A: carry=(a&128)!=0;a=(a<<1)&255; break;
        case Op::SLR_A: carry=(a&1)!=0;a>>=1; break;
        case Op::STAW_wa: write(0xff00+x,a); break;
        case Op::STAX_D: write((de())&65535,a); break;
        case Op::STAX_D_xx: write((de()+x)&65535,a); break;
        case Op::STAX_H: write((hl())&65535,a); break;
        case Op::STEAX_D: storeWord((de())&65535,ea,address); break;
        case Op::STEAX_D_xx: storeWord((de()+x)&65535,ea,address); break;
        case Op::STEAX_Dp: storeWord((de())&65535,ea,address);de(de()+2); break;
        case Op::STEAX_H: storeWord((hl())&65535,ea,address); break;
        case Op::STEAX_Hp: storeWord((hl())&65535,ea,address);hl(hl()+2); break;
        case Op::SUBNBX_D: a=sub(a,read(de()),255);skip=!carry; break;
        case Op::SUB_A_B: a=sub(a,b,255); break;
        case Op::SUINB_A_xx: a=sub(a,x,255);skip=!carry; break;
        case Op::SUI_A_xx: a=sub(a,x,255); break;
        case Op::XRAW_wa: a^=read(0xff00+x); break;
        case Op::XRI_A_xx: a^=x; break;
        }
    }
};
} // namespace
FirmwareControlTrace::Result FirmwareControlTrace::run(const State& state,const Tables& tables) noexcept
{
    Cpu cpu(state,tables);
    for(unsigned guard=0;guard<8192;++guard) {
        if(cpu.pc>=addressMap.size()||addressMap[cpu.pc]<0){cpu.error=true;break;}
        const auto& instruction=firmwareTraceDetail::program[static_cast<unsigned>(addressMap[cpu.pc])];
        if(cpu.skip) {
            cpu.result.states+=instruction.skippedStates;
            cpu.pc+=instruction.bytes;
            cpu.skip=false;
        } else {
            cpu.execute(instruction);
            cpu.result.states+=instruction.states;
        }
        if(cpu.error)break;
        if(cpu.pc==0x02ec) {
            cpu.result.valid=cpu.ordinal==23&&cpu.stackSize==0;
            break;
        }
    }
    cpu.result.stoppedAt=static_cast<std::uint16_t>(cpu.pc);
    return cpu.result;
}
} // namespace youknow
