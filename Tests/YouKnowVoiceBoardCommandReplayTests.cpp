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
    struct Card {
        std::uint16_t envelope;
        bool attack, decay, phase, gate, run, reset, high;
        float pitch;
        std::uint32_t count;
        double clocks, ramp;
    };
    static Card card(const YouKnowEngine& e,int n){const auto& v=e.voices_[static_cast<std::size_t>(n)];return{v.envelope.level,v.envelope.attackPhase,v.envelope.decayPhase,v.envelope.phase,v.envelope.gate,v.envelope.running,v.dcoResetPending,v.dco.pitOutHigh,v.currentMidi,v.dco.divider,v.dco.pitClocksToEvent,v.dco.rampValue};}
    static const auto& trace(const YouKnowEngine& e){return e.firmwareControlTrace_;}
    static bool pending(const YouKnowEngine& e){return e.assignmentRescanPending_;}
    static unsigned resetMask(const YouKnowEngine& e){return e.firmwareControlState_.ram[0];}
    static void prestage(YouKnowEngine& e){e.prestageDcoPitchTransaction(e.voices_[0],e.rangeClockClocksToFallingEdge_,0,false);}
    static bool waitingCount(const YouKnowEngine& e){return e.voices_[0].dco.pitState==YouKnowEngine::Dco::PitState::awaitingCount;}
    static void protectedPrestage(YouKnowEngine& e){auto& d=e.voices_[0].dco;d.pitWriteState=YouKnowEngine::Dco::PitWriteState::awaitingPitchPrestage;d.cpuStatesToWrite=YouKnowEngine::pitResetDiToControlStates-.5;}
    static bool waitingLoad(const YouKnowEngine& e){return e.voices_[0].dco.pitState==YouKnowEngine::Dco::PitState::awaitingInitialLoad;}
    static void seedDecay(YouKnowEngine& e){auto& v=e.voices_[0];v.envelope.level=1000;v.envelope.gate=v.envelope.running=true;v.envelope.attackPhase=v.envelope.decayPhase=v.envelope.phase=true;e.refreshFirmwareControlTrace();}
    static void advance(YouKnowEngine& e,double states){e.controlScanPhase_=states*YouKnowEngine::controlScanHz/YouKnowEngine::voiceCpuStateHz;e.advanceFirmwareControlEvents(e.controlScanPhase_);}
};
}
namespace {
using Engine=youknow::YouKnowEngine;
using Access=youknow::YouKnowTestAccess;
using Trace=youknow::FirmwareControlTrace;
int failures=0;
void require(bool condition,const char* message){if(!condition&&failures++<20)std::cerr<<message<<'\n';}
void process(Engine& e,int samples,int block=73,std::vector<float>* capture=nullptr){std::vector<float> l(block),r(block);for(int i=0;i<samples;i+=block){const int n=std::min(block,samples-i);e.process(l.data(),r.data(),n);if(capture)capture->insert(capture->end(),l.begin(),l.begin()+n);}}
std::unique_ptr<Engine> engine(){auto e=std::make_unique<Engine>();e->selectConverterTimingProfile(Engine::ConverterTimingProfile::FirmwareControlNoInterrupt);e->selectVoiceBoardCommandReplay(true);youknow::EngineParameters p;p.keyMode=youknow::KeyMode::Unison;p.attack=0;p.decay=.5f;p.sustain=.7f;p.cutoff=.6f;p.envDepth=0;p.calibration=0;p.velocityDepth=0;p.noiseLevel=0;e->setParameters(p);e->prepare(48000,1);process(*e,1600);return e;}
void checkScopeAndService(){
    auto normal=std::make_unique<Engine>();normal->prepare(48000,1);require(!normal->serviceVoiceBoardNoteOn(0,60),"command replay requires explicit selection and complete timing profile");
    auto e=engine();require(!Access::pending(*e),"replay mode does not create automatic unison reassignment");
    e->noteOn(70,1);e->reassertKeyMode();for(int i=0;i<6;++i)require(!Access::card(*e,i).gate,"ordinary keyboard path is documented as bypassed during command replay");
    require(!e->serviceVoiceBoardNoteOn(-1,60)&&!e->serviceVoiceBoardNoteOn(6,60)&&!e->serviceVoiceBoardNoteOn(0,128)&&!e->serviceVoiceBoardNoteOff(6),"invalid decoded service inputs are rejected");
    std::array<Access::Card,6> before;for(int i=0;i<6;++i)before[i]=Access::card(*e,i);
    require(e->serviceVoiceBoardNoteOn(0,60),"valid same-note service accepted");
    auto c=Access::card(*e,0);require(c.gate&&c.attack&&!c.run&&!c.reset,"same-note early branch sets gate/attack without reset or premature FF11");
    require(c.pitch==before[0].pitch,"service preserves current glide word");
    for(int i=0;i<6;++i){c=Access::card(*e,i);require(c.high==before[i].high&&c.count==before[i].count&&c.clocks==before[i].clocks&&c.ramp==before[i].ramp,"handler service itself invents no analog/timer phase jump");if(i)require(c.gate==before[i].gate&&c.envelope==before[i].envelope,"card service preserves other cards' envelopes/gates");}
    process(*e,1);require(Access::card(*e,0).run,"first completed FF11 store sees serviced gate");
    const auto oldPitch=Access::card(*e,0).pitch;
    require(e->serviceVoiceBoardNoteOn(0,72),"running pitch change accepted");
    c=Access::card(*e,0);require(!c.reset&&c.pitch==oldPitch,"running changed-byte command takes count-only path and waits for glide store");
    process(*e,64);require(Access::card(*e,0).pitch==72,"PORTAMENTO off transfers board byte only at the traced glide store");
    require(e->serviceVoiceBoardNoteOff(0),"off service accepted");c=Access::card(*e,0);require(!c.gate&&c.run&&!c.phase,"off clears gate/FF33 but retains FF11 until next pass");
    process(*e,1);require(!Access::card(*e,0).run,"subsequent FF11 store observes gate off");
    require(e->serviceVoiceBoardNoteOn(0,72),"equal byte after release accepted");require(!Access::card(*e,0).reset,"equal note does not reset when FF11 is clear");
    require(e->serviceVoiceBoardNoteOn(0,74),"different byte before next FF11 accepted");require(Access::card(*e,0).reset,"changed pitch reads still-clear FF11 even though FF10 was set by previous service");
}
void checkHoldRunSnapshot(){
    auto e=engine();require(e->serviceVoiceBoardNoteOn(0,60),"HOLD fixture note accepted");process(*e,1000);
    e->setSustainPedal(true);const auto before=Access::card(*e,0);
    require(e->serviceVoiceBoardNoteOff(0),"HOLD off-service accepted");auto after=Access::card(*e,0);
    require(!after.gate&&after.run&&after.phase==before.phase&&after.attack==before.attack&&after.decay==before.decay,"HOLD preserves FF11 and phase latches on voice off");
    process(*e,1);require(Access::card(*e,0).run,"new pass retains held FF11 after the gate cleared");
    require(e->serviceVoiceBoardNoteOn(0,73),"held changed pitch accepted");require(!Access::card(*e,0).reset,"changed held note remains count-only using actual FF11");
}
void checkPartialStore(){
    auto e=engine();Access::seedDecay(*e);const auto trace=Access::trace(*e);unsigned clear=0,store=0;
    for(std::size_t i=0;i<trace.count;++i){const auto& event=trace.events[i];if(event.kind==Trace::EventKind::RamByte&&event.card==7&&!(event.value&1)&&!clear)clear=event.states;if(event.kind==Trace::EventKind::Envelope&&event.card==0)store=event.states;}
    require(clear&&store>clear,"fixture spans phase-store→ENV-store interruption window");Access::advance(*e,clear);const auto before=Access::card(*e,0);
    require(e->serviceVoiceBoardNoteOff(0),"partial-pass off service accepted");auto after=Access::card(*e,0);require(after.envelope==1000&&!after.attack&&after.decay&&!after.phase&&!after.gate&&after.run,"actual service keeps completed phase store and not-yet-overwritten ENV/FF11");
    require(after.high==before.high&&after.clocks==before.clocks&&after.ramp==before.ramp,"partial ENV interruption preserves DCO phase");
}
void checkResetRequestAndBranchAreSeparate(){
    const auto setup=[] {auto e=engine();require(e->serviceVoiceBoardNoteOn(0,72),"reset witness service accepted");return e;};
    const auto clearTime=[](const Engine& e){const auto& trace=Access::trace(e);for(std::size_t i=0;i<trace.count;++i){const auto& event=trace.events[i];if(event.kind==Trace::EventKind::RamByte&&event.card==0&&!(event.value&1))return event.states;}return 0u;};
    auto before=setup();const unsigned clear=clearTime(*before);require(clear>0,"trace exposes FF00-clear instruction completion");
    Access::advance(*before,clear-.1);require(Access::card(*before,0).reset,"request remains pending before04B8 completes");
    require(before->serviceVoiceBoardNoteOn(1,60),"other-card service before04B8 accepted");require((Access::resetMask(*before)&1)!=0,"restart beforeclear preserves still-pending reset request");
    auto after=setup();Access::advance(*after,clearTime(*after));require(!Access::card(*after,0).reset,"04B8 completion clears live reset request beforeDI");
    const auto phaseBefore=Access::card(*after,0);require(after->serviceVoiceBoardNoteOn(1,60),"other-card service after04B8 accepted");
    require((Access::resetMask(*after)&1)==0&&!Access::card(*after,0).reset,"restart cannot resurrect consumed FF00 from an abandoned branch");
    const auto phaseAfter=Access::card(*after,0);require(phaseAfter.clocks==phaseBefore.clocks&&phaseAfter.high==phaseBefore.high,"abandoned pre-DI reset branch does not force timer phase");
    auto continuing=setup();Access::advance(*continuing,clearTime(*continuing));Access::prestage(*continuing);
    require(Access::waitingCount(*continuing),"without an interrupt the captured branch still writes Mode3 afterclearing its RAM bit");
    auto protectedPath=setup();Access::advance(*protectedPath,clearTime(*protectedPath));Access::protectedPrestage(*protectedPath);
    require(protectedPath->serviceVoiceBoardNoteOn(1,60),"service afterDI uses explicit protected-write boundary policy");
    require(Access::waitingLoad(*protectedPath)&&(Access::resetMask(*protectedPath)&1)==0,"DI-protected captured reset finishes once without reasserting FF00");
}
double phase(const Access::Card& c){const double high=(c.count+1)/2;return c.high?(high-c.clocks)/c.count:1.0-c.clocks/c.count;}
double wrapped(double value){return value-std::floor(value+.5);}
struct Render{std::vector<float> audio;std::array<Access::Card,6> card;};
Render replay(int syntheticGap,int block){
    auto e=engine();Render result;
    // Sensitivity inputs, not measured byte/RXB/ISR timestamps. A5's proven
    // card order is0..5; neither this test nor the API asserts wire contiguity.
    for(int card=0;card<6;++card){require(e->serviceVoiceBoardNoteOn(card,72),"ordered six-card service accepted");if(card<5)process(*e,syntheticGap,block,&result.audio);}
    process(*e,4800-static_cast<int>(result.audio.size()),block,&result.audio);
    for(int card=0;card<6;++card)result.card[card]=Access::card(*e,card);
    process(*e,960,block);
    for(int card=0;card<6;++card){const auto later=Access::card(*e,card);require(later.count==result.card[0].count,"steady six voices use one common integer pitch count");const double oldRelative=wrapped(phase(result.card[card])-phase(result.card[0]));const double newRelative=wrapped(phase(later)-phase(Access::card(*e,0)));require(std::abs(wrapped(oldRelative-newRelative))<1e-9,"fixed common count preserves relative phase without independent detune");}
    return result;
}
void checkAudibleSensitivity(){
    const auto simultaneous=replay(0,73),spaced=replay(80,73),partitioned=replay(80,1);
    require(spaced.audio==partitioned.audio,"service-sequence audio is host-block partition invariant");
    double error=0;for(std::size_t i=2400;i<spaced.audio.size();++i){const double d=simultaneous.audio[i]-spaced.audio[i];error+=d*d;}
    const double rms=std::sqrt(error/2400);require(rms>1e-7,"supplied service spacing changes steady unison audio through reset/phase history");
    std::cout<<"synthetic service-gap sensitivity RMS="<<rms<<" (0 vs80 host samples at48kHz; not hardware calibration)\n";
}
}
int main(){checkScopeAndService();checkHoldRunSnapshot();checkPartialStore();checkResetRequestAndBranchAreSeparate();checkAudibleSensitivity();std::cout<<(failures?"FAIL ":"PASS ")<<"VoiceBoardCommandReplay failures="<<failures<<'\n';return failures?1:0;}
