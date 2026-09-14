#include "../Source/DSP/YouKnowEngine.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

namespace youknow {
struct YouKnowTestAccess {
    static void seed(YouKnowEngine& engine) {
        for (int i=0;i<6;++i) {
            auto& voice=engine.voices_[static_cast<std::size_t>(i)];
            voice.envelope.level=static_cast<std::uint16_t>(1000+i*2000);
            voice.envelope.gate=voice.envelope.running=true;
            voice.envelope.attackPhase=voice.envelope.decayPhase=voice.envelope.phase=true;
            voice.currentMidi=static_cast<float>(48+i*3)+0.5f;
            voice.targetMidi=static_cast<float>(60+i);
        }
        engine.refreshFirmwareControlTrace();
    }
    static const auto& trace(const YouKnowEngine& e){return e.firmwareControlTrace_;}
    static const auto& env(const YouKnowEngine& e,int i){return e.voices_[static_cast<std::size_t>(i)].envelope;}
    static const auto& ram(const YouKnowEngine& e){return e.firmwareControlState_.ram;}
    static double phase(const YouKnowEngine& e){return e.controlScanPhase_;}
    static double period(const YouKnowEngine& e){return e.converterPassEndPhase_;}
    static void advance(YouKnowEngine& e,double states) {
        e.controlScanPhase_=states*YouKnowEngine::controlScanHz/YouKnowEngine::voiceCpuStateHz;
        e.advanceFirmwareControlEvents(e.controlScanPhase_);
    }
    static void releaseAndRestart(YouKnowEngine& e) {
        for(auto& v:e.voices_)v.envelope.noteOff();
        e.restartVoiceBoardScanAfterSerialVoiceCommand();
    }
    static void commit(YouKnowEngine& e,std::size_t ordinal){e.performConverterWrite(e.converterWriteOrder()[ordinal],e.activeParameters_);}
    static bool rescan(const YouKnowEngine& e){return e.assignmentRescanPending_;}
    static void atFinalWrite(YouKnowEngine& e) {
        e.controlScanPhase_=e.converterEventPhases_.back();
        e.nextConverterWrite_=22;
    }
    static float vca(const YouKnowEngine& e,int i){return e.voices_[static_cast<std::size_t>(i)].vcaControlTarget;}
};
}
namespace {
using Trace=youknow::FirmwareControlTrace;
using Engine=youknow::YouKnowEngine;
using Access=youknow::YouKnowTestAccess;
int failures=0;
void require(bool okay,const char* message){if(!okay&&failures++<24)std::cerr<<message<<'\n';}
unsigned word(const Trace::State& state,unsigned address){return state.ram[address]+256u*state.ram[address+1];}
void put(Trace::State& state,unsigned address,unsigned value){state.ram[address]=value&255;state.ram[address+1]=(value>>8)&255;}
Trace::Tables tables(){Trace::Tables t;t.attack.fill(0x4000);for(unsigned i=0;i<128;++i)t.portamento[i]=i;for(unsigned i=0;i<104;++i){t.pitchCv[i]=32+i*12;t.pitchDivider[i]=60000-i*500;}return t;}
Trace::State base(){Trace::State s;s.ram[0x37]=6;s.ram[0x1e]=0x40;for(unsigned i=0;i<6;++i){s.ram[9+i]=60;put(s,0x71+2*i,60*256);}return s;}
std::vector<Trace::Event> events(const Trace::Result& r,Trace::EventKind kind){std::vector<Trace::Event> out;for(std::size_t i=0;i<r.count;++i)if(r.events[i].kind==kind)out.push_back(r.events[i]);return out;}
unsigned product(unsigned x,unsigned coefficient){return(x>>8)*(coefficient>>8)+((x>>8)*(coefficient&255)>>8)+((x&255)*(coefficient>>8)>>8);}
unsigned envelope(unsigned level,bool run,bool attack,bool decay,bool phase,unsigned inc,unsigned sustain,unsigned dc,unsigned rc){
    if(run&&!phase)return std::min(0x3fffu,level+inc);
    if((run&&phase)||(!run&&attack&&decay))return level>sustain?sustain+product(level-sustain,dc):sustain;
    return product(level,rc);
}
void checkNominalInstructions(){
    auto s=base();auto t=tables();auto r=Trace::run(s,t);
    require(r.valid&&r.stoppedAt==0x02ec,"complete nominal pass must return to 02EC");
    auto cv=events(r,Trace::EventKind::Converter),inhibit=events(r,Trace::EventKind::Inhibit);
    require(cv.size()==23&&inhibit.size()==23,"23 physical enable/inhibit events");
    require(inhibit.front().states==111&&cv.front().states==186,"independently summed first RES anchors");
    for(unsigned i=0;i<23;++i)require(cv[i].states-inhibit[i].states==75,"loadDac inhibit→enable is 75 nominal NMOS states");
    for(unsigned i=4;i<9;++i)require(cv[i].states-cv[i-1].states==867,"running DCO interwrite independent audit");
    auto hold=s;hold.ram[0x1e]|=1;auto h=Trace::run(hold,t);auto hcv=events(h,Trace::EventKind::Converter);
    require(hcv.front().states-cv.front().states==18,"HOLD path adds 18 states before RES");
    s.ram[0]=0x3f;r=Trace::run(s,t);cv=events(r,Trace::EventKind::Converter);
    for(unsigned i=4;i<9;++i)require(cv[i].states-cv[i-1].states==973,"reset DCO interwrite independent audit");
    require(r.finalState.ram[0]==0,"six executed timer reset branches consume their bits");
    s.ram[0]=0;s.ram[10]=0;r=Trace::run(s,t);cv=events(r,Trace::EventKind::Converter);
    require(cv[4].states-cv[3].states==879,"low note clamp adds 12 states");
    s.ram[10]=130;r=Trace::run(s,t);cv=events(r,Trace::EventKind::Converter);
    require(cv[4].states-cv[3].states==893,"high note clamp adds 26 states");
    // DSLL sends EA15 through CY to RLL A four times at 069F..06AD.
    // NEC 78C10A p32 explicitly lists CY←EA15; NMOS costs remain separate.
    auto bend=base();bend.ram[0x5c]=8;bend.ram[0x5d]=bend.ram[0x5e]=255;
    bend.ram[0x84]=bend.ram[0x85]=255;bend.ram[6]=255;
    auto b=Trace::run(bend,t);require(b.valid,"bend carry witness executes");
    require(word(b.finalState,0x68)==static_cast<unsigned>(Engine::dcoPitchBendWordOffset(1,1)),"four DSLL→RLL carry steps extract complete bend word");
    require(b.finalState.ram[0x69]!=0,"carry witness must exercise the extracted upper nibble");
    bend.adcComplete=true;b=Trace::run(bend,t);require(!b.finalState.adcComplete,"SKIT acknowledges provided ADC completion flag");
}
void checkEnvelopeBranches(){
    const auto t=tables();
    for(unsigned flags=0;flags<32;++flags)for(unsigned level:{0u,1u,255u,256u,8192u,16383u}) {
        auto s=base();s.ram[7]=(flags&1)?63:0;s.ram[8]=(flags&2)?63:0;
        s.ram[0x33]=(flags&4)?63:0;s.ram[0x10]=(flags&8)?63:0;s.ram[0x11]=(flags&16)?63:0;
        s.ram[0x1e]|=1;put(s,0x23,8192);put(s,0x21,0xabcd);put(s,0x25,0x1234);
        for(unsigned i=0;i<6;++i)put(s,0x27+2*i,level);
        const auto r=Trace::run(s,t);require(r.valid,"all envelope latch branches execute");
        const bool run=(flags&24)!=0;
        const auto expected=envelope(level,run,flags&1,flags&2,flags&4,0x4000,8192,0xabcd,0x1234);
        for(unsigned i=0;i<6;++i)require(word(r.finalState,0x27+2*i)==expected,"instruction trace agrees with independent envelope branch/product oracle");
        const auto env=events(r,Trace::EventKind::Envelope),cv=events(r,Trace::EventKind::Converter);
        for(unsigned i=0;i<6;++i){require(env[i].states<cv[9+2*i].states,"ENV store precedes PWM/previous-card VCA");require(cv[11+2*i].value==(expected>>2),"pipelined VCA reads its own stored word");}
    }
    // Data-dependent execution makes the pass duration change; coefficient
    // values affect ENV work and VCF overflow branches, not a chart rescale.
    auto a=base();a.ram[0x10]=63;a.ram[7]=63;
    auto b=a;b.ram[0x33]=63;put(b,0x21,0xffff);put(b,0x23,8192);
    for(unsigned i=0;i<6;++i)put(b,0x27+2*i,16000);
    require(Trace::run(a,t).states!=Trace::run(b,t).states,"attack and decay paths have different complete pass durations");
}
void checkPortamentoAndSpacing(){
    auto t=tables();auto s=base();s.ram[0x7d]=127;
    std::array<unsigned,6> current{48*256,60*256,63*256+12,72*256+1,90*256,60*256+32};
    std::array<unsigned,6> target{60,60,63,72,70,61};
    for(unsigned i=0;i<6;++i){put(s,0x71+2*i,current[i]);s.ram[9+i]=target[i];}
    auto r=Trace::run(s,t);auto porta=events(r,Trace::EventKind::Portamento);auto cv=events(r,Trace::EventKind::Converter);
    for(unsigned i=0;i<6;++i){const int delta=static_cast<int>(target[i]*256)-static_cast<int>(current[i]);require(word(r.finalState,0x71+2*i)==current[i]+std::clamp(delta,-127,127),"six distinct glide words match saturating fixed-point oracle");require(porta[i].states<cv[2].states,"all glide stores complete before SUB");}
    std::uint32_t random=0x78945321,minimumGap=~0u;
    for(unsigned trial=0;trial<3000;++trial){
        auto rnd=[&]{random^=random<<13;random^=random>>17;random^=random<<5;return random;};
        auto q=base();q.ram[0]=rnd()&63;q.ram[7]=rnd()&63;q.ram[8]=rnd()&63;q.ram[0x10]=rnd()&63;q.ram[0x11]=rnd()&63;q.ram[0x33]=rnd()&63;
        q.ram[0x1e]=rnd()&255;q.ram[0x37]=rnd()&7;q.ram[0x4a]=rnd()&3;q.ram[0x7d]=rnd()&255;q.ram[0x45]=rnd()&127;
        for(unsigned address:{0x21u,0x25u,0x58u,0x6cu,0x68u})put(q,address,rnd()&65535);
        for(unsigned address:{0x23u,0x3du,0x4du,0x56u,0x5au,0x65u})put(q,address,rnd()&0x1fff);
        for(unsigned address:{0x41u,0x42u,0x47u,0x48u,0x49u,0x61u,0x64u})q.ram[address]=rnd()&255;
        q.ram[0x5c]=(rnd()&1)?8:0;
        for(unsigned i=0;i<4;++i){q.ram[0x5d+i]=rnd()&255;q.ram[0x80+i]=rnd()&255;q.ram[0x84+i]=rnd()&255;}
        for(unsigned i=0;i<6;++i){q.ram[9+i]=rnd()&127;put(q,0x71+2*i,rnd()&0x7fff);put(q,0x27+2*i,rnd()&0x3fff);}
        const auto result=Trace::run(q,t);require(result.valid,"bounded varied firmware branches terminate");
        const auto writes=events(result,Trace::EventKind::Converter);
        for(std::size_t i=1;i<writes.size();++i)minimumGap=std::min(minimumGap,writes[i].states-writes[i-1].states);
    }
    require(minimumGap>125,"32kHz profile grid fits one converter event in every interval");
    std::cout<<"minimum exercised converter gap "<<minimumGap<<" states\n";
}
void checkCausalEngineStores(){
    auto e=std::make_unique<Engine>();e->selectConverterTimingProfile(Engine::ConverterTimingProfile::FirmwareControlNoInterrupt);e->prepare(48000,1);
    youknow::EngineParameters p;p.decay=0;p.release=1;p.sustain=0;p.calibration=0;p.velocityDepth=0;e->setParameters(p);Access::seed(*e);
    const auto initial=Access::trace(*e);auto envs=events(initial,Trace::EventKind::Envelope);
    unsigned phaseClear=0;
    for(std::size_t i=0;i<initial.count;++i){const auto& ev=initial.events[i];if(ev.kind==Trace::EventKind::RamByte&&ev.card==7&&!(ev.value&1)){phaseClear=ev.states;break;}}
    require(phaseClear&&phaseClear<envs.front().states,"attack latch clears in RAM before envelope word store");
    Access::advance(*e,phaseClear-0.01);require(Access::env(*e,0).attackPhase,"store does not apply before instruction completes");
    Access::advance(*e,phaseClear);require(!Access::env(*e,0).attackPhase&&Access::env(*e,0).level==1000,"partial pass applies phase latch without prematurely committing ENV");
    Access::releaseAndRestart(*e);require(!Access::env(*e,0).attackPhase&&Access::env(*e,0).level==1000,"command restart retains completed phase store and preceding ENV value");
    const auto restart=Access::trace(*e);const auto release=events(restart,Trace::EventKind::Envelope);
    const unsigned expected=product(1000,Engine::envelopeDecayReleaseMultiplier(1));
    require(release.front().value==expected,"interrupted decay followed by key-up chooses release using retained flag");
    Access::advance(*e,release.front().states);require(Access::env(*e,0).level==expected,"actual engine consumes trace envelope event");
    for(unsigned i=0;i<6;++i)Access::commit(*e,11+2*i);
    const auto outputs=events(restart,Trace::EventKind::Converter);
    for(unsigned i=0;i<6;++i)require(std::abs(Access::vca(*e,i)-outputs[11+2*i].value/4095.0f)<1e-7,"converter commits frozen own-card VCA payload");
}
struct Snapshot{std::array<unsigned,6> level{};std::array<std::uint8_t,256> ram{};std::uint32_t states{};};
Snapshot render(double rate,int factor,int block){
    auto e=std::make_unique<Engine>();e->selectConverterTimingProfile(Engine::ConverterTimingProfile::FirmwareControlNoInterrupt);e->prepare(rate,factor);
    youknow::EngineParameters p;p.attack=.8f;p.decay=.7f;p.sustain=.4f;p.release=.8f;p.portamento=.2f;p.calibration=0;p.velocityDepth=0;e->setParameters(p);e->noteOn(60,1);
    std::vector<float> left(block),right(block);const int count=static_cast<int>(rate*.09);
    for(int start=0;start<count;start+=block)e->process(left.data(),right.data(),std::min(block,count-start));
    require(e->firmwareControlTraceValid(),"live engine maintains valid nominal trace");
    Snapshot result;for(int i=0;i<6;++i)result.level[i]=Access::env(*e,i).level;result.ram=Access::ram(*e);result.states=e->firmwareControlPassStates();return result;
}
void checkRenderGrids(){
    for(double rate:{8000.,44100.,48000.,96000.})for(int factor:{1,4}){
        auto a=render(rate,factor,1),b=render(rate,factor,73);require(a.level==b.level&&a.ram==b.ram&&a.states==b.states,"live instruction/ENV state is block partition invariant");
    }
    auto a=render(48000,1,73),b=render(48000,4,73);require(a.level==b.level&&a.ram==b.ram&&a.states==b.states,"full firmware state independent of quality grid");
}
void checkAssignmentRescan(){
    auto e=std::make_unique<Engine>();e->selectConverterTimingProfile(Engine::ConverterTimingProfile::FirmwareControlNoInterrupt);e->prepare(768000,1);
    youknow::EngineParameters p;p.keyMode=youknow::KeyMode::Unison;e->setParameters(p);e->noteOn(60,1);
    std::array<float,4096> left{},right{};e->process(left.data(),right.data(),4096);e->process(left.data(),right.data(),4096);
    require(!Access::rescan(*e),"initial unison assignment has completed before reassert test");
    e->reassertKeyMode();require(Access::rescan(*e),"unison reassert arms a separated gate-off pass");
    const double period=Access::period(*e);
    // The last DAC poll itself is not completion of the nominal loop.
    Access::atFinalWrite(*e);e->process(left.data(),right.data(),1);
    require(Access::rescan(*e),"reassignment must wait past final NOISE write until 07B5 returns");
    for(int guard=0;guard<128&&Access::rescan(*e);++guard)e->process(left.data(),right.data(),1);
    require(!Access::rescan(*e)&&period>0,"completed off-gate pass reassigns held unison key");
}
}
int main(){checkNominalInstructions();checkEnvelopeBranches();checkPortamentoAndSpacing();checkCausalEngineStores();checkRenderGrids();checkAssignmentRescan();std::cout<<(failures?"FAIL ":"PASS ")<<"FirmwareControlTrace failures="<<failures<<'\n';return failures?1:0;}
