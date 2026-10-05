#include "../Source/PluginProcessor.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <atomic>
#include <thread>
static void check(bool x,const char* message) { if(!x) { std::fprintf(stderr,"FAIL: %s\n",message); std::abort(); } }
static void choose(YouKnowAudioProcessor& p,float value) {
    auto* parameter=p.parameters.getParameter(youknow::parameters::originalPerformance);
    check(parameter && !parameter->isAutomatable() && parameter->getVersionHint()==11,"session-only appended timing parameter");
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}
static void block(YouKnowAudioProcessor& p,bool note=false) {
    juce::AudioBuffer<float> buffer(2,512); juce::MidiBuffer midi;
    if(note) midi.addEvent(juce::MidiMessage::noteOn(1,60,juce::uint8(100)),73);
    p.processBlock(buffer,midi);
    check(p.getOriginalPerformanceHealthyForTest(),"plugin original pipeline healthy");
    for(int c=0;c<2;++c) for(int i=0;i<512;++i) check(std::isfinite(buffer.getSample(c,i)),"plugin audio finite");
}
static void samples(YouKnowAudioProcessor& p,int count,juce::MidiBuffer* events=nullptr) {
    juce::AudioBuffer<float> buffer(2,count); juce::MidiBuffer empty;
    p.processBlock(buffer,events?*events:empty);
    check(p.getOriginalPerformanceHealthyForTest(),"source-aware plugin pipeline healthy");
}
static void sourceAwareInput() {
    YouKnowAudioProcessor p;
    p.prepareToPlay(48000,512); samples(p,1);
    auto* cutoff=p.parameters.getParameter(youknow::parameters::cutoff);
    const auto before=p.getOriginalPerformanceForTest().state().assigner.ram[0x95];
    const float target=42.f/127;
    cutoff->setValueNotifyingHost(cutoff->convertTo0to1(target)); samples(p,1);
    check(p.getOriginalPerformanceForTest().pending()==0,
          "physical panel edit adds no incoming MIDI bytes");
    check(p.getOriginalPerformanceForTest().state().assigner.ram[0x95]==before,
          "physical panel edit waits for original ADC and foreground");
    for(int i=0;i<12;++i) samples(p,173);
    check(p.getOriginalPerformanceForTest().state().assigner.ram[0x95]==42,
          "physical panel edit reaches native tone RAM");

    std::array<std::uint8_t,youknow::sysex::patchMessageBytes> raw{};
    check(youknow::sysex::writePatchMessage(p.currentPatch(),13,raw.data(),raw.size())==raw.size(),
          "full tone fixture encoded");
    raw[2]=0x30; raw[4]=127; // Preserve a real numbered packet, not a re-encoded manual tone.
    auto message=juce::MidiMessage::createSysExMessage(raw.data()+1,int(raw.size())-2);
    juce::MidiBuffer midi; midi.addEvent(message,0);
    samples(p,1,&midi);
    check(p.getOriginalPerformanceForTest().pending()==24,
          "same-value full tone retains its complete wire traffic");
    for(int i=0;i<6;++i) samples(p,173);
    check(p.getOriginalPerformanceForTest().pending()==0,
          "full tone uses one24-byte frame, with no duplicate parameter traffic");
    check(p.getOriginalPerformanceForTest().state().assigner.ram[0x95]==42,
          "nonzero-channel full tone reaches original RAM");
    p.flushPendingMidiEvents(); samples(p,1);
    check(p.getOriginalPerformanceForTest().pending()==0,
          "host reflection does not transmit the full tone again");

    std::array<std::uint8_t,youknow::sysex::parameterMessageBytes> control{};
    check(youknow::sysex::writeParameterMessage(5,9,7,control.data(),control.size())==control.size(),
          "incoming control fixture encoded");
    midi.addEvent(juce::MidiMessage::createSysExMessage(control.data()+1,int(control.size())-2),0);
    samples(p,1,&midi);
    check(p.getOriginalPerformanceForTest().pending()==7,
          "incoming control preserves one seven-byte packet");
    for(int i=0;i<9;++i) samples(p,173);
    p.flushPendingMidiEvents(); samples(p,173);
    check(p.getOriginalPerformanceForTest().state().assigner.ram[0x95]==9,
          "nonzero-channel incoming control remains after host reflection");
    // FF5E is the cutoff pot's current ADC; FF72 retains its compared history.
    check(p.getOriginalPerformanceForTest().state().assigner.ram[0x5e]==88
          && p.getOriginalPerformanceForTest().state().assigner.ram[0x72]==88,
          "incoming SysEx does not move physical panel ADC history");
}

static void setSlider(YouKnowAudioProcessor& p,const char* id,float value) {
    auto* parameter=p.parameters.getParameter(id);
    check(parameter!=nullptr,"slider fixture parameter exists");
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

static void pausedReflectionAndNewerPanelGesture(bool fullTone,bool cutoffGesture) {
    YouKnowAudioProcessor p;
    setSlider(p,youknow::parameters::cutoff,20.f/127);
    setSlider(p,youknow::parameters::resonance,30.f/127);
    p.prepareToPlay(48000,512);
    for(int i=0;i<12;++i) samples(p,173);
    const auto baseline=p.getOriginalPerformanceForTest().state().assigner.ram;
    check(baseline[0x95]==20 && baseline[0x96]==30
          && baseline[0x5e]==44 && baseline[0x62]==64,
          "reflection interleave starts from independent stationary slider codes");

    juce::MidiBuffer midi;
    if(fullTone) {
        auto incoming=p.currentPatch();
        incoming.cutoff=90.f/127; incoming.resonance=80.f/127;
        std::array<std::uint8_t,youknow::sysex::patchMessageBytes> raw{};
        check(youknow::sysex::writePatchMessage(incoming,9,raw.data(),raw.size())==raw.size(),
              "paused full-tone reflection fixture encoded");
        midi.addEvent(juce::MidiMessage::createSysExMessage(raw.data()+1,int(raw.size())-2),0);
    } else {
        std::array<std::uint8_t,youknow::sysex::parameterMessageBytes> raw{};
        check(youknow::sysex::writeParameterMessage(5,90,9,raw.data(),raw.size())==raw.size(),
              "paused single-control reflection fixture encoded");
        midi.addEvent(juce::MidiMessage::createSysExMessage(raw.data()+1,int(raw.size())-2),0);
    }
    samples(p,1,&midi);
    check(p.getOriginalPerformanceForTest().pending()==(fullTone?24u:7u),
          "paused reflection preserves only the literal received packet");
    // There is no message loop in this test. Let the physical packet arrive
    // before the new slider gesture, while its APVTS reflection remains queued.
    // This gives an unambiguous receive-before-panel order without fitting the
    // relative timing of a nearby ADC scan and receive ISR.
    for(int i=0;i<12;++i) samples(p,173);
    const auto received=p.getOriginalPerformanceForTest().state().assigner.ram;
    check(received[0x95]==90 && received[0x96]==(fullTone?80:30)
          && p.getOriginalPerformanceForTest().pending()==0,
          "physical receive commits before the paused UI reflection");
    for(unsigned i=0;i<16;++i)
        check(received[0x58+i]==baseline[0x58+i]
              && received[0x6c+i]==baseline[0x6c+i],
              "received tone leaves every physical ADC and accepted history stationary");
    check(std::abs(p.currentPatch().cutoff-20.f/127)<1.e-6f
          && std::abs(p.currentPatch().resonance-30.f/127)<1.e-6f,
          "incoming reflection remains paused while the hardware tone has changed");

    setSlider(p,cutoffGesture?youknow::parameters::cutoff:youknow::parameters::resonance,
              cutoffGesture?60.f/127:50.f/127);
    samples(p,1);
    check(p.getOriginalPerformanceForTest().pending()==0,
          "newer native gesture does not retransmit a paused incoming tone");
    for(int i=0;i<12;++i) samples(p,173);
    const auto checkPanel=[&p,&baseline,cutoffGesture] {
        const auto& ram=p.getOriginalPerformanceForTest().state().assigner.ram;
        // Literal inverse coordinates below the upper ADC stretch: cutoff 60
        // uses raw 124, resonance 50 uses raw 104. Only that physical pot moved.
        const unsigned moved=cutoffGesture?6u:10u;
        const unsigned movedRaw=cutoffGesture?124u:104u;
        for(unsigned i=0;i<16;++i)
            check(ram[0x58+i]==(i==moved?movedRaw:baseline[0x58+i])
                  && ram[0x6c+i]==(i==moved?movedRaw:baseline[0x6c+i]),
                  "newer native gesture moves only its physical ADC and accepted history");
    };
    checkPanel();
    const auto panelTone=p.getOriginalPerformanceForTest().state().assigner.ram;
    // A-5 08BE can retain bit 7 as panel ownership after a received patch;
    // compare the audible stored coordinate independently of that provenance.
    check((panelTone[0x95]&127u)==(cutoffGesture?60u:90u)
          && (panelTone[0x96]&127u)==(cutoffGesture?(fullTone?80u:30u):50u),
          "later physical slider gesture follows the received tone in actual A5 store order");

    p.flushPendingMidiEvents();
    const auto ui=p.currentPatch();
    check(std::abs(ui.cutoff-float(cutoffGesture?60:90)/127)<1.e-6f
          && std::abs(ui.resonance-float(cutoffGesture?(fullTone?80:30):50)/127)<1.e-6f,
          "delayed reflection retains the newer gesture and reflects untouched incoming fields");
    samples(p,1);
    check(p.getOriginalPerformanceForTest().pending()==0,
          "flushing delayed reflection adds no incoming DIN bytes");
    for(int i=0;i<12;++i) samples(p,173);
    checkPanel();
    const auto& settled=p.getOriginalPerformanceForTest().state().assigner.ram;
    check((settled[0x95]&127u)==(panelTone[0x95]&127u)
          && (settled[0x96]&127u)==(panelTone[0x96]&127u)
          && p.getOriginalPerformanceForTest().pending()==0,
          "delayed host reflection preserves the physical tone order without duplicate transport");
}

static void pausedReflectionAndKeyModeGesture() {
    YouKnowAudioProcessor p;
    p.prepareToPlay(48000,512);
    for(int i=0;i<12;++i) samples(p,173);
    const auto baseline=p.getOriginalPerformanceForTest().state().assigner.ram;
    check((baseline[0xc8]&6u)==2,"key-mode interleave starts in actual Poly1");
    const auto before=p.currentPatch();
    std::array<std::uint8_t,youknow::sysex::parameterMessageBytes> raw{};
    check(youknow::sysex::writeParameterMessage(5,9,6,raw.data(),raw.size())==raw.size(),
          "key-mode paused-reflection fixture encoded");
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::createSysExMessage(raw.data()+1,int(raw.size())-2),0);
    samples(p,1,&midi);
    // The incoming tone owns none of the assign buttons. Change only those
    // two UI parameters while its tone reflection is still waiting.
    p.setKeyModeFromUi(youknow::KeyMode::Unison);
    samples(p,1);
    check(p.getOriginalPerformanceForTest().pending()==raw.size(),
          "key-mode gesture adds no bytes to the paused incoming control");
    for(int i=0;i<12;++i) samples(p,173);
    check((p.getOriginalPerformanceForTest().state().assigner.ram[0xc8]&6u)==6
          && p.getOriginalPerformanceForTest().state().assigner.ram[0x95]==9,
          "native Unison press executes while only a MIDI tone shadow is active");
    check(std::abs(p.currentPatch().cutoff-before.cutoff)<1.e-6f,
          "key-mode gesture does not prematurely reflect the incoming tone field");
    p.flushPendingMidiEvents();
    for(int i=0;i<12;++i) samples(p,173);
    const auto& settled=p.getOriginalPerformanceForTest().state().assigner.ram;
    check((settled[0xc8]&6u)==6 && settled[0x95]==9
          && p.getOriginalPerformanceForTest().pending()==0,
          "incoming tone reflection retains native Unison without duplicate DIN traffic");
    check(p.parameters.getRawParameterValue(youknow::parameters::poly1)->load()==1
          && p.parameters.getRawParameterValue(youknow::parameters::poly2)->load()==1,
          "incoming tone reflection retains both newer assign-button UI values");
    for(unsigned i=0;i<16;++i)
        check(settled[0x58+i]==baseline[0x58+i]
              && settled[0x6c+i]==baseline[0x6c+i],
              "assign-button gesture and incoming reflection move no physical tone ADC");
}

static void resetBeforeIncomingReflection() {
    YouKnowAudioProcessor p;
    setSlider(p,youknow::parameters::cutoff,20.f/127);
    p.prepareToPlay(48000,512);
    for(int i=0;i<12;++i) samples(p,173);
    std::array<std::uint8_t,youknow::sysex::parameterMessageBytes> raw{};
    check(youknow::sysex::writeParameterMessage(5,90,5,raw.data(),raw.size())==raw.size(),
          "reset-before-reflection fixture encoded");
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::createSysExMessage(raw.data()+1,int(raw.size())-2),0);
    samples(p,1,&midi);
    for(int i=0;i<12;++i) samples(p,173);
    check(p.getOriginalPerformanceForTest().state().assigner.ram[0x95]==90
          && p.getOriginalPerformanceForTest().pending()==0
          && std::abs(p.currentPatch().cutoff-20.f/127)<1.e-6f,
          "received cutoff is physical while its older UI image still awaits reflection");
    p.reset();
    // Reset seeds a new warm panel image from the adopted tone. Compare with
    // that snapshot, rather than imposing the pre-reset physical pot code.
    const auto resetPanel=p.getOriginalPerformanceForTest().state().assigner.ram;
    for(int i=0;i<12;++i) samples(p,173);
    check(p.getOriginalPerformanceForTest().state().assigner.ram[0x95]==90
          && p.getOriginalPerformanceForTest().pending()==0,
          "transport reset keeps the accepted tone while incoming reflection is paused");
    const auto checkResetPanel=[&p,&resetPanel] {
        const auto& ram=p.getOriginalPerformanceForTest().state().assigner.ram;
        for(unsigned i=0;i<16;++i)
            check(ram[0x58+i]==resetPanel[0x58+i]
                  && ram[0x6c+i]==resetPanel[0x6c+i],
                  "reset and delayed reflection manufacture no physical slider gesture");
    };
    checkResetPanel();
    p.flushPendingMidiEvents(); samples(p,1);
    check(p.getOriginalPerformanceForTest().pending()==0,
          "late incoming reflection after reset adds no DIN bytes");
    for(int i=0;i<12;++i) samples(p,173);
    checkResetPanel();
    check(p.getOriginalPerformanceForTest().state().assigner.ram[0x95]==90
          && std::abs(p.currentPatch().cutoff-90.f/127)<1.e-6f,
          "late reflection adopts the reset-retained tone without replacing it");
}

struct RecallInitOnFirstLfoReflection final : juce::AudioProcessorListener {
    explicit RecallInitOnFirstLfoReflection(int index) : lfoIndex(index) {}
    void audioProcessorParameterChanged(juce::AudioProcessor* processor,int index,float) override {
        if(!recalled && index==lfoIndex) {
            recalled=true;
            static_cast<YouKnowAudioProcessor*>(processor)->setCurrentProgram(0);
        }
    }
    void audioProcessorChanged(juce::AudioProcessor*,const ChangeDetails&) override {}
    int lfoIndex;
    bool recalled=false;
};

static void hostRecallDuringIncomingReflection() {
    YouKnowAudioProcessor p;
    p.prepareToPlay(48000,512);
    for(int i=0;i<12;++i) samples(p,173);
    const auto init=p.programPatch(0);
    auto incoming=init;
    incoming.lfoRate=3.f/127;
    incoming.range=youknow::DcoRange::Four;
    incoming.pulse=true;
    incoming.saw=false;
    std::array<std::uint8_t,youknow::sysex::patchMessageBytes> raw{};
    check(youknow::sysex::writePatchMessage(incoming,12,raw.data(),raw.size())==raw.size(),
          "reentrant recall full-tone fixture encoded");
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::createSysExMessage(raw.data()+1,int(raw.size())-2),0);
    samples(p,1,&midi);
    for(int i=0;i<12;++i) samples(p,173);
    check(p.currentPatch().range==init.range && p.currentPatch().pulse==init.pulse,
          "late packed-switch reflection has not touched the INIT UI before host recall");
    auto* lfo=p.parameters.getParameter(youknow::parameters::lfoRate);
    check(lfo!=nullptr,"first reflected LFO parameter exists");
    RecallInitOnFirstLfoReflection listener(lfo->getParameterIndex());
    p.addListener(&listener);
    p.flushPendingMidiEvents();
    p.removeListener(&listener);
    std::array<std::uint8_t,youknow::sysex::toneByteCount> expected{},actual{};
    youknow::sysex::toneBytesFromPatch(init,expected.data());
    youknow::sysex::toneBytesFromPatch(p.currentPatch(),actual.data());
    check(listener.recalled && actual==expected
          && std::abs(p.currentPatch().lfoRate-init.lfoRate)<1.e-6f
          && p.getCurrentProgram()==0 && !p.currentProgramIsEdited(),
          "reentrant INIT recall supersedes every later setter in the old full-tone reflection");
    // Range and pulse were already INIT on the UI when recall happened, so
    // their setters need not publish new edit revisions. The changed recall
    // generation itself must retire the older packet before those late fields.
    samples(p,1);
    check(p.getOriginalPerformanceForTest().pending()==24,
          "reentrant host recall sends only its own complete INIT tone");
    for(int i=0;i<12;++i) samples(p,173);
    const auto& ram=p.getOriginalPerformanceForTest().state().assigner.ram;
    for(unsigned i=0;i<16;++i)
        check((ram[0x90+i]&127u)==expected[i],"reentrant INIT recall reaches continuous A5 tone stores");
    check((ram[0x8e]&127u)==expected[16] && (ram[0x8f]&127u)==expected[17]
          && p.getOriginalPerformanceForTest().pending()==0,
          "reentrant INIT recall retains its wave/range switches after the old reflection returns");
}

static void publishedNativePanelOrigin(bool beforeSnapshot) {
    YouKnowAudioProcessor p;
    setSlider(p,youknow::parameters::cutoff,20.f/127);
    setSlider(p,youknow::parameters::resonance,30.f/127);
    p.prepareToPlay(48000,512);
    for(int i=0;i<12;++i) samples(p,173);
    const auto baseline=p.getOriginalPerformanceForTest().state().assigner.ram;
    std::array<std::uint8_t,youknow::sysex::parameterMessageBytes> prior{};
    check(youknow::sysex::writeParameterMessage(5,90,4,prior.data(),prior.size())==prior.size(),
          "native publication origin fixture encoded");
    juce::MidiBuffer first;
    first.addEvent(juce::MidiMessage::createSysExMessage(prior.data()+1,int(prior.size())-2),0);
    samples(p,1,&first);
    for(int i=0;i<12;++i) samples(p,173);
    check(p.getOriginalPerformanceForTest().state().assigner.ram[0x95]==90
          && p.getOriginalPerformanceForTest().state().assigner.ram[0x5e]==44
          && std::abs(p.currentPatch().cutoff-20.f/127)<1.e-6f,
          "MIDI-owned cutoff equals the next native value while its physical pot remains elsewhere");

    if(beforeSnapshot) {
        setSlider(p,youknow::parameters::cutoff,90.f/127);
        samples(p,1);
        check(p.getOriginalPerformanceForTest().pending()==0,
              "equal-tone native gesture before a snapshot adds no incoming DIN bytes");
    } else {
        auto incoming=p.currentPatch(); incoming.cutoff=90.f/127; incoming.resonance=80.f/127;
        std::array<std::uint8_t,youknow::sysex::patchMessageBytes> raw{};
        check(youknow::sysex::writePatchMessage(incoming,10,raw.data(),raw.size())==raw.size(),
              "snapshot-to-MIDI native origin fixture encoded");
        juce::MidiBuffer later;
        later.addEvent(juce::MidiMessage::createSysExMessage(raw.data()+1,int(raw.size())-2),511);
        std::atomic<bool> captured{false},resume{false};
        p.setMidiReflectionSnapshotBarrierForTest(&captured,&resume);
        std::thread audio([&] { samples(p,512,&later); });
        const double deadline=juce::Time::getMillisecondCounterHiRes()+2000;
        while(!captured.load(std::memory_order_acquire)
              && juce::Time::getMillisecondCounterHiRes()<deadline)
            std::this_thread::yield();
        const bool reached=captured.load(std::memory_order_acquire);
        if(reached) setSlider(p,youknow::parameters::cutoff,90.f/127);
        resume.store(true,std::memory_order_release); audio.join();
        check(reached,"native publication occurs after the captured audio snapshot");
        check(p.getOriginalPerformanceForTest().pending()==raw.size(),
              "native publication before a later MIDI event preserves only that full-tone packet");
    }
    for(int i=0;i<12;++i) samples(p,173);
    const auto checkPanel=[&p,&baseline] {
        const auto& ram=p.getOriginalPerformanceForTest().state().assigner.ram;
        for(unsigned i=0;i<16;++i)
            check(ram[0x58+i]==(i==6?184u:baseline[0x58+i])
                  && ram[0x6c+i]==(i==6?184u:baseline[0x6c+i]),
                  "explicit native origin moves the equal-valued cutoff pot and no MIDI-owned pot");
    };
    checkPanel();
    check((p.getOriginalPerformanceForTest().state().assigner.ram[0x95]&127u)==90
          && p.getOriginalPerformanceForTest().state().assigner.ram[0x96]==(beforeSnapshot?30:80),
          "equal-valued native cutoff and later received tone reach actual A5 stores");
    p.flushPendingMidiEvents(); samples(p,1);
    check(p.getOriginalPerformanceForTest().pending()==0,
          "reflection of captured native revisions adds no duplicate DIN traffic");
    for(int i=0;i<12;++i) samples(p,173);
    checkPanel();
    check(std::abs(p.currentPatch().cutoff-90.f/127)<1.e-6f
          && std::abs(p.currentPatch().resonance-float(beforeSnapshot?30:80)/127)<1.e-6f,
          "native and MIDI origin ordering reaches the final host mirror");
}
int main() {
    juce::ScopedJuceInitialiser_GUI init;
    YouKnowAudioProcessor p;
    check(p.parameters.getRawParameterValue(youknow::parameters::originalPerformance)->load()==1,
          "new instances automatically use Original timing");
    p.prepareToPlay(48000,512); block(p,true); block(p);
    check(p.getOriginalPerformanceModeForTest() && p.getActiveVoiceCount()>0,"host MIDI reaches original pipeline");
    p.setCurrentProgram(2); block(p);
    check(p.getOriginalPerformanceModeForTest(),"program recall retains timing policy");
    p.requestPanic(); block(p);
    check(p.getActiveVoiceCount()==0,"UI panic clears original notes and pending serial work");
    juce::MemoryBlock state; p.getStateInformation(state);
    YouKnowAudioProcessor restored; restored.setStateInformation(state.getData(),int(state.getSize()));
    restored.prepareToPlay(44100,512); block(restored,true); block(restored);
    check(restored.getOriginalPerformanceModeForTest() && restored.getActiveVoiceCount()>0,"session roundtrip retains original mode");
    // A pre-parameter session explicitly supplies historical Direct even when loaded
    // into an instance currently using Original.
    auto legacy=restored.parameters.copyState();
    for(int i=legacy.getNumChildren();--i>=0;) if(legacy.getChild(i).getProperty("id").toString()==youknow::parameters::originalPerformance)
        legacy.removeChild(i,nullptr);
    auto xml=legacy.createXml(); juce::MemoryBlock bytes;
    juce::AudioProcessor::copyXmlToBinary(*xml,bytes);
    restored.setStateInformation(bytes.getData(),int(bytes.getSize())); block(restored);
    check(!restored.getOriginalPerformanceModeForTest(),"old state migrates to Direct");
    choose(restored,1); block(restored); choose(restored,0); block(restored,true);
    check(!restored.getOriginalPerformanceModeForTest() && restored.getActiveVoiceCount()>0,"runtime mode switch returns to Direct");
    juce::MemoryBlock directState; restored.getStateInformation(directState);
    YouKnowAudioProcessor directRestored;
    directRestored.setStateInformation(directState.getData(),int(directState.getSize()));
    directRestored.prepareToPlay(48000,512); block(directRestored,true);
    check(!directRestored.getOriginalPerformanceModeForTest(),"explicit saved Direct survives new-instance default");
    sourceAwareInput();
    for(bool fullTone:{false,true}) for(bool cutoffGesture:{false,true})
        pausedReflectionAndNewerPanelGesture(fullTone,cutoffGesture);
    pausedReflectionAndKeyModeGesture();
    resetBeforeIncomingReflection();
    hostRecallDuringIncomingReflection();
    publishedNativePanelOrigin(true);
    publishedNativePanelOrigin(false);
    std::puts("Original performance plugin contracts passed");
}
