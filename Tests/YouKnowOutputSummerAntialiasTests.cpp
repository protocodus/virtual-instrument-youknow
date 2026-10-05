// The actual IC6 stage is checked against an independently integrated
// continuous generalized-algebraic law and the analogue GBW magnitude.
// Exact-bin wanted/folded lines separate numerical aliasing from a new tone
// law. Noise-context checks use independent integer/fractional event ledgers.
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <tuple>
#include <vector>
namespace { bool guardAllocation=false; unsigned allocations=0; }
void* operator new(std::size_t n) { if(guardAllocation) ++allocations; if(auto* p=std::malloc(n)) return p; throw std::bad_alloc{}; }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
namespace youknow {
struct YouKnowTestAccess {
 static float clip(float x) {return YouKnowEngine::outputSummerClip(x);}
 static EngineParameters parameters(const YouKnowEngine& e) {return e.activeParameters_;}
 static void summer(YouKnowEngine& e,float& l,float& r) {e.processOutputSummer(l,r,e.activeParameters_);}
 static void wet(YouKnowEngine& e,double r) { e.chorus_.finiteMuteDriveEnabled_=true; e.chorus_.wetInputConductanceRatio_=r; e.chorus_.muteDriveMuted_=r==0; }
 static auto context(const YouKnowEngine& e) {return e.outputSummerDelayedContext_;}
 static auto hostContext(YouKnowEngine& e) {return e.outputSummerHostNoiseContext();}
 static double poleRatio(const YouKnowEngine& e) {return e.outputFiniteWetRatio_;}
 static auto random(const YouKnowEngine& e) {return std::tuple{e.noiseState_,e.outputNoiseStateLeft_,e.outputNoiseStateRight_,e.outputWiperNoiseStateLeft_,e.outputWiperNoiseStateRight_,e.headphoneIntrinsicNoiseStateLeft_,e.voices_[0].vcaShotRandom.state,e.chorus_.lineA_.noiseState,e.chorus_.lineA_.transferNoiseState};}
 static float preclip(const YouKnowEngine& e) {const auto& a=e.outputSummerAntialias_.left; return a.inputs[(a.inputWrite-1+64)&63];}
 static bool empty(const YouKnowEngine& e) {
  const auto zero=[](auto x){return x==0;};const auto& a=e.outputSummerAntialias_;
  return a.write==0 && std::all_of(a.left.inputs.begin(),a.left.inputs.end(),zero) && std::all_of(a.right.outputs.begin(),a.right.outputs.end(),zero)
   && e.outputSummerHostContextWrite_==0 && std::all_of(e.outputSummerHostContexts_.begin(),e.outputSummerHostContexts_.end(),[](auto x){return x.wetRatio==0&&!x.wetConnected;})
   && std::all_of(e.latencyPadLeft_.begin(),e.latencyPadLeft_.end(),zero);
 }
 static auto physical(const YouKnowEngine& e) {return std::tuple{e.outputSlewStateLeft_,e.outputSlewStateRight_,e.outputSummerMagnitudeLeft_.previousInput,e.outputSummerMagnitudeLeft_.difference};}
 static int write(const YouKnowEngine& e) {return e.outputSummerAntialias_.write;}
 static int padding(const YouKnowEngine& e) {return e.latencyPadSamples_;}
 static bool qualifiedRebuild(YouKnowEngine& e,int factor) {for(auto& v:e.voices_) v.active=false;e.anyVoiceActive_=false;e.oversamplingIdleSamples_=e.oversamplingQuietSamples_;e.rateTransition_=YouKnowEngine::RateTransition::FadingOut;e.rateTransitionGain_=0;return e.setOversamplingFactor(factor);}
 static double physicalContextDelay(YouKnowEngine& e) {
  e.firstDecimator_.reset();e.secondDecimator_.reset();e.refreshLatencyPad();
  const int factor=e.oversampling_;double total=0,weighted=0;
  // Excite the LAST internal substep of a host frame, matching the actual
  // context capture anchor. The known input impulse occurs at host frame32.
  for(int frame=0;frame<256;++frame) {
   std::array<float,4> stage{};if(frame==32)stage[factor-1]=1;
   float l=0,r=0;
   if(factor==4){float a=0,b=0,ar=0,br=0;e.downsamplePair(e.firstDecimator_,stage[0],stage[0],stage[1],stage[1],a,ar);e.downsamplePair(e.firstDecimator_,stage[2],stage[2],stage[3],stage[3],b,br);e.downsamplePair(e.secondDecimator_,a,ar,b,br,l,r);}
   else if(factor==2)e.downsamplePair(e.firstDecimator_,stage[0],stage[0],stage[1],stage[1],l,r);
   else l=r=stage[0];
   e.applyLatencyPad(l,r);total+=l;weighted+=frame*double(l);
  }
  return weighted/total-32;
 }
 static double impulseCentre(YouKnowEngine& e) {
  e.firstDecimator_.reset();e.secondDecimator_.reset();e.refreshLatencyPad();e.voices_[0].vcaAntialias.reset();e.outputSummerAntialias_.reset();
  const int factor=e.oversampling_;double total=0,weighted=0;
  for(int frame=0;frame<512;++frame) {
   std::array<float,4> stage{};
   for(int step=0;step<factor;++step) {
    float x=frame*factor+step==e.correctionHalfWidth?1.f:0.f;
    if(e.activeParameters_.enableVoiceVcaAntialias) x=e.voices_[0].vcaAntialias.process(x,1,e.voiceVcaAntialiasKernel_,[](float y){return y;});
    if(e.activeParameters_.enableOutputSummerAntialias) x=e.outputSummerAntialias_.process(x,x,{},e.voiceVcaAntialiasKernel_,[](float y){return y;}).left;
    stage[step]=x;
   }
   float l=0,r=0;
   if(factor==4) {float a=0,b=0,ar=0,br=0;e.downsamplePair(e.firstDecimator_,stage[0],stage[0],stage[1],stage[1],a,ar);e.downsamplePair(e.firstDecimator_,stage[2],stage[2],stage[3],stage[3],b,br);e.downsamplePair(e.secondDecimator_,a,ar,b,br,l,r);}
   else if(factor==2) e.downsamplePair(e.firstDecimator_,stage[0],stage[0],stage[1],stage[1],l,r);
   else l=r=stage[0];
   e.applyLatencyPad(l,r);total+=l;weighted+=frame*double(l);
  }
  return weighted/total;
 }
};}
namespace {
using namespace youknow;constexpr double pi=std::numbers::pi;
void require(bool x,const char* message) {if(!x) throw std::runtime_error(message);}
std::unique_ptr<YouKnowEngine> prepared(double rate,bool corrected,int factor=1,bool product=false) {
 auto e=std::make_unique<YouKnowEngine>(); if(product) ProductFidelityProfile::configureBeforePrepare(*e);e->prepare(rate,256,factor);
 EngineParameters p;if(product) ProductFidelityProfile::applyTo(p);p.enableOutputSummerAntialias=corrected;
 p.calibration=p.aging=p.chorusNoise=p.noiseLevel=0;p.enableCommonVcaNoise=false;p.enableOpAmpSlewLimiting=false;p.enableOutputSummerMagnitudePole=true;
 p.enableOutputSummerMuteLoading=false;p.cutoff=p.sustain=1;p.attack=p.release=0;p.vcaMode=VcaMode::Gate;p.resonance=.1f;p.highPass=HighPassMode::Boost;p.volume=1;p.sawEnabled=p.pulseEnabled=true;p.subLevel=1;
 e->setParameters(p);return e;
}
std::complex<double> projection(const std::vector<float>& y,int bin) {std::complex<double> sum{};for(int n=0;n<int(y.size());++n) sum+=double(y[n])*std::polar(1.,-2*pi*bin*n/y.size());return sum*(2./y.size());}
long double coefficient(long double amplitude,int harmonic) {
 constexpr int n=65536;long double sum=0;for(int i=0;i<n;++i) {const long double t=2*std::numbers::pi_v<long double>*i/n;const long double x=amplitude*std::cos(t);const auto z=std::abs(x)/13.5L;const auto y=x/std::pow(1+std::pow(z,8),.125L);sum+=y*std::cos(harmonic*t);}return 2*sum/n/2.6L;
}
std::vector<float> sine(double fs,bool aa,double volts,int bin,int n) {auto e=prepared(fs,aa);std::vector<float> y(n);for(int i=-n;i<n;++i) {float l=float(volts/2.6*std::cos(2*pi*bin*i/n)),r=l;YouKnowTestAccess::summer(*e,l,r);if(i>=0)y[i]=l;}return y;}
void analogueHarmonicsAndAliases() {
 constexpr int n=8192,bin=1019;double worstWanted=0,minimumImprovement=1000;
 for(double fs:{44100.,48000.,96000.}) {const int b=fs==96000.?bin/2:bin;const double f=fs*b/n;
  for(double volts:{2.6,6.,12.,18.}) {auto raw=sine(fs,false,volts,b,n),aa=sine(fs,true,volts,b,n);
   for(int h:{1,3}) {if(h*f>20000)continue;const double ideal=std::abs(double(coefficient(volts,h)))/std::sqrt(1+std::pow(h*f/YouKnowEngine::outputSummerBandwidthHz(),2));if(ideal<1e-5)continue;const double got=std::abs(projection(aa,h*b));const double db=20*std::log10(got/ideal);worstWanted=std::max(worstWanted,std::abs(db));std::cout<<"wanted "<<fs<<" "<<volts<<" h"<<h<<" error="<<db<<"dB\n";require(std::abs(db)<.01,"wanted analogue harmonic/GBW mismatch");}
   if(volts>=12) {double before=0,after=0;for(int h:{5,7,9,11,13,15}) {int folded=(h*b)%n;folded=std::min(folded,n-folded);if(h*f<fs/2 || fs*folded/n>20000)continue;before+=std::norm(projection(raw,folded));after+=std::norm(projection(aa,folded));}const double improvement=10*std::log10(before/after);minimumImprovement=std::min(minimumImprovement,improvement);require(improvement>25,"folded IC6 harmonics not removed");std::cout<<"alias "<<fs<<"Hz "<<volts<<"Vp improvement="<<improvement<<"dB\n";}
  }
 }
 std::cout<<"wanted worst="<<worstWanted<<"dB minimum alias improvement="<<minimumImprovement<<"dB\n";
 // Already-high grids keep the actual physical clip/pole byte exact.
 for(double fs:{176400.,192000.,384000.}) {auto a=prepared(fs,false),b=prepared(fs,true);for(int i=0;i<5000;++i) {float l=float(10*std::sin(i*.19)),r=float(7*std::cos(i*.073)),al=l,ar=r;YouKnowTestAccess::summer(*a,al,ar);YouKnowTestAccess::summer(*b,l,r);require(al==l&&ar==r,"high-grid bypass changed actual stage");}}
}
void moderatePassband() {
 constexpr int n=4096;double worst=0;const double fundamental=double(coefficient(2.6L,1));
 for(double fs:{8000.,44100.,48000.,96000.,192000.}) {
  const int last=int(std::min(20000.,.45*fs)*n/fs);
  for(int point=1;point<=8;++point) {const int bin=std::max(1,last*point/8);auto y=sine(fs,true,2.6,bin,n);
   const double frequency=fs*bin/n;const double physical=fundamental/std::sqrt(1+std::pow(frequency/YouKnowEngine::outputSummerBandwidthHz(),2));const double error=20*std::log10(std::abs(projection(y,bin))/physical);worst=std::max(worst,std::abs(error));require(std::abs(error)<.045,"moderate LINE passband changed");
  }
 }
 std::cout<<"moderate physical passband worst="<<worst<<"dB through20k\n";
}
void contextChronology() {
 for(double fs:{8000.,44100.,48000.,96000.,192000.}) for(int requested:{1,2,4}) {
  auto e=prepared(fs,true,requested,true);const int factor=e->getOversamplingFactor();auto p=YouKnowTestAccess::parameters(*e);p.enableOutputSummerMuteLoading=true;p.enableChorusFiniteMuteDrive=p.enableChorusMuteDrive=true;e->setParameters(p);
  const int local=VoiceVcaAntialias::factorForRate(fs*factor)>1?48:0;const double downstream=(factor==4?141./4:factor==2?47./2:0)+YouKnowTestAccess::padding(*e);const double measured=YouKnowTestAccess::physicalContextDelay(*e);std::cout<<"context impulse "<<fs<<" "<<factor<<" measured="<<measured<<" expected="<<downstream<<"\n";require(std::abs(measured-downstream)<1e-3,"host-context chronology disagrees with actual last-substep decimator impulse");const int whole=int(std::floor(downstream));const double frac=downstream-whole;
  std::vector<double> internal(2000,0),host(500,0);
  for(int frame=0;frame<500;++frame) {for(int step=0;step<factor;++step) {int k=frame*factor+step;double ratio=(k%73<21)?0:double((k*17)%997)/997;internal[k]=ratio;YouKnowTestAccess::wet(*e,ratio);float l=.2f,r=-.4f;YouKnowTestAccess::summer(*e,l,r);const double expected=k>=local?internal[k-local]:0;auto c=YouKnowTestAccess::context(*e);require(c.wetRatio==expected&&c.wetConnected==(expected!=0),"internal conductance timestamp mismatch");require(YouKnowTestAccess::poleRatio(*e)==expected,"physical pole used future wet conductance");}
   host[frame]=YouKnowTestAccess::context(*e).wetRatio;const auto c=YouKnowTestAccess::hostContext(*e);const double a=frame>=whole?host[frame-whole]:0,b=frame>whole?host[frame-whole-1]:0;require(std::abs(c.wetRatio-(a+frac*(b-a)))<1e-15,"host resistor POWER used future wet conductance");require(c.wetConnected==((frac<.5?a:b)!=0),"nominal binary host context timestamp mismatch");
  }
 }
 // The diagnostic raw path neither reconstructs nor advances numerical state.
 auto raw=prepared(48000,false);for(int i=0;i<500;++i) {float l=.1f,r=.2f;YouKnowTestAccess::summer(*raw,l,r);}require(YouKnowTestAccess::write(*raw)==0,"disabled AA incurred history work");
}
void latencyAndLifecycle() {
 for(double fs:{8000.,44100.,48000.,96000.,176400.,192000.}) for(int factor:{1,2,4}) for(int selectors=0;selectors<4;++selectors) {
  auto e=prepared(fs,(selectors&2)!=0,factor);auto p=YouKnowTestAccess::parameters(*e);p.enableVoiceVcaAntialias=(selectors&1)!=0;e->setParameters(p);
  const int expected=selectors==0?41:(fs>=176400?41:selectors==3?120:72);require(e->getProcessingLatencySamples()==expected,"reported selector/rate latency mismatch");
  const double centre=YouKnowTestAccess::impulseCentre(*e);require(std::abs(centre-expected)<=.50001,"actual FIR/decimator/pad impulse disagrees with latency");require(YouKnowTestAccess::padding(*e)<128,"padding ring too small");
  e->resetForHostStop();require(YouKnowTestAccess::empty(*e),"warm host stop retained numerical history");
  float l=1,r=-1;YouKnowTestAccess::summer(*e,l,r);p.enableOutputSummerAntialias=!p.enableOutputSummerAntialias;e->setParameters(p);require(YouKnowTestAccess::empty(*e),"diagnostic latency change retained FIR/pad history");
  float pl=.3f,pr=-.5f;YouKnowTestAccess::summer(*e,pl,pr);const auto physical=YouKnowTestAccess::physical(*e);const int oldFactor=e->getOversamplingFactor(),oldWrite=YouKnowTestAccess::write(*e);
  require(YouKnowTestAccess::qualifiedRebuild(*e,factor==1?4:1),"qualified rate rebuild failed");if(e->getOversamplingFactor()!=oldFactor)require(YouKnowTestAccess::empty(*e),"applied quality rebuild retained wrong-grid history");else require(YouKnowTestAccess::write(*e)==oldWrite,"clamped no-op quality request erased numerical state");require(YouKnowTestAccess::physical(*e)==physical,"quality rebuild erased physical IC6 state");
  e->reset();require(YouKnowTestAccess::empty(*e),"hard reset retained numerical history");std::array<float,128> silentL{},silentR{};e->process(silentL.data(),silentR.data(),128);require(std::all_of(silentL.begin(),silentL.end(),[](float x){return x==0;}),"idle reset emitted residual LINE signal");
 }
}
struct Render {std::vector<float> l,r;double preclip{};};
Render render(bool corrected,int block,bool noise=false) {
 auto e=prepared(48000,corrected,1,true);auto p=YouKnowTestAccess::parameters(*e);p.chorus=ChorusMode::One;p.enableOpAmpSlewLimiting=true;p.enableOutputSummerMuteLoading=true;if(noise)p.calibration=.8f;e->setParameters(p);
 for(int n:{48,55,60,64,67,72})e->noteOn(n,127);
 Render out{std::vector<float>(24000),std::vector<float>(24000),0};
 for(int start=0;start<24000;) {if(start==12000){p.chorus=ChorusMode::Off;e->setParameters(p);}int length=std::min({block,24000-start,start<12000?12000-start:24000-start});guardAllocation=true;e->process(out.l.data()+start,out.r.data()+start,length);guardAllocation=false;out.preclip=std::max(out.preclip,std::abs(double(YouKnowTestAccess::preclip(*e)))*2.6);start+=length;}
 require(allocations==0,"audio callback allocated");return out;
}
void engineWitnessAndChronology() {
 for(bool enabled:{false,true}) {auto a=render(enabled,1,true),b=render(enabled,173,true);require(a.l==b.l&&a.r==b.r,"IC6 processing depends on host blocks");require(a.l!=a.r,"independent L/R chorus lost stereo");}
 auto a=prepared(48000,false,1,true),b=prepared(48000,true,1,true);auto p=YouKnowTestAccess::parameters(*a);p.calibration=.8f;p.chorus=ChorusMode::Two;a->setParameters(p);p.enableOutputSummerAntialias=true;b->setParameters(p);for(int n:{48,55,60,64,67,72}){a->noteOn(n,127);b->noteOn(n,127);}std::array<float,128> l{},r{};for(int i=0;i<100;++i){a->process(l.data(),r.data(),128);b->process(l.data(),r.data(),128);require(YouKnowTestAccess::random(*a)==YouKnowTestAccess::random(*b),"AA changed physical RNG chronology");}
 auto before=render(false,1),after=render(true,1);double delta=0,energy=0;for(int n=1000;n<23952;++n){double x=before.l[n],y=after.l[n+48];delta+=(x-y)*(x-y);energy+=x*x;require(std::isfinite(y),"actual LINE output became nonfinite");}
 std::cout<<"hot LINE+Boost IC6 peak="<<after.preclip<<"Vp aligned residual="<<10*std::log10(delta/energy)<<"dBc (whole-engine diagnostic)\n";
 require(after.preclip>4,"actual hot LINE fixture did not exercise IC6 signal");
}

void stereoSpecializationParity() {
 // Independent general-channel routines are the frozen arithmetic oracle;
 // context uses a separate event ledger rather than either stereo method.
 const auto sameFloat=[](float a,float b) {return std::bit_cast<std::uint32_t>(a)==std::bit_cast<std::uint32_t>(b);};
 const auto sameContext=[](OutputSummerAntialias::Context a,OutputSummerAntialias::Context b) {
  return std::bit_cast<std::uint64_t>(a.wetRatio)==std::bit_cast<std::uint64_t>(b.wetRatio) && a.wetConnected==b.wetConnected;
 };
 const auto sameHistory=[&](const VoiceVcaAntialias& a,const VoiceVcaAntialias& b) {
  return a.inputWrite==b.inputWrite && a.outputWrite==b.outputWrite
   && std::equal(a.inputs.begin(),a.inputs.end(),b.inputs.begin(),sameFloat)
   && std::equal(a.gains.begin(),a.gains.end(),b.gains.begin(),sameFloat)
   && std::equal(a.temperatures.begin(),a.temperatures.end(),b.temperatures.begin(),sameFloat)
   && std::equal(a.outputs.begin(),a.outputs.end(),b.outputs.begin(),sameFloat);
 };
 const auto clip=[](float x) {return YouKnowTestAccess::clip(x);};
 std::uint32_t random=0x539a2d71;
 const auto next=[&]() {random^=random<<13;random^=random>>17;random^=random<<5;return random;};
 unsigned compared=0;
 for(double rate:{44100.,48000.,96000.,192000.}) {
  OutputSummerAntialias candidate;
  VoiceVcaAntialias oracleLeft,oracleRight;
  std::array<OutputSummerAntialias::Context,VoiceVcaAntialias::inputRingSize> ledger{};
  int ledgerWrite=0;
  auto kernel=VoiceVcaAntialias::prepare(rate);
  for(int frame=0;frame<4096;++frame) {
   if(frame==513 || frame==1769) {candidate.reset();oracleLeft.reset();oracleRight.reset();ledger={};ledgerWrite=0;}
   // Deliberately retain histories through direct/2x/4x kernel changes as
   // well as testing the engine's usual reset boundary above.
   if(frame==2057) kernel=VoiceVcaAntialias::prepare(rate==96000.?48000.:rate==192000.?96000.:192000.);
   if(frame==3001) kernel=VoiceVcaAntialias::prepare(rate);
   if(frame==1537 && kernel.factor>1) {
    // Public independent histories must keep working via the fallback.
    const float a=candidate.right.process(.2345f,1,kernel,clip);
    const float b=oracleRight.process(.2345f,1,kernel,clip);
    require(sameFloat(a,b),"independent IC6 history setup differs");
   }
   float l=static_cast<float>((double(next())/4294967295.-.5)*40);
   float r=static_cast<float>((double(next())/4294967295.-.5)*27);
   if(frame%193==0) l=0.f;
   if(frame%197==0) r=-0.f;
   if(frame<96) {l=frame==0?17.f:0.f;r=frame==7?-19.f:0.f;}
   const double ratio=frame%73<21?0:double(next())/4294967295.;
   const OutputSummerAntialias::Context context{ratio,ratio!=0};
   ledger[static_cast<std::size_t>(ledgerWrite)]=context;
   const int delay=kernel.factor>1?VoiceVcaAntialias::delaySamples:0;
   const auto expectedContext=ledger[static_cast<std::size_t>((ledgerWrite-delay+64)&63)];
   ledgerWrite=(ledgerWrite+1)&63;
   const float expectedLeft=oracleLeft.process(l,1,kernel,clip);
   const float expectedRight=oracleRight.process(r,1,kernel,clip);
   guardAllocation=true;
   const auto result=candidate.processStereo(l,r,context,kernel,clip);
   guardAllocation=false;
   require(sameFloat(result.left,expectedLeft) && sameFloat(result.right,expectedRight),"stereo IC6 FIR changed channel arithmetic bits");
   require(sameContext(result.context,expectedContext) && candidate.write==ledgerWrite,"stereo IC6 FIR changed context chronology");
   if(frame%61==0 || frame==4095) {
    require(sameHistory(candidate.left,oracleLeft) && sameHistory(candidate.right,oracleRight),"stereo IC6 FIR changed complete channel histories");
    require(std::equal(candidate.contexts.begin(),candidate.contexts.end(),ledger.begin(),sameContext),"stereo IC6 FIR changed context history");
   }
   ++compared;
  }
 }
 // A stateful shape proves that vectorizing reconstruction does not
 // interleave L/R callback chronology or skip startup shape evaluations.
 for(double rate:{48000.,96000.,192000.}) {
  OutputSummerAntialias candidate;
  VoiceVcaAntialias oracleLeft,oracleRight;
  const auto kernel=VoiceVcaAntialias::prepare(rate);
  unsigned calls=0,expectedCalls=0;
  const auto shape=[&](float x) {return clip(x)+float(calls++%17)*.03125f;};
  const auto expectedShape=[&](float x) {return clip(x)+float(expectedCalls++%17)*.03125f;};
  for(int frame=0;frame<512;++frame) {
   const float l=float(std::sin(frame*.019)*8),r=float(std::cos(frame*.073)*11);
   const float a=oracleLeft.process(l,1,kernel,expectedShape),b=oracleRight.process(r,1,kernel,expectedShape);
   const auto result=candidate.processStereo(l,r,{},kernel,shape);
   require(sameFloat(result.left,a) && sameFloat(result.right,b) && calls==expectedCalls,"stereo IC6 FIR changed stateful shape order");
  }
 }
 require(allocations==0,"stereo IC6 FIR allocated");
 std::cout<<"stereo IC6 specialization bit parity="<<compared<<" random/hot/startup/reset/rate frames\n";
}

}
int main(){try{stereoSpecializationParity();analogueHarmonicsAndAliases();moderatePassband();contextChronology();latencyAndLifecycle();engineWitnessAndChronology();std::cout<<"Output summer antialias tests passed; sizeof Engine="<<sizeof(YouKnowEngine)<<" bytes\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
