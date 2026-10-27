// Time-varying C25/C28 load qualification. Roland jack board p.15 has a
// 1 uF coupling capacitor, 22k bleed and 39k summer input through the JFET.
// The first oracle integrates that physical scalar current analytically;
// it does not call the production matrix builder/integrator. The second
// exercises the full finite-follower network and physical mute trajectory
// against a converged finer grid. Neither refits hardware coordinates.
// https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=15
#include "DSP/YouKnowChorus.h"
#include "DSP/YouKnowChorusRateIsland.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace youknow {
struct YouKnowTestAccess {
    static double capacitor(Chorus& c,double x,double previous,double current,bool continuous)
    {
        c.wetInputConductanceRatio_=current;
        const auto& t=c.finiteWetTransition(continuous?previous:-1.0);
        double next=t.stateByColumn[5][5]*x;
        for(int column=0;column<5;++column)next+=t.stateByColumn[column][5];
        for(int sample=0;sample<4;++sample)next+=t.driveBySample[sample][5];
        return next;
    }
    static auto transition(Chorus& c,double previous,double current)
    {
        c.wetInputConductanceRatio_=current;
        return c.finiteWetTransition(previous);
    }
    static float post(Chorus& c,bool muted,float held,bool continuous)
    {
        const double previous=c.wetInputConductanceRatio_;
        c.advanceMuteDrive(muted);
        const auto& transition=c.finiteWetTransition(continuous?previous:-1.0);
        c.lineA_.held=held;
        return c.lineA_.process(0.0f,0.0,c.sampleRate_,transition,0.0f)
             * static_cast<float>(c.wetInputConductanceRatio_);
    }
    static const auto& charge(const Chorus& c){return c.lineA_.exactOutputState;}
    static auto nodes(const Chorus& c)
    {return std::array{c.muteDriveNodeVolts_,c.muteDriveHoldVolts_,c.clockMuteVolts_};}
    static auto rng(const Chorus& c)
    {return std::array{c.lineA_.noiseState,c.lineB_.noiseState};}
    static auto fallbacks(const Chorus& c)
    {return c.lineA_.nonlinearOutput.fallbackCount+c.lineB_.nonlinearOutput.fallbackCount;}
};
}
namespace {
using namespace youknow;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void prime(Chorus& c,bool continuous,ChorusMode mode=ChorusMode::One)
{
    float l,r;c.process(0,mode,0,l,r,1,true,true,false,
        ChorusTimingProfile::HardwareEvidence,true,true,false,0,1,true,continuous);
}
// Compare the public island adapter with explicitly selected physical chorus
// stepping on the same interpolated stream, including a complete mute cycle.
void islandMuteLoading()
{
    for(double rate:{48000.,192000.}) for(bool continuous:{false,true})
    {
        auto actual=std::make_unique<Chorus>(),reference=std::make_unique<Chorus>();
        const auto kernel=VoiceVcaAntialias::prepare(rate);
        for(auto* c:{actual.get(),reference.get()})
        {
            require(c->configureSupportProfile(ChorusSupportProfile::Nominal2SA1015),"finite support rejected");
            c->prepare(rate*kernel.factor);
        }
        auto adapter=std::make_unique<ChorusRateIsland>(),oracle=std::make_unique<ChorusRateIsland>();
        ChorusRateIsland::Controls controls;
        controls.timing=ChorusTimingProfile::HardwareEvidence;
        controls.narrow=controls.muteDrive=controls.clockMute=controls.finiteMute=controls.finiteTr5=true;
        controls.continuousMuteLoading=continuous;
        for(int n=0;n<static_cast<int>(.3*rate);++n)
        {
            controls.mode=controls.hardwareMode=n<static_cast<int>(.1*rate)?ChorusMode::One:
                n<static_cast<int>(.2*rate)?ChorusMode::Off:ChorusMode::Two;
            const float input=.02f*static_cast<float>(std::sin(n*.07));
            const auto expected=oracle->processWith(input,controls,kernel,[&](float drive,const auto& c){
                ChorusRateIsland::Result result;
                reference->setHardwareModeSelection(c.hardwareMode);
                reference->process(drive,c.mode,c.noise,result.left,result.right,c.calibration,
                    c.narrow,c.muteDrive,true,c.timing,c.clockMute,c.finiteMute,
                    c.correlatedNoise,c.transferFraction,c.transferCorrelation,c.finiteTr5,continuous);
                result.context={reference->wetInputConductanceRatio(),reference->isWetInputConnected()};
                return result;
            });
            const auto result=adapter->process(input,controls,kernel,*actual,[](float x){return x;});
            require(result.left==expected.left&&result.right==expected.right,
                "island adapter lost continuous mute loading");
            require(YouKnowTestAccess::charge(*actual)==YouKnowTestAccess::charge(*reference),
                "island adapter changed mute capacitor charge");
        }
    }
}
void analyticRamp()
{
    for(double fs:{8000.,48000.,192000.})
    {
        auto c=std::make_unique<Chorus>();c->prepare(fs);
        double old=0,corrected=0,oldError=0,newError=0;
        constexpr double rampSeconds=.002;
        const int count=static_cast<int>(std::llround(rampSeconds*fs));
        for(int n=1;n<=count;++n)
        {
            const double before=static_cast<double>(n-1)/count;
            const double after=static_cast<double>(n)/count;
            old=YouKnowTestAccess::capacitor(*c,old,before,after,false);
            corrected=YouKnowTestAccess::capacitor(*c,corrected,before,after,true);
            const double t=n/fs;
            // x'=(1-x)*(1/22k+g(t)/39k)/1uF, g(t)=t/rampSeconds.
            const double exact=1-std::exp(-(t/22000.+t*t/(2*rampSeconds*39000.))/1e-6);
            oldError=std::max(oldError,std::abs(old-exact));
            newError=std::max(newError,std::abs(corrected-exact));
        }
        std::cout<<fs<<" Hz analytic C25 max error: endpoint "<<oldError
                 <<", continuous "<<newError<<std::endl;
        // The separately qualified 16-cell conductance interpolation has
        // <8e-9 one-step scalar charge error at every supported grid. Its
        // worst-case accumulated bound remains after the time quadrature fix.
        require(newError<count*8e-9,"continuous C25 load failed analytic capacitor-current oracle");
        require(oldError>100*newError,"moving-load reconstruction did not improve the independent oracle");
    }
}
void endpointMaps()
{
    auto c=std::make_unique<Chorus>();
    require(c->configureSupportProfile(ChorusSupportProfile::Nominal2SA1015Nonlinear),"nonlinear support rejected");
    c->prepare(192000);
    for(const auto pair:{std::array{.1,.8},std::array{.8,.1},std::array{.01,0.},std::array{0.,.01}})
    {
        const auto moving=YouKnowTestAccess::transition(*c,pair[0],pair[1]);
        const auto endpoint=YouKnowTestAccess::transition(*c,-1,pair[1]);
        const auto midpoint=YouKnowTestAccess::transition(*c,-1,.5*(pair[0]+pair[1]));
        require(moving.stateByColumn==midpoint.stateByColumn
            &&moving.heldOutputMap.fullIntervalDrive==midpoint.heldOutputMap.fullIntervalDrive,
            "state transport lost its interval load");
        require(moving.outputByState==endpoint.outputByState
            &&moving.outputDirect==endpoint.outputDirect
            &&moving.nonlinear.junctionByState==endpoint.nonlinear.junctionByState
            &&moving.nonlinear.junctionByInput==endpoint.nonlinear.junctionByInput
            &&moving.nonlinear.junctionByCurrent==endpoint.nonlinear.junctionByCurrent
            &&moving.nonlinear.outputByCurrent==endpoint.nonlinear.outputByCurrent,
            "moving load delayed the physical endpoint voltage/current readout");
    }
}
void physicalSwitch()
{
    // The product chorus rate island uses the exact held-output forcing at
    // >=176.4 kHz. Keep every comparison on that same physical-step scheme;
    // lower grids deliberately use a different BLEP/cubic reconstruction.
    for(double fs:{192000.,384000.})
    {
        const int ratio=static_cast<int>(768000/fs);
        auto old=std::make_unique<Chorus>(),candidate=std::make_unique<Chorus>(),fine=std::make_unique<Chorus>();
        for(auto* c:{old.get(),candidate.get(),fine.get()})
        {
            require(c->configureSupportProfile(ChorusSupportProfile::Nominal2SA1015),"finite support rejected");
            c->prepare(c==fine.get()?768000:fs);prime(*c,true);
        }
        double oldError=0,newError=0,oldCharge=0,newCharge=0;
        for(int n=0;n<static_cast<int>(.4*fs);++n)
        {
            const double time=n/fs;
            const bool muted=time<.2;
            const float source=time<.08?0.0f:time<.3?.01f:.02f;
            const auto a=YouKnowTestAccess::post(*old,muted,source,false);
            const auto b=YouKnowTestAccess::post(*candidate,muted,source,true);
            float reference=0;
            for(int sub=0;sub<ratio;++sub)reference=YouKnowTestAccess::post(*fine,muted,source,true);
            oldError=std::max(oldError,std::abs(static_cast<double>(a-reference)));
            newError=std::max(newError,std::abs(static_cast<double>(b-reference)));
            oldCharge=std::max(oldCharge,std::abs(YouKnowTestAccess::charge(*old)[5]-YouKnowTestAccess::charge(*fine)[5]));
            newCharge=std::max(newCharge,std::abs(YouKnowTestAccess::charge(*candidate)[5]-YouKnowTestAccess::charge(*fine)[5]));
        }
        std::cout<<fs<<" Hz physical switch max wet error: "<<oldError<<" -> "<<newError
                 <<"; C25 charge error "<<oldCharge<<" -> "<<newCharge<<std::endl;
        require(newError<oldError*.25&&newCharge<oldCharge*.25,
            "continuous physical switch failed finer-grid voltage/charge comparison");
        require(YouKnowTestAccess::nodes(*old)==YouKnowTestAccess::nodes(*candidate),"load reconstruction moved physical control timing");
        require(YouKnowTestAccess::rng(*old)==YouKnowTestAccess::rng(*candidate),"load reconstruction consumed different noise draws");
    }
}
void steadyAndReset()
{
    for(const auto mode:{ChorusMode::Off,ChorusMode::One,ChorusMode::Two})
    {
        auto a=std::make_unique<Chorus>(),b=std::make_unique<Chorus>();
        for(auto* c:{a.get(),b.get()})
        {
            require(c->configureSupportProfile(ChorusSupportProfile::Nominal2SA1015Nonlinear),"nonlinear support rejected");
            c->prepare(192000);
        }
        for(int repeat=0;repeat<2;++repeat)
        {
            a->reset();b->reset();
            for(int n=0;n<12000;++n)
            {
                const float input=.01f*std::sin(n*.031f);
                float al,ar,bl,br;
                a->process(input,mode,.3f,al,ar,1,true,true,true,
                    ChorusTimingProfile::HardwareEvidence,true,true,true,.9171323f,1,true,false);
                b->process(input,mode,.3f,bl,br,1,true,true,true,
                    ChorusTimingProfile::HardwareEvidence,true,true,true,.9171323f,1,true,true);
                require(al==bl&&ar==br,"steady/cold/reset chorus audio changed");
            }
            require(YouKnowTestAccess::fallbacks(*a)==0&&YouKnowTestAccess::fallbacks(*b)==0,"nonlinear solve recovered during stable qualification");
        }
    }
}
void movingNonlinear()
{
    auto a=std::make_unique<Chorus>(),b=std::make_unique<Chorus>();
    for(auto* c:{a.get(),b.get()})
    {
        require(c->configureSupportProfile(ChorusSupportProfile::Nominal2SA1015Nonlinear),"nonlinear support rejected");
        c->prepare(192000);
    }
    double maxDelta=0;
    for(int n=0;n<96000;++n)
    {
        // A genuine On/Off/On transition, plus a reversed command before
        // the capacitors settle. Both copies receive identical audio/noise.
        const auto mode=n<5760?ChorusMode::One:n<38400?ChorusMode::Off
            :n<48000?ChorusMode::Two:n<53760?ChorusMode::Off:ChorusMode::One;
        const float input=.01f*std::sin(n*.0213f)+.003f*std::sin(n*.131f);
        float al,ar,bl,br;
        a->process(input,mode,.2f,al,ar,1,true,true,true,
            ChorusTimingProfile::HardwareEvidence,true,true,true,.9171323f,1,true,false);
        b->process(input,mode,.2f,bl,br,1,true,true,true,
            ChorusTimingProfile::HardwareEvidence,true,true,true,.9171323f,1,true,true);
        require(std::isfinite(bl)&&std::isfinite(br),"nonlinear moving load produced nonfinite output");
        require(YouKnowTestAccess::nodes(*a)==YouKnowTestAccess::nodes(*b),"moving load altered control capacitor chronology");
        require(YouKnowTestAccess::rng(*a)==YouKnowTestAccess::rng(*b),"moving load altered physical clock/noise chronology");
        if(n<5760)require(al==bl&&ar==br,"moving-load selection changed settled audio");
        maxDelta=std::max({maxDelta,std::abs(static_cast<double>(al-bl)),std::abs(static_cast<double>(ar-br))});
    }
    require(maxDelta>0,"continuous load absent from actual Chorus processing");
    require(YouKnowTestAccess::fallbacks(*a)==0&&YouKnowTestAccess::fallbacks(*b)==0,"nonlinear moving-load solve recovered");
    std::cout<<"Actual nonlinear switched chorus maximum correction "<<maxDelta<<"\n";
}
}
int main()
{
    try{islandMuteLoading();analyticRamp();endpointMaps();physicalSwitch();steadyAndReset();movingNonlinear();}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    std::cout<<"Continuous chorus mute-load checks passed\n";
}
