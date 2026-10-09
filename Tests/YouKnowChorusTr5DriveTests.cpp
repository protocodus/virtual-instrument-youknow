// Independent component-current oracle for Roland jack-board p15 Tr6/R45/
// R44/Tr5 and the actual C16/C13/C15 network. Timing is a named beta=150,
// Vbe=.6 V, ideal-saturation prior, not a measurement of an installed unit.
#include "DSP/YouKnowChorus.h"
#include "DSP/YouKnowProductFidelity.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

namespace { thread_local bool countAllocations=false; thread_local std::size_t allocationCount=0; }
void* operator new(std::size_t n) { if(countAllocations)++allocationCount; if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc(); }
void* operator new[](std::size_t n) {return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace youknow {
struct YouKnowTestAccess {
    static void advance(Chorus& c,bool off) {c.advanceMuteDrive(off);}
    static auto nodes(const Chorus& c) {return std::array{c.muteDriveNodeVolts_,c.muteDriveHoldVolts_,c.clockMuteVolts_};}
    static void nodes(Chorus& c,const std::array<double,3>& x) {c.muteDriveNodeVolts_=x[0];c.muteDriveHoldVolts_=x[1];c.clockMuteVolts_=x[2];}
    static auto buckets(const Chorus& c) {return c.lineA_.cells;}
    static auto rng(const Chorus& c) {return std::array{c.lineA_.noiseState,c.lineB_.noiseState,c.lineA_.transferNoiseState,c.lineB_.transferNoiseState};}
    static auto engineRng(const YouKnowEngine& e) {return std::array{e.outputNoiseStateLeft_,e.outputNoiseStateRight_,e.commonVcaNoiseState_,e.chorus_.lineA_.noiseState,e.chorus_.lineB_.noiseState};}
    static auto builds(const Chorus& c) {return c.supportBuildCount_;}
    static auto generator(const Chorus& c,std::size_t region) {return c.support_.clockMuteTransitions[(region&16u)?16u+(region&7u):region].generator;}
}; }
namespace {
using namespace youknow;
using State=std::array<double,3>;
void require(bool b,const char* m) {if(!b)throw std::runtime_error(m);}
double baseCurrent() {
    // Independently solve the clamped NPN base. The conducting PNP collector
    // is at +15 V under the retained ideal saturation coordinate.
    constexpr double emitter=-15,source=15,base=emitter+.6;
    return (source-base)/100000-(base-emitter)/47000;
}
State currents(const State& x,bool off,bool finite=true,double beta=150) {
    const double b=std::min(-15+(x[1]+15)*39000/599000,-14.4);
    const double cb=std::min(-15+(x[2]+15)*33000/363000,-14.4);
    const double bridge=(x[2]-x[0])/330000+std::max(x[2]-x[0]-.6,0.)/10000;
    const double r48=(x[0]-x[1])/150000;
    const double demand=(x[0]+15)/330;
    const double sink=off?0:finite?std::min(demand,beta*baseCurrent()):demand;
    return {((15-x[0])/10000-sink-r48+bridge)/2.2e-6,
             (r48-(x[1]-b)/560000)/1e-6,
             (-bridge-2*(x[2]-cb)/330000)/2.2e-6};
}
State add(State x,const State& k,double dt) {for(int i=0;i<3;++i)x[i]+=dt*k[i];return x;}
void integrate(State& x,bool off,double dt,bool finite=true,double maxDt=2e-6,double beta=150) {
    const int n=std::max(1,static_cast<int>(std::ceil(dt/maxDt))); dt/=n;
    for(int j=0;j<n;++j){auto a=currents(x,off,finite,beta),b=currents(add(x,a,dt/2),off,finite,beta),c=currents(add(x,b,dt/2),off,finite,beta),d=currents(add(x,c,dt),off,finite,beta);for(int i=0;i<3;++i)x[i]+=dt*(a[i]+2*b[i]+2*c[i]+d[i])/6;}
}
void process(Chorus& c,ChorusMode mode,bool finite=true) {
    float l,r;c.process(0,mode,0,l,r,1,true,true,false,
                       ChorusTimingProfile::Shipping,true,true,false,0,1,finite);
}
double gate(double h) {
    const double b=std::min(-15+(h+15)*39000/599000,-14.4);
    const double ib=std::max((h-b)/560000-(b+15)/39000,0.);
    return std::min(0.,std::max(-15.,15-100000*150*ib)+.6);
}
void sourceAndRegions() {
    require(std::abs(baseCurrent()-Chorus::tr5AvailableBaseCurrent())<1e-18,"R45 feed minus R44 bleed disagrees with source KCL");
    require(std::abs(Chorus::tr5CollectorCurrentLimit-150*baseCurrent())<1e-15,"Tr5 current limit is not derived from the base drive");
    auto c=std::make_unique<Chorus>();c->prepare(48000);process(*c,ChorusMode::Off);
    const State off=YouKnowTestAccess::nodes(*c);
    std::cout<<"R45/R44 Ib "<<baseCurrent()*1e6<<" uA, nominal Ic "<<150*baseCurrent()*1000<<" mA; Off-state R46 demand "<<(off[0]+15)/330*1000<<" mA\n";
    require((off[0]+15)/330>2*150*baseCurrent(),"independent source screen no longer enters the finite-current region");
    // Check every prepared affine region against independent resistor KCL at
    // actual points inside it. This catches a source sign, missing bleed or
    // accidental R46 sink retained in the current-limited branch.
    for(double n:{-14.5,-8.,1.,14.})for(double h:{-14.,2.})for(double v:{-14.,-4.,14.})for(bool commandOff:{false,true})for(bool finite:{false,true}) {
        State x{n,h,v};
        std::size_t region=(v-n>.6?1u:0u)|(h>-15+.6*599000/39000?2u:0u)|(v>-8.4?4u:0u)|(commandOff?0u:8u);
        if(finite&&!commandOff&&(n+15)/330>150*baseCurrent())region|=16u;
        const auto a=YouKnowTestAccess::generator(*c,region);const auto expected=currents(x,commandOff,finite);
        for(int row=0;row<3;++row){double actual=a[row][3];for(int col=0;col<3;++col)actual+=a[row][col]*x[col];actual*=48000;require(std::abs(actual-expected[row])<.004,"prepared region violates independent node-current KCL");}
    }
    // Entering saturation changes slope, while collector current and all
    // capacitor first derivatives agree at the region boundary. Check every
    // simultaneous diode/base state against both prepared generators.
    for(std::size_t bits=0;bits<8;++bits) {
        const State x{Chorus::tr5CurrentLimitNodeVolts,-7.,-9.};
        const auto saturated=YouKnowTestAccess::generator(*c,8u|bits);
        const auto limited=YouKnowTestAccess::generator(*c,24u|bits);
        for(int row=0;row<3;++row){double a=saturated[row][3],b=limited[row][3];for(int col=0;col<3;++col){a+=saturated[row][col]*x[col];b+=limited[row][col]*x[col];}require(std::abs(a-b)<2e-15,"Tr5 active/saturated boundary jumps capacitor current");}
    }
}
void nodalHistory(double fs) {
    auto c=std::make_unique<Chorus>();c->prepareSupportRates(48000);c->prepare(fs);process(*c,ChorusMode::Off);
    State oracle=YouKnowTestAccess::nodes(*c),finer=oracle;
    constexpr std::array sequence{std::pair{false,.18},std::pair{true,.04},std::pair{false,.006},std::pair{true,.30},std::pair{false,.2}};
    double error=0,convergence=0;int firstWet=-1,firstClock=-1,expectedWet=-1,expectedClock=-1;
    for(std::size_t segment=0;segment<sequence.size();++segment) {
        auto [off,seconds]=sequence[segment];
        for(int n=0;n<std::llround(seconds*fs);++n) {
            YouKnowTestAccess::advance(*c,off);integrate(oracle,off,1/fs,true,1e-6);integrate(finer,off,1/fs,true,.5e-6);
            const auto actual=YouKnowTestAccess::nodes(*c);
            for(int i=0;i<3;++i){error=std::max(error,std::abs(actual[i]-finer[i]));convergence=std::max(convergence,std::abs(oracle[i]-finer[i]));require(actual[i]>-15.000001&&actual[i]<15.000001,"finite drive escaped supply rails");}
            if(segment==0) {
                if(firstWet<0&&c->isWetInputConnected())firstWet=n+1;
                if(firstClock<0&&!c->clocksStopped())firstClock=n+1;
                if(expectedWet<0&&gate(finer[1])>-1.8)expectedWet=n+1;
                if(expectedClock<0&&finer[2]<-8.4)expectedClock=n+1;
            }
        }
    }
    require(error<2e-6&&convergence<2e-7,"Tr5 event trajectories failed step-halved independent nodal reference");
    require(firstWet==expectedWet&&firstClock==expectedClock,"finite drive chronology differed from nodal gate/clock reference");
    std::cout<<fs<<" Hz Tr5 first clock/wet "<<1000*firstClock/fs<<"/"<<1000*firstWet/fs<<" ms; node error "<<error<<" V, RK4 convergence "<<convergence<<" V\n";
    auto nodes=YouKnowTestAccess::nodes(*c);auto rng=YouKnowTestAccess::rng(*c);const auto builds=YouKnowTestAccess::builds(*c);const double nextFs=fs==48000?192000:48000;
    c->prepare(nextFs,true);require(YouKnowTestAccess::nodes(*c)==nodes&&YouKnowTestAccess::rng(*c)==rng,"rate change reset physical charge or random history");require(YouKnowTestAccess::builds(*c)==builds,"cached rate change rebuilt control matrices");
    YouKnowTestAccess::advance(*c,false);integrate(finer,false,1/nextFs,true,1e-6);for(int i=0;i<3;++i)require(std::abs(YouKnowTestAccess::nodes(*c)[i]-finer[i])<2e-6,"rate change lost finite-current history");
    c->reset();process(*c,ChorusMode::Off);require(!c->isWetInputConnected()&&c->clocksStopped(),"reset failed the Off physical rest");
}
void corners() {
    for(double fs:{8000.,192000.}) {
        auto c=std::make_unique<Chorus>();c->prepare(fs);process(*c,ChorusMode::Off);
        for(const State& start:{State{Chorus::tr5CurrentLimitNodeVolts,Chorus::muteDriveThresholdVolts,Chorus::clockMuteThresholdVolts},State{Chorus::tr5CurrentLimitNodeVolts+.001,-4.,Chorus::tr5CurrentLimitNodeVolts+.601},State{Chorus::tr5CurrentLimitNodeVolts-.001,-4.,Chorus::tr5CurrentLimitNodeVolts+.599}})for(bool off:{false,true}) {
            YouKnowTestAccess::nodes(*c,start);State x=start;
            for(int n=0;n<32;++n){YouKnowTestAccess::advance(*c,off);integrate(x,off,1/fs,true,.25e-6);for(int i=0;i<3;++i)require(std::abs(YouKnowTestAccess::nodes(*c)[i]-x[i])<2e-7,"current/junction simultaneous crossing lost capacitor charge");}
        }
    }
}
void sensitivity() {
    // Same component family/current-gain specimens are sensitivity screens,
    // not selectable product calibration or invented timing constants.
    auto c=std::make_unique<Chorus>();c->prepare(192000);process(*c,ChorusMode::Off);const auto start=YouKnowTestAccess::nodes(*c);
    for(double beta:{120.,150.,200.}){State x=start;int wet=-1,clock=-1;for(int n=0;n<25000;++n){integrate(x,false,5e-6,true,1e-6,beta);if(clock<0&&x[2]<-8.4)clock=n+1;if(wet<0&&gate(x[1])>-1.8)wet=n+1;}std::cout<<"beta "<<beta<<": first clock/wet "<<clock*.005<<"/"<<wet*.005<<" ms\n";require(wet>clock&&clock>0,"same-part drive sensitivity broke mute/clock ordering");}
}
void stoppedBucketMemory() {
    auto c=std::make_unique<Chorus>();c->prepare(48000);process(*c,ChorusMode::One);
    for(int n=0;n<4800;++n){float l,r;c->process(.2f*std::sin(n*.031f),ChorusMode::One,.3f,l,r,1,true,true,false,ChorusTimingProfile::Shipping,true,true,false,0,1,true);}
    for(int n=0;n<48000;++n)process(*c,ChorusMode::Off);
    require(c->clocksStopped(),"finite drive never stopped the physical clocks");
    const auto buckets=YouKnowTestAccess::buckets(*c);const auto rng=YouKnowTestAccess::rng(*c);
    State oracle=YouKnowTestAccess::nodes(*c);int restart=-1,expectedRestart=-1;
    for(int n=0;n<1000;++n){process(*c,ChorusMode::One);integrate(oracle,false,1/48000.,true,.5e-6);if(expectedRestart<0&&oracle[2]<-8.4)expectedRestart=n+1;if(!c->clocksStopped()){restart=n+1;break;}require(YouKnowTestAccess::buckets(*c)==buckets&&YouKnowTestAccess::rng(*c)==rng,"finite drive command cleared held buckets or advanced a stopped noise stream");}
    require(restart>0&&expectedRestart>0&&std::abs(restart-expectedRestart)<=1,"finite Tr5 restart did not follow the actual retained charge's nodal reference");
    require(!c->isWetInputConnected(),"finite Tr5 admitted the return before restoring the clocks");
    for(int n=0;n<32;++n)process(*c,ChorusMode::One);
    require(YouKnowTestAccess::rng(*c)!=rng,"physical clock restart failed to advance event noise");
}
struct Render {std::vector<float> audio;std::array<std::uint32_t,5> rng;};
Render render(int block,bool finite,int quality=1,float chorusNoise=.3f) {
    auto e=std::make_unique<YouKnowEngine>();ProductFidelityProfile::configureBeforePrepare(*e);e->prepare(48000,256,quality);EngineParameters p;ProductFidelityProfile::applyTo(p);p.enableChorusFiniteTr5Drive=finite;p.chorus=ChorusMode::One;p.chorusNoise=chorusNoise;e->setParameters(p);for(int note:{48,55,60,64,67,72})e->noteOn(note,.7f);
    Render r;r.audio.resize(43200);std::vector<float> right(r.audio.size());
    const std::array boundaries{4800,9600,12000,31200,31680,33600,43200};
    std::size_t i=0;std::size_t next=0;
    while(i<r.audio.size()) {
        if(next>0&&i==static_cast<std::size_t>(boundaries[next-1])){p.chorus=next%2?ChorusMode::Off:ChorusMode::One;e->setParameters(p);}
        const int n=std::min<int>(block,boundaries[next]-i);countAllocations=true;e->process(r.audio.data()+i,right.data()+i,n);countAllocations=false;i+=n;if(i==static_cast<std::size_t>(boundaries[next]))++next;
    }
    r.rng=YouKnowTestAccess::engineRng(*e);return r;
}
void engineAndCost() {
    require(!EngineParameters{}.enableChorusFiniteTr5Drive,"raw reference selected finite Tr5");EngineParameters p;ProductFidelityProfile::applyTo(p);require(p.enableChorusFiniteTr5Drive,"product did not select finite Tr5");
    allocationCount=0;
    for(int q:{1,4}) {
        auto a=render(1,true,q),b=render(173,true,q),raw=render(173,false,q);
        require(a.audio==b.audio&&a.rng==b.rng,"finite Tr5 depends on block partition");require(std::equal(a.audio.begin(),a.audio.begin()+4800,raw.audio.begin()),"settled On finite drive changed its unchanged physical endpoint");
        for(int i=0;i<3;++i)require(a.rng[i]==raw.rng[i],"Tr5 timing changed independent non-chorus noise streams");
        double d=0,s=0;for(std::size_t i=0;i<a.audio.size();++i){require(std::isfinite(a.audio[i]),"finite Tr5 produced nonfinite audio");d+=std::pow(a.audio[i]-raw.audio[i],2);s+=std::pow(raw.audio[i],2);}require(d>0,"finite Tr5 did not affect switched full-engine audio");std::cout<<"q"<<q<<" switched engine delta "<<10*std::log10(d/s)<<" dBc\n";
        auto signal=render(173,true,q,0),reference=render(173,false,q,0);d=0;s=0;for(std::size_t i=0;i<signal.audio.size();++i){d+=std::pow(signal.audio[i]-reference.audio[i],2);s+=std::pow(reference.audio[i],2);}require(d>0,"finite Tr5 difference only changed BBD noise");std::cout<<"q"<<q<<" switched engine delta with BBD hiss disabled "<<10*std::log10(d/s)<<" dBc\n";
    }
    require(allocationCount==0,"finite Tr5 audio callback allocated");
    // Warm-cache preparation cost and complete callback cost include matrix
    // selection/crossing work; building ALL would regenerate frozen demos.
    auto c=std::make_unique<Chorus>();c->prepareSupportRates(48000);constexpr int repeats=20;auto start=std::chrono::steady_clock::now();for(int i=0;i<repeats;++i)c->prepareSupportRates(48000);auto end=std::chrono::steady_clock::now();
    std::cout<<"sizeof Engine/Chorus "<<sizeof(YouKnowEngine)<<"/"<<sizeof(Chorus)<<" bytes; cached prepare "<<std::chrono::duration<double,std::milli>(end-start).count()/repeats<<" ms\n";
    double rawTime=0,newTime=0;for(int j=0;j<4;++j)for(bool finite:{false,true}){auto begin=std::chrono::steady_clock::now();auto r=render(173,finite);const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();(finite?newTime:rawTime)+=elapsed;require(!r.audio.empty(),"benchmark render missing");}std::cout<<"Six-voice switched cost ratio "<<newTime/rawTime<<"\n";
}
}
int main(){try{sourceAndRegions();for(double fs:{8000.,44100.,48000.,192000.})nodalHistory(fs);corners();sensitivity();stoppedBucketMemory();engineAndCost();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}std::cout<<"Finite Tr5 drive checks passed\n";}
