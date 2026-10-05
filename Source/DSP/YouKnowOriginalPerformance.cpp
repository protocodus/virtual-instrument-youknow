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
    clearKeyboardNotes();
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
    // Column 5 holds the three two-position switches and HPF contacts;
    // column 4's DCO/chorus switches are momentary. Seed the physical
    // detents and preceding scan together, so reset manufactures no edit.
    inputs_.panel[5]=r[0xa7]=tone_[17];
    switchContact_=switchRelease_=switchObserved_=switchTargetPending_=false;
    switchTarget_=tone_[16]; switchTargetMask_=0; switchObservedPass_=0;
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
        std::copy_n(queue_.begin()+head_,count_,queue_.begin());
        std::copy_n(queueChannel_.begin()+head_,count_,queueChannel_.begin()); head_=0;
    }
    // The plug-in has always accepted every MIDI channel. Preserve literal
    // bytes, but present each complete host message to that matching A-5
    // receive channel at its first-byte boundary. This is product omni
    // routing, not a claim that the unit changes channel for every message.
    // A-5 069F compares SysEx channel against FFBD; 05B5 does the same for
    // channel voice messages. Routing cannot overtake an earlier queued frame.
    int channel=-1;
    if(bytes.size()>=4 && bytes[0]==0xf0 && bytes[1]==0x41)
        channel=bytes[3]&15;
    else if(bytes[0]>=0x80 && bytes[0]<0xf0) channel=bytes[0]&15;
    bool first=true;
    for(auto byte:bytes) {
        queueChannel_[head_+count_]=static_cast<std::int8_t>(first?channel:-1);
        queue_[head_+count_++]={t,byte}; t+=1280; first=false;
    }
    nextRx_=t;
    return true;
}
bool OriginalPerformance::keyboardNoteOn(int sourceNote, int midiPitch) noexcept {
    if(sourceNote<0 || sourceNote>=128 || midiPitch<0 || midiPitch>=128) return false;
    (void)keyboardNoteOff(sourceNote);
    if(midiPitch<36 || midiPitch>96) {
        keyboardContacts_[static_cast<std::size_t>(sourceNote)]=
            static_cast<std::int16_t>(128+midiPitch);
        return false;
    }
    // A-5 019E..0201 scans FF50..57 separately, then ORs the local key bits
    // with DIN's FF40..4C. Tables 0008..000F and 09CA..09D2 map column*8+bit
    // to MIDI24 plus the warm image's transpose12: the 61 keys are36..96.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic1.txt#L229-L289
    const auto contact=static_cast<unsigned>(midiPitch-36);
    keyboardContacts_[static_cast<std::size_t>(sourceNote)]=static_cast<std::int16_t>(contact);
    inputs_.keyboard[contact>>3u]|=static_cast<std::uint8_t>(1u<<(contact&7u));
    return true;
}
int OriginalPerformance::keyboardNoteOff(int sourceNote) noexcept {
    if(sourceNote<0 || sourceNote>=128) return -1;
    auto& retained=keyboardContacts_[static_cast<std::size_t>(sourceNote)];
    if(retained<0) return -1;
    const auto contact=static_cast<unsigned>(retained);
    retained=-1;
    if(contact>=128) return static_cast<int>(contact-128);
    // A transpose change can map two source keys to one physical contact.
    // The contact remains down until every local owner releases it.
    if(std::find(keyboardContacts_.begin(),keyboardContacts_.end(),
                 static_cast<std::int16_t>(contact))==keyboardContacts_.end())
        inputs_.keyboard[contact>>3u]&=static_cast<std::uint8_t>(~(1u<<(contact&7u)));
    return -2;
}
void OriginalPerformance::clearKeyboardNotes() noexcept {
    keyboardContacts_.fill(-1);
    inputs_.keyboard.fill(0);
}
bool OriginalPerformance::parameters(const EngineParameters& p,std::uint64_t now,
    ParameterInputSource source) noexcept {
    const auto next=tone(p);
    if(source==ParameterInputSource::ToneRecall) {
        // A host/library recall has no physical program-button or patch-RAM
        // source. Send one documented manual tone, including equal values,
        // rather than inventing eighteen incoming control gestures.
        std::array<std::uint8_t,sysex::patchMessageBytes> bytes{};
        bytes[0]=0xf0; bytes[1]=0x41; bytes[2]=0x31;
        std::copy(next.begin(),next.end(),bytes.begin()+5); bytes.back()=0xf7;
        if(!message(bytes,now)) return false;
    } else if(source==ParameterInputSource::Panel) {
        std::uint32_t fields=0;
        for(unsigned i=0;i<16;++i)
            if(next[i]!=tone_[i]) fields|=1u<<i;
        const unsigned first=next[16]^tone_[16], second=next[17]^tone_[17];
        if(first&7u) fields|=1u<<16;
        if(first&0x10u) fields|=1u<<17;
        if(first&8u) fields|=1u<<18;
        if(second&1u) fields|=1u<<19;
        if(second&4u) fields|=1u<<20;
        if(second&2u) fields|=1u<<21;
        if(second&0x18u) fields|=1u<<22;
        if(first&0x60u) fields|=(1u<<23)|(1u<<24);
        panelParameters(p,fields);
    }
    // MIDI reflection updates only the host-facing mirror. The incoming
    // frame owns its bytes and ROM stores; physical knob/contact positions
    // survive patch reception, as they do on the instrument.
    tone_=next;
    // Assign mode is a live performance contact, absent from the eighteen
    // tone bytes. A pending tone reflection must not suppress its UI press.
    if(wantedMode_!=static_cast<unsigned>(p.keyMode)) keyMode(static_cast<unsigned>(p.keyMode));
    return true;
}

void OriginalPerformance::panelParameters(const EngineParameters& p,
    std::uint32_t fields) noexcept {
    // A-5 049D..04B5 compares ADC histories, 087E..089B conditions,
    // stores and sends native pot edits before optional outgoing SysEx.
    // Explicit ownership moves the physical pot even when MIDI has already
    // adopted that same value. Unedited pots and the tone mirror stay put.
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic1.txt#L638-L651
    // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic1.txt#L1233-L1296
    const auto next=tone(p);
    for(unsigned i=0;i<16;++i) {
        const auto address=FirmwareAssignerIo::panelParameterAddress[i];
        const unsigned parameter=address-0x90;
        if(fields&(1u<<parameter)) inputs_.panelAdc[i]=rawFor(next[parameter]);
    }
    unsigned stable=0;
    if(fields&(1u<<19)) stable|=1u;
    if(fields&(1u<<20)) stable|=4u;
    if(fields&(1u<<21)) stable|=2u;
    if(fields&(1u<<22)) stable|=0x18u;
    inputs_.panel[5]=static_cast<std::uint8_t>(
        (inputs_.panel[5]&~stable)|(next[17]&stable));
    unsigned momentary=0;
    if(fields&(1u<<16)) momentary|=7u;
    if(fields&(1u<<17)) momentary|=0x10u;
    if(fields&(1u<<18)) momentary|=8u;
    if(fields&((1u<<23)|(1u<<24))) momentary|=0x60u;
    if(momentary) queueSwitchTarget(next[16],static_cast<std::uint8_t>(momentary));
}

void OriginalPerformance::beginSwitchContact() noexcept {
    if(!switchTargetPending_) return;
    switchTargetPending_=false;
    const auto actual=state_.assigner.ram[0x8e];
    const auto mask=switchTargetMask_; switchTargetMask_=0;
    unsigned contacts=(switchTarget_^actual)&mask&0x18u;
    if((mask&7u) && (switchTarget_&7u)!=(actual&7u)) contacts|=switchTarget_&7u;
    if((mask&0x60u) && (switchTarget_&0x60u)!=(actual&0x60u))
        contacts|=(switchTarget_&0x20u)?0x20u:(switchTarget_&0x40u)?0x40u:0x80u;
    inputs_.panel[4]=static_cast<std::uint8_t>(contacts);
    switchContact_=contacts!=0; switchObserved_=false;
}
void OriginalPerformance::queueSwitchTarget(std::uint8_t next,std::uint8_t mask) noexcept {
    if(!mask) return;
    switchTarget_=static_cast<std::uint8_t>((switchTarget_&~mask)|(next&mask));
    switchTargetMask_|=mask; switchTargetPending_=true;
    if(!switchContact_ && !switchRelease_) beginSwitchContact();
}
void OriginalPerformance::serviceSwitchContact(bool scanned) noexcept {
    if(!switchContact_ && !switchRelease_) return;
    if(scanned && !switchObserved_ && state_.assigner.ram[0xa6]==inputs_.panel[4]) {
        switchObserved_=true; switchObservedPass_=state_.assigner.foregroundPasses;
    }
    if(!switchObserved_ || state_.assigner.foregroundPasses<=switchObservedPass_) return;
    switchObserved_=false;
    if(switchContact_) {
        // Complete the native press and a scanned release before another
        // press. Rapid host snapshots replace the desired detent, never a
        // contact which the real foreground has not yet consumed.
        inputs_.panel[4]=0; switchContact_=false; switchRelease_=true;
    } else {
        switchRelease_=false; beginSwitchContact();
    }
}
void OriginalPerformance::keyMode(unsigned mode) noexcept {
    wantedMode_=std::min(mode,2u); modeContact_=true; modeObserved_=false;
    inputs_.panel[6]=static_cast<std::uint8_t>(contact(wantedMode_));
}
void OriginalPerformance::serviceModeContact(bool scanned) noexcept {
    if(!modeContact_) return;
    // Observe the real FFA8 scan-history write, then complete that foreground
    // pass before releasing the contact. A repeated LED value is not a read.
    if(scanned && !modeObserved_ && (state_.assigner.ram[0xa8]&6u)==contact(wantedMode_)) {
        modeObserved_=true; modeObservedPass_=state_.assigner.foregroundPasses;
    }
    if(modeObserved_ && state_.assigner.foregroundPasses>modeObservedPass_
        && (state_.assigner.ram[0xc8]&6u)==contact(wantedMode_)) {
        inputs_.panel[6]=0; modeContact_=false;
    }
}
bool OriginalPerformance::advance(YouKnowEngine& engine,std::uint64_t target) noexcept {
    auto configuration=configuration_;
    if(modeContact_ || switchContact_ || switchRelease_ || switchTargetPending_) {
        configuration.inputServiceContext=this;
        configuration.inputService=[](void* context,const FirmwareAssignerScheduler::State&,
            const FirmwareAssignerScheduler::Event& event) noexcept {
            auto& owner=*static_cast<OriginalPerformance*>(context);
            const bool scanned=event.kind==FirmwareAssignerScheduler::EventKind::RamWrite;
            owner.serviceSwitchContact(scanned && event.address==0xffa6);
            owner.serviceModeContact(scanned && event.address==0xffa8);
        };
    }
    std::array<FirmwareSerialPinDecoder::ByteReady,64> ready{};
    std::array<FirmwareSerialTrace::ByteReady,64> serial{};
    // Engine advances at most64 host samples, including its supported8kHz
    // floor: <=25 receive bytes. Even one-byte complete messages need two
    // slices per routing boundary;64 leaves bounded bridge-pressure retries.
    for(unsigned attempt=0;attempt<64;++attempt) {
        const bool routePending=count_ && queueChannel_[head_]>=0
            && queue_[head_].states<=target;
        auto sliceTarget=routePending?queue_[head_].states-1u:target;
        std::size_t inputCount=0;
        if(!routePending) {
            inputCount=std::min(count_,FirmwareAssignerAudioBridge::maximumInputEvents);
            for(std::size_t i=1;i<inputCount;++i)
                if(queueChannel_[head_+i]>=0) {
                    inputCount=i;
                    sliceTarget=std::min(sliceTarget,queue_[head_+i].states-1u);
                    break;
                }
        }
        FirmwareAssignerAudioBridge::Output output{ready,0};
        const auto result=FirmwareAssignerAudioBridge::advanceTo(state_,configuration,tables_,
            inputs_,std::span(queue_).subspan(head_,inputCount),sliceTarget,output);
        head_+=result.consumedInputs; count_-=result.consumedInputs;
        if(count_==0) head_=0;
        for(std::size_t i=0;i<output.count;++i) serial[i]={ready[i].states,ready[i].value};
        if(!engine.appendFirmwareSerialBytes(std::span(serial).first(output.count))) return false;
        if(result.status==FirmwareAssignerAudioBridge::Status::ReachedTarget) {
            if(routePending) {
                state_.assigner.ram[0xbd]=static_cast<std::uint8_t>(queueChannel_[head_]);
                queueChannel_[head_]=-1;
                continue;
            }
            if(sliceTarget!=target) continue;
            return true;
        }
        if(result.status!=FirmwareAssignerAudioBridge::Status::OutputFull
            && result.status!=FirmwareAssignerAudioBridge::Status::WorkLimit) return false;
    }
    return false;
}
}
