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
int main() {
    allocatorAndBitmap();
    check(repeatedMode(1)==repeatedMode(173),"same-mode contact processing is block invariant");
    burstAndRecovery();
    startupControlsAndLifecycle();
    lowRateWireBursts();
    for(bool afterRead:{false,true})
        check(rapidContactAudio(1,afterRead)==rapidContactAudio(173,afterRead),
              "rapid native switch replacement preserves full stereo audio and firmware state across host blocks");
    for(unsigned block:{1u,173u}) {
        rawToneReceiptAndOrdering(block);
        rawToneIncrementalFrontiers(block);
        physicalPanelAndIncomingTone(block);
        libraryToneRecall(block);
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
