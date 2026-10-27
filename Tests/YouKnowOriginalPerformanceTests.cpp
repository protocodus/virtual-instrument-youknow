#include "../Source/DSP/YouKnowEngine.h"
#include "../Source/DSP/YouKnowProductFidelity.h"
#include "../Source/DSP/YouKnowActiveProductFidelity.h"
#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <span>
#include <tuple>
#include <vector>
static bool watch=false;
static unsigned allocations=0;
void* operator new(std::size_t n) { if(watch) ++allocations; if(auto p=std::malloc(n?n:1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
static void check(bool b,const char* message) { if(!b) { std::fprintf(stderr,"FAIL: %s\n",message); std::abort(); } }
using namespace youknow;
using E=YouKnowEngine;
using ParameterSource=E::ParameterInputSource;
namespace youknow {
struct YouKnowTestAccess {
    static bool gate(const E& e,unsigned i) { return e.voices_[i].keyDown; }
    static auto held(const E& e,unsigned i) { return e.heldNoteCounts_[i]; }
    static bool enqueue(E& e,std::span<const std::uint8_t> bytes) {
        return e.originalPerformance_.message(bytes,static_cast<std::uint64_t>(e.firmwareSerialAudioStates_));
    }
    static void warm(E& e,double seconds,float fraction) {
        e.thermalWarmupSeconds_=seconds; e.thermalWarmupFraction_=fraction;
    }
    static auto thermal(const E& e) {
        return std::pair{e.thermalWarmupSeconds_,e.thermalWarmupFraction_};
    }
    static auto ic40(const E& e) { return e.firmwareSerialIc40_; }
    static auto chorus(const E& e) { return e.firmwareSerialCircuitParameters_.chorus; }
    static float lfo(const E& e) { return e.lfoValue_; }
    static float delayLevel(const E& e) { return e.lfoDelayLevel_; }
    static double delayProgress(const E& e) { return e.lfoSyncDelayProgress_; }
    static unsigned cpuWord(const E& e,unsigned a) {
        const auto& ram=e.firmwareSerialState_.control.ram; return ram[a]+256u*ram[a+1];
    }
    static auto cardGains(const E& e) { return e.originalUnisonCardGain_; }
    static auto cardHistory(const E& e,unsigned card) {
        const auto& v=e.voices_[card];
        return std::tuple{v.active,v.keyDown,v.sustained,v.envelope.level,
            v.currentMidi,v.dcoCv,v.dco.rampValue,v.dco.pitClocksToEvent,
            v.cutoffCounts,v.filter.state,v.vcaControl,v.vca,v.energy,
            v.noiseState,v.vcaInputVolts};
    }
};
}
EngineParameters patch() {
    EngineParameters p;
    p.lfoRate=64.f/127; p.lfoDelay=p.dcoLfoDepth=p.pwmDepth=p.noiseLevel=0;
    p.cutoff=100.f/127; p.resonance=40.f/127; p.envDepth=p.vcfLfoDepth=p.keyFollow=0;
    p.vcaLevel=100.f/127; p.attack=0; p.decay=p.release=p.subLevel=20.f/127; p.sustain=100.f/127;
    p.range=DcoRange::Eight; p.pulseEnabled=true; p.sawEnabled=false;
    p.pwmSource=PwmSource::Lfo; p.highPass=HighPassMode::Three;
    p.chorus=ChorusMode::Off; p.keyMode=KeyMode::Poly2;
    p.calibration=p.aging=p.chorusNoise=p.velocityDepth=0;
    ProductFidelityProfile::applyTo(p);
    return p;
}
std::unique_ptr<E> create(double rate,int factor,const EngineParameters& p=patch()) {
    auto e=std::make_unique<E>();
    ProductFidelityProfile::configureBeforePrepare(*e);
    e->setParameters(p); e->setOriginalPerformanceMode(true); e->prepare(rate,256,factor);
    return e;
}
void render(E& e,unsigned count,unsigned block,std::vector<float>* audio=nullptr) {
    std::array<float,256> l{},r{};
    while(count) {
        const unsigned n=std::min(count,block);
        watch=true; e.process(l.data(),r.data(),int(n)); watch=false;
        check(allocations==0,"original callback allocates nothing");
        if(!e.originalPerformanceHealthy()) {
            std::fprintf(stderr,"A5 pc=%x now=%llu B2 status=%u\n",e.originalPerformance().state().assigner.registers.pc,
                (unsigned long long)e.originalPerformance().state().assigner.now,unsigned(e.firmwareSerialStatus()));
        }
        check(e.originalPerformanceHealthy(),"continuous A5/module/B2 pipeline healthy");
        for(unsigned i=0;i<n;++i) {
            check(std::isfinite(l[i])&&std::isfinite(r[i]),"original audio finite");
            if(audio) { audio->push_back(l[i]); audio->push_back(r[i]); }
        }
        count-=n;
    }
}
std::vector<float> score(double rate,int factor,unsigned block) {
    auto e=create(rate,factor);
    std::vector<float> audio;
    e->noteOn(60,1); e->noteOn(67,1); e->noteOn(72,1);
    render(*e,4096,block,&audio);
    if(!e->getActiveVoiceCount()) {
        const auto& a=e->originalPerformance().state().assigner;
        const auto& b=e->firmwareSerialState();
        std::fprintf(stderr,"A5 now=%llu pc=%x midi=%x voices=%x,%x,%x TX=%u; B2 now=%llu pc=%x ordinal=%llu ram0=%x ram1=%x gates=%x ENV=%x\n",
            (unsigned long long)a.now,a.registers.pc,a.ram[0x46],a.ram[0x88],a.ram[0x89],a.ram[0x8a],
            unsigned(e->originalPerformance().pending()),(unsigned long long)b.now,b.registers.pc,
            (unsigned long long)b.ordinal,b.control.ram[0],b.control.ram[1],b.control.ram[0x11],b.control.ram[0x27]);
    }
    check(e->getActiveVoiceCount()>0,"host notes reach original voice cards");
    e->setSustainPedal(true); render(*e,1024,block,&audio);
    e->noteOff(60); e->noteOff(67); e->noteOff(72); render(*e,4096,block,&audio);
    e->setSustainPedal(false); render(*e,2048,block,&audio);
    auto p=patch(); p.cutoff=30.f/127; p.sawEnabled=true; p.pulseEnabled=false;
    e->setParameters(p); render(*e,4096,block,&audio);
    check(e->originalPerformance().state().assigner.ram[0x95]==30,"host panel edit reaches original A5 stored cutoff");
    e->noteOn(55,1); render(*e,2048,block,&audio);
    e->releaseAllNotes(); render(*e,2048,block,&audio);
    check(!YouKnowTestAccess::held(*e,55),"all notes off clears host bookkeeping");
    e->allNotesOff(); check(e->originalPerformance().pending()==0,"panic clears pending DIN bytes");
    check(e->firmwareSerialAudioStates()==0 && e->originalPerformance().state().assigner.now==0,"panic resets all three clocks together");
    e->noteOn(60,1); render(*e,1024,block,&audio);
    check(e->getActiveVoiceCount()>0,"new performance after panic works");
    return audio;
}

unsigned gates(const E& e) {
    unsigned mask=0;
    for(unsigned i=0;i<6;++i) if(YouKnowTestAccess::gate(e,i)) mask|=1u<<i;
    return mask;
}

EngineParameters voiceCountPatch() {
    auto p=patch();
    p.keyMode=KeyMode::Unison;
    p.sawEnabled=p.pulseEnabled=false;
    p.subLevel=p.resonance=p.envDepth=p.keyFollow=0;
    p.cutoff=1; p.highPass=HighPassMode::One;
    p.noiseLevel=p.vcaLevel=32.f/127;
    p.vcaMode=VcaMode::Gate; p.sustain=1; p.release=0;
    return p;
}
double power(const std::vector<float>& audio) {
    double result=0;
    for(float sample:audio) result+=static_cast<double>(sample)*sample;
    return result/audio.size();
}
auto firmwareSnapshot(const E& e) {
    const auto& a=e.originalPerformance().state().assigner;
    const auto& b=e.firmwareSerialState();
    return std::tuple{a.ram,a.now,a.foregroundPasses,a.registers.pc,
        b.control.ram,b.now,b.ordinal,b.registers.pc,
        e.originalPerformance().state().uart.frameOrdinal};
}
void originalUnisonVoiceCounts() {
    auto p=voiceCountPatch();
    auto full=create(48000,1,p);
    full->noteOn(60,1); render(*full,8192,173);
    std::vector<float> fullAudio;
    render(*full,8192,173,&fullAudio);
    const double fullPower=power(fullAudio);
    check(fullPower>1.e-15,"voice-count audio fixture is audible");
    for(int requested:{1,2,6,12}) {
        p.polyphony=requested;
        auto e=create(48000,1,p);
        e->noteOn(60,1); render(*e,8192,173);
        std::vector<float> audio;
        render(*e,8192,173,&audio);
        const int audible=std::min(requested,6);
        check(e->getEffectiveVoiceLimit()==audible,
              "Original Unison exposes the physical six-card maximum");
        check(gates(*e)==63,"reduced Original Unison keeps all six firmware gates");
        check(e->getActiveVoiceCount()==audible
              && e->getDisplayVoiceMask()==((1<<audible)-1),
              "Original Unison count and lamps describe the audible cards");
        const auto gains=YouKnowTestAccess::cardGains(*e);
        for(unsigned card=0;card<6;++card) {
            check(gains[card]==(card<static_cast<unsigned>(audible)?1.f:0.f),
                  "startup card selection reaches exact gain endpoints");
            check(YouKnowTestAccess::cardHistory(*e,card)
                      ==YouKnowTestAccess::cardHistory(*full,card),
                  "muting a card preserves its oscillator/filter/VCA/noise history");
        }
        check(firmwareSnapshot(*e)==firmwareSnapshot(*full),
              "audible Unison count leaves the complete six-card firmware timeline intact");
        // Shared noise is coherent across the identical nominal cards. At
        // this small drive the output should scale with the number summed,
        // not merely display fewer lamps while retaining six-card audio.
        const double ratio=std::sqrt(power(audio)/fullPower);
        check(std::abs(ratio-static_cast<double>(audible)/6)<.02,
              "reduced Original Unison changes actual audio by the selected card count");
        if(audible==6)
            check(audio==fullAudio,"six and above-six Original Unison retain exact six-card audio");
    }
    for(auto mode:{KeyMode::Poly1,KeyMode::Poly2}) {
        p=voiceCountPatch(); p.keyMode=mode; p.polyphony=1;
        auto e=create(48000,1,p);
        for(int note:{48,52,55,60,64,67}) e->noteOn(note,1);
        render(*e,4096,173);
        check(e->getEffectiveVoiceLimit()==6 && gates(*e)==63
              && e->getActiveVoiceCount()==6 && e->getDisplayVoiceMask()==63,
              "Original Poly modes retain six audible assignments even at requested count one");
        e->releaseAllNotes(); render(*e,4096,173);
        check(gates(*e)==0 && e->getActiveVoiceCount()==0,
              "all Original Poly assignments release at requested count one");
    }
}
auto originalUnisonCountTransitions(unsigned block) {
    auto p=voiceCountPatch();
    auto e=create(48000,1,p);
    std::vector<float> audio;
    e->noteOn(60,1); render(*e,4096,block,&audio);
    p.polyphony=1; e->setParameters(p);
    render(*e,1,block,&audio);
    const auto fading=YouKnowTestAccess::cardGains(*e);
    check(fading[0]==1 && fading[1]>0 && fading[1]<1,
          "live count reduction fades card output rather than stepping it");
    check(gates(*e)==63,"count reduction does not manufacture firmware note offs");
    render(*e,255,block,&audio);
    check(e->getActiveVoiceCount()==1 && e->getDisplayVoiceMask()==1,
          "count reduction settles to one audible card");
    p.polyphony=2; e->setParameters(p); render(*e,256,block,&audio);
    check(e->getActiveVoiceCount()==2 && e->getDisplayVoiceMask()==3,
          "count increase fades a running second card into the stack");
    e->setSustainPedal(true); render(*e,2048,block,&audio);
    e->noteOff(60); render(*e,2048,block,&audio);
    check(gates(*e)==0 && e->getActiveVoiceCount()==2,
          "sustain retains the selected Original Unison cards after key up");
    e->setSustainPedal(false); render(*e,4096,block,&audio);
    check(e->getActiveVoiceCount()==0,"pedal up releases selected and hidden Unison cards");
    e->noteOn(67,1); render(*e,4096,block,&audio);
    check(gates(*e)==63 && e->getActiveVoiceCount()==2,
          "a fresh note after release restores the requested stack without stale tails");
    e->allNotesOff();
    check(e->getActiveVoiceCount()==0 && e->getDisplayVoiceMask()==0,
          "Original panic clears audible count and lamps");
    e->noteOn(60,1); render(*e,4096,block,&audio);
    check(e->getActiveVoiceCount()==2,"Original panic retains the requested stack size");
    e->reset(); e->noteOn(60,1); render(*e,4096,block,&audio);
    check(gates(*e)==63 && e->getActiveVoiceCount()==2,
          "cold reset primes the requested audible stack while retaining six-card firmware");
    return std::tuple{audio,firmwareSnapshot(*e),e->getDisplayVoiceMask()};
}
void originalUnisonHiddenTails() {
    auto p=voiceCountPatch(); p.polyphony=1;
    p.vcaMode=VcaMode::Envelope; p.release=90.f/127;
    auto e=create(48000,1,p);
    e->noteOn(60,1); render(*e,8192,173);
    e->noteOff(60); render(*e,2048,173);
    check(gates(*e)==0 && e->getActiveVoiceCount()==1,
          "hidden-tail fixture retains an audible envelope release");
    p.keyMode=KeyMode::Poly1; e->setParameters(p); render(*e,4096,173);
    check((e->originalPerformance().state().assigner.ram[0xc8]&6)==2,
          "hidden-tail fixture reaches actual Original Poly1");
    const auto gains=YouKnowTestAccess::cardGains(*e);
    for(unsigned card=1;card<6;++card)
        check(gains[card]==0,"leaving Unison cannot resurrect a suppressed release tail");
    // A new physical assignment admits a formerly suppressed card without
    // resetting the other cards' release histories.
    for(int note:{48,52,55,60,64,67}) e->noteOn(note,1);
    render(*e,4096,173);
    check(gates(*e)==63 && e->getActiveVoiceCount()==6 && e->getDisplayVoiceMask()==63,
          "fresh Poly assignments reopen all six previously masked cards");
    p.release=0; e->setParameters(p); render(*e,4096,173);
    e->releaseAllNotes(); render(*e,8192,173);
    check(gates(*e)==0 && e->getActiveVoiceCount()==0,
          "Unison-to-Poly mode changes leave no stuck audible tails");
}
void allocatorAndBitmap() {
    // Literal original A-5 0AAF..0AF7/0AF8..0B2F: Poly1 pushes each
    // released card behind the other free cards, retaining same-note reuse.
    // 0A76..0A87 Poly2 instead scans the fixed note table from card0.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic1.txt
    for(auto mode:{KeyMode::Poly1,KeyMode::Poly2}) {
        auto p=patch(); p.keyMode=mode;
        auto e=create(48000,1,p);
        e->noteOn(60,1); render(*e,2048,173);
        check(gates(*e)==1,"first original assignment uses card0");
        e->noteOff(60); render(*e,2048,173);
        check(gates(*e)==0,"original note off clears the assigned card gate");
        e->noteOn(62,1); render(*e,2048,173);
        check(gates(*e)==(mode==KeyMode::Poly1?2u:1u),
              "actual Poly1 free-card rotation differs from Poly2 fixed allocation");
        e->noteOff(62); render(*e,2048,173);
        e->noteOn(62,1); render(*e,2048,173);
        check(gates(*e)==(mode==KeyMode::Poly1?2u:1u),
              "original released-note history reuses its card");
    }
    auto e=create(48000,1);
    e->noteOn(60,1); e->noteOn(60,1); render(*e,2048,173);
    check(gates(*e)==1 && YouKnowTestAccess::held(*e,60)==1,
          "repeated original MIDI Ons share one bitmap bit");
    e->noteOff(60); render(*e,2048,173);
    check(gates(*e)==0 && YouKnowTestAccess::held(*e,60)==0,
          "one original Off clears repeated Ons rather than retaining a host press count");
    e->noteOn(60,1); render(*e,2048,173);
    e->noteOn(60,0); render(*e,2048,173);
    check(gates(*e)==0,"original velocity-zero On follows the literal Off path");
}
std::tuple<std::vector<float>,std::uint64_t,unsigned> repeatedMode(unsigned block) {
    auto p=patch(); p.keyMode=KeyMode::Poly1;
    auto e=create(48000,1,p);
    e->noteOn(60,1); render(*e,2048,block);
    const auto frames=e->originalPerformance().state().uart.frameOrdinal;
    std::vector<float> audio;
    e->reassertKeyMode(); render(*e,4096,block,&audio);
    const auto sent=e->originalPerformance().state().uart.frameOrdinal-frames;
    // A-5 0303 calls0B70 even when the desired LED was already selected:
    // FD plus six voice-offs must cross the physical module output.
    check(sent>=7,"same-mode physical press reaches original stop-all side effects");
    check((e->originalPerformance().state().assigner.ram[0xc8]&6)==2,
          "same-mode press retains actual Poly1 selection");
    e->reassertKeyMode(); render(*e,4096,block,&audio);
    check(e->originalPerformance().state().uart.frameOrdinal-frames>=14,
          "release is scanned and a second identical contact has a new edge");
    return {std::move(audio),e->originalPerformance().state().uart.frameOrdinal,gates(*e)};
}
void burstAndRecovery() {
    std::array<std::uint8_t,1080> burst{};
    for(unsigned i=0;i<burst.size()/3;++i) {
        burst[3*i]=0xb0; burst[3*i+1]=1; burst[3*i+2]=static_cast<std::uint8_t>((i%126)+1);
    }
    auto run=[&](unsigned block) {
        auto e=create(48000,1);
        check(YouKnowTestAccess::enqueue(*e,burst),"queue admits a burst above bridge prefix capacity");
        check(e->originalPerformance().pending()>FirmwareAssignerAudioBridge::maximumInputEvents,
              "burst actually exceeds1024 input events");
        std::vector<float> audio;
        render(*e,18000,block,&audio);
        check(e->originalPerformance().pending()==0,"bounded prefixes drain the complete long burst");
        // Original A5 CC1 receive0762..077E marks the last received value
        // with its pending bit, then foreground forwards the unmarked byte.
        check((e->originalPerformance().state().assigner.ram[0xa2]&127)==burst.back(),
              "all serialized controller bytes retain their order through the long burst");
        return std::tuple{std::move(audio),e->originalPerformance().state().uart.frameOrdinal};
    };
    check(run(1)==run(173),"long queued DIN burst is audio and wire invariant to host blocks");
    std::array<std::uint8_t,4095> full{};
    for(unsigned i=0;i<full.size()/3;++i) { full[3*i]=0xb0; full[3*i+1]=1; full[3*i+2]=32; }
    auto e=create(48000,1);
    check(YouKnowTestAccess::enqueue(*e,full),"queue fills to its documented byte limit");
    e->noteOn(60,1);
    check(!e->originalPerformanceHealthy() && e->originalPerformance().pending()==0
          && gates(*e)==0 && e->firmwareSerialAudioStates()==0,
          "input overflow fails closed, clears stale input and exposes unhealthy status");
    e->reset(); e->noteOn(60,1); render(*e,2048,173);
    check(gates(*e)==1,"explicit reset recovers ordinary performance after queue failure");
    check(YouKnowTestAccess::enqueue(*e,full),"parameter overflow scenario fills the queue");
    auto p=patch(); p.cutoff=17.f/127; p.masterTuneCents=12.5f;
    e->setParameters(p,ParameterSource::MidiReflection);
    const std::array<std::uint8_t,7> cutoff{0xf0,0x41,0x32,0,5,17,0xf7};
    check(!e->receiveOriginalPerformanceMidi(cutoff),"raw tone overflow reports failure");
    check(!e->originalPerformanceHealthy() && e->originalPerformance().pending()==0,
          "tone queue overflow resets coherently and exposes failure");
    check(e->originalPerformance().state().assigner.ram[0x95]==17
          && e->firmwareSerialState().control.ram[0x88]==160,
          "overflow recovery seeds the newly adopted tone and physical Tune, not the old image");
    e->reset(); e->noteOn(67,1); render(*e,2048,173);
    check(gates(*e)==1,"newly adopted tone remains playable after failure recovery");
}

// A-5 062A..066A stores each received full-tone payload byte and marks it
// pending, including equal values; 0683..06AF distinguishes 30/31 from 32.
// These literal byte counts and open receive frontiers are ROM/wire checks,
// not fitted attack latency or an atomic-F7 patch convention.
// https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic1.txt#L869-L945
constexpr std::array<std::uint8_t,24> currentTone{
    0xf0,0x41,0x31,0,0,
    64,0,0,0,0,100,40,0,0,0,100,0,20,100,20,20,
    0x2a,0,0xf7
};

void rawToneReceiptAndOrdering(unsigned block) {
    auto e=create(48000,1);
    render(*e,4096,block);
    const auto frames=e->originalPerformance().state().uart.frameOrdinal;
    watch=true;
    const bool accepted=e->receiveOriginalPerformanceMidi(currentTone);
    watch=false;
    check(accepted && allocations==0,"equal raw full tone is accepted without allocation");
    check(e->originalPerformance().pending()==currentTone.size(),
          "equal full-tone values retain the actual 24-byte transport");
    e->setParameters(patch(),ParameterSource::MidiReflection);
    check(e->originalPerformance().pending()==currentTone.size(),
          "equal incoming tone reflection neither drops nor duplicates its wire bytes");
    e->noteOn(60,1);
    // The note's velocity is byte 27, ready no earlier than 27*1280 states:
    // 8.64 ms at the original 4 MHz state rate. 400 frames at 48k are 8.333 ms.
    render(*e,400,block);
    check(gates(*e)==0,"following note cannot overtake the equal full-tone packet");
    render(*e,4096,block);
    check(gates(*e)==1,"following note executes after the preserved packet");
    check(e->originalPerformance().state().uart.frameOrdinal>frames+2,
          "equal tone values still produce physical module traffic");
    e->noteOff(60); render(*e,4096,block);
    const auto repeatedFrames=e->originalPerformance().state().uart.frameOrdinal;
    check(e->receiveOriginalPerformanceMidi(currentTone),"identical repeated full tone accepted");
    render(*e,4096,block);
    check(e->originalPerformance().state().uart.frameOrdinal>repeatedFrames,
          "identical repeated full tone is forwarded again by the original foreground");
}

void rawToneIncrementalFrontiers(unsigned block) {
    auto e=create(48000,1);
    render(*e,4096,block);
    auto changed=currentTone;
    for(unsigned i=0;i<16;++i) changed[5+i]=static_cast<std::uint8_t>(5+i);
    changed[21]=0x32; changed[22]=4;
    check(e->receiveOriginalPerformanceMidi(changed),"literal changed full tone accepted");
    // Five header bytes precede the first value. At 48k, 107/122/367 frames
    // end about 8917/10167/30583 states after the declared frame start;
    // respectively before bytes 7, 8 and 24 can become ready. The allowance
    // after the preceding byte includes the actual receive ISR work.
    render(*e,107,block);
    const auto& first=e->originalPerformance().state().assigner.ram;
    check(first[0x90]==5 && first[0x91]==0 && first[0x95]==100,
          "full-tone first payload commits while later values remain untouched");
    render(*e,15,block);
    const auto& second=e->originalPerformance().state().assigner.ram;
    check(second[0x90]==5 && second[0x91]==6 && second[0x92]==0,
          "second full-tone payload commits without an individual parameter frame");
    render(*e,245,block);
    const auto& last=e->originalPerformance().state().assigner.ram;
    for(unsigned i=0;i<16;++i)
        check(last[0x90+i]==changed[5+i],"all continuous payloads commit before final F7");
    check(last[0x8e]==changed[21] && last[0x8f]==changed[22],
          "both packed switch payloads commit before final F7");
    check(e->originalPerformance().pending()==1,"final F7 has not arrived at the last payload frontier");
    render(*e,4096,block);
    check(e->originalPerformance().pending()==0,"full-tone receive completes and drains");
}

void physicalPanelAndIncomingTone(unsigned block) {
    auto e=create(48000,1);
    render(*e,4096,block);
    auto p=patch();
    const auto& initial=e->originalPerformance().state().assigner.ram;
    check(initial[0x95]==100 && initial[0x5e]==204 && initial[0x72]==204,
          "cutoff starts at the independent warm panel raw/stored coordinates");
    p.cutoff=20.f/127;
    e->setParameters(p);
    check(e->originalPerformance().pending()==0,"physical slider does not create incoming DIN traffic");
    check(e->originalPerformance().state().assigner.ram[0x95]==100,
          "slider movement cannot anticipate its ADC and foreground store");
    render(*e,4096,block);
    const auto& panel=e->originalPerformance().state().assigner.ram;
    check(panel[0x95]==20 && panel[0x5e]==44 && panel[0x72]==44,
          "physical cutoff passes original raw ADC conditioning and accepted-history store");
    const std::array<std::uint8_t,7> incoming{0xf0,0x41,0x32,0,5,30,0xf7};
    check(e->receiveOriginalPerformanceMidi(incoming),"incoming cutoff parameter accepted");
    p.cutoff=30.f/127;
    e->setParameters(p,ParameterSource::MidiReflection);
    check(e->originalPerformance().pending()==incoming.size(),
          "incoming tone reflection adds no synthetic receive frame");
    render(*e,4096,block);
    const auto& remote=e->originalPerformance().state().assigner.ram;
    check(remote[0x95]==30 && remote[0x5e]==44 && remote[0x72]==44,
          "incoming tone changes stored cutoff while leaving the physical pot stationary");
    const auto& module=e->firmwareSerialState().control.ram;
    check(unsigned(module[0x3d])+256u*module[0x3e]==30u*128u,
          "received cutoff reaches the real module parameter interpreter");
    e->setParameters(p,ParameterSource::MidiReflection);
    render(*e,4096,block);
    const auto& stationary=e->originalPerformance().state().assigner.ram;
    check(stationary[0x95]==30 && stationary[0x5e]==44 && stationary[0x72]==44,
          "unchanged panel scans and host reflection cannot restore the old physical-pot value");
    p.cutoff=31.f/127;
    e->setParameters(p);
    render(*e,4096,block);
    const auto& moved=e->originalPerformance().state().assigner.ram;
    // A-5 08BE marks a panel-owned value with bit7 after a received tone.
    check((moved[0x95]&0x7fu)==31 && moved[0x5e]==66 && moved[0x72]==66,
          "a later genuine slider gesture retakes cutoff through the original ADC path");
}

void libraryToneRecall(unsigned block) {
    auto beforeAudio=create(48000,1);
    beforeAudio->setParameters(patch(),ParameterSource::ToneRecall);
    check(beforeAudio->originalPerformance().pending()==currentTone.size(),
          "same-value explicit library recall before first audio retains its full frame");
    auto e=create(48000,1);
    render(*e,4096,block);
    auto recalled=patch();
    recalled.cutoff=17.f/127; recalled.resonance=80.f/127;
    recalled.attack=30.f/127; recalled.release=40.f/127;
    e->setParameters(recalled,ParameterSource::ToneRecall);
    check(e->originalPerformance().pending()==currentTone.size(),
          "library recall uses one full manual-tone frame regardless of changed-control count");
    render(*e,4096,block);
    const auto& ram=e->originalPerformance().state().assigner.ram;
    check(ram[0x95]==17 && ram[0x96]==80 && ram[0x9b]==30 && ram[0x9e]==40,
          "library full tone reaches original per-payload receive stores");
    check(ram[0x5e]==204 && ram[0x72]==204,
          "library tone recall leaves the physical cutoff pot and history stationary");
}

void lowRateWireBursts() {
    for(auto rate:{8000.,16000.}) {
        auto e=create(rate,1);
        for(int i=0;i<24;++i) e->noteOn(48+i,1);
        check(e->originalPerformance().pending()==72,
              "low-rate burst preserves all complete note packets");
        // A64-sample piece at8k spans25 DIN bytes and eight message starts;
        // the channel-routing slices must fit their bounded work budget.
        render(*e,512,173);
        check(e->originalPerformance().pending()==0 && gates(*e)!=0,
              "supported low-rate note burst drains without a routing work fault");
        for(int i=0;i<24;++i) e->noteOff(48+i);
        render(*e,512,173);
        check(e->originalPerformance().pending()==0 && gates(*e)==0,
              "supported low-rate release burst clears the actual hardware gates");
    }
}

void replacedPanelSwitchContacts(unsigned block) {
    auto e=create(48000,1);
    render(*e,4096,block);
    auto p=patch();
    // Switch byte one: range bits 0..2, pulse/saw bits 3/4,
    // chorus-off/mode-I bits 5/6. Native buttons are edge contacts,
    // so a replaced request must not turn a held press into another toggle.
    p.range=DcoRange::Four; p.pulseEnabled=false; p.sawEnabled=true;
    p.chorus=ChorusMode::One;
    e->setParameters(p);
    p.range=DcoRange::Sixteen; p.pulseEnabled=true; p.chorus=ChorusMode::Two;
    e->setParameters(p);
    check(e->originalPerformance().pending()==0,
          "replaced native switch presses add no incoming DIN bytes");
    render(*e,4096,block);
    check((e->originalPerformance().state().assigner.ram[0x8e]&0x7fu)==0x19,
          "replacement before the first contact scan reaches the newest switch target");

    p.range=DcoRange::Four; p.pulseEnabled=p.sawEnabled=false;
    p.chorus=ChorusMode::Off;
    e->setParameters(p);
    unsigned waited=0;
    while(e->originalPerformance().state().assigner.ram[0xa6]==0 && waited<4096) {
        render(*e,1,1); ++waited;
    }
    check(waited<4096,"native switch press reaches the actual contact history");
    p.range=DcoRange::Eight; p.pulseEnabled=true; p.chorus=ChorusMode::One;
    e->setParameters(p);
    render(*e,4096,block);
    check((e->originalPerformance().state().assigner.ram[0x8e]&0x7fu)==0x4a,
          "replacement after the contact read releases and re-presses for the newest target");

    auto remote=currentTone;
    remote[3]=11; remote[21]=0x51; // 16', saw, chorus I.
    check(e->receiveOriginalPerformanceMidi(remote),"mixed incoming switch tone accepted");
    p=patch(); p.range=DcoRange::Sixteen; p.pulseEnabled=false;
    p.sawEnabled=true; p.chorus=ChorusMode::One;
    e->setParameters(p,ParameterSource::MidiReflection);
    p.range=DcoRange::Four; p.sawEnabled=false; p.chorus=ChorusMode::Two;
    e->setParameters(p);
    check(e->originalPerformance().pending()==remote.size(),
          "panel presses mixed with a tone keep only the original received bytes");
    render(*e,122,block);
    p.range=DcoRange::Eight; p.sawEnabled=true; p.chorus=ChorusMode::Off;
    e->setParameters(p);
    render(*e,4096,block);
    check(e->originalPerformance().pending()==0,"mixed incoming switch tone drains");
    // After the received switch payload has arrived, a fresh physical gesture
    // wins. Replace it again before its scan to exercise retained ROM state,
    // rather than using the preceding host reflection as the toggle baseline.
    p.range=DcoRange::Four; p.pulseEnabled=true; p.chorus=ChorusMode::Two;
    e->setParameters(p);
    p.range=DcoRange::Sixteen; p.sawEnabled=false; p.chorus=ChorusMode::One;
    e->setParameters(p);
    render(*e,4096,block);
    check((e->originalPerformance().state().assigner.ram[0x8e]&0x7fu)==0x49,
          "newest physical switch target survives rapid replacements around received tone data");
    render(*e,4096,block);
    check((e->originalPerformance().state().assigner.ram[0x8e]&0x7fu)==0x49,
          "settled switch contacts produce no later accidental toggle");
}

auto rapidContactAudio(unsigned block,bool afterRead) {
    auto e=create(48000,1);
    render(*e,4096,block);
    e->noteOn(60,1); render(*e,2048,block);
    auto p=patch();
    p.range=DcoRange::Four; p.pulseEnabled=false; p.sawEnabled=true;
    p.chorus=ChorusMode::One;
    e->setParameters(p);
    if(afterRead) {
        unsigned waited=0;
        while(e->originalPerformance().state().assigner.ram[0xa6]==0 && waited<4096) {
            render(*e,1,1); ++waited;
        }
        check(waited<4096,"rapid audio fixture reaches the real native contact read");
    }
    p.range=DcoRange::Sixteen; p.pulseEnabled=true; p.sawEnabled=false;
    p.chorus=ChorusMode::Two;
    e->setParameters(p);
    std::vector<float> audio;
    render(*e,8192,block,&audio);
    e->noteOff(60); render(*e,1024,block,&audio);
    const auto& a=e->originalPerformance().state().assigner;
    const auto& b=e->firmwareSerialState();
    return std::tuple{audio,a.ram,a.now,a.foregroundPasses,a.registers.pc,
        b.control.ram,b.now,b.ordinal,b.registers.pc};
}

void omniReceivedToneAndGeneratedNotes(unsigned block) {
    auto e=create(48000,1);
    render(*e,4096,block);
    for(unsigned channel=0;channel<16;++channel) {
        auto full=currentTone;
        full[3]=static_cast<std::uint8_t>(channel);
        full[10]=static_cast<std::uint8_t>(40+channel);
        check(e->receiveOriginalPerformanceMidi(full),"omni full tone accepted");
        auto p=patch(); p.cutoff=float(full[10])/127;
        e->setParameters(p,ParameterSource::MidiReflection);
        e->noteOn(60,1);
        check(e->originalPerformance().pending()==full.size()+3,
              "channel sideband preserves a full tone plus generated note byte count");
        render(*e,400,block);
        check(gates(*e)==0,"generated channel-zero On cannot overtake a received channel tone");
        render(*e,2048,block);
        check(e->originalPerformance().state().assigner.ram[0x95]==full[10],
              "every received full-tone channel reaches the original cutoff store");
        check(gates(*e)==1,"generated channel-zero On works after every received tone channel");
        check(e->originalPerformance().state().assigner.ram[0xbd]==0,
              "generated note restores the original receive channel at its message boundary");

        const std::array<std::uint8_t,7> parameter{
            0xf0,0x41,0x32,static_cast<std::uint8_t>(channel),5,
            static_cast<std::uint8_t>(80+channel),0xf7
        };
        check(e->receiveOriginalPerformanceMidi(parameter),"omni parameter frame accepted");
        p.cutoff=float(parameter[5])/127;
        e->setParameters(p,ParameterSource::MidiReflection);
        e->noteOff(60);
        check(e->originalPerformance().pending()==parameter.size()+3,
              "channel sideband preserves a parameter frame plus generated Off byte count");
        render(*e,2048,block);
        check(e->originalPerformance().state().assigner.ram[0x95]==parameter[5],
              "every received parameter channel reaches the original cutoff store");
        check(gates(*e)==0,"generated channel-zero Off works after every received parameter channel");
        check(e->originalPerformance().state().assigner.ram[0xbd]==0
              && e->originalPerformance().pending()==0,
              "interleaved received and generated messages drain with channel zero restored");
    }
}
void startupControlsAndLifecycle() {
    auto p=patch();
    p.masterTuneCents=12.5f; p.portamento=128.f/255;
    p.benderLfoDepth=128.f/255; p.benderVcfDepth=64.f/255; p.benderDcoDepth=192.f/255;
    auto e=create(48000,1,p);
    const auto& initial=e->firmwareSerialState().control.ram;
    check(initial[0x88]==160 && initial[0x89]==128 && initial[0x8a]==255
          && initial[0x8b]==128 && initial[0x8c]==64 && initial[0x8d]==192
          && initial[0x8e]==255 && initial[0x8f]==0,
          "pre-prepare controls seed the actual B2 raw-to-processed ADC coordinates");
    render(*e,2048,173);
    check(e->firmwareSerialState().control.ram[0x61]==32,
          "nonzero startup Tune reaches original B2 signed tuning cache");
    e->setPitchBend(.75f); e->setModWheel(.8f); render(*e,4096,173);
    const auto& controlled=e->firmwareSerialState().control.ram;
    check(controlled[0x62]!=0 && (controlled[0x65]||controlled[0x66])
          && (controlled[0x67]||controlled[0x68]||controlled[0x69]) && controlled[0x64]!=0,
          "startup nonzero panel depths produce actual original VCF, DCO and LFO modulation");
    // The plugin enables Original after prepare and parameter adoption.
    auto late=std::make_unique<E>(); ProductFidelityProfile::configureBeforePrepare(*late);
    late->prepare(48000,256,1); late->setParameters(p); late->setOriginalPerformanceMode(true);
    check(late->firmwareSerialState().control.ram[0x88]==160
          && late->firmwareSerialState().control.ram[0x8d]==192,
          "post-prepare enable installs the same physical controller panel");
    p=patch(); p.chorus=ChorusMode::OneTwo;
    auto both=create(48000,1,p);
    check((YouKnowTestAccess::ic40(*both)&3)==2 && YouKnowTestAccess::chorus(*both)==ChorusMode::Two,
          "Original startup consistently collapses the product chorus extension to II");
    render(*both,2048,173);
    check((YouKnowTestAccess::ic40(*both)&3)==2 && YouKnowTestAccess::chorus(*both)==ChorusMode::Two,
          "later physical IC40 stores retain the same II startup selection");
    e=create(48000,1); e->noteOn(60,1); render(*e,2048,173);
    p=patch(); p.keyTranspose=7; e->setParameters(p); e->noteOff(60); render(*e,2048,173);
    check(gates(*e)==0,"transpose edit cannot redirect Off away from its held wire note");
    e->noteOn(60,1); render(*e,2048,173);
    p.keyTranspose=-7; e->setParameters(p); e->noteOn(60,1); render(*e,2048,173);
    check(std::popcount(gates(*e))==1,"repress after a transpose edit releases the previous wire pitch");
    e->noteOff(60); render(*e,2048,173);
    check(gates(*e)==0,"new transposed press also releases without a stuck gate");
    YouKnowTestAccess::warm(*e,47.5,.9375f);
    const auto thermal=YouKnowTestAccess::thermal(*e);
    e->allNotesOff();
    check(YouKnowTestAccess::thermal(*e)==thermal,"Original panic preserves the powered chassis temperature");
    check(e->originalPerformance().state().assigner.now==0 && e->firmwareSerialAudioStates()==0,
          "temperature-preserving panic still resets all firmware clocks");
}
void holdListeningFixture() {
    // Exact frozen8-second48k/1x pedal A/B score, including250ms preroll.
    // Its old1-sample down/up pairs set both A-5 pending flags before service;
    // the original OFF-before-ON foreground order left the stack held forever.
    EngineParameters p; ActiveProductFidelityProfile::applyTo(p);
    p.vcfTanhMode=VcfTanhMode::PolyZoned; p.vcfFastEarlyMode=VcfFastEarlyMode::Cubic;
    p.vcfSolverMode=VcfSolverMode::Rk4Single; p.calibration=1; p.aging=0; p.polyphony=6;
    p.sawEnabled=true; p.pulseEnabled=false; p.range=DcoRange::Eight;
    p.subLevel=p.noiseLevel=p.pwmDepth=0; p.pwmSource=PwmSource::Manual;
    p.cutoff=.62f; p.resonance=0; p.envDepth=.25f; p.keyFollow=0;
    p.dcoLfoDepth=p.vcfLfoDepth=0; p.attack=0; p.decay=.25f; p.sustain=.6f; p.release=.1f;
    p.vcaLevel=.7f; p.volume=.55f; p.chorus=ChorusMode::Off;
    p.highPass=HighPassMode::One; p.velocityDepth=0; p.keyMode=KeyMode::Unison;
    auto e=std::make_unique<E>(); ActiveProductFidelityProfile::configureBeforePrepare(*e);
    check(e->configureThermalStart(true),"listening fixture selects settled thermal state");
    e->selectConverterTimingProfile(E::ConverterTimingProfile::MeasuredChartGeometry);
    e->prepare(48000,128,1); e->setParameters(p); e->setOriginalPerformanceMode(true);
    render(*e,12000,128);
    unsigned at=0;
    const auto advance=[&](unsigned frame){render(*e,frame-at,128);at=frame;};
    for(auto [start,note]:{std::pair{4800u,48},std::pair{105600u,50},std::pair{206400u,43}}) {
        advance(start); e->noteOn(note,1);
        advance(start+9600); e->setSustainPedal(true);
        advance(start+9601); e->setSustainPedal(false);
        advance(start+28800); e->noteOff(note);
    }
    advance(336000);
    std::vector<float> tail; render(*e,48000,128,&tail);
    double energy=0; for(float sample:tail) energy+=double(sample)*sample;
    check((e->firmwareSerialState().control.ram[0x1e]&1)==0
          && e->firmwareSerialState().control.ram[0x10]==0
          && e->firmwareSerialState().control.ram[0x11]==0
          && e->getActiveVoiceCount()==0,
          "exact listening score ends with HOLD, gates, run mask and active voices all cleared");
    check(std::sqrt(energy/tail.size())<1e-5,
          "exact listening score's final second reaches the circuit noise floor");
}
std::vector<float> coalescedHoldBursts(unsigned block) {
    auto p=patch(); p.keyMode=KeyMode::Unison;
    auto e=create(48000,1,p); e->noteOn(60,1); render(*e,2048,block);
    std::vector<float> audio;
    for(bool last:{false,true}) {
        // More than one foreground pass of opposite receive requests, with
        // repeated values too: the final received value must own HOLD.
        for(unsigned i=0;i<32;++i) {
            const std::array<std::uint8_t,3> message{0xb0,64,
                static_cast<std::uint8_t>((i==31?last:((i/2)&1))?127:0)};
            check(e->receiveOriginalPerformanceMidi(message),"coalesced HOLD burst accepted");
        }
        render(*e,4096,block,&audio);
        check(bool(e->firmwareSerialState().control.ram[0x1e]&1)==last,
              "last received HOLD command wins across both burst directions");
    }
    e->setSustainPedal(false); e->noteOff(60); render(*e,4096,block,&audio);
    check(e->firmwareSerialState().control.ram[0x11]==0,
          "burst leaves no stuck Unison run bits after release");
    return audio;
}
void mixedRawAndPedalRequests(unsigned block) {
    constexpr std::array<std::uint8_t,3> down{0xbf,64,1}, up{0xb3,64,0};
    auto e=create(48000,1);
    check(e->receiveOriginalPerformanceMidi(down),"raw nonzero CC64 is accepted on any routed channel");
    render(*e,2048,block);
    check((e->firmwareSerialState().control.ram[0x1e]&1)!=0,"raw CC64 reaches the B-2 HOLD bit");
    e->setSustainPedal(false); render(*e,2048,block);
    check((e->firmwareSerialState().control.ram[0x1e]&1)==0,
          "pedal setter can release a settled raw CC64 request");
    e->setSustainPedal(true); render(*e,1,block);
    check(e->receiveOriginalPerformanceMidi(up),"raw CC64 off follows a pending pedal setter");
    render(*e,2048,block);
    e->setSustainPedal(true); render(*e,2048,block);
    check((e->firmwareSerialState().control.ram[0x1e]&1)!=0,
          "raw CC64 off updates the request used by the next pedal setter");
    e->setSustainPedal(false); render(*e,2048,block);
    check(e->receiveOriginalPerformanceMidi(down),"queued raw CC64 down is accepted");
    render(*e,1,block);
    e->setSustainPedal(false); render(*e,2048,block);
    check((e->firmwareSerialState().control.ram[0x1e]&1)==0,
          "pedal setter can reverse a raw CC64 request before it reaches the firmware");
}
void directVelocityZero() {
    for(auto mode:{KeyMode::Poly1,KeyMode::Poly2,KeyMode::Unison}) {
        auto p=patch(); p.keyMode=mode;
        auto e=create(48000,1,p);
        e->setOriginalPerformanceMode(false);
        e->noteOn(60,0); render(*e,2048,173);
        check(gates(*e)==0 && YouKnowTestAccess::held(*e,60)==0,
              "unmatched Direct zero-velocity On cannot create a sounding assignment");
        e->noteOn(60,1); e->noteOn(60,1); render(*e,2048,173);
        e->noteOn(60,0);
        check(gates(*e)!=0 && YouKnowTestAccess::held(*e,60)==1,
              "Direct zero-velocity On releases one counted host press");
        e->noteOn(60,0); render(*e,2048,173);
        check(gates(*e)==0 && YouKnowTestAccess::held(*e,60)==0,
              "last Direct zero-velocity On releases the Poly or Unison gate");
    }
}
void replacedLocalFallback(unsigned block) {
    // Source96 at +12 uses the existing MIDI fallback outside the61-key
    // local matrix. A new press owns its new pitch, but must retire the old
    // wire pitch before that source's retained owner is overwritten.
    for(int nextTranspose:{0,7}) {
        auto p=patch(); p.keyTranspose=12;
        auto e=create(48000,1,p);
        e->noteOnFromLocalKeyboard(96,1); render(*e,2048,block);
        check(std::popcount(gates(*e))==1,"local out-of-range press reaches the DIN fallback");
        p.keyTranspose=nextTranspose; e->setParameters(p);
        e->noteOnFromLocalKeyboard(96,1); render(*e,2048,block);
        check(std::popcount(gates(*e))==1,
              "replaced local fallback retires its old wire pitch for native and fallback destinations");
        e->noteOffFromLocalKeyboard(96); render(*e,2048,block);
        check(gates(*e)==0,"replaced local source releases without an orphaned wire note");
    }
    auto p=patch(); p.keyTranspose=12;
    auto e=create(48000,1,p);
    e->noteOnFromLocalKeyboard(96,1); render(*e,2048,block);
    e->noteOnFromLocalKeyboard(96,1);
    check(e->originalPerformance().pending()==3,
          "same-pitch local fallback repeat keeps one On without an invented Off/On retrigger");
    render(*e,2048,block);
    e->noteOffFromLocalKeyboard(96); render(*e,2048,block);
    check(gates(*e)==0,"same-pitch fallback repeat still has one local owner");
}
std::vector<float> rapidSustainReversal(unsigned block) {
    auto p=patch(); p.keyMode=KeyMode::Unison;
    auto e=create(48000,1,p);
    e->noteOn(60,1); render(*e,2048,block);
    std::vector<float> audio;
    e->setSustainPedal(true);
    render(*e,1,block,&audio); // Less than one 320 us DIN byte: B-2 still has HOLD off.
    const auto pending=e->originalPerformance().pending();
    e->setSustainPedal(true);
    check(e->originalPerformance().pending()==pending,
          "repeated requested HOLD state does not duplicate queued input while firmware lags");
    e->setSustainPedal(false);
    render(*e,2048,block,&audio);
    check((e->firmwareSerialState().control.ram[0x1e]&1)==0,
          "rapid pedal release survives the delayed B-2 HOLD mirror");
    e->noteOff(60); render(*e,2048,block,&audio);
    check(e->firmwareSerialState().control.ram[0x11]==0,
          "rapid pedal reversal leaves no stuck six-voice sustained stack");

    e->setSustainPedal(true); render(*e,2048,block,&audio);
    e->setSustainPedal(false); render(*e,1,block,&audio);
    e->setSustainPedal(true); render(*e,2048,block,&audio);
    check((e->firmwareSerialState().control.ram[0x1e]&1)!=0,
          "rapid pedal repress survives the delayed B-2 HOLD mirror");
    e->allNotesOff();
    e->setSustainPedal(true);
    check(e->originalPerformance().pending()==3,
          "panic resets the requested HOLD state as well as the actual firmware state");
    render(*e,2048,block);
    check((e->firmwareSerialState().control.ram[0x1e]&1)!=0,
          "pedal down after panic reaches the original voice board");
    return audio;
}
void rawLeverTargetsSurviveTransitions() {
    for(bool stop:{false,true}) {
        auto e=create(48000,1);
        constexpr unsigned bend=14337;
        const std::array<std::uint8_t,3> pitch{0xef,bend&127u,bend>>7u};
        const std::array<std::uint8_t,3> mod{0xb7,1,102};
        check(e->receiveOriginalPerformanceMidi(pitch)
              && e->receiveOriginalPerformanceMidi(mod),"raw lever requests are accepted");
        if(stop) e->allSoundOff();
        else { e->setOriginalPerformanceMode(false); e->setOriginalPerformanceMode(true); }
        check(e->originalPerformance().pending()==6,
              "controller retention requeues both latest raw lever positions");
        const auto queued=e->originalPerformance().pending();
        e->setPitchBend(float(bend-8192u)/8192.f); e->setModWheel(102.f/127);
        check(e->originalPerformance().pending()==queued,
              "raw requests retain exact controller quantization for setter deduplication");
        render(*e,2048,173);
        check(e->originalPerformance().state().assigner.ram[0xa1]!=0
              && (e->originalPerformance().state().assigner.ram[0xa2]&127u)==102,
              "retained raw lever requests reach original firmware after transition");
        e->setPitchBend(0); e->setModWheel(0); render(*e,2048,173);
        check((e->originalPerformance().state().assigner.ram[0xa1]&127u)==0
              && (e->originalPerformance().state().assigner.ram[0xa2]&127u)==0,
              "setter zero is not suppressed after a raw MIDI lever request");
    }
}
// Host LFO sync replaces B-2's free-running triangle and delay clock in this
// replay, so the panel's own LFO rate and delay become inaudible while synced.
void lfoSync() {
    using Access=YouKnowTestAccess;
    auto synced=[](float rate,float delay) {
        auto p=patch(); p.lfoRate=rate; p.lfoDelay=delay; p.dcoLfoDepth=p.vcfLfoDepth=p.pwmDepth=60.f/127;
        p.lfoSyncRateHz=2.f; p.lfoSyncDelaySeconds=0.f;
        auto e=create(48000,1,p); e->noteOn(60,1);
        std::vector<float> audio; render(*e,3000,83,&audio); // 1/16 s: phase 0.125
        return std::pair{std::move(e),std::move(audio)};
    };
    auto [slow,slowAudio]=synced(10.f/127,0);
    auto [fast,fastAudio]=synced(110.f/127,90.f/127);
    check(slowAudio==fastAudio,"synced replay ignores B-2's own LFO rate and delay");
    check(std::abs(Access::lfo(*slow)-.5f)<2e-3f,"replay reports the synced half-scale triangle at phase 0.125");
    check(Access::cpuWord(*slow,0x4b)==0&&Access::delayLevel(*slow)==1.f,"sync holds B-2's rate and completes its delay");

    auto p=patch(); p.dcoLfoDepth=60.f/127; p.lfoDelay=0;
    auto e=create(48000,1,p); render(*e,512,83);
    const unsigned ownRate=Access::cpuWord(*e,0x4b);
    p.lfoSyncRateHz=2.f; p.lfoSyncDelaySeconds=.05f; e->setParameters(p); render(*e,512,83);
    check(ownRate!=0&&Access::cpuWord(*e,0x4b)==0,"sync takes over B-2's free-running rate");
    // B-2 re-arms the delay on the first voice-on after a pass with no voice
    // running; in this replay that restarts the synced delay, not the host.
    e->noteOn(60,1); render(*e,9600,83);
    check(Access::delayLevel(*e)==1.f,"synced delay completes while the phrase holds");
    e->noteOff(60); render(*e,2400,83);
    e->noteOn(62,1); render(*e,1440,83);
    check(Access::delayProgress(*e)<1.0&&Access::delayLevel(*e)==0.f,"B-2's phrase re-arm restarts the synced delay");
    p.lfoSyncRateHz=0; p.lfoSyncDelaySeconds=-1; e->setParameters(p); render(*e,512,83);
    const float before=Access::lfo(*e); render(*e,2400,83);
    check(Access::cpuWord(*e,0x4b)==ownRate&&Access::lfo(*e)!=before,"B-2's own LFO runs again after sync ends");
}
// Host automation can send bend and CC1 far faster than DIN carries them.
// Before pending values merged, a sweep at one message per sample delayed
// notes by over a second, then overflowed the 4,096-byte input and cut a
// held chord. The wire must stay short and the chord keep sounding.
void denseControllerSweep(unsigned block) {
    auto e=create(48000,1);
    e->noteOn(60,1); e->noteOn(64,1); e->noteOn(67,1);
    render(*e,4800,block);
    std::array<float,256> l{},r{};
    float tailPeak=0;
    for(unsigned i=0;i<24000;i+=block) {
        const float phase=float(i)/24000.f;
        e->setPitchBend(std::sin(6.2831853f*4*phase));
        e->setModWheel(phase);
        // At most the message on the wire plus one waiting bend and one CC1.
        check(e->originalPerformance().pending()<=9,"pending bend/CC1 merge into one message each");
        e->process(l.data(),r.data(),int(block));
        if(i>=19200) for(unsigned k=0;k<block;++k)
            tailPeak=std::max({tailPeak,std::abs(l[k]),std::abs(r[k])});
    }
    check(e->originalPerformanceHealthy(),"a dense bend/CC1 sweep keeps the serial pipeline healthy");
    check(tailPeak>0.01f,"the held chord still sounds at the end of a dense bend/CC1 sweep");
    e->setPitchBend(0); e->setModWheel(0); render(*e,4096,block);
    check(e->originalPerformance().state().assigner.ram[0xa2]==0,"the final CC1 value reaches A-5 after merging");
}
// The plug-in's panel is the only one a user sees, so a recall also moves
// its switches. Before it did, A-5 merged each later column-5 edit over the
// detents the panel held before the recall: moving a switch back to where
// the recall had left it went unseen, and moving another revealed a hidden
// VCA position, ringing a Gate patch on with its envelope release.
void recalledSwitchRow(unsigned block) {
    auto e=create(48000,1);
    render(*e,4096,block);
    auto p=patch();
    p.envPolarity=EnvPolarity::Inverted;
    e->setParameters(p,ParameterSource::ToneRecall); render(*e,4096,block);
    p.envPolarity=EnvPolarity::Normal;
    e->setParameters(p); render(*e,4096,block);
    const auto& ram=e->originalPerformance().state().assigner.ram;
    check((ram[0x8f]&2u)==0,"a switch moved back to its pre-recall detent reaches A-5");

    p.vcaMode=VcaMode::Gate;
    e->setParameters(p); render(*e,4096,block);
    p.vcaMode=VcaMode::Envelope;
    e->setParameters(p,ParameterSource::ToneRecall); render(*e,4096,block);
    p.pwmSource=PwmSource::Manual;
    e->setParameters(p); render(*e,4096,block);
    check((ram[0x8f]&5u)==1,"moving one switch after a recall changes only that switch");
}
// A host bend at one message per sample keeps DIN busy, and A-5 07C5..07CE
// yields its route change to every received byte, so before merged frames
// left that window each queued panel edit waited until the stream stopped.
void densePanelEdits(unsigned block) {
    auto e=create(48000,1);
    e->noteOn(60,1);
    render(*e,4800,block);
    auto p=patch();
    std::array<float,256> l{},r{};
    for(unsigned i=0;i<14400;i+=block) {
        e->setPitchBend(std::sin(6.2831853f*float(i)/4800.f));
        if(i==2400) { p.cutoff=17.f/127; e->setParameters(p); }
        if(i==4800) { p.cutoff=60.f/127; e->setParameters(p); }
        e->process(l.data(),r.data(),int(block));
    }
    check(e->originalPerformanceHealthy(),"a dense bend with panel edits keeps the serial pipeline healthy");
    check((e->originalPerformance().state().assigner.ram[0x95]&127u)==60,
          "panel edits reach A-5 while a dense bend stream plays");
}
int main() {
    holdListeningFixture();
    check(coalescedHoldBursts(1)==coalescedHoldBursts(173),
          "coalesced HOLD correction preserves complete stereo audio across blocks");
    rawLeverTargetsSurviveTransitions();
    directVelocityZero();
    replacedLocalFallback(1); replacedLocalFallback(173);
    mixedRawAndPedalRequests(1); mixedRawAndPedalRequests(173);
    check(rapidSustainReversal(1)==rapidSustainReversal(173),
          "rapid pedal reversals preserve full stereo audio across host blocks");
    lfoSync();
    originalUnisonVoiceCounts();
    check(originalUnisonCountTransitions(1)==originalUnisonCountTransitions(173),
          "audible Unison count transitions preserve stereo audio and firmware across host blocks");
    originalUnisonHiddenTails();
    allocatorAndBitmap();
    check(repeatedMode(1)==repeatedMode(173),"same-mode contact processing is block invariant");
    burstAndRecovery();
    startupControlsAndLifecycle();
    denseControllerSweep(1); denseControllerSweep(8);
    densePanelEdits(1); densePanelEdits(8);
    lowRateWireBursts();
    for(bool afterRead:{false,true})
        check(rapidContactAudio(1,afterRead)==rapidContactAudio(173,afterRead),
              "rapid native switch replacement preserves full stereo audio and firmware state across host blocks");
    for(unsigned block:{1u,173u}) {
        rawToneReceiptAndOrdering(block);
        rawToneIncrementalFrontiers(block);
        physicalPanelAndIncomingTone(block);
        libraryToneRecall(block);
        recalledSwitchRow(block);
        replacedPanelSwitchContacts(block);
        omniReceivedToneAndGeneratedNotes(block);
    }
    for(auto rate:{44100.,48000.,96000.}) for(int factor:{1,4}) {
        check(score(rate,factor,1)==score(rate,factor,173),"absolute firmware and stereo output invariant to host blocks");
    }
    auto e=create(48000,1);
    e->noteOn(60,1); render(*e,1024,83);
    auto p=patch(); p.keyMode=KeyMode::Unison; e->setParameters(p); render(*e,4096,83);
    check((e->originalPerformance().state().assigner.ram[0xc8]&6)==6,"physical POLY contacts select original unison");
    e->reassertKeyMode(); render(*e,4096,83);
    p.keyMode=KeyMode::Poly1; e->setParameters(p); render(*e,4096,83);
    check((e->originalPerformance().state().assigner.ram[0xc8]&6)==2,"physical POLY contacts select original Poly1");
    e->setPitchBend(.75f); e->setModWheel(.8f); render(*e,2048,83);
    check(e->originalPerformance().state().assigner.ram[0xa1]!=0,"original pitch-bend receive path");
    check(e->originalPerformance().state().assigner.ram[0xa2]!=0,"original CC1 receive path");
    (void)e->setOversamplingFactor(4); render(*e,2048,83);
    check(e->originalPerformanceHealthy(),"quality change retains continuous sender");
    e->reset(); check(e->originalPerformance().pending()==0,"reset clears pending input");
    e->setOriginalPerformanceMode(false); e->noteOn(60,1); render(*e,1024,83);
    check(e->getActiveVoiceCount()>0,"direct mode returns with fresh gates");
    std::puts("Original performance contracts passed");
}
