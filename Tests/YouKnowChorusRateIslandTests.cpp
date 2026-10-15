// Numerical chronology tests use an independent integer timestamp ledger;
// frequency response is qualified by the existing physical-node chorus suite.
// This test also runs the complete engine's new routing, latency and lifecycle.
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductHardwareRealism.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <vector>
namespace { bool guardAllocation=false; unsigned allocations=0; }
void* operator new(std::size_t n) { if(guardAllocation) ++allocations; if(auto* p=std::malloc(n?n:1)) return p; throw std::bad_alloc{}; }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
namespace youknow {
struct YouKnowTestAccess {
 static double chorusRate(const YouKnowEngine& e){return e.chorus_.sampleRate_;}
 static auto builds(const YouKnowEngine& e){return e.chorus_.supportBuildCount_;}
 static double phase(const YouKnowEngine& e){return e.chorus_.getLfoPhase();}
 static auto directContext(YouKnowEngine& e){e.activeParameters_.enableOutputSummerAntialias=false;e.outputSummerDelayedContext_={.375,true};return e.outputSummerHostNoiseContext();}
 static int cursor(const YouKnowEngine& e){return e.chorusRateIsland_.inputWrite;}
 static auto clocks(const YouKnowEngine& e){return std::tuple{e.chorus_.lineA_.clockPhase,e.chorus_.lineB_.clockPhase};}
 static EngineParameters parameters(const YouKnowEngine& e){return e.activeParameters_;}
 static double directGridImpulseCentre(YouKnowEngine& e){
  // Exercise the actual decimator/pad state installed by the selector. Do
  // not call refreshLatencyPad here: that would conceal a missed refresh.
  e.firstDecimator_.reset();e.secondDecimator_.reset();
  e.latencyPadLeft_.fill(0);e.latencyPadRight_.fill(0);e.latencyPadWriteIndex_=0;
  double sum=0,moment=0;
  for(int frame=0;frame<256;++frame){
   std::array<float,4> stage{};
   for(int step=0;step<4;++step)
    if(frame*4+step==e.correctionHalfWidth)stage[static_cast<std::size_t>(step)]=1;
   float firstL=0,firstR=0,secondL=0,secondR=0,left=0,right=0;
   e.downsamplePair(e.firstDecimator_,stage[0],stage[0],stage[1],stage[1],firstL,firstR);
   e.downsamplePair(e.firstDecimator_,stage[2],stage[2],stage[3],stage[3],secondL,secondR);
   e.downsamplePair(e.secondDecimator_,firstL,firstR,secondL,secondR,left,right);
   e.applyLatencyPad(left,right);sum+=left;moment+=frame*double(left);
  }
  return moment/sum;
 }
 static bool rebuild(YouKnowEngine& e,int factor){for(auto& v:e.voices_)v.active=false;e.anyVoiceActive_=false;e.oversamplingIdleSamples_=e.oversamplingQuietSamples_;e.rateTransition_=YouKnowEngine::RateTransition::FadingOut;e.rateTransitionGain_=0;return e.setOversamplingFactor(factor);}
};}
namespace {
using namespace youknow;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void near(double actual,double expected,double epsilon,const char* message){if(!std::isfinite(actual)||std::abs(actual-expected)>epsilon){std::cerr<<message<<": "<<actual<<" vs "<<expected<<'\n';throw std::runtime_error(message);}}
void transportAndControls(){
 for(double rate:{48000.,96000.,192000.}){
  ChorusRateIsland island;
  auto k=VoiceVcaAntialias::prepare(rate);
  const int delay=k.factor>1?48:0;
  int calls=0;double sum=0,moment=0;
  for(int frame=0;frame<320;++frame){
   ChorusRateIsland::Controls c;c.noise=float(frame)/512;c.mode=frame%2?ChorusMode::One:ChorusMode::Off;c.hardwareMode=frame%3?ChorusMode::Two:ChorusMode::One;
   const auto process=[&](float x,const ChorusRateIsland::Controls& p){
    const int timestamp=k.factor>1?frame-24:frame;
    near(p.noise,double(timestamp)/512,0,"control has wrong input timestamp");
    require(p.mode==(timestamp%2?ChorusMode::One:ChorusMode::Off),"mute command timestamp mismatch");
    require(p.hardwareMode==(timestamp%3?ChorusMode::Two:ChorusMode::One),"independent IC40 mode timestamp mismatch");
    ++calls;return ChorusRateIsland::Result{x,2*x,{p.noise,p.mode!=ChorusMode::Off}};};
   guardAllocation=true;auto y=island.processWith(frame==96?1.f:0.f,c,k,process);guardAllocation=false;
   const int timestamp=frame-delay;
   near(y.context.wetRatio,timestamp>=0?double(timestamp)/512:0,0,"output context timestamp mismatch");
   require(y.context.wetConnected==(timestamp>=0&&timestamp%2!=0),"output context mode mismatch");
   near(y.right,2.0*y.left,1e-9,"stereo transport changed relative level");
   sum+=y.left;moment+=frame*double(y.left);
  }
  near(sum,1,2e-7,"transport DC gain");near(moment/sum,96+delay,2e-5,"transport latency");
  require(calls==(320-(k.factor>1?24:0))*k.factor,"cold start advanced physical time before timestamp0");
  ChorusRateIsland::Controls old;old.noise=.125f;island.resetWarm(old);int warmCalls=0;
  ChorusRateIsland::Controls next;next.noise=.75f;
  (void)island.processWith(0,next,k,[&](float,const auto& c){++warmCalls;near(c.noise,k.factor>1?.125:.75,0,"warm control priming");return ChorusRateIsland::Result{};});
  require(warmCalls==k.factor,"warm numerical reset froze physical clock");
 }
 require(allocations==0,"island transport allocated");
}
std::unique_ptr<YouKnowEngine> prepared(double rate,int factor,bool island){
 auto e=std::make_unique<YouKnowEngine>();ProductHardwareRealismProfile::configureBeforePrepare(*e);e->prepare(rate,64,factor);
 EngineParameters p;ProductHardwareRealismProfile::applyTo(p);p.enableChorusRateIsland=island;p.calibration=0;p.aging=0;p.chorusNoise=0;p.noiseLevel=0;p.chorus=ChorusMode::Two;p.cutoff=.8f;p.resonance=.1f;p.attack=0;p.sustain=1;p.release=.1f;p.sawEnabled=p.pulseEnabled=true;p.pwmDepth=.57f;e->setParameters(p);return e;
}
void directIdentity(){
 for(double rate:{176400.,192000.,384000.}){
  auto a=prepared(rate,1,false),b=prepared(rate,1,true);a->noteOn(72,1);b->noteOn(72,1);
  std::array<float,64> al{},ar{},bl{},br{};
  for(int block=0;block<24;++block){a->process(al.data(),ar.data(),64);b->process(bl.data(),br.data(),64);require(al==bl&&ar==br,"direct high-grid flag changed audio");}
  require(a->getProcessingLatencySamples()==b->getProcessingLatencySamples(),"direct flag changed latency");
  const auto ac=YouKnowTestAccess::directContext(*a),bc=YouKnowTestAccess::directContext(*b);
  require(ac.wetRatio==.375&&bc.wetRatio==ac.wetRatio&&bc.wetConnected==ac.wetConnected,"direct flag changed raw host-noise context");
 }
}
void directQualitySelectorLatency(){
 // At48k/global4 the chorus itself is already direct. Enabling its island
 // still changes the maximum reported across quality settings, because the
 // possible1x setting needs its48-sample FIR. The current4x path must receive
 // the matching pad without re-preparing its physical chorus state.
 auto e=prepared(48000,4,false);
 auto p=YouKnowTestAccess::parameters(*e);
 p.enableVoiceVcaAntialias=false;p.enableVoiceFilterRateIsland=false;
 p.enableOutputSummerAntialias=false;e->setParameters(p);
 require(e->getOversamplingFactor()==4,"direct selector fixture did not reach4x");
 std::array<float,64> left{},right{};e->process(left.data(),right.data(),64);
 for(bool enabled:{false,true,false,true}){
  const auto phase=YouKnowTestAccess::phase(*e),builds=double(YouKnowTestAccess::builds(*e));
  const auto clocks=YouKnowTestAccess::clocks(*e);
  p.enableChorusRateIsland=enabled;e->setParameters(p);
  const int expected=enabled?72:41;
  require(e->getProcessingLatencySamples()==expected,"direct selector maximum latency mismatch");
  const double centre=YouKnowTestAccess::directGridImpulseCentre(*e);
  near(centre,expected,.50001,"direct selector left an obsolete latency pad");
  near(YouKnowTestAccess::phase(*e),phase,0,"direct selector reset physical LFO");
  require(YouKnowTestAccess::clocks(*e)==clocks,"direct selector changed physical BBD clocks");
  near(double(YouKnowTestAccess::builds(*e)),builds,0,"direct selector rebuilt support coefficients");
 }
}
void ratesAndLifecycle(){
 // Low-host fallback must preserve the previous IC6 reconstruction rate.
 for(const auto& row:std::array<std::array<double,4>,7>{{
     {{8000,1,4,4}},{{16000,2,4,1}},{{32000,2,4,1}},
     {{32000,4,2,1}},{{44100,1,4,4}},{{44100,2,2,2}},{{44100,4,1,1}}}}){
  const double parent=row[0]*row[1];
  require(VoiceVcaAntialias::factorForRate(parent)==int(row[2]),"low-host IC6 test coordinate");
  require(ChorusRateIsland::factorForRate(parent,int(row[1]))==int(row[3]),"low-host island reduced existing IC6 rate");
 }
 for(double rate:{8000.,16000.,24000.,32000.,44100.,48000.,96000.,192000.}){
  auto e=prepared(rate,1,true),legacy=prepared(rate,1,false);
  const auto builds=YouKnowTestAccess::builds(*e);
  require(e->getProcessingLatencySamples()==legacy->getProcessingLatencySamples(),"shared island added reported latency");
  std::array<float,64> l{},r{};
  for(int factor:{2,4,1}){
   const auto phase=YouKnowTestAccess::phase(*e);
   guardAllocation=true;(void)YouKnowTestAccess::rebuild(*e,factor);guardAllocation=false;
   near(YouKnowTestAccess::phase(*e),phase,0,"quality rebuild reset physical LFO phase");
   const double parent=rate*e->getOversamplingFactor();
   near(YouKnowTestAccess::chorusRate(*e),parent*ChorusRateIsland::factorForRate(parent,e->getOversamplingFactor()),0,"chorus local rate incorrect");
   require(YouKnowTestAccess::builds(*e)==builds,"quality rebuild constructed uncached support");
   const int cursor=YouKnowTestAccess::cursor(*e);const auto clocks=YouKnowTestAccess::clocks(*e);
   (void)e->setOversamplingFactor(factor);
   require(YouKnowTestAccess::cursor(*e)==cursor&&YouKnowTestAccess::clocks(*e)==clocks,"no-op quality request reset histories");
   guardAllocation=true;e->process(l.data(),r.data(),64);guardAllocation=false;
   const double advance=Chorus::settingsFor(ChorusMode::Two,ChorusTimingProfile::HardwareEvidence).rateHz*64/rate;
   double delta=YouKnowTestAccess::phase(*e)-phase;if(delta<0)delta+=1;
   // The first rate request that is a no-op still has the cold startup hold.
   require(delta>0&&delta<=advance+1e-12,"quality rebuild did not advance physical time");
   for(float x:l)require(std::isfinite(x),"nonfinite island render");
  }
 }
 require(allocations==0,"engine island/rate change allocated");
}
}
int main(){try{transportAndControls();directIdentity();directQualitySelectorLatency();ratesAndLifecycle();std::cout<<"Chorus rate-island transport, controls, high-grid identity and lifecycle passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
