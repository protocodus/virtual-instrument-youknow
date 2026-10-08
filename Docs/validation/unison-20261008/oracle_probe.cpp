#include "DSP/YouKnowEngine.h"
#include <array>
#include <cstdio>
#include <memory>
#include <algorithm>
using namespace youknow;
namespace youknow {
struct YouKnowTestAccess {
    static bool keyed(const YouKnowEngine& e, int i) { return e.voices_[i].keyDown; }
    static float pitch(const YouKnowEngine& e, int i) { return e.voices_[i].currentMidi; }
    static unsigned gates(const YouKnowEngine& e) { unsigned n=0; for(int i=0;i<6;++i) if(e.voices_[i].envelope.gate) n|=1u<<i; return n; }
    static unsigned runs(const YouKnowEngine& e) { unsigned n=0; for(int i=0;i<6;++i) if(e.voices_[i].envelope.running) n|=1u<<i; return n; }
    static bool pending(const YouKnowEngine& e) { return e.assignmentRescanPending_; }
};
}
using A=YouKnowTestAccess;
void step(YouKnowEngine& e,int n=1) { std::array<float,64> l{},r{}; while(n){int c=std::min(64,n);e.process(l.data(),r.data(),c);n-=c;} }
EngineParameters patch(KeyMode mode) { EngineParameters p; p.keyMode=mode;p.polyphony=6;p.portamento=.8f;p.chorus=ChorusMode::Off;p.attack=.2f;p.decay=.2f;p.sustain=.3f;p.release=.2f;return p; }
auto create(bool original, KeyMode mode) {auto e=std::make_unique<YouKnowEngine>();e->configurePhysicalVoicePowerOnGlide(true);e->configureSingleVoiceLastNotePriority(true);e->setParameters(patch(mode));e->setOriginalPerformanceMode(original);e->prepare(48000,64,1);step(*e,4096);return e;}
void dump(const char* name,int frame,const YouKnowEngine& e){std::printf("%s frame=%d gates=%02x running=%02x pending=%d current=",name,frame,A::gates(e),A::runs(e),A::pending(e));for(int c=0;c<6;++c) std::printf("%s%.6f",c?",":"",A::pitch(e,c));std::puts("");}
int main(){
 for(bool original:{false,true}){
  const char* name=original?"original":"direct";
  auto e=create(original,KeyMode::Unison);dump(name,-1,*e);e->noteOn(48,1);dump(name,0,*e);
  unsigned gates=A::gates(*e);for(int i=1;i<=2048;++i){step(*e);if(A::gates(*e)!=gates){gates=A::gates(*e);dump(name,i,*e);}}
  e=create(original,KeyMode::Poly2);e->noteOn(48,1);step(*e,4096);e->noteOn(72,1);step(*e,4096);dump(original?"partial-original-before":"partial-direct-before",0,*e);
  e->setParameters(patch(KeyMode::Unison));gates=A::gates(*e);for(int i=1;i<=4096;++i){step(*e);if(A::gates(*e)!=gates){gates=A::gates(*e);dump(original?"partial-original":"partial-direct",i,*e);}}
 }
 for(bool sustain:{false,true}) for(bool original:{false,true}){
  auto e=create(original,KeyMode::Unison);auto p=patch(KeyMode::Unison);p.portamento=0;e->setParameters(p);e->noteOn(60,1);step(*e,4096);e->noteOn(64,1);step(*e,4096);if(sustain){e->setSustainPedal(true);step(*e,4096);}
  const char* name=original?(sustain?"release-original-hold":"release-original"):(sustain?"release-direct-hold":"release-direct");dump(name,-1,*e);e->noteOff(60);dump(name,0,*e);
  unsigned gates=A::gates(*e), runs=A::runs(*e);for(int i=1;i<=4096;++i){step(*e);if(A::gates(*e)!=gates||A::runs(*e)!=runs){gates=A::gates(*e);runs=A::runs(*e);dump(name,i,*e);}}
 }
}
