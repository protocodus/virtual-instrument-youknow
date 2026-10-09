// Independent resistor-current, collector-KCL and channel-law references for
// Roland jack board p15. Timing numbers qualify named priors, not a real unit.
#include "DSP/YouKnowChorus.h"
#include "DSP/YouKnowProductFidelity.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <new>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace { thread_local bool countAllocations=false; thread_local std::size_t allocationCount=0; }
void* operator new(std::size_t n) { if(countAllocations)++allocationCount; if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace youknow {
struct YouKnowTestAccess {
    static void advance(Chorus& c,bool off) { c.advanceMuteDrive(off); }
    static auto nodes(const Chorus& c) { return std::array{c.muteDriveNodeVolts_,c.muteDriveHoldVolts_,c.clockMuteVolts_}; }
    static auto history(const Chorus& c) { return std::array{c.lineA_.noiseState,c.lineB_.noiseState}; }
    static void ratio(Chorus& c,double g) { c.wetInputConductanceRatio_=g; }
    static const auto& transition(Chorus& c) { return c.finiteWetTransition(); }
    static auto postState(const Chorus& c) {return c.lineA_.exactOutputState;}
    static auto postDepth(const Chorus& c) {return c.lineA_.nonlinearOutput.currentHistoryDepth;}
    static auto postTopology(const Chorus& c) {return c.lineA_.nonlinearOutput.topology;}
    static void advancePost(Chorus& c) {c.lineA_.held=.01f;c.lineA_.process(0,0,c.sampleRate_,c.finiteWetTransition(),0);}
    static auto engineRng(const YouKnowEngine& e) { return std::array{e.outputNoiseStateLeft_,e.outputNoiseStateRight_,e.commonVcaNoiseState_,e.chorus_.lineA_.noiseState,e.chorus_.lineB_.noiseState}; }
}; }
namespace {
using namespace youknow;
using State=std::array<double,3>;
void require(bool b,const char* m) { if(!b)throw std::runtime_error(m); }
State current(const State& x,bool off) {
    double b=std::min(-15+(x[1]+15)*39000/599000,-14.4);
    double cb=std::min(-15+(x[2]+15)*33000/363000,-14.4);
    double bridge=(x[2]-x[0])/330000+std::max(x[2]-x[0]-.6,0.)/10000;
    double r48=(x[0]-x[1])/150000;
    return {((15-x[0])/10000-(off?0:(x[0]+15)/330)-r48+bridge)/2.2e-6,
             (r48-(x[1]-b)/560000)/1e-6,
             (-bridge-2*(x[2]-cb)/330000)/2.2e-6};
}
State add(State x,const State& k,double dt) {for(int i=0;i<3;++i)x[i]+=dt*k[i];return x;}
void integrate(State& x,bool off,double dt) {
    const int n=std::max(1,static_cast<int>(std::ceil(dt/2e-6))); dt/=n;
    for(int j=0;j<n;++j){auto a=current(x,off),b=current(add(x,a,dt/2),off),c=current(add(x,b,dt/2),off),d=current(add(x,c,dt),off);for(int i=0;i<3;++i)x[i]+=dt*(a[i]+2*b[i]+2*c[i]+d[i])/6;}
}
double gateOracle(double h,double beta=150) {
    // First solve the base node under the junction inequality, then subtract
    // the bleed current. No production threshold/helper is used.
    double b=std::min(-15+(h+15)*39000/599000,-14.4);
    double ib=std::max((h-b)/560000-(b+15)/39000,0.);
    double vc=std::max(-15.,15-100000*beta*ib);
    return std::min(0.,vc+.6);
}
double channelOracle(double gate) {
    const double g=gate<=-1.8?0:2*.0028/1.8*(1+gate/1.8);
    return g==0?0:39000/(39000+1/g);
}
void prime(Chorus& c,ChorusMode mode,bool finite=true) {float l,r;c.process(0,mode,0,l,r,1,true,true,false,ChorusTimingProfile::Shipping,true,finite);}
void components() {
    for(double h=-15;h<=10;h+=.00031){double g=gateOracle(h);require(std::abs(ChorusMuteDrive::gateVolts(h)-g)<3e-12,"Tr4 collector/base KCL disagrees with independent oracle");require(std::abs(ChorusMuteDrive::conductanceRatio(g)-channelOracle(g))<2e-15,"JFET channel divider disagrees with independent square law");}
    std::cout<<"Nominal on wet attenuation "<<20*std::log10(channelOracle(0))<<" dB\n";
}
void chronology() {
    for(double fs:{8000.,48000.,192000.}) {
        auto c=std::make_unique<Chorus>(); c->prepareSupportRates(48000);c->prepare(fs);prime(*c,ChorusMode::One);
        State x=YouKnowTestAccess::nodes(*c);
        double maxNode=0,maxGate=0;int removed=-1,admitted=-1;
        for(bool off:{true,false}) {
            for(int n=0;n<static_cast<int>(fs*(off?3.0:.5));++n){YouKnowTestAccess::advance(*c,off);integrate(x,off,1/fs);auto a=YouKnowTestAccess::nodes(*c);for(int i=0;i<3;++i)maxNode=std::max(maxNode,std::abs(a[i]-x[i]));maxGate=std::max(maxGate,std::abs(c->muteGateVolts()-gateOracle(x[1])));require(std::abs(c->wetInputConductanceRatio()-channelOracle(c->muteGateVolts()))<2e-15,"wet loading diverged from the physical gate");if(off&&removed<0&&!c->isWetInputConnected())removed=n+1;if(!off&&admitted<0&&c->isWetInputConnected())admitted=n+1;}
        }
        require(maxNode<2e-6&&maxGate<6e-5,"three-capacitor/collector trajectories failed nodal oracle");
        require(std::abs(removed/fs-.08808)<1/fs+.00003,"finite collector changed the expected prior-qualified removal timing");
        require(std::abs(admitted/fs-.10784)<1/fs+.00003,"finite collector changed the expected prior-qualified admission timing");
        auto nodes=YouKnowTestAccess::nodes(*c); auto rng=YouKnowTestAccess::history(*c);double g=c->wetInputConductanceRatio();c->prepare(fs==48000?192000:48000,true);require(nodes==YouKnowTestAccess::nodes(*c)&&rng==YouKnowTestAccess::history(*c)&&g==c->wetInputConductanceRatio(),"rate rebuild lost charge/gate/bucket chronology");
        std::cout<<fs<<" Hz: removed "<<removed/fs*1000<<" ms, readmitted "<<admitted/fs*1000<<" ms; max nodal/gate error "<<maxNode<<"/"<<maxGate<<" V\n";
        c->reset();prime(*c,ChorusMode::Off);require(!c->isWetInputConnected()&&c->wetInputConductanceRatio()==0,"reset/off left channel conducting");
    }
}
void couplingGrid() {
    // With ideal followers and all five lowpass nodes at their DC equilibrium,
    // only C25's physical voltage changes. Independently solve its resistor
    // current: dx/dt=(1-x)*(1/22k+g/39k)/1uF. This also bounds the numerical
    // conductance-grid interpolation against exact exponential charge decay.
    for(double fs:{8000.,48000.,192000.}){auto c=std::make_unique<Chorus>();c->prepare(fs);double error=0;for(double g=.0001;g<1;g+=.0013){YouKnowTestAccess::ratio(*c,g);const auto& t=YouKnowTestAccess::transition(*c);double x=0;for(int j=0;j<4;++j)x+=t.driveBySample[j][5];for(int k=0;k<5;++k)x+=t.stateByColumn[k][5];double exact=1-std::exp(-(1/22000.+g/39000.)/1e-6/fs);error=std::max(error,std::abs(x-exact));}std::cout<<fs<<" Hz C25 interpolated charge error "<<error<<"\n"; require(error<8e-9,"conductance-grid interpolation failed independent C25 charge bound");}
}
void outputHistory()
{
    auto c=std::make_unique<Chorus>();
    require(c->configureSupportProfile(ChorusSupportProfile::Nominal2SA1015Nonlinear),"support configuration failed");
    c->prepare(192000);
    YouKnowTestAccess::ratio(*c,.99);
    for(int i=0;i<100;++i)YouKnowTestAccess::advancePost(*c);
    require(YouKnowTestAccess::postDepth(*c)==3,"nonlinear post history did not prime");
    for(double g:{.03125,.2,.001,.8})
    {
        const auto before=YouKnowTestAccess::postState(*c);
        YouKnowTestAccess::ratio(*c,g);
        (void)YouKnowTestAccess::transition(*c);
        require(before==YouKnowTestAccess::postState(*c),"load selection reset capacitor charge");
        YouKnowTestAccess::advancePost(*c);
        require(YouKnowTestAccess::postDepth(*c)==3&&YouKnowTestAccess::postTopology(*c)==3,
                "positive conductance-grid crossing reset residual-current chronology");
    }
    for(double g:{0.,.99})
    {
        const auto before=YouKnowTestAccess::postState(*c);
        YouKnowTestAccess::ratio(*c,g);
        (void)YouKnowTestAccess::transition(*c);
        require(before==YouKnowTestAccess::postState(*c),"open/reopen selection discarded physical charge");
        YouKnowTestAccess::advancePost(*c);
        require(YouKnowTestAccess::postTopology(*c)==(g==0?2:3),"open/reopen algebraic topology stale");
        const auto after=YouKnowTestAccess::postState(*c);
        for(int i=0;i<6;++i)require(std::isfinite(after[i])&&std::abs(after[i]-before[i])<.005,
                                  "open/reopen introduced a discontinuous capacitor reset");
    }
}
struct Result{std::vector<float> l;std::array<std::uint32_t,5> rng;};
Result render(int block,bool finite){auto e=std::make_unique<YouKnowEngine>();ProductFidelityProfile::configureBeforePrepare(*e);e->prepare(48000,256,1);EngineParameters p;ProductFidelityProfile::applyTo(p);p.enableChorusFiniteMuteDrive=finite;p.chorus=ChorusMode::One;p.chorusNoise=.3f;e->setParameters(p);e->noteOn(60,1);Result r;r.l.resize(24000);std::vector<float> right(r.l.size());for(std::size_t i=0;i<r.l.size();){if(i==9600){p.chorus=ChorusMode::Off;e->setParameters(p);}if(i==19200){p.chorus=ChorusMode::One;e->setParameters(p);}const auto boundary=i<9600?9600:i<19200?19200:r.l.size();int n=std::min<int>(block,boundary-i);countAllocations=true;e->process(r.l.data()+i,right.data()+i,n);countAllocations=false;i+=n;}r.rng=YouKnowTestAccess::engineRng(*e);return r;}
void engine(){allocationCount=0;require(!EngineParameters{}.enableChorusFiniteMuteDrive,"raw reference changed");EngineParameters p;ProductFidelityProfile::applyTo(p);require(p.enableChorusFiniteMuteDrive,"product did not select finite collector");auto a=render(1,true),b=render(173,true),raw=render(173,false);require(a.l==b.l&&a.rng==b.rng,"finite drive depends on block partition");require(a.rng==raw.rng,"finite drive changed independent clock/RNG chronology");double d=0,s=0;for(std::size_t i=0;i<a.l.size();++i){require(std::isfinite(a.l[i]),"nonfinite finite-gate output");d+=std::pow(a.l[i]-raw.l[i],2);s+=std::pow(raw.l[i],2);}require(d>0,"finite gate absent from deterministic signal difference");require(allocationCount==0,"finite-gate audio callback allocated");std::cout<<"Full switched engine delta "<<10*std::log10(d/s)<<" dBc\n";}
}
int main(){try{components();chronology();couplingGrid();outputHistory();engine();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}std::cout<<"Finite Tr4/JFET checks passed\n";}
