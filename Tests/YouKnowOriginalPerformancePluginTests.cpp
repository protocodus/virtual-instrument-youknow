#include "../Source/PluginProcessor.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
int main() {
    juce::ScopedJuceInitialiser_GUI init;
    YouKnowAudioProcessor p;
    choose(p,1); p.prepareToPlay(48000,512); block(p,true); block(p);
    check(p.getOriginalPerformanceModeForTest() && p.getActiveVoiceCount()>0,"host MIDI reaches original pipeline");
    p.setCurrentProgram(2); block(p);
    check(p.getOriginalPerformanceModeForTest(),"program recall retains timing policy");
    p.requestPanic(); block(p);
    check(p.getActiveVoiceCount()==0,"UI panic clears original notes and pending serial work");
    juce::MemoryBlock state; p.getStateInformation(state);
    YouKnowAudioProcessor restored; restored.setStateInformation(state.getData(),int(state.getSize()));
    restored.prepareToPlay(44100,512); block(restored,true); block(restored);
    check(restored.getOriginalPerformanceModeForTest() && restored.getActiveVoiceCount()>0,"session roundtrip retains original mode");
    // A pre-parameter session supplies Direct's layout default even when loaded
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
    std::puts("Original performance plugin contracts passed");
}
