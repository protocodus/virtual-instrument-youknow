#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace {bool guard=false;unsigned allocations=0;}
void* operator new(std::size_t n){if(guard)++allocations;if(void*p=std::malloc(n))return p;throw std::bad_alloc{};}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void*p)noexcept{std::free(p);}
void operator delete[](void*p)noexcept{std::free(p);}
void operator delete(void*p,std::size_t)noexcept{std::free(p);}
void operator delete[](void*p,std::size_t)noexcept{std::free(p);}
namespace youknow {
struct YouKnowTestAccess {
    static double rate(const YouKnowEngine&e){return e.oversampledRate_;}
    static double vca(YouKnowEngine&e,float gain,float drive,float warmup=0){auto&v=e.voices_[0];v.active=true;v.vca=gain;v.vcaInputTrim=1;
        e.thermalWarmupFraction_=warmup;e.cards_[0].vcaInputCouplingG=0;v.vcaInputCoupling.state=0;return e.finishVoiceFilter(v,drive);}
    static auto random(const YouKnowEngine&e){return e.voices_[0].vcaShotRandom;}
    static auto kernel(const YouKnowEngine&e){return e.voiceVcaAntialiasKernel_;}
    static void enableTail(YouKnowEngine&e){e.activeParameters_.enableBa662TailMirrorNoise=true;}
    static void enableOta(YouKnowEngine&e,bool enabled){e.activeParameters_.enableOtaShotNoise=enabled;}
    static void context(YouKnowEngine&e,int frame,float T){e.voices_[0].vcaAntialias.temperatures[static_cast<std::size_t>(frame)&63]=T;}
    static float context(const YouKnowEngine&e,int frame){return e.voices_[0].vcaAntialias.temperatures[static_cast<std::size_t>(frame)&63];}
    static double resonanceDiffusion(YouKnowEngine&e,double k,double y){auto&v=e.voices_[0];auto&f=v.filter;
        f.reset();f.offsetVoltage.fill(0);f.resonanceOffsetVolts=0;f.inputCompensationCoefficient=0;f.gScale.fill(1);f.resonanceHeadroomFollowsStage=false;
        const double loopH=2*.026*100000/1500;f.state[3]=loopH*std::atanh(y);
        YouKnowEngine::VoiceFilterFrame frame;frame.shotNoiseOrigin=f.state;frame.shotNoiseHeadroom=2*.026*(68000+560)/560;
        frame.shotNoiseFeedback=k;frame.shotNoiseOmega=2*std::numbers::pi*248/e.oversampledRate_;
        (void)e.applyVoiceFilterShotNoise(v,frame,static_cast<float>(f.state[3]));return v.filterShotCache.d[0];}
    static auto streams(const YouKnowEngine&e){std::array<std::tuple<std::uint32_t,double,bool,std::uint32_t,double,bool,std::uint32_t>,YouKnowEngine::maxVoices>r{};
        for(std::size_t i=0;i<r.size();++i){const auto&v=e.voices_[i];r[i]={v.filterShotRandom.state,v.filterShotRandom.spare,v.filterShotRandom.hasSpare,v.vcaShotRandom.state,v.vcaShotRandom.spare,v.vcaShotRandom.hasSpare,v.noiseState};}return std::pair{e.noiseState_,r};}
};
}
namespace {
using namespace youknow;
void require(bool ok,const char*msg){if(!ok)throw std::runtime_error(msg);}
using M=std::array<std::array<long double,3>,3>;using V=std::array<long double,3>;
V solve(M a,V b){for(std::size_t i=0;i<3;++i){std::size_t pivot=i;for(std::size_t j=i+1;j<3;++j)if(std::abs(a[j][i])>std::abs(a[pivot][i]))pivot=j;
    std::swap(a[i],a[pivot]);std::swap(b[i],b[pivot]);const auto d=a[i][i];for(std::size_t j=i;j<3;++j)a[i][j]/=d;b[i]/=d;
    for(std::size_t k=0;k<3;++k)if(k!=i){const auto f=a[k][i];for(std::size_t j=i;j<3;++j)a[k][j]-=f*a[i][j];b[k]-=f*b[i];}}return b;}
// Independent three-node transistor KCL: shared base, sense emitter and
// output emitter. Quiet forced collector current is row0; rows1/2 are
// emitter KCL with separate 500ohm Norton thermal sources. Stamp each of
// the two collector and two resistor current sources, solve and observe the
// output collector. No production degeneration/source-weight formula used.
double nodal(double current,double y,double kelvin){if(current==0)return 0;constexpr long double q=1.602176634e-19L,k=1.380649e-23L,R=500;
    const long double gm=current*q/(k*kelvin);const M matrix{{{{gm,-gm,0}},{{-gm,gm+1/R,0}},{{-gm,0,gm+1/R}}}};
    const std::array<V,4> forcing{{{{-1,1,0}},{{0,0,1}},{{0,-1,0}},{{0,0,-1}}}};long double psd=0;
    for(std::size_t source=0;source<4;++source){const auto v=solve(matrix,forcing[source]);const auto out=gm*(v[0]-v[2])+(source==1?1:0);
        psd+=out*out*(source<2?2*q*current:4*k*kelvin/R);}
    return psd*y*y;}
void currentOracle(){double worst=0;for(double current:{1e-12,1e-9,1e-6,.000302079,.002})for(double y:{-1.,-.8,-.1,0.,.1,.8,1.})for(double T:{273.15,298.15,313.15,358.15}){
    const double reference=nodal(current,y,T),actual=Ba662Noise::tailMirrorCurrentPsd(current,y,T);if(reference>0){worst=std::max(worst,std::abs(actual/reference-1));require(std::abs(actual/reference-1)<2e-12,"tail source PSD disagrees with collector/resistor KCL");}else require(actual==0,"balanced pair steered tail noise");}
    require(Ba662Noise::tailMirrorCurrentPsd(0,1,298.15)==0,"closed tail generated mirror current");
    std::cout<<"four-source transistor/Johnson KCL relative error="<<worst<<"; sign-even steering and thermal grid passed\n";}
std::unique_ptr<YouKnowEngine> make(bool tail,double fs=48000,int quality=1,bool aa=false){auto e=std::make_unique<YouKnowEngine>();EngineParameters p;
    p.enableOtaShotNoise=true;p.enableBa662OutputMirrorNoise=true;p.enableBa662TailMirrorNoise=tail;p.enableVoiceVcaAntialias=aa;
    p.enableVoiceVcaTemperature=false;p.enableSpatialThermalGradient=false;p.calibration=1;p.enableVcfEarlyEffect=false;p.enableCardJohnsonFloor=false;p.chorus=ChorusMode::Off;p.chorusNoise=p.noiseLevel=0;
    e->setParameters(p);e->prepare(fs,128,quality);return e;}
void engineDensity(){constexpr double q=1.602176634e-19,R=47000,convert=1./2.6;double worst=0;const double full=YouKnowEngine::VoiceVcaSignalLaw::fullControlTailAmps,service=YouKnowEngine::VoiceVcaControlLaw::gain(4064.f/4095.f);
    for(double host:{8000.,44100.,48000.,96000.,192000.})for(int quality:{1,2,4}){
        for(double sign:{-1.,1.}){auto e=make(true,host,quality);const double fs=YouKnowTestAccess::rate(*e);const float drive=static_cast<float>(sign*YouKnowEngine::VoiceVcaSignalLaw::headroomVolts*.8);const double y=std::tanh(drive/YouKnowEngine::VoiceVcaSignalLaw::headroomVolts),tail=full/service*.4f;
            const double psd=2*q*tail*(1-y*y)+2*q*tail*(3-y)+nodal(tail,y,298.15),expected=psd*R*R*fs/2*convert*convert;
            const float centre=YouKnowEngine::VoiceVcaSignalLaw::shape(drive)*.4f*YouKnowEngine::VoiceVcaSignalLaw::serviceGain()*static_cast<float>(convert);double power=0;constexpr int count=32768;
            for(int i=0;i<count;++i){const double x=YouKnowTestAccess::vca(*e,.4f,drive)-centre;power+=x*x/count;}
            worst=std::max(worst,std::abs(power/expected-1));require(std::abs(power/expected-1)<.035,"actual steered VCA tail density/rate is wrong");
            for(int i=0;i<30;++i)require(YouKnowTestAccess::vca(*e,0,drive)==0,"tail noise bypassed closed VCA current");}
        for(double y:{-.8,0.,.8}){auto raw=make(false,host,quality),tail=make(true,host,quality);constexpr double k=1.2,H=2*.026*(68000+560)/560,loopH=2*.026*100000/1500;
            const double difference=YouKnowTestAccess::resonanceDiffusion(*tail,k,y)-YouKnowTestAccess::resonanceDiffusion(*raw,k,y);const double sensitivity=1-std::pow(std::tanh(-k*loopH*y/H),2),w=2*std::numbers::pi*248;
            const double expected=nodal(k*loopH/68000,-y,298.15)*std::pow(w*sensitivity*68000,2)/(2*YouKnowTestAccess::rate(*tail));
            if(expected>0)require(std::abs(difference/expected-1)<3e-6,"RES tail-current/steering mapping is wrong");else require(difference==0,"balanced RES has tail mirror noise");}
        auto raw=make(false,host,quality,true),tail=make(true,host,quality,true);for(int i=0;i<100;++i)require(YouKnowTestAccess::vca(*raw,.3f,0)==YouKnowTestAccess::vca(*tail,.3f,0),"balanced VCA changed when tail contribution is zero");}
    std::cout<<"actual VCA variance relative="<<worst<<"; all rates/qualities, drive signs, current gating and RES passed\n";}
void thermalTimeline(){double worst=0;constexpr double q=1.602176634e-19,R=47000;const double full=YouKnowEngine::VoiceVcaSignalLaw::fullControlTailAmps,service=YouKnowEngine::VoiceVcaControlLaw::gain(4064.f/4095.f);
    for(int quality:{1,2,4}){auto e=make(true,48000,quality,true);auto random=YouKnowTestAccess::random(*e);const auto kernel=YouKnowTestAccess::kernel(*e);const double fs=YouKnowTestAccess::rate(*e);
        constexpr int count=900;std::vector<float>gains(count),drives(count),temperatures(count),local(count*kernel.factor);VoiceVcaAntialias context;std::size_t calls=0;
        for(int i=0;i<count;++i){gains[i]=i<150?0:i<350?.4f:i<500?.8f:i<650?.05f:0;drives[i]=static_cast<float>(YouKnowEngine::VoiceVcaSignalLaw::headroomVolts*(i&64?.85:-.7));temperatures[i]=25.f+15.f*(i&32?1.f:0.f)+273.15f;
            // Context-only timestamp proof at exact precision, separately
            // from full-engine output rounding. Deliberate rapid warmup
            // steps stress chronology; they are not a physical thermal law.
            (void)context.processWithTemperatureNoise(drives[i],gains[i],temperatures[i],kernel,[](float){return 0.f;},[&](double,double g,double T){const int n=static_cast<int>(calls/kernel.factor),phase=static_cast<int>(calls%kernel.factor);++calls;
                const int at=kernel.factor>1?n-24:n;const double a=at>=0?temperatures[at]:0,b=at+1>=0&&at+1<=n?temperatures[at+1]:0;
                const double ga=at>=0?gains[at]:0,gb=at+1>=0&&at+1<=n?gains[at+1]:0,f=kernel.factor>1?static_cast<double>(phase)/kernel.factor:0;
                require(T==a+f*(b-a)&&g==ga+f*(gb-ga),"temperature context does not share gain timestamp");return 0.;});
            for(int phase=0;phase<kernel.factor;++phase){const int at=kernel.factor>1?i-24:i;const double f=kernel.factor>1?static_cast<double>(phase)/kernel.factor:0;
                const double ga=at>=0?gains[at]:0,gb=at+1>=0&&at+1<=i?gains[at+1]:0,T0=at>=0?temperatures[at]:0,T1=at+1>=0&&at+1<=i?temperatures[at+1]:0,g=ga+f*(gb-ga),T=T0+f*(T1-T0);double drive=at>=0?drives[at]:0;
                if(phase>0){drive=0;for(int tap=0;tap<VoiceVcaAntialias::delaySamples;++tap)if(i>=tap)drive+=kernel.interpolation[phase][tap]*drives[i-tap];}
                const double tail=full/service*std::max(0.,g),y=std::tanh(drive/YouKnowEngine::VoiceVcaSignalLaw::headroomVolts),normal=random.next();
                const double psd=2*q*tail*(1-y*y)+2*q*tail*(3-y)+(tail>0?nodal(tail,y,T):0);
                const double noise=normal*R/YouKnowEngine::VoiceVcaSignalLaw::serviceGain()*std::sqrt(psd*fs*kernel.factor/2);
                local[i*kernel.factor+phase]=static_cast<float>(YouKnowEngine::VoiceVcaSignalLaw::shape(static_cast<float>(drive))*g+noise);}
            double expected=local[i*kernel.factor];if(kernel.factor>1){expected=0;for(int tap=0;tap<kernel.taps;++tap){const int at=i*kernel.factor-tap;if(at>=0)expected+=kernel.decimation[tap]*local[at];}}
            expected*=YouKnowEngine::VoiceVcaSignalLaw::serviceGain()*(1.f/2.6f);const double actual=YouKnowTestAccess::vca(*e,gains[i],drives[i],i&32?1.f:0.f);worst=std::max(worst,std::abs(actual-expected));require(std::abs(actual-expected)<9e-7,"actual tail-noise current/temperature/FIR history is wrong");}}
    // Warm tail-off AA audio and its currents, then select the tail component
    // live. A reference has each preceding temperature independently stamped
    // into the ring. A newly enabled feature must not read zero/stale T.
    for(bool parentEnabled:{false,true}) {
    auto live=make(false,48000,1,true),reference=make(false,48000,1,true);
    YouKnowTestAccess::enableOta(*live,parentEnabled);YouKnowTestAccess::enableOta(*reference,parentEnabled);
    for(int i=0;i<80;++i){const float f=i&16?1.f:0.f,T=25.f+15.f*f+273.15f;
        require(YouKnowTestAccess::vca(*live,.4f,8,f)==YouKnowTestAccess::vca(*reference,.4f,8,f),"tail-off histories disagree");
        require(YouKnowTestAccess::context(*live,i)==T,"disabled tail failed to keep AA temperature history warm");YouKnowTestAccess::context(*reference,i,T);}
    YouKnowTestAccess::enableTail(*live);YouKnowTestAccess::enableTail(*reference);
    YouKnowTestAccess::enableOta(*live,true);YouKnowTestAccess::enableOta(*reference,true);
    for(int i=0;i<100;++i)require(YouKnowTestAccess::vca(*live,.4f,8,i&16?1.f:0.f)==YouKnowTestAccess::vca(*reference,.4f,8,i&16?1.f:0.f),"live tail selection used stale delayed T");
    }
    std::cout<<"temperature/gain exact timestamp + actual full-current FIR oracle max="<<worst<<"; live selection history passed\n";}
struct Render{std::vector<float>audio;decltype(YouKnowTestAccess::streams(std::declval<YouKnowEngine&>()))streams;};
Render render(bool tail,bool ota,float character,double fs,int quality,int block){auto e=std::make_unique<YouKnowEngine>();EngineParameters p;ProductFidelityProfile::applyTo(p);p.enableBa662TailMirrorNoise=tail;p.enableOtaShotNoise=ota;p.calibration=character;p.chorus=ChorusMode::Off;p.chorusNoise=p.noiseLevel=0;
    p.attack=p.release=0;p.sustain=1;p.cutoff=.55f;p.resonance=.4f;p.polyphony=16;e->setParameters(p);e->prepare(fs,block,quality);e->noteOn(60,1);Render result;result.audio.resize(2048);std::vector<float>right(2048);int at=0;
    for(int segment=0;segment<4;++segment){if(segment==1)e->allNotesOff();if(segment==2)for(int i=0;i<16;++i)e->noteOn(48+i,1);if(segment==3){e->allNotesOff();e->setOversamplingFactor(quality==1?4:1);}const int end=(segment+1)*512;
        while(at<end){const int n=std::min(block,end-at);guard=true;e->process(result.audio.data()+at,right.data()+at,n);guard=false;at+=n;}}
    for(float x:result.audio)require(std::isfinite(x),"tail lifecycle produced nonfinite audio");result.streams=YouKnowTestAccess::streams(*e);return result;}
void lifecycle(){require(!EngineParameters{}.enableBa662TailMirrorNoise,"raw tail flag changed");EngineParameters p;ProductFidelityProfile::applyTo(p);require(p.enableBa662TailMirrorNoise,"product omitted tail source");
    for(double rate:{8000.,44100.,48000.,96000.,192000.})for(int quality:{1,2,4}){const auto zero=render(false,true,0,rate,quality,128),onZero=render(true,true,0,rate,quality,128);require(zero.audio==onZero.audio&&zero.streams==onZero.streams,"tail changed Character0 reference");
        const auto off=render(false,true,1,rate,quality,137),on=render(true,true,1,rate,quality,137),single=render(true,true,1,rate,quality,1);require(on.audio==single.audio&&on.streams==single.streams,"tail block partition changed");require(off.streams==on.streams,"tail changed stream chronology");require(off.audio!=on.audio,"tail component did not reach engine");
        require(render(false,false,1,rate,quality,128).audio==render(true,false,1,rate,quality,128).audio,"tail ignored OTA parent flag");}
    auto used=make(true),fresh=make(true);for(int i=0;i<35;++i)(void)YouKnowTestAccess::vca(*used,1,7);used->reset();require(YouKnowTestAccess::streams(*used)==YouKnowTestAccess::streams(*fresh),"tail hard-reset chronology failed");require(allocations==0,"tail process allocated");
    std::cout<<"Character0/parent-off exact, RNG/reset/retirement/idle/quality/block/noalloc passed\n";}
double cost(bool tail,int voices,int quality){auto e=std::make_unique<YouKnowEngine>();EngineParameters p;ProductFidelityProfile::applyTo(p);p.enableBa662TailMirrorNoise=tail;p.chorus=ChorusMode::Off;p.chorusNoise=p.noiseLevel=0;p.polyphony=voices;p.attack=0;p.sustain=1;p.cutoff=.55f;p.resonance=.4f;p.calibration=.7f;
    e->setParameters(p);e->prepare(48000,128,quality);for(int i=0;i<voices;++i)e->noteOn(48+2*i,1);std::array<float,128>left{},right{};for(int i=0;i<30;++i)e->process(left.data(),right.data(),128);
    const auto start=std::chrono::steady_clock::now();for(int i=0;i<150;++i)e->process(left.data(),right.data(),128);return std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();}
void costs(){for(int voices:{6,16})for(int quality:{1,4}){double before=0,after=0;for(int repeat=0;repeat<3;++repeat){before+=cost(false,voices,quality);after+=cost(true,voices,quality);}std::cout<<"CPU "<<voices<<" voices quality"<<quality<<": "<<before/3<<" -> "<<after/3<<" s/.4s, ratio="<<after/before<<"\n";}}
}
int main(){std::cout<<std::unitbuf;try{currentOracle();engineDensity();thermalTimeline();lifecycle();costs();}catch(const std::exception&e){guard=false;std::cerr<<e.what()<<'\n';return 1;}}
