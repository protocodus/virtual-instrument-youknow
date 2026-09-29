// Clock quadrature is checked against the continuous affine-delay integral,
// independently of the production midpoint rule. The signal oracle resolves
// each physical staircase edge and integrates the analog reconstruction KCL.
// The physical transport explicitly advances Panasonic C0..C258 on alternating
// phases; input captures and composite-output changes are a half-cycle apart.
#include "DSP/YouKnowChorus.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace { std::atomic<bool> countAllocations {false}; std::atomic<unsigned> allocations {0}; }
void* operator new(std::size_t n) { if(countAllocations) ++allocations; if(void* p=std::malloc(n?n:1))return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }

namespace youknow {
struct YouKnowTestAccess {
 struct State { int index; double phase,lfo; std::uint32_t rng; float held,transfer; bool stopped; };
 static State state(const Chorus& c,bool right=false) { const auto& l=right?c.lineB_:c.lineA_;return {l.writeIndex,l.clockPhase,c.lfoPhase_,l.noiseState,l.held,l.transferState,c.clocksStopped_}; }
 static void phase(Chorus& c,double value) { c.lfoPhase_=value; }
 static auto cells(const Chorus& c) {return c.lineA_.cells;}
 static float core(Chorus& c,float input,double clock) {return c.lineA_.processClockedCore(input,clock,c.sampleRate_,1.0f);}
 static double wet(const Chorus& c,bool right=false) {const auto& l=right?c.lineB_:c.lineA_;return l.exactOutputState[4]-l.exactOutputState[5];}
}; }
namespace {
using namespace youknow;
using Probe=YouKnowTestAccess;
constexpr double pi=std::numbers::pi_v<double>;
void require(bool b,const char* message) {if(!b)throw std::runtime_error(message);}
std::uint32_t next(std::uint32_t x) {x^=x<<13;x^=x>>17;x^=x<<5;return x;}
float noise(std::uint32_t x) {return static_cast<float>(x&0xffffffu)*(2.0f/16777215.0f)-1.0f;}
double tri(double p) {p-=std::floor(p);return 4*std::min(p,1-p)-1;}
struct Integral {double cycles{},errorBound{};};
Integral integrateClock(double phase,double rate,double dt,double centre,double sweep,int side) {
 Integral result;
 double elapsed=0;
 while(elapsed<dt) {
  const double p=phase+rate*elapsed,wrapped=p-std::floor(p);
  const bool rising=wrapped<.5;
  const double boundary=std::floor(p)+(rising?.5:1.);
  const double end=rate>0?std::min(dt,elapsed+(boundary-p)/rate):dt;
  const double step=end-elapsed;
  if(!(step>0)) {elapsed=std::nextafter(elapsed,dt);continue;}
  const double d0=centre+side*sweep*tri(p);
  const double slope=side*sweep*(rising?4:-4)*rate;
  result.cycles+=std::abs(slope)<1e-20?128*step/d0:128/slope*std::log1p(slope*step/d0);
  const double minimum=std::min(d0,d0+slope*step);
  // Composite midpoint error <= max|f''| * dt^3/24, f=128/d.
  result.errorBound+=128*slope*slope*step*step*step/(12*minimum*minimum*minimum);
  elapsed=end;
 }
 return result;
}
void process(Chorus& c,float input,ChorusMode mode,ChorusTimingProfile timing,bool stop=false) {
 float l,r;c.process(input,mode,0,l,r,false,false,1,false,true,stop,false,timing,stop);
 require(std::isfinite(l)&&std::isfinite(r),"clock process produced nonfinite output");
}
void phaseOracle() {
 double worst=0;
 for(double rate:{8000.,44100.,48000.,192000.,768000.})
  for(double initial:{.4998,.9998}) {
   auto c=std::make_unique<Chorus>();c->prepare(rate);Probe::phase(*c,initial);
   std::array<double,2> cycles{},bounds{};std::array<std::uint64_t,2> edges{};
   auto settings=Chorus::settingsFor(ChorusMode::One,ChorusTimingProfile::OwnerBlend);
   const int frames=static_cast<int>(rate*.13);
   allocations=0;countAllocations=true;
   for(int n=0;n<frames;++n) {
    const auto mode=n<frames*2/5?ChorusMode::One:n<frames*3/5?ChorusMode::Off:ChorusMode::Two;
    if(mode!=ChorusMode::Off)settings=Chorus::settingsFor(mode,ChorusTimingProfile::OwnerBlend);
    const auto before=std::array{Probe::state(*c),Probe::state(*c,true)};
    process(*c,0,mode,ChorusTimingProfile::OwnerBlend);
    for(int side=0;side<2;++side) {
     const auto after=Probe::state(*c,side!=0);
     const auto reference=integrateClock(before[side].lfo,settings.rateHz,1/rate,settings.centreDelaySeconds,settings.sweepSeconds,side?-1:1);
     cycles[side]+=reference.cycles;bounds[side]+=reference.errorBound;
     const auto shifts=(after.index-before[side].index+128)%128;edges[side]+=shifts;
     const int outputs=shifts+int(after.phase>=.5)-int(before[side].phase>=.5);
     auto rng=before[side].rng;for(int i=0;i<outputs;++i)rng=next(rng);
     require(after.rng==rng,"moving clocks did not draw exactly once per output half-cycle");
     const double error=std::abs(double(edges[side])+after.phase-cycles[side]);worst=std::max(worst,error);
     // Floating reductions of <=100k cycles add far less than this term;
     // the dt^2 term is the analytical quadrature bound, not a golden fit.
     require(error<1.05*bounds[side]+2e-8,"clock phase exceeds continuous midpoint error bound");
    }
   }
   countAllocations=false;require(allocations==0,"moving-clock audio path allocated");
  }
 std::cout<<"Continuous clock maximum phase error "<<worst<<" cycles\n";
}
void rateAndStopContinuity() {
 auto c=std::make_unique<Chorus>();c->prepareSupportRates(48000);c->prepare(48000);
 Probe::phase(*c,.4997);
 for(double rate:{48000.,192000.,96000.,48000.}) {
  const auto before=Probe::state(*c);const auto cells=Probe::cells(*c);
  c->prepare(rate,true);const auto after=Probe::state(*c);
  require(before.index==after.index&&before.phase==after.phase&&before.rng==after.rng&&before.lfo==after.lfo&&cells==Probe::cells(*c),"rate change reset physical clock or buckets");
  const auto settings=Chorus::settingsFor(ChorusMode::Two,ChorusTimingProfile::OwnerBlend);
  double actual=after.phase,exact=after.phase,bound=0;
  for(int n=0;n<int(rate*.012);++n) {
   const auto old=Probe::state(*c);process(*c,0,ChorusMode::Two,ChorusTimingProfile::OwnerBlend);
   const auto now=Probe::state(*c);
   actual+=(now.index-old.index+128)%128+now.phase-old.phase;
   const auto integral=integrateClock(old.lfo,settings.rateHz,1/rate,settings.centreDelaySeconds,settings.sweepSeconds,1);
   exact+=integral.cycles;bound+=integral.errorBound;
  }
  require(std::abs(actual-exact)<1.05*bound+2e-8,"rate boundary changed physical clock integral");
 }
 c->prepare(48000);process(*c,0,ChorusMode::Off,ChorusTimingProfile::Shipping,true);
 require(Probe::state(*c).stopped,"settled Off did not stop physical clocks");
 bool restarted=false,stoppedAgain=false;
 for(int n=0;n<28000;++n) {
  const auto old=Probe::state(*c);const auto cells=Probe::cells(*c);
  const auto mode=n<1000?ChorusMode::Off:n<6000?ChorusMode::One:ChorusMode::Off;
  process(*c,.01f,mode,ChorusTimingProfile::Shipping,true);const auto now=Probe::state(*c);
  if(now.stopped)require(now.phase==old.phase&&now.index==old.index&&now.rng==old.rng&&cells==Probe::cells(*c),"stopped clock changed a physical bucket/phase/RNG");
  require(now.lfo!=old.lfo,"clock stop stopped the free-running LFO");
  restarted|=!now.stopped;stoppedAgain|=restarted&&now.stopped;
 }
 require(restarted&&stoppedAgain,"clock stop/restart scene missed its transitions");
}
void constantClockLedger() {
 for(const auto [rate,clock]:std::array<std::array<double,2>,5>{{{8000,200000},{44100,37001},{48000,80000},{192000,37001},{768000,200000}}}) {
  auto c=std::make_unique<Chorus>();c->prepare(rate);
  double phase=0;int index=0;std::uint32_t rng=0x9e3779b9u;
  for(int n=0;n<2048;++n) {
   const double end=phase+clock/rate;
   const int outputs=int(std::floor(end+.5))-int(phase>=.5);
   for(int i=0;i<outputs;++i)rng=next(rng);
   index=(index+int(std::floor(end)))%128;phase=end-std::floor(end);
   const auto value=Probe::core(*c,0,clock);const auto state=Probe::state(*c);
   require(std::isfinite(value)&&state.phase==phase&&state.index==index&&state.rng==rng,"constant-clock ledger changed");
   if(rng!=0x9e3779b9u)require(state.held==noise(rng)*Chorus::independentLineRandomAmplitude,"noise source amplitude or RNG draw changed");
  }
 }
}
// Independent analytic input network from component KCL (ideal follower
// profile), with its unbuffered coupling/shunt interaction retained.
std::complex<double> inputResponse(double f) {
 const std::complex<double> s(0,2*pi*f);
 const auto sk=[&](double fb,double sh) {return 1.0/(1.0+2*22000*sh*s+22000.*22000*fb*sh*s*s);};
 return sk(820e-12,680e-12)*sk(1.8e-9,270e-12)
  *(s*100000.0*.1e-6)/(1.0+s*(100000*.1e-6+10000*2.2e-9+100000*2.2e-9)+s*s*100000.0*10000.0*.1e-6*2.2e-9);
}
double saturate(double x) {
 const double v=std::abs(x)/double(1.1246614f);
 return x/std::pow(1+double(1.2044546f)*v*v+std::pow(v,double(12.9395323f)),1/double(12.9395323f));
}
struct Event {double time,value;};
std::vector<Event> exactEvents(double duration,Chorus::ModeSettings settings,int side,double frequency,double amplitude) {
 std::vector<Event> result;result.reserve(120000);
 std::array<double,259> capacitor{};bool outputEdge=true;
 double loss=0,time=0,phase=0,remaining=.5;
 const auto input=inputResponse(frequency);
 while(time<duration) {
  const double wrapped=phase-std::floor(phase);const bool rising=wrapped<.5;
  const double end=std::min(duration,time+((rising?.5:1)-wrapped)/settings.rateHz);
  if(!(end>time)) {time=std::nextafter(time,duration);phase+=settings.rateHz*(time-std::nextafter(time,0.));continue;}
  double local=0;
  const double startDelay=settings.centreDelaySeconds+side*settings.sweepSeconds*tri(phase);
  const double slope=side*settings.sweepSeconds*(rising?4:-4)*settings.rateHz;
  while(local<end-time) {
   const double delay=startDelay+slope*local,available=end-time-local;
   const double count=128/slope*std::log1p(slope*available/delay);
   if(count<remaining) {remaining-=count;local=end-time;break;}
   const double step=delay/slope*std::expm1(remaining*slope/128);
   local+=step;remaining=.5;
   const double t=time+local;
   if(outputEdge) {
    for(int node=258;node>=2;node-=2) {capacitor[node]=capacitor[node-1];capacitor[node-1]=0;}
    capacitor[258]=0;
    const double emerging=capacitor[256]+capacitor[257];
    loss+=double(.8654743f)*(emerging-loss);result.push_back({t,loss});
   } else {
    capacitor[0]=saturate(amplitude*std::imag(input*std::polar(1.,2*pi*frequency*t)));
    for(int node=257;node>=1;node-=2) {capacitor[node]=capacitor[node-1];capacitor[node-1]=0;}
   }
   outputEdge=!outputEdge;
  }
  phase+=settings.rateHz*(end-time);time=end;
 }
 return result;
}
using State=std::array<double,6>;
State derivative(const State& x,double held) {
 constexpr double g=1/22000.,drive=1/3500.+1/47000.;
 constexpr double a=820e-12,b=680e-12,c=1.8e-9,d=270e-12;
 constexpr double corner=(1/22000.+1/39000.)/1e-6;
 return {(drive*held-(drive+g)*x[0]+g*x[1])/2.2e-9,
   g*x[0]/a+(g/b-2*g/a)*x[1]+(-g/b+g/a)*x[2],g*(x[1]-x[2])/b,
   g*x[2]/c+(g/d-2*g/c)*x[3]+(-g/d+g/c)*x[4],g*(x[3]-x[4])/d,
   corner*(x[4]-x[5])};
}
void rk4(State& x,double held,double dt) {
 const auto a=derivative(x,held);State work{};
 for(int k=0;k<6;++k)work[k]=x[k]+dt*.5*a[k];const auto b=derivative(work,held);
 for(int k=0;k<6;++k)work[k]=x[k]+dt*.5*b[k];const auto c=derivative(work,held);
 for(int k=0;k<6;++k)work[k]=x[k]+dt*c[k];const auto d=derivative(work,held);
 for(int k=0;k<6;++k)x[k]+=dt/6*(a[k]+2*b[k]+2*c[k]+d[k]);
}
struct OutputOracle {
 const std::vector<Event>& events;State state{};std::size_t index{};double time{},held{};
 double advance(double target,double maximumStep) {
  while(time<target) {
   const double edge=index<events.size()?events[index].time:std::numeric_limits<double>::infinity();
   const double end=std::min(target,edge),length=end-time;
   if(length>0) {const int steps=std::max(1,int(std::ceil(length/maximumStep)));for(int i=0;i<steps;++i)rk4(state,held,length/steps);time=end;}
   if(edge<=time&&index<events.size())held=events[index++].value;
   else if(!(length>0))break;
  }
  return state[4]-state[5];
 }
};
void movingHighTone() {
 constexpr double duration=.72,frequency=15000,amplitude=.03;
 const auto mode=ChorusMode::Two;
 const auto timing=ChorusTimingProfile::OwnerBlend;
 const auto settings=Chorus::settingsFor(mode,timing);
 for(int side:{1,-1}) {
  const auto events=exactEvents(duration,settings,side,frequency,amplitude);
  for(double rate:{192000.,384000.}) {
   auto c=std::make_unique<Chorus>();c->prepare(rate);
   OutputOracle oracle{events},refined{events};double error=0,energy=0,convergence=0,edgeError=0,edgeEnergy=0;std::size_t edgeOrdinal=0;
   for(int n=1;n<=int(duration*rate);++n) {
    const auto before=Probe::state(*c,side<0);const double time=n/rate;
    process(*c,float(amplitude*std::sin(2*pi*frequency*time)),mode,timing);
    const auto after=Probe::state(*c,side<0);
    const int outputEdges=(after.index-before.index+128)%128+int(after.phase>=.5)-int(before.phase>=.5);
    edgeOrdinal+=outputEdges;
    const double reference=oracle.advance(time,1/(rate*4));
    const double fine=refined.advance(time,1/(rate*8));
    if(time<.25)continue;
    const double actual=Probe::wet(*c,side<0);
    error+=(actual-fine)*(actual-fine);energy+=fine*fine;convergence+=(reference-fine)*(reference-fine);
    if(outputEdges>0&&edgeOrdinal>0&&edgeOrdinal<=events.size()) {
     const double expected=events[edgeOrdinal-1].value;
     edgeError+=(after.held-expected)*(after.held-expected);edgeEnergy+=expected*expected;
    }
   }
   const double nrms=std::sqrt(error/energy),conv=std::sqrt(convergence/energy),er=std::sqrt(edgeError/edgeEnergy);
   std::cout<<"Moving15k line "<<(side>0?'A':'B')<<" at "<<rate<<" Hz NRMS "<<20*std::log10(nrms)<<" dB, oracle refinement "<<20*std::log10(conv)<<" dB, captured-edge error "<<20*std::log10(er)<<" dB\n";
   require(conv<1e-4,"high-tone continuous output oracle failed refinement");
   require(nrms<.01,"moving high-tone wet residual exceeds existing -40dB physical-oracle gate");
   require(er<.005,"moving high-tone BBD edge samples deviate more than0.5percent");
  }
 }
}
}
int main() {
 try {phaseOracle();rateAndStopContinuity();constantClockLedger();movingHighTone();std::cout<<"Chorus clock integration checks passed\n";return 0;}
 catch(const std::exception& e) {countAllocations=false;std::cerr<<e.what()<<'\n';return 1;}
}
