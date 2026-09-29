// Numerical qualification of fractional BBD output-hold recovery. The
// physical-node oracle deliberately retains the current model's effective
// 3.5k source and loaded-source normalization; these are not newly measured
// MN3009 parameters. Roland postfilter topology, Service Notes p.15:
// https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=15
// Independent node KCL/RK4 and a refined nonlinear trapezoidal DAE integrate
// the actual hold segments. Neither imports runtime state matrices, fits
// delay/gain, or removes genuine clock images from the reference waveform.
#include "DSP/YouKnowChorus.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <numbers>
#include <stdexcept>

namespace { std::atomic<bool> counting {false}; std::atomic<unsigned> allocations {0}; }
void* operator new(std::size_t n) { if (counting) ++allocations; if (void* p=std::malloc(n?n:1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
namespace youknow {
struct YouKnowTestAccess {
 static const auto& transition(const Chorus& c,bool connected=true) { return connected?c.support_.exactOutputConnected:c.support_.exactOutputMuted; }
 static float process(Chorus& c,float input,double clock,bool connected=true,const Chorus::InputCaptureInterval* capture=nullptr) {
  return c.lineA_.process(input,clock,c.sampleRate_,transition(c,connected),0,true,capture);
 }
 static auto state(const Chorus& c) { return c.lineA_.exactOutputState; }
 static auto nonlinear(const Chorus& c) { return c.lineA_.nonlinearOutput; }
 static float held(const Chorus& c) { return c.lineA_.held; }
 static int eventCount(const Chorus& c) { return c.lineA_.outputEventCount; }
 static void phase(Chorus& c,double phase) { c.lineA_.clockPhase=phase; }
 static void hold(Chorus& c,float value) { c.lineA_.held=c.lineA_.transferState=value; }
 static void seed(Chorus& c,const std::array<double,6>& state,float held) { c.lineA_.exactOutputState=state;hold(c,held); }
}; }
namespace {
using youknow::Chorus;using youknow::ChorusSupportProfile;using Probe=youknow::YouKnowTestAccess;
constexpr double pi=std::numbers::pi_v<double>,beta=200,vt=1.380649e-23*298.15/1.602176634e-19;
template<std::size_t N> using Vector=std::array<double,N>;
template<std::size_t N> using Matrix=std::array<Vector<N>,N>;
void require(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
template<std::size_t N> Vector<N> solve(Matrix<N> a,Vector<N> b) {
 for(std::size_t col=0;col<N;++col) {
  std::size_t pivot=col;for(std::size_t row=col+1;row<N;++row)if(std::abs(a[row][col])>std::abs(a[pivot][col]))pivot=row;
  require(std::abs(a[pivot][col])>1e-20,"singular output physical MNA");std::swap(a[col],a[pivot]);std::swap(b[col],b[pivot]);
  const double inverse=1/a[col][col];for(std::size_t j=col;j<N;++j)a[col][j]*=inverse;b[col]*=inverse;
  for(std::size_t row=0;row<N;++row)if(row!=col) {const double factor=a[row][col];for(std::size_t j=col;j<N;++j)a[row][j]-=factor*a[col][j];b[row]-=factor*b[col];}
 }return b;
}
template<std::size_t N> void stamp(Matrix<N>& a,int p,int q,double value) {
 if(p>=0)a[p][p]+=value;if(q>=0)a[q][q]+=value;if(p>=0&&q>=0){a[p][q]-=value;a[q][p]-=value;}
}
struct PhysicalOutput {
 // Eight nodes: source,j1,b1,e1,j2,b2,e2,wet output. Six physical caps.
 static constexpr std::array<std::array<int,2>,6> terminals {{{0,-1},{1,3},{2,-1},{4,6},{5,-1},{6,7}}};
 static constexpr Vector<6> capacitance {2.2e-9,820e-12,684e-12,1.8e-9,274e-12,1e-6};
 Matrix<8> g{},c{};Vector<8> drive{},voltage{};Vector<2> ic{};
 Matrix<6> a{};Vector<6> b{},readout{},linearState{};double direct{};
 explicit PhysicalOutput(bool connected=true) {
  Matrix<7> dc{};Vector<7> target{};
  stamp(dc,0,-1,1/3500.+1/47000.);target[0]=-10.37/3500;
  stamp(dc,0,1,1/44000.);stamp(dc,2,3,1/44000.);
  for(int i=0;i<2;++i) {int base=1+2*i,emitter=base+1,current=5+i;stamp(dc,emitter,-1,1/10000.);dc[emitter][current]=1;dc[base][current]=-1/(beta+1);dc[current][emitter]=1;dc[current][base]=-1;target[current]=.61;}
  const auto bias=solve(dc,target);for(int i=0;i<2;++i)ic[i]=bias[5+i]*beta/(beta+1);
  const auto r=[&](int p,int q,double value){stamp(g,p,q,1/value);};
  r(0,-1,3500);r(0,-1,47000);r(0,1,22000);r(1,2,22000);r(3,4,22000);r(4,5,22000);r(3,-1,10000);r(6,-1,10000);r(7,-1,22000);if(connected)r(7,-1,39000);
  drive[0]=1/3500.+1/47000.;
  for(int i=0;i<6;++i)stamp(c,terminals[i][0],terminals[i][1],capacitance[i]);
  for(int i=0;i<2;++i) {int base=2+3*i,emitter=base+1;double gm=ic[i]/vt;stamp(g,base,emitter,gm/beta);g[emitter][emitter]+=gm;g[emitter][base]-=gm;}
  // Independent capacitor-voltage constraints and node KCL yield a linear
  // derivative for RK4. No production transition/generator is imported.
  const auto constraint=constraintMatrix();
  for(int column=0;column<7;++column) {
   Vector<14> rhs{};if(column<6)rhs[8+column]=1;else std::copy(drive.begin(),drive.end(),rhs.begin());
   const auto x=solve(constraint,rhs);
   for(int row=0;row<6;++row) {if(column<6)a[row][column]=x[8+row]/capacitance[row];else b[row]=x[8+row]/capacitance[row];}
   if(column<6)readout[column]=x[7];else direct=x[7];
  }
 }
 Matrix<14> constraintMatrix() const {
  Matrix<14> result{};for(int r=0;r<8;++r)for(int col=0;col<8;++col)result[r][col]=g[r][col];
  for(int i=0;i<6;++i) {const auto [p,q]=terminals[i];result[p][8+i]=result[8+i][p]=1;if(q>=0)result[q][8+i]=result[8+i][q]=-1;}
  return result;
 }
 Vector<6> equilibrium() const {
  const auto v=solve(g,drive);Vector<6> x{};for(int i=0;i<6;++i){auto [p,q]=terminals[i];x[i]=v[p]-(q>=0?v[q]:0);}return x;
 }
 Vector<6> derivative(const Vector<6>& x,double input) const {
  Vector<6> out{};for(int r=0;r<6;++r){out[r]=b[r]*input;for(int col=0;col<6;++col)out[r]+=a[r][col]*x[col];}return out;
 }
 void linearStep(double input,double dt) {
  const auto k1=derivative(linearState,input);Vector<6> work{};
  for(int i=0;i<6;++i)work[i]=linearState[i]+dt*.5*k1[i];const auto k2=derivative(work,input);
  for(int i=0;i<6;++i)work[i]=linearState[i]+dt*.5*k2[i];const auto k3=derivative(work,input);
  for(int i=0;i<6;++i)work[i]=linearState[i]+dt*k3[i];const auto k4=derivative(work,input);
  for(int i=0;i<6;++i)linearState[i]+=dt/6*(k1[i]+2*k2[i]+2*k3[i]+k4[i]);
 }
 Vector<8> current(const Vector<8>& v) const {
  Vector<8> result{};for(int i=0;i<2;++i){int base=2+3*i,emitter=base+1;double y=(v[emitter]-v[base])/vt,q=ic[i]*(std::expm1(y)-y);result[base]+=q/beta;result[emitter]-=q*(1+1/beta);}return result;
 }
 void nonlinearStep(double input,double dt) {
  Matrix<8> matrix{};auto rhs=current(voltage);
  for(int r=0;r<8;++r){rhs[r]+=2*drive[r]*input;for(int col=0;col<8;++col){matrix[r][col]=2/dt*c[r][col]+g[r][col];rhs[r]-=2*g[r][col]*voltage[col];}}
  // Increment form avoids cancellation of large C/dt*absolute-voltage
  // products at fine reference steps, without loosening Newton tolerance.
  Vector<8> change{};bool converged=false;
  for(int iteration=0;iteration<20;++iteration) {
   Vector<8> next{};for(int i=0;i<8;++i)next[i]=voltage[i]+change[i];auto residual=current(next);auto jacobian=matrix;
   for(int r=0;r<8;++r){residual[r]+=rhs[r];for(int col=0;col<8;++col)residual[r]-=matrix[r][col]*change[col];}
   for(int i=0;i<2;++i){int base=2+3*i,emitter=base+1;double slope=ic[i]/vt*std::expm1((next[emitter]-next[base])/vt);stamp(jacobian,base,emitter,slope/beta);jacobian[emitter][emitter]+=slope;jacobian[emitter][base]-=slope;}
   const auto correction=solve(jacobian,residual);double largest=0;for(int i=0;i<8;++i){change[i]+=correction[i];largest=std::max(largest,std::abs(correction[i]));}
   if(largest<2e-12){converged=true;break;}
  }
  require(converged,"physical output DAE did not converge");for(int i=0;i<8;++i)voltage[i]+=change[i];
 }
 double linearOutput(double input) const {double out=direct*input;for(int i=0;i<6;++i)out+=readout[i]*linearState[i];return out;}
};
std::unique_ptr<Chorus> device(double rate,ChorusSupportProfile profile=ChorusSupportProfile::Nominal2SA1015Nonlinear) {
 auto c=std::make_unique<Chorus>();require(c->configureSupportProfile(profile),"profile rejected before prepare");c->prepare(rate);return c;
}
void heldMap() {
 for(double rate:{176400.,192000.,384000.,768000.})for(bool connected:{false,true}) {
  auto chorus=device(rate,ChorusSupportProfile::Nominal2SA1015);const auto& transition=Probe::transition(*chorus,connected);const auto& map=transition.heldOutputMap;
  require(map.available,"HQ output map unavailable");require(map.valueAt(0)==Vector<6>{},"G(0) is not zero");
  PhysicalOutput physical(connected);const auto equilibrium=physical.equilibrium();
  for(int row=0;row<6;++row){double next=map.fullIntervalDrive[row];for(int col=0;col<6;++col)next+=transition.stateByColumn[col][row]*equilibrium[col];require(std::abs(next-equilibrium[row])<2e-14,"held forcing lost DC equilibrium");}
  for(double fraction:{.0001,.03125,.173,.499,.501,.827,.9999,1.}) {
   auto fine=physical,coarse=physical;
   for(int i=0;i<64;++i)coarse.linearStep(1,fraction/(rate*64));
   for(int i=0;i<128;++i)fine.linearStep(1,fraction/(rate*128));
   const auto actual=map.valueAt(fraction);
   for(int i=0;i<6;++i){require(std::abs(fine.linearState[i]-coarse.linearState[i])<2e-10,"independent held-map RK4 needs refinement");require(std::abs(actual[i]-fine.linearState[i])<2e-10,"fractional held forcing differs from physical node integration");}
  }
 }
}
float acquire(float input) {double v=std::abs(double(input))/double(1.1246614f);return float(input/std::pow(1+double(1.2044546f)*v*v+std::pow(v,double(12.9395323f)),1/double(12.9395323f)));}
struct Staircase {
 std::array<float,128> cells{};int cursor{};std::uint64_t half=1;float held{};double time{};
 PhysicalOutput coarse,fine;double rate,clock;int refinement;bool nonlinear;
 Staircase(double r,double c,int steps,bool nonlinearReference=true):rate(r),clock(c),refinement(steps),nonlinear(nonlinearReference){}
 void integrate(double target) {
  const double length=target-time;if(!(length>0))return;
  const int n=std::max(1,int(std::ceil(length*rate*refinement)));
  // Both references land exactly on each source discontinuity. Each segment
  // is genuinely constant, not a trapezoidal average across the next jump.
  for(int i=0;i<n;++i) {if(nonlinear)coarse.nonlinearStep(2.6*held,length/n);else coarse.linearStep(held,length/n);}
  for(int i=0;i<2*n;++i) {if(nonlinear)fine.nonlinearStep(2.6*held,length/(2*n));else fine.linearStep(held,length/(2*n));}
  time=target;
 }
 double step(int frame,const Chorus::InputCaptureInterval& curve) {
  const double end=frame/rate,start=(frame-1)/rate;
  while(.5*double(half)/clock<=end) {
   const double edge=.5*double(half)/clock;
   if(half&1){integrate(edge);held+=.8654743f*(cells[(cursor+1)%128]-held);}
   else {cursor=(cursor+1)%128;const double fraction=std::clamp((edge-start)*rate,0.,1.);double value=0;for(int k=11;k>=0;--k)value=value*fraction+curve.outputByPower[k];cells[cursor]=acquire(float(value));}
   ++half;
  }
  integrate(end);return nonlinear?fine.voltage[7]/2.6:fine.linearOutput(held);
 }
};
void linearEvents() {
 for(double rate:{176400.,192000.,768000.})for(double clock:{40000.,200000.}) {
  auto c=device(rate,ChorusSupportProfile::Nominal2SA1015);Staircase oracle(rate,clock,8,false);
  double error=0,energy=0,refinement=0;
  for(int frame=1;frame<=int(rate*.008);++frame) {
   Chorus::InputCaptureInterval curve;curve.enabled=true;
   // Continuous at audio boundaries: a coincident input edge may fall on
   // either adjacent interval through floating phase rounding, with the same
   // source value. No artificial source jump can contaminate this oracle.
   curve.initialOutput=curve.outputByPower[0]=.2*std::sin((frame-1)*.33);
   curve.finalOutput=.2*std::sin(frame*.33);
   curve.outputByPower[1]=curve.finalOutput-curve.initialOutput;
   allocations=0;counting=true;
   const double actual=Probe::process(*c,float(curve.finalOutput),clock,true,&curve);
   counting=false;require(allocations==0,"live linear output event recovery allocated");
   const double reference=oracle.step(frame,curve);
   error+=(actual-reference)*(actual-reference);energy+=reference*reference;
   const double difference=oracle.coarse.linearOutput(oracle.held)-reference;refinement+=difference*difference;
  }
  require(energy>1e-8,"linear event fixture never reached live delayed samples");
  const double nrms=std::sqrt(error/energy),convergence=std::sqrt(refinement/energy);
  std::cout<<"Linear held output "<<rate<<"/"<<clock<<" NRMS="<<nrms<<" oracle="<<convergence<<'\n';
  require(convergence<1e-7,"linear held-event reference needs refinement");
  require(nrms<1e-6,"linear output events disagree with physical piecewise-constant integration");
 }
}
void nonlinearHold(double rate,double clock,double frequency,double amplitude) {
 auto c=device(rate);Staircase oracle(rate,clock,128);double error=0,energy=0,refinement=0;
 for(int frame=1;frame<=int(rate*.012);++frame) {
  Chorus::InputCaptureInterval curve;curve.enabled=true;double power=1,factorial=1;
  const double angle=2*pi*frequency*(frame-1)/rate,omega=2*pi*frequency/rate;
  for(int k=0;k<=11;++k){if(k){power*=omega;factorial*=k;}curve.outputByPower[k]=amplitude*std::sin(angle+k*pi/2)*power/factorial;}
  curve.initialOutput=curve.outputByPower[0];for(double coefficient:curve.outputByPower)curve.finalOutput+=coefficient;
  allocations=0;counting=true;
  const double actual=Probe::process(*c,float(curve.finalOutput),clock,true,&curve);
  counting=false;require(allocations==0,"live nonlinear output event recovery allocated");
  const double reference=oracle.step(frame,curve);
  if(frame>int(rate*.008)){error+=(actual-reference)*(actual-reference);energy+=reference*reference;const double difference=oracle.coarse.voltage[7]/2.6-reference;refinement+=difference*difference;}
 }
 const double nrms=std::sqrt(error/energy),convergence=std::sqrt(refinement/energy);
 std::cout<<"Nonlinear held output "<<rate<<"/"<<clock<<"/"<<frequency<<" NRMS="<<nrms<<" oracle="<<convergence<<'\n';
 require(convergence<8e-6,"nonlinear held-output reference needs refinement");
 require(nrms<2e-5,"nonlinear event recovery escaped physical DAE error budget");
 require(Probe::nonlinear(*c).fallbackCount==0,"qualified nonlinear hold needed fallback");
}
void lifecycleAndPolicy() {
 for(double rate:{8000.,48000.,96000.,176400.,192000.,768000.}) {
  auto c=device(rate);require(Probe::transition(*c).heldOutputMap.available==(rate>=176400),"held recovery selected on wrong grid");
  require(Probe::eventCount(*c)==0,"prepare retained old output events");
  Probe::hold(*c,.1f);allocations=0;counting=true;
  for(int frame=0;frame<24;++frame) {
   const bool connected=frame<8||frame>=16;
   const auto result=Probe::process(*c,0,0,connected);
   require(std::isfinite(result)&&Probe::held(*c)==.1f,"no-event support changed physical hold or became nonfinite");
   require(Probe::eventCount(*c)==0,"stopped clock retained stale event forcing");
   const auto state=Probe::nonlinear(*c);
   require(state.valid&&state.topology==(connected?3:2)&&state.fallbackCount==0,"load transition lost nonlinear topology/history validity");
  }
  counting=false;require(allocations==0,"event output recovery allocated");
  c->reset();require(Probe::eventCount(*c)==0&&Probe::state(*c)==Vector<6>{}&&!Probe::nonlinear(*c).valid,"reset retained output forcing/current state");
  if(rate>=176400) {
   Probe::phase(*c,.49);Probe::process(*c,.2f,200000);
   const int expected=rate==176400||rate==192000?2:1;
   require(Probe::eventCount(*c)==expected,"multiple fractional output edges were not retained");
   c->prepare(48000,true);require(!Probe::transition(*c).heldOutputMap.available&&Probe::eventCount(*c)==0,"HQ transition retained an event map or stale event");
  }
 }
 // An output event exactly at the sample end has G(0)=0 and cannot change
 // any physical capacitor at that instant, even though the held source jumps.
 auto c=device(192000,ChorusSupportProfile::Nominal2SA1015);Probe::hold(*c,.2f);Probe::phase(*c,0);
 PhysicalOutput physical;const auto equilibrium=physical.equilibrium();Vector<6> seeded{};for(int i=0;i<6;++i)seeded[i]=.2f*equilibrium[i];Probe::seed(*c,seeded,.2f);
 Probe::process(*c,0,96000);const auto actual=Probe::state(*c);
 require(Probe::eventCount(*c)==1&&Probe::held(*c)!=.2f,"endpoint fixture missed its held-source jump");
 for(int i=0;i<6;++i)require(std::abs(actual[i]-seeded[i])<2e-14,"zero-age edge changed a capacitor instantaneously");
}
}
int main() {
 try {heldMap();lifecycleAndPolicy();linearEvents();nonlinearHold(176400,40000,15000,.424264);nonlinearHold(192000,20000,15000,.424264);nonlinearHold(192000,40000,1000,.8);nonlinearHold(384000,40000,15000,.424264);std::cout<<"Output hold recovery contracts passed\n";}
 catch(const std::exception& e){counting=false;std::cerr<<e.what()<<'\n';return 1;}
}
