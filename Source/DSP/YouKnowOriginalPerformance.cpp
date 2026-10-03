#include "YouKnowOriginalPerformance.h"
#include "YouKnowEngine.h"
#include "YouKnowSysEx.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace youknow {
namespace {
std::array<std::uint8_t, 18> tone(const EngineParameters& p) noexcept {
    sysex::Patch patch;
    patch.lfoRate=p.lfoRate; patch.lfoDelay=p.lfoDelay; patch.dcoLfo=p.dcoLfoDepth;
    patch.pwm=p.pwmDepth; patch.noise=p.noiseLevel; patch.cutoff=p.cutoff;
    patch.resonance=p.resonance; patch.vcfEnv=p.envDepth; patch.vcfLfo=p.vcfLfoDepth;
    patch.keyFollow=p.keyFollow; patch.vcaLevel=p.vcaLevel; patch.attack=p.attack;
    patch.decay=p.decay; patch.sustain=p.sustain; patch.release=p.release;
    patch.sub=p.subLevel; patch.range=p.range; patch.saw=p.sawEnabled;
    patch.pulse=p.pulseEnabled; patch.pwmSource=p.pwmSource; patch.vcaMode=p.vcaMode;
    patch.envPolarity=p.envPolarity; patch.highPass=p.highPass; patch.chorus=p.chorus;
    std::array<std::uint8_t,18> bytes{};
    sysex::toneBytesFromPatch(patch, bytes.data());
    return bytes;
}
// Inverse of original A-5 08C3..08D5, including its upper ADC stretch.
// Choose the lowest code yielding each stored value. No voltage calibration
// is inferred: this seeds a stationary warm panel, not an analog pot model.
std::uint8_t rawFor(unsigned value) noexcept {
    for (unsigned raw=0; raw<256; ++raw) {
        const unsigned processed=raw<=4?0:raw<=246?raw-4:std::min(255u,2*raw-250);
        if ((processed>>1)>=value) return static_cast<std::uint8_t>(raw);
    }
    return 255;
}
unsigned contact(unsigned mode) noexcept { return mode==0?2:mode==1?4:6; }
}
void OriginalPerformance::reset(const EngineParameters& p) noexcept {
    FirmwareAssignerScheduler::State warm;
    FirmwareUartTrace::State uart;
    inputs_={}; tone_=tone(p); head_=count_=0; nextRx_=0;
    auto& r=warm.ram;
    for(unsigned i=0;i<16;++i) {
        const auto address=FirmwareAssignerIo::panelParameterAddress[i];
        const auto raw=rawFor(tone_[address-0x90]);
        inputs_.panelAdc[i]=raw; r[0x58+i]=r[0x6c+i]=raw;
        r[address]=tone_[address-0x90];
    }
    for(unsigned i=0;i<4;++i) r[0x68+i]=inputs_.directAdc[i];
    warm.io.conversion=inputs_.directAdc;
    for(unsigned i=0;i<6;++i) { r[0x80+i]=0x88+i; r[0x88+i]=0x80; }
    r[0x86]=0x88; r[0x4f]=0x21; r[0xb6]=2;
    r[0xb8]=r[0x8e]=r[0xc7]=tone_[16]; r[0x8f]=tone_[17];
    r[0xba]=0x10; r[0xbb]=1; r[0xbc]=8; r[0xbe]=12;
    r[0xc5]=r[0xc6]=4; r[0xcb]=0xfc;
    // Function III permits both original SysEx and pitch-bend receive. There
    // are no program-button contacts; plug-in program selection sends a tone.
    inputs_.panel[6]=r[0xa8]=0;
    wantedMode_=static_cast<unsigned>(p.keyMode); modeContact_=modeObserved_=false;
    r[0xc8]=static_cast<std::uint8_t>(0x40|contact(wantedMode_));
    (void)FirmwareAssignerAudioBridge::reset(state_,warm,uart);
}
bool OriginalPerformance::message(std::span<const std::uint8_t> bytes, std::uint64_t now) noexcept {
    if(bytes.empty()) return true;
    if(bytes.size()>inputCapacity-count_) return false;
    if(now>std::numeric_limits<std::uint64_t>::max()-1280u) return false;
    auto t=std::max(nextRx_,now+1280u);
    if(bytes.size()>(std::numeric_limits<std::uint64_t>::max()-t)/1280u) return false;
    if(head_+count_+bytes.size()>inputCapacity) {
        std::copy_n(queue_.begin()+head_,count_,queue_.begin()); head_=0;
    }
    for(auto byte:bytes) { queue_[head_+count_++]={t,byte}; t+=1280; }
    nextRx_=t;
    return true;
}
bool OriginalPerformance::parameters(const EngineParameters& p,std::uint64_t now) noexcept {
    const auto next=tone(p);
    unsigned changed=0;
    for(unsigned i=0;i<18;++i) changed+=next[i]!=tone_[i];
    if(changed*sysex::parameterMessageBytes>inputCapacity-count_) return false;
    for(unsigned i=0;i<18;++i) if(next[i]!=tone_[i]) {
        std::array<std::uint8_t,sysex::parameterMessageBytes> bytes{};
        (void)sysex::writeParameterMessage(i,next[i],0,bytes.data(),bytes.size());
        (void)message(bytes,now);
    }
    tone_=next;
    if(wantedMode_!=static_cast<unsigned>(p.keyMode)) keyMode(static_cast<unsigned>(p.keyMode));
    return true;
}
void OriginalPerformance::keyMode(unsigned mode) noexcept {
    wantedMode_=std::min(mode,2u); modeContact_=true; modeObserved_=false;
    inputs_.panel[6]=static_cast<std::uint8_t>(contact(wantedMode_));
}
bool OriginalPerformance::advance(YouKnowEngine& engine,std::uint64_t target) noexcept {
    std::array<FirmwareSerialPinDecoder::ByteReady,64> ready{};
    std::array<FirmwareSerialTrace::ByteReady,64> serial{};
    for(unsigned attempt=0;attempt<8;++attempt) {
        FirmwareAssignerAudioBridge::Output output{ready,0};
        const auto result=FirmwareAssignerAudioBridge::advanceTo(state_,configuration_,tables_,
            inputs_,std::span(queue_).subspan(head_,std::min(count_,FirmwareAssignerAudioBridge::maximumInputEvents)),target,output);
        head_+=result.consumedInputs; count_-=result.consumedInputs;
        if(count_==0) head_=0;
        for(std::size_t i=0;i<output.count;++i) serial[i]={ready[i].states,ready[i].value};
        if(!engine.appendFirmwareSerialBytes(std::span(serial).first(output.count))) return false;
        if(result.status==FirmwareAssignerAudioBridge::Status::ReachedTarget) {
            if(modeContact_) {
                // The LED can already equal a repeated press. Wait for the
                // contact read and completion of that foreground pass before
                // opening it; host block size cannot shorten away the press.
                if(!modeObserved_ && (state_.assigner.ram[0xa8]&6u)==contact(wantedMode_)) {
                    modeObserved_=true; modeObservedPass_=state_.assigner.foregroundPasses;
                }
                if(modeObserved_ && state_.assigner.foregroundPasses>modeObservedPass_
                    && (state_.assigner.ram[0xc8]&6u)==contact(wantedMode_)) {
                    inputs_.panel[6]=0; modeContact_=false;
                }
            }
            return true;
        }
        if(result.status!=FirmwareAssignerAudioBridge::Status::OutputFull
            && result.status!=FirmwareAssignerAudioBridge::Status::WorkLimit) return false;
    }
    return false;
}
}
