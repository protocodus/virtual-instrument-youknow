// Independent voltage-domain Tr20 and fine-step C58 KCL oracle. The
// reconstruction changes only numerical sampling between existing current
// endpoints. No original-unit accuracy or audible preference is inferred.
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductHardwareRealism.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace youknow
{
struct YouKnowTestAccess
{
    static std::array<double, 3> state(const YouKnowEngine& e)
    { const auto& v=e.voices_[0]; return {v.vcaControl,v.vcaJunctionCharge,v.vcaGain}; }
    static auto interval(const YouKnowEngine& e) { return e.voices_[0].vcaControlInterval; }
    static auto context(const YouKnowEngine& e)
    {
        const auto& v=e.voices_[0];
        return v.filterIsland.contexts[static_cast<std::size_t>((v.filterIsland.frames-1)&31)].vcaControl;
    }
    static auto contextBytes() { return sizeof(YouKnowEngine::VoiceFilterIslandContext); }
};
}

namespace
{
using Reconstruction=youknow::VcaControlReconstruction;
using Engine=youknow::YouKnowEngine;
using Probe=youknow::YouKnowTestAccess;
constexpr long double span=static_cast<float>(youknow::ControlDac::positiveSpanVolts(
    youknow::ControlDac::maximumCode));
constexpr long double referenceIe=.0006509655627366167L*201.0L/200.0L;
constexpr long double referenceVt=1.380649e-23L*298.15L/1.602176634e-19L;
void require(bool b,const char* message) { if(!b) throw std::runtime_error(message); }

long double emitter(long double supply,long double resistance,long double celsius)
{
    const long double vt=referenceVt*(celsius+273.15L)/298.15L;
    const long double vbe=.61L-.0016L*(celsius-25);
    long double junction=vbe;
    for(int iteration=0;iteration<80;++iteration)
    {
        const long double current=referenceIe*std::exp((junction-vbe)/vt);
        const long double step=(junction+resistance*current-supply)/(1+resistance*current/vt);
        const auto before=junction;
        junction-=step;
        if(junction==before || std::abs(step)<1e-19L) break;
    }
    return referenceIe*std::exp((junction-vbe)/vt);
}
long double current(long double q,long double temperature)
{ return emitter(.26L+span*q,22000,temperature); }
long double steadyCharge(long double control,long double temperature)
{ return control-10000*emitter(.26L+span*control,32000,temperature)/span; }
long double derivative(long double q,long double target,long double temperature)
{ return (target-q)/.001L-current(q,temperature)/(span*.1e-6L); }
long double advance(long double q,long double target,long double seconds,long double temperature,int steps=16)
{
    const long double dt=seconds/steps;
    for(int step=0;step<steps;++step)
    {
        const auto a=derivative(q,target,temperature);
        const auto b=derivative(q+dt*a/2,target,temperature);
        const auto c=derivative(q+dt*b/2,target,temperature);
        const auto d=derivative(q+dt*c,target,temperature);
        q+=dt*(a+2*b+2*c+d)/6;
    }
    return q;
}
double gainAt(long double q,long double temperature,long double service)
{
    const auto ie=current(q,temperature);
    const auto control=q+10000*ie/span;
    return control>.001L ? static_cast<double>(ie/service) : 0;
}
double slopeAt(long double q,long double target,long double dt,long double temperature,long double service)
{
    const auto ie=current(q,temperature);
    const auto control=q+10000*ie/span;
    return Reconstruction::gainSlope(gainAt(q,temperature,service),control,target,dt,span,
        referenceVt*(temperature+273.15L)/298.15L,service);
}

void independentOracle()
{
    for(double temperature:{25.,63.})
        for(auto pair:{std::array<double,2>{.025,1},std::array<double,2>{1,.025}})
        {
            const auto q=steadyCharge(pair[0],temperature);
            require(std::abs(advance(q,pair[1],1.L/8000,temperature)
                -advance(q,pair[1],1.L/8000,temperature,64))<1e-7,
                "independent C58 fine-step oracle did not converge");
        }
    for(double rate:{8000.,44100.,48000.,96000.})
    {
        double oldSquared=0,newSquared=0,maxError=0;
        for(double temperature:{25.,41.37,63.})
            for(auto pair:{std::array<double,2>{.025,1},std::array<double,2>{1,.025},
                           std::array<double,2>{.15,.8},std::array<double,2>{.8,.15}})
                for(double event:{0.,.17,.53,1.})
                {
                    const long double dt=1/rate;
                    const auto service=emitter(.26L+span,32000,temperature);
                    long double q=steadyCharge(pair[0],temperature);
                    for(int frame=0;frame<static_cast<int>(rate*.004);++frame)
                    {
                        const auto begin=q;
                        const double split=frame==0 ? event : 0;
                        const double previous=frame==0 ? pair[0] : pair[1];
                        const auto atEvent=advance(begin,previous,dt*split,temperature);
                        q=advance(atEvent,pair[1],dt*(1-split),temperature);
                        const double first=gainAt(begin,temperature,service),last=gainAt(q,temperature,service);
                        Reconstruction interval;
                        interval.startGain=static_cast<float>(first);
                        interval.startSlope=static_cast<float>(slopeAt(begin,previous,dt,temperature,service));
                        interval.endSlope=static_cast<float>(slopeAt(q,pair[1],dt,temperature,service));
                        interval.eventPosition=static_cast<float>(split);
                        interval.eventGain=static_cast<float>(gainAt(atEvent,temperature,service));
                        interval.eventBeforeSlope=static_cast<float>(slopeAt(atEvent,previous,dt,temperature,service));
                        interval.eventAfterSlope=static_cast<float>(slopeAt(atEvent,pair[1],dt,temperature,service));
                        for(double position:{.25,.5,.75,1.})
                        {
                            const auto reference=position<split
                                ? advance(begin,previous,dt*position,temperature)
                                : advance(atEvent,pair[1],dt*(position-split),temperature);
                            const double expected=gainAt(reference,temperature,service);
                            const double old=first+position*(last-first);
                            const double actual=interval.sample(last,position);
                            oldSquared+=(old-expected)*(old-expected);
                            newSquared+=(actual-expected)*(actual-expected);
                            maxError=std::max(maxError,std::abs(actual-expected));
                            require(actual>=std::min(first,last)-1e-7 && actual<=std::max(first,last)+1e-7,
                                "reconstruction escaped physical monotone current bounds");
                        }
                        require(interval.sample(last,1)==last,"endpoint gain changed");
                    }
                }
        const double improvement=10*std::log10(oldSquared/newSquared);
        std::cout<<"VCA "<<rate<<" Hz oracle improvement "<<improvement
                 <<" dB, maximum gain error "<<maxError<<'\n';
        require(improvement>20,"physical VCA reconstruction did not improve fine-step oracle by20dB");
        require(maxError<(rate==8000 ? .006 : rate==96000 ? .000025 : .0002),
            "VCA reconstruction error exceeds qualified bound");
    }
    Reconstruction settled;
    settled.startGain=.5f;settled.eventPosition=2;
    for(double position:{0.,.17,.5,.99,1.})
        require(settled.sample(.5,position)==.5,"settled VCA gain changed");
}

std::unique_ptr<Engine> makeEngine(bool enabled)
{
    auto e=std::make_unique<Engine>();
    require(youknow::ProductHardwareRealismProfile::tryConfigureBeforePrepare(*e),"profile rejected");
    youknow::EngineParameters p;
    youknow::ProductHardwareRealismProfile::applyTo(p);
    p.enableVoiceVcaControlReconstruction=enabled;
    p.enableVcfSettlingReconstruction=false;
    p.enableContinuousChorusMuteLoading=false;
    p.calibration=0;p.aging=0;p.chorus=youknow::ChorusMode::Off;
    p.enableOtaShotNoise=false;p.enableCardJohnsonFloor=false;
    p.enableCommonVcaNoise=false;p.chorusNoise=0;p.noiseLevel=0;
    p.vcaMode=youknow::VcaMode::Gate;p.cutoff=.7f;p.resonance=0;
    p.sawEnabled=true;p.pulseEnabled=false;p.subLevel=0;
    e->setParameters(p);e->setInitialCircuitRate(youknow::CircuitRate::Normal);
    e->prepare(48000,64,1);
    return e;
}

void productionWitness()
{
    auto before=makeEngine(false),after=makeEngine(true);
    before->noteOn(60,1);after->noteOn(60,1);
    double difference=0,energy=0;int intervals=0,events=0;
    for(int frame=0;frame<2400;++frame)
    {
        if(frame==900){before->noteOff(60);after->noteOff(60);}
        float al,ar,bl,br;
        before->process(&al,&ar,1);after->process(&bl,&br,1);
        require(Probe::state(*before)==Probe::state(*after),"reconstruction changed physical C58 endpoints/service gain");
        difference+=(al-bl)*(al-bl);energy+=al*al;
        const auto interval=Probe::interval(*after);
        if(interval.valid())
        {
            ++intervals;
            events+=interval.eventPosition<=1;
            const auto context=Probe::context(*after);
            require(context.eventPosition==interval.eventPosition && context.startGain==interval.startGain
                &&context.startSlope==interval.startSlope&&context.endSlope==interval.endSlope,
                "production FIR transport lost physical VCA interval");
        }
    }
    require(intervals>100 && events>0,"production converter path did not reconstruct VCA control");
    require(difference>1e-15 && difference<energy*.001,"VCA reconstruction route is disconnected or excessive");
    std::cout<<"Production VCA residual "<<10*std::log10(difference/energy)
             <<" dBr; intervals="<<intervals<<" fractional writes="<<events
             <<" engine bytes="<<sizeof(Engine)<<" context bytes="<<Probe::contextBytes()<<'\n';
    after->reset();
    require(!Probe::interval(*after).valid(),"cold reset retained VCA interval");
}

void blockPartition()
{
    const auto render=[](bool singles) {
        auto e=makeEngine(true);e->noteOn(60,1);
        std::vector<float> out(4096*2);
        for(int frame=0;frame<4096;)
        {
            if(frame==900)e->noteOff(60);
            int count=singles ? 1 : std::min(64,4096-frame);
            if(frame<900)count=std::min(count,900-frame);
            std::array<float,64> l{},r{};
            e->process(l.data(),r.data(),count);
            for(int i=0;i<count;++i){out[2*(frame+i)]=l[i];out[2*(frame+i)+1]=r[i];}
            frame+=count;
        }
        return out;
    };
    require(render(true)==render(false),"physical VCA reconstruction depends on host block partition");
}

void cpuCost()
{
    // Interleaved repeated same-binary comparison at the shipped Normal grid.
    // These are wall-clock medians, not real-time deadline guarantees.
    std::array<std::unique_ptr<Engine>,2> engines{makeEngine(false),makeEngine(true)};
    for(auto& e:engines) for(int key:{48,55,60,64,67,72}) e->noteOn(key,1);
    std::array<float,64> l{},r{};
    for(auto& e:engines) for(int block=0;block<128;++block)e->process(l.data(),r.data(),64);
    std::array<std::vector<double>,2> timings;
    for(int run=0;run<8;++run)for(int offset=0;offset<2;++offset)
    {
        const int variant=(run+offset)&1;
        auto start=std::chrono::steady_clock::now();
        for(int block=0;block<512;++block) engines[variant]->process(l.data(),r.data(),64);
        timings[variant].push_back(std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());
    }
    for(auto& t:timings)std::sort(t.begin(),t.end());
    const double a=(timings[0][3]+timings[0][4])/2,b=(timings[1][3]+timings[1][4])/2;
    std::cout<<"VCA Normal six-voice CPU median "<<a<<" -> "<<b<<" seconds, ratio "<<b/a<<'\n';
}
}
int main(int argc,char** argv)
{
    try {
        if(argc==2 && std::strcmp(argv[1],"--benchmark")==0){cpuCost();return 0;}
        independentOracle();productionWitness();blockPartition();
        std::cout<<"VCA control reconstruction tests passed\n";return 0;
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
