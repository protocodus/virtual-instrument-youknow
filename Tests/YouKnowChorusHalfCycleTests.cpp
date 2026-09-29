// Panasonic MN3009, printed p.43 (PDF p.2), labels C0..C258 and the
// complementary switches. The alternating useful OUT1/OUT2 drive reads
// C256/C257; the following transport oracle tests timing, not their impedance.
// https://www.experimentalistsanonymous.com/diy/Datasheets/MN3009.pdf#page=2
// This explicit capacitor transport is independent of the runtime's ring.
// Its tagged aperture also agrees with Holters/Parker 2018, Eq.1:
// https://www.dafx.de/paper-archive/2018/papers/DAFx2018_paper_12.pdf#page=2
#include "DSP/YouKnowChorus.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

namespace { std::atomic<bool> counting{false}; std::atomic<unsigned> allocations{0}; }
void* operator new(std::size_t n) { if(counting)++allocations; if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
namespace youknow {
struct YouKnowTestAccess {
 struct State {int index;double phase;std::uint32_t rng;float held,transfer;};
 static State state(const Chorus& c) {const auto& l=c.lineA_;return {l.writeIndex,l.clockPhase,l.noiseState,l.held,l.transferState};}
 static auto cells(const Chorus& c) {return c.lineA_.cells;}
 static float process(Chorus& c,float input,double clock,float noiseScale=1) {return c.lineA_.processClockedCore(input,clock,c.sampleRate_,noiseScale);}
 static void inputHistory(Chorus& c,float a,float b,float d) {c.lineA_.previousInput=a;c.lineA_.previousInput2=b;c.lineA_.previousInput3=d;}
}; }
namespace {
using youknow::Chorus;
using Probe=youknow::YouKnowTestAccess;
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
std::uint32_t next(std::uint32_t x) {x^=x<<13;x^=x>>17;x^=x<<5;return x;}
float noise(std::uint32_t x) {return static_cast<float>(x&0xffffffu)*(2.0f/16777215.0f)-1.0f;}
float bounded(float value) {
 const double v=std::abs(double(value))/double(1.1246614f);
 return float(value/std::pow(1+double(1.2044546f)*v*v+std::pow(v,double(12.9395323f)),1/double(12.9395323f)));
}
struct Capacitors {
 std::array<float,259> c{};
 void input(float value) {
  c[0]=value;
  // Simultaneous CP1 transfers: descend so no newly written stage is read.
  for(int i=257;i>=1;i-=2) {c[i]=c[i-1];c[i-1]=0;}
 }
 float output() {
  // CP2 transfers the even stages; Q258 returns the spent packet to VDD.
  for(int i=258;i>=2;i-=2) {c[i]=c[i-1];c[i-1]=0;}
  c[258]=0;
  return c[256]+c[257];
 }
 float composite() const {return c[256]+c[257];}
};
void taggedStages() {
 Capacitors physical;physical.input(1);
 for(int half=1;half<=258;++half) {
  if(half&1)physical.output();else physical.input(0);
  const float expected=(half==255||half==256)?1.0f:0.0f;
  require(physical.composite()==expected,"259-node tagged packet has wrong output aperture");
  if(half==255)require(physical.c[256]==1&&physical.c[257]==0,"first follower is not C256");
  if(half==256)require(physical.c[256]==0&&physical.c[257]==1,"second follower is not C257");
 }
 // The first real input is at phase 1 (phase 0 is the implicit reset zero).
 auto chorus=std::make_unique<Chorus>();chorus->prepare(96000);
 Capacitors oracle;float transfer=0;
 for(int frame=1;frame<=530;++frame) {
  const float input=frame==4?.1f:0;
  Probe::process(*chorus,input,24000,0);
  if(frame%4==0)oracle.input(bounded(input));
  if(frame%4==2)transfer+=.8654743f*(oracle.output()-transfer);
  const auto actual=Probe::state(*chorus);
  require(std::abs(actual.transfer-transfer)<2e-8f,"runtime disagrees with explicit tagged stages");
  if(frame<514)require(actual.transfer==0,"runtime tag appeared before phase128.5");
  if(frame==514)require(actual.transfer>.08f,"runtime tag missed phase128.5");
 }
 std::cout<<"Tagged packet: first output127.5 periods after input, hold center128 periods\n";
}
struct Event {double sample;float value;};
struct Reference {
 Capacitors physical;
 std::uint64_t half{1},inputs{},outputs{};
 std::uint32_t rng{0x9e3779b9u};
 float transfer{},held{};
 void event(float input,float noiseScale) {
  if(half&1) {
   transfer+=.8654743f*(physical.output()-transfer);
   rng=next(rng);held=transfer+noise(rng)*Chorus::independentLineRandomAmplitude*noiseScale;++outputs;
  } else {physical.input(bounded(input));++inputs;}
  ++half;
 }
};
void ledgersAndStop() {
 bool stoppedBefore=false,stoppedAfter=false,stoppedAtHalf=false;
 // Non-binary ratios avoid exact endpoint coincidences; the tagged and16k
 // rows separately exercise exact half boundaries without floating ambiguity.
 for(int stopFrame:{650,651})
 for(const auto [rate,clock]:std::array<std::array<double,2>,6>{{{8000,200000},{16000,200000},{44100,37001},{48000,79999},{192000,37001},{768000,199999}}}) {
  auto chorus=std::make_unique<Chorus>();chorus->prepare(rate);Reference ref;
  // Constant input makes the sampled charge independent of interpolation.
  constexpr float input=.125f;Probe::inputHistory(*chorus,input,input,input);
  double total=0;allocations=0;counting=true;
  for(int frame=0;frame<1600;++frame) {
   const bool stop=frame>=stopFrame&&frame<stopFrame+80;
   const auto before=Probe::state(*chorus);const auto buckets=Probe::cells(*chorus);
   const double step=stop?0:clock/rate;total+=step;
   while(.5*double(ref.half)<=total)ref.event(input,1);
   require(std::isfinite(Probe::process(*chorus,input,stop?0:clock)),"half-cycle core is nonfinite");
   const auto actual=Probe::state(*chorus);
   if(actual.index!=int(ref.inputs%128)||actual.rng!=ref.rng)std::cerr<<"ledger rate "<<rate<<" clock "<<clock<<" frame "<<frame<<" total "<<total<<" actual index "<<actual.index<<" expected "<<ref.inputs%128<<" phase "<<actual.phase<<" outputs "<<ref.outputs<<"\n";
   require(actual.index==int(ref.inputs%128)&&actual.rng==ref.rng,"input/output/RNG event ledgers disagree");
   require(std::abs(actual.phase-(total-std::floor(total)))<2e-9,"full-period phase changed");
   require(std::abs(actual.held-ref.held)<3e-8f,"output hold differs from physical capacitor transport");
   if(frame==stopFrame) {stoppedBefore|=actual.phase<.5;stoppedAfter|=actual.phase>.5;stoppedAtHalf|=actual.phase==.5;}
   if(stop)require(actual.phase==before.phase&&actual.rng==before.rng&&actual.index==before.index&&actual.held==before.held&&buckets==Probe::cells(*chorus),"clock stop changes physical charge or RNG");
  }
  counting=false;require(allocations==0,"half-cycle processing allocates");
 }
 require(stoppedBefore&&stoppedAfter&&stoppedAtHalf,"stop/restart omitted a half-phase region");
}
// Independent convolution with the four-point cubic-Lagrange reconstruction
// kernel. Gauss-Legendre order3 integrates each cubic piece exactly. This is
// neither production's BLEP polynomial nor its future ring cursor.
double kernel(double x) {
 x=std::abs(x);
 if(x<1)return (x+1)*(x-1)*(x-2)/2;
 if(x<2)return -(x-1)*(x-2)*(x-3)/6;
 return 0;
}
double convolve(double sample,const std::vector<Event>& events) {
 const double lo=sample-2,hi=sample+2;
 std::vector<double> knots{lo,sample-1,sample,sample+1,hi};
 for(const auto& event:events)if(event.sample>lo&&event.sample<hi)knots.push_back(event.sample);
 std::sort(knots.begin(),knots.end());
 double sum=0;std::size_t eventIndex=0;float held=0;
 for(std::size_t i=1;i<knots.size();++i) {
  const double a=knots[i-1],b=knots[i];
  while(eventIndex<events.size()&&events[eventIndex].sample<=a+1e-11)held=events[eventIndex++].value;
  const double midpoint=.5*(a+b),half=.5*(b-a),q=std::sqrt(3./5.);
  sum+=held*half*(5./9.*kernel(midpoint-half*q-sample)+8./9.*kernel(midpoint-sample)+5./9.*kernel(midpoint+half*q-sample));
 }
 return sum;
}
void independentLookahead() {
 std::array<double,3> worst{};
 for(int scene=0;scene<3;++scene)
 for(const auto [rate,clock]:std::array<std::array<double,2>,5>{{{8000,200000},{44100,80001},{48000,37001},{192000,37001},{768000,199999}}}) {
  constexpr int frames=1024;const double increment=clock/rate;
  const float noiseScale=scene==1?0.0f:1.0f;
  // A nonconstant degree-one signal is exactly within the input interpolator's
  // polynomial space. It fills the transport before the comparison ends, so
  // using the wrong future ring cursor cannot hide behind empty/same buckets.
  const auto signal=[&](double sample) {return scene==0?0.0f:float(.005+.00004*sample);};
  std::vector<Event> events;Reference ref;
  while(.5*double(ref.half)/increment<frames+3) {
   const double time=.5*double(ref.half)/increment;const bool output=(ref.half&1)!=0;
   ref.event(signal(time),noiseScale);if(output)events.push_back({time,ref.held});
  }
  auto chorus=std::make_unique<Chorus>();chorus->prepare(rate);
  Probe::inputHistory(*chorus,signal(0),signal(-1),signal(-2));
  // The zero-signal row isolates rounded random jumps. Signal rows additionally
  // allow float input samples/interpolation, table approximation and arithmetic:
  // 16 ulps at the largest signal is conservative versus these few operations,
  // while a one-bucket cursor error is >=1.6e-6 even at the fastest clock ratio.
  const double tolerance=(4+2*Chorus::maximumBlepEvents)*std::numeric_limits<float>::epsilon()*Chorus::independentLineRandomAmplitude
     +(scene?16*std::numeric_limits<float>::epsilon()*signal(frames+3):0);
  std::uint32_t liveRng=0x9e3779b9u;std::uint64_t liveDraws=0;
  for(int frame=1;frame<=frames;++frame) {
   const auto actual=Probe::process(*chorus,signal(frame),clock,noiseScale);
   const double expected=convolve(frame,events),error=std::abs(actual-expected);worst[scene]=std::max(worst[scene],error);
   require(error<tolerance,"half-phase BLEP past/future reconstruction differs from independent integral");
   const auto draws=std::uint64_t(std::floor(frame*increment+.5));
   while(liveDraws<draws) {liveRng=next(liveRng);++liveDraws;}
   require(Probe::state(*chorus).rng==liveRng,"BLEP lookahead consumed live random state");
  }
 }
 std::cout<<"Independent held convolution max errors (noise, signal, mixed): "<<worst[0]<<", "<<worst[1]<<", "<<worst[2]<<" model units\n";
}
}
int main() {
 try {taggedStages();ledgersAndStop();independentLookahead();std::cout<<"Chorus half-cycle checks passed\n";return 0;}
 catch(const std::exception& e) {counting=false;std::cerr<<e.what()<<'\n';return 1;}
}
