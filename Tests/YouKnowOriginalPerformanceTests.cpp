#include "../Source/DSP/YouKnowEngine.h"
#include "../Source/DSP/YouKnowProductFidelity.h"
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
static void check(bool b,const char* message) { if(!b) { std::fprintf(stderr,"FAIL: %s\n",message); std::abort(); } }
using namespace youknow;
using E=YouKnowEngine;
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
    check(e->originalPerformance().state().assigner.ram[0x95]==30,"host tone edit parsed by original A5 SysEx");
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
    e->setParameters(p);
    check(!e->originalPerformanceHealthy() && e->originalPerformance().pending()==0,
          "tone queue overflow resets coherently and exposes failure");
    check(e->originalPerformance().state().assigner.ram[0x95]==17
          && e->firmwareSerialState().control.ram[0x88]==160,
          "overflow recovery seeds the newly adopted tone and physical Tune, not the old image");
    e->reset(); e->noteOn(67,1); render(*e,2048,173);
    check(gates(*e)==1,"newly adopted tone remains playable after failure recovery");
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
int main() {
    allocatorAndBitmap();
    check(repeatedMode(1)==repeatedMode(173),"same-mode contact processing is block invariant");
    burstAndRecovery();
    startupControlsAndLifecycle();
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
