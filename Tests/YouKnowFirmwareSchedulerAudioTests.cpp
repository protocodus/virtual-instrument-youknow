// Continuous original A-5 MIDI/foreground/ADC -> real module pin edges -> B-2
// comparison receiver -> actual product DSP. No hand-authored voice commands.
// Inputs are an explicit warm state, RXB-ready schedule and physical slider step.
// Mapping the decoded frame end to B-2 RXB-ready is a declared scenario; it is
// not a measured internal receive-latch subcycle or propagation-delay estimate.
#include "../Source/DSP/YouKnowEngine.h"
#include "../Source/DSP/YouKnowFirmwareAssignerScheduler.h"
#include "../Source/DSP/YouKnowProductFidelity.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

using namespace youknow;
using A = FirmwareAssignerScheduler;
using S = FirmwareSerialTrace;
using Engine = YouKnowEngine;
namespace U = FirmwareUartTrace;
namespace Io = FirmwareAssignerIo;
std::uint64_t assertions = 0;
void check(bool value, const char *message) {
    ++assertions;
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::abort(); }
}
// In original patch-byte order90..9F, all chosen below the upper ADC stretch.
constexpr std::array<unsigned,16> patch {64,0,0,0,0,100,40,0,0,0,100,0,20,100,20,20};
constexpr std::uint64_t endTime=960000, sliderTime=300000;
struct Fixture { A::State cpu; U::State uart; Io::Inputs inputs; };
Fixture warm() {
    Fixture f;
    auto &r=f.cpu.ram;
    for (unsigned i=0;i<16;++i) {
        const unsigned address=Io::panelParameterAddress[i];
        f.inputs.panelAdc[i]=static_cast<std::uint8_t>(4+2*patch[address-0x90]);
        r[0x58+i]=r[0x6c+i]=f.inputs.panelAdc[i]; r[address]=patch[address-0x90];
    }
    for (unsigned i=0;i<4;++i) r[0x68+i]=f.inputs.directAdc[i];
    f.cpu.io.conversion=f.inputs.directAdc;
    for (unsigned i=0;i<6;++i) {r[0x80+i]=0x88+i;r[0x88+i]=0x80;}
    r[0x86]=0x88; r[0x4f]=0x21; r[0xb6]=2;
    r[0xb8]=r[0x8e]=r[0xc7]=0x2a; // Eight, pulse, chorus off
    r[0xba]=0x10; r[0xbb]=1; r[0xbc]=8; r[0xbe]=12;
    r[0xc5]=r[0xc6]=4; r[0xc8]=0x42; r[0xcb]=0xfc;
    return f;
}
std::vector<A::InputEvent> midi() {
    std::vector<A::InputEvent> result;
    auto message=[&](std::uint64_t t,std::initializer_list<unsigned> bytes) {
        for(auto byte:bytes) {result.push_back({t,static_cast<std::uint8_t>(byte)});t+=1280;}
    };
    message(10000,{0x90,60,64}); message(80000,{0x90,67,64});
    message(200000,{0xb0,64,127});
    message(250000,{0x80,60,0}); message(280000,{0x80,67,0});
    message(600000,{0xb0,64,0});
    return result;
}
struct Stream {
    A::State state; std::vector<U::Event> wire; std::vector<A::Event> cpu;
};
Stream sender(bool slider,unsigned phase=37,
              Io::AnmWritePhase anm=Io::AnmWritePhase::RestartConversion) {
    auto f=warm(); A::Configuration config; config.uart.idleBaudGridPhase=phase;
    config.anmWritePhase=anm;
    // No display lookup is reached in this no-button fixture. Blank display
    // tables are explicit unused scenario data, never substituted patch memory.
    A::Tables tables; const auto incoming=midi(); std::size_t cursor=0;
    Stream result;
    for (const auto target:{sliderTime,endTime}) {
        for(unsigned guard=0;guard<1000;++guard) {
            A::Events events; std::array<U::Event,256> storage;
            U::EventBuffer wire{storage.data(),storage.size(),0};
            auto step=A::advanceTo(f.cpu,f.uart,config,tables,f.inputs,
                std::span<const A::InputEvent>(incoming).subspan(cursor),target,events,wire);
            cursor+=step.consumedInputs;
            result.cpu.insert(result.cpu.end(),events.entries.begin(),events.entries.begin()+events.count);
            result.wire.insert(result.wire.end(),storage.begin(),storage.begin()+wire.count);
            if(step.status==A::Status::ReachedTarget)break;
            if(step.status!=A::Status::OutputFull && step.status!=A::Status::InstructionBudget)
                std::cerr << "sender status=" << unsigned(step.status) << " pc=" << std::hex
                          << f.cpu.registers.pc << std::dec << " time=" << f.cpu.now << '\n';
            check(step.status==A::Status::OutputFull || step.status==A::Status::InstructionBudget,
                  "continuous A-5 remains inside qualified normal path");
        }
        check(f.cpu.now==target && f.uart.now==target,"CPU and UART reach the continuous target");
        if(slider) f.inputs.panelAdc[6]=44; // original table0036 -> cutoff95 -> byte20
    }
    check(cursor==incoming.size(),"A-5 receives every scheduled MIDI byte");
    check(f.cpu.foregroundPasses>20,"A-5 executes repeated full foreground passes");
    check(f.cpu.ram[0xc1]==f.cpu.ram[0xc2],"real FIFO drains by the audio endpoint");
    check(!f.uart.frameActive && !f.uart.txBufferFull,"real UART finishes without CPU tail substitution");
    check(!f.cpu.patchRamAvailable,"normal performance needs no invented patch memory");
    check(f.cpu.ram[0x95]==(slider?20:100),"physical ADC slider reaches original stored cutoff byte");
    result.state=f.cpu; return result;
}
struct Decoded { std::uint64_t start; std::uint8_t value; };
std::vector<Decoded> decode(const Stream &stream) {
    std::vector<std::pair<std::uint64_t,bool>> edges; bool previous=true;
    for(const auto &e:stream.wire) if(e.kind==U::EventKind::PinLevels) {
        bool level=(e.pinLevels&2)!=0;
        if(level!=previous)edges.emplace_back(e.states,level);
        previous=level;
    }
    auto levelAt=[&](std::uint64_t t) {
        bool level=true; for(auto [at,v]:edges){if(at>t)break;level=v;} return level;
    };
    // Independent8N1 constants from original12MHz/24/16, not imported from UART.
    std::vector<Decoded> result; std::uint64_t next=0;
    for(auto [t,v]:edges) {
        if(v || t<next || t+1280>endTime)continue;
        check(!levelAt(t+64),"pin decoder confirms start center");
        unsigned byte=0;
        for(unsigned bit=0;bit<8;++bit)byte|=unsigned(levelAt(t+192+128*bit))<<bit;
        check(levelAt(t+1216),"pin decoder confirms stop center");
        result.push_back({t,static_cast<std::uint8_t>(byte)});next=t+1280;
    }
    return result;
}
std::vector<S::ByteReady> schedule(const Stream &stream) {
    std::vector<S::ByteReady> result;
    for(auto e:decode(stream))result.push_back({e.start+1280,e.value});
    return result;
}
std::unique_ptr<Engine> engine(std::span<const S::ByteReady> bytes,unsigned rate,unsigned factor) {
    auto e=std::make_unique<Engine>();ProductFidelityProfile::configureBeforePrepare(*e);
    EngineParameters p;
    auto f=[](unsigned i){return float(patch[i])/127.f;};
    p.lfoRate=f(0);p.lfoDelay=f(1);p.dcoLfoDepth=f(2);p.pwmDepth=f(3);
    p.noiseLevel=f(4);p.cutoff=f(5);p.resonance=f(6);p.envDepth=f(7);
    p.vcfLfoDepth=f(8);p.keyFollow=f(9);p.vcaLevel=f(10);p.attack=f(11);
    p.decay=f(12);p.sustain=f(13);p.release=f(14);p.subLevel=f(15);
    p.range=DcoRange::Eight;p.pulseEnabled=true;p.sawEnabled=false;
    p.chorus=ChorusMode::Off;p.highPass=static_cast<HighPassMode>(3);
    p.pwmSource=PwmSource::Lfo;p.vcaMode=VcaMode::Envelope;p.envPolarity=EnvPolarity::Normal;
    p.calibration=p.aging=p.chorusNoise=p.velocityDepth=0;
    ProductFidelityProfile::applyTo(p);e->setParameters(p);
    Engine::FirmwareSerialReplayConfiguration replay;replay.schedule=bytes;
    check(e->configureFirmwareSerialReplay(replay),"actual DSP accepts continuous sender-derived bytes");
    e->prepare(rate,128,int(factor));return e;
}
int main() {
    const auto changed=sender(true), unchanged=sender(false);
    const auto bytes=schedule(changed), reference=schedule(unchanged);
    std::vector<std::uint8_t> payload;
    for(auto byte:bytes)payload.push_back(byte.value);
    const std::vector<std::uint8_t> expected{0x88,60,0x89,67,0x86,0x80,0x81,0x95,20,0x87};
    check(payload==expected,"original MIDI, HOLD and ADC paths generate expected module protocol");
    // Independently executed original ROM bytes, identical warm inputs and
    // explicit UART phase37/ADC restart scenario. No candidate descriptor was
    // used to derive these10 frame-end expectations. These are model-condition
    // timestamps, not installed-unit measurements or universal delay bounds.
    constexpr std::array<std::uint64_t,10> ready{
        20005,21285,89125,90405,211749,258725,288165,338725,340005,609317};
    for(unsigned i=0;i<ready.size();++i)
        check(bytes[i].states==ready[i],"independent raw-ROM schedule agrees with module pin decoder");
    check(changed.state.foregroundPasses==142 && unchanged.state.foregroundPasses==144,
          "independent raw-ROM pass counts include the work caused by slider movement");
    unsigned frames=0;for(auto event:changed.wire)frames+=event.kind==U::EventKind::FrameEnd;
    check(frames==17,"all17 serial frames finish; seven MIDI-route frames stay off module input");
    for(auto anm:{Io::AnmWritePhase::RestartConversion,
                 Io::AnmWritePhase::PreserveConversionBoundary,
                 Io::AnmWritePhase::PreserveCompleteScanPhase})
        for(unsigned phase:{0,37,127}) {
            const auto variant=schedule(sender(true,phase,anm));
            std::vector<std::uint8_t> values;for(auto e:variant)values.push_back(e.value);
            check(values==expected,"declared UART and ADC phase scenarios preserve this performance");
        }
    check(bytes.size()>reference.size(),"moving the physical cutoff adds original module traffic");
    std::uint64_t changeAt=endTime, headerAt=endTime;
    auto fixedCutoff=bytes;
    for(unsigned i=0;i+1<bytes.size();++i)if(bytes[i].value==0x95 && bytes[i+1].value==20)
    {
        headerAt=bytes[i].states;changeAt=bytes[i+1].states;
        fixedCutoff[i+1].value=100;
    }
    check(changeAt>sliderTime && changeAt<600000,"cutoff byte follows real ADC and mainloop work");
    unsigned reads=0,adc=0;
    for(auto e:changed.cpu) {reads+=e.kind==A::EventKind::ReceiveRead;adc+=e.kind==A::EventKind::AdcConversion;}
    check(reads==midi().size() && adc>1000,"both serial parser and full ADC stay active");
    std::cout << "{\"wire_bytes\":" << bytes.size() << ",\"cutoff_ready\":" << changeAt
              << ",\"foreground_passes\":" << changed.state.foregroundPasses << ",\"audio\":[";
    bool first=true;
    for(unsigned rate:{44100,48000,96000})for(unsigned factor:{1,4}) {
        auto live=engine(bytes,rate,factor),control=engine(reference,rate,factor),block=engine(bytes,rate,factor);
        // Explicit counterfactual receiver control: same generated byte times,
        // interrupts and HOLD traffic, changing only95's cutoff payload20->100.
        // This prevents later sender-induced HOLD timing differences from
        // satisfying the assertion that the cutoff value reaches audible DSP.
        auto valueControl=engine(fixedCutoff,rate,factor);
        const unsigned n=rate*24/100;std::vector<float> l(n),r(n),cl(n),cr(n),bl(n),br(n),vl(n),vr(n);
        double energy=0,difference=0,valueDifferenceBeforeHoldOff=0;unsigned prefix=0,valuePrefix=0;
        for(unsigned i=0;i<n;++i) {
            live->process(&l[i],&r[i],1);control->process(&cl[i],&cr[i],1);
            valueControl->process(&vl[i],&vr[i],1);
            check(std::isfinite(l[i]) && std::isfinite(r[i]),"sender-derived audio stays finite");
            energy+=double(l[i])*l[i]+double(r[i])*r[i];
            difference+=std::pow(double(l[i])-cl[i],2)+std::pow(double(r[i])-cr[i],2);
            if(std::uint64_t(i+1)*4000000<=headerAt*rate) {
                check(l[i]==cl[i] && r[i]==cr[i],"audio is identical before the first added module header");++prefix;
            }
            if(std::uint64_t(i+1)*4000000<=changeAt*rate) {
                check(l[i]==vl[i] && r[i]==vr[i],"matched-timestamp control stays identical before cutoff payload");++valuePrefix;
            }
            if(std::uint64_t(i)*4000000>=changeAt*rate && std::uint64_t(i+1)*4000000<=600000ull*rate)
                valueDifferenceBeforeHoldOff+=std::pow(double(l[i])-vl[i],2)+std::pow(double(r[i])-vr[i],2);
        }
        for(unsigned i=0;i<n;) {unsigned count=std::min(83u,n-i);block->process(bl.data()+i,br.data()+i,int(count));i+=count;}
        check(l==bl && r==br,"continuous sender audio is block invariant");
        check(energy>1e-8 && difference>1e-8,"note sound and physical cutoff effect reach real DSP");
        check(valueDifferenceBeforeHoldOff>1e-8,"cutoff payload alone changes audible DSP before HOLD release");
        check(live->firmwareSerialStatus()==S::Status::ReachedTarget,"B-2 consumes full continuous stream");
        check(live->firmwareSerialState().control.ram[0x3d]==0 &&
              live->firmwareSerialState().control.ram[0x3e]==10,"B-2 original95 handler commits byte20 cutoff word");
        if(!first)std::cout << ',';first=false;
        std::cout << "{\"rate\":" << rate << ",\"factor\":" << factor << ",\"energy\":" << energy
                  << ",\"difference\":" << difference << ",\"value_difference_before_hold_off\":" << valueDifferenceBeforeHoldOff
                  << ",\"exact_prefix\":" << prefix << ",\"value_prefix\":" << valuePrefix << '}';
    }
    std::cout << "],\"assertions\":" << assertions << "}\n";
}
