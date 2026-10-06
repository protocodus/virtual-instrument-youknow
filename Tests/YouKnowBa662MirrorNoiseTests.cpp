#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numbers>
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
    static double vca(YouKnowEngine&e,float gain){auto&v=e.voices_[0];v.active=true;v.vca=gain;v.vcaInputTrim=1;return e.finishVoiceFilter(v,0);}
    static auto random(const YouKnowEngine&e){return e.voices_[0].vcaShotRandom;}
    static auto kernel(const YouKnowEngine&e){return e.voiceVcaAntialiasKernel_;}
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
// Independent transistor nodal oracle. Three Wilson mirrors plus AS662 p3's
// series level-shift diode D2 have seven free input/base/collector nodes.
// All ten collector/diode sources are stamped individually into KCL. IN-
// first drives a PNP mirror, then the lower NPN mirror through D2; IN+ uses
// the other PNP mirror. Supplies and final output are AC-ground. D2's own
// source changes a free collector voltage, not output current in this limit.
// This additional AS662 node is a topology cross-check, not an identification
// of a separate original-BA662 shot-noise amplitude.
// No closed-form mirror source weights or production PSD helper is used.
constexpr std::size_t nodeCount=7;
using M=std::array<std::array<long double,nodeCount>,nodeCount>;using V=std::array<long double,nodeCount>;
V solve(M a,V b){for(std::size_t i=0;i<nodeCount;++i){std::size_t pivot=i;for(std::size_t j=i+1;j<nodeCount;++j)if(std::abs(a[j][i])>std::abs(a[pivot][i]))pivot=j;
    require(std::abs(a[pivot][i])>1e-40L,"singular nodal oracle");std::swap(a[i],a[pivot]);std::swap(b[i],b[pivot]);const auto d=a[i][i];
    for(std::size_t j=i;j<nodeCount;++j)a[i][j]/=d;b[i]/=d;
    for(std::size_t k=0;k<nodeCount;++k)if(k!=i){const auto f=a[k][i];for(std::size_t j=i;j<nodeCount;++j)a[k][j]-=f*a[i][j];b[k]-=f*b[i];}}
    return b;}
struct Bjt{int collector,emitter,base;long double gm,current;};
double nodal(double current,double y){const long double ip=current*(1+y)/2,im=current*(1-y)/2,vt=.02569257912108585L;
    std::array<Bjt,10> devices{};M matrix{};
    for(int mirror=0;mirror<3;++mirror){const int a=mirror*2,b=a+1;const long double i=mirror==0?ip:im;
        devices[mirror*3]={mirror==2?6:a,-1,b,i/vt,i};devices[mirror*3+1]={b,-1,b,i/vt,i};devices[mirror*3+2]={mirror==1?4:-1,b,a,i/vt,i};}
    devices[9]={4,6,4,im/vt,im}; // D2: upper mirror output -> sense collector.
    const auto stamp=[&](int row,int col,long double x){if(row>=0&&col>=0)matrix[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)]+=x;};
    for(const auto&t:devices){stamp(t.collector,t.base,t.gm);stamp(t.collector,t.emitter,-t.gm);stamp(t.emitter,t.base,-t.gm);stamp(t.emitter,t.emitter,t.gm);}
    long double psd=0;
    for(std::size_t source=0;source<devices.size();++source){V forcing{};const auto&t=devices[source];
        if(t.collector>=0)forcing[t.collector]-=1;if(t.emitter>=0)forcing[t.emitter]+=1;const auto v=solve(matrix,forcing);
        long double out=0;for(std::size_t index:{std::size_t{2},std::size_t{8}}){const auto&o=devices[index];out-=o.gm*(v[o.base]-v[o.emitter])+(index==source?1:0);}
        if(source==9)require(std::abs(out)<1e-14L,"D2 level-shift source escaped ideal mirror feedback");
        psd+=out*out*2*1.602176634e-19L*t.current;}
    return psd;}
void transistorOracle(){double worst=0;for(double tail:{1e-9,1e-6,.000302079})for(double y:{-.999,-.8,-.2,0.,.2,.8,.999}){
    const double reference=nodal(tail,y),actual=Ba662Noise::outputMirrorCurrentPsd(tail,y);worst=std::max(worst,std::abs(actual/reference-1));require(std::abs(actual/reference-1)<1e-12,"mirror PSD disagrees with individual transistor KCL");}
    require(Ba662Noise::outputMirrorCurrentPsd(0,.5)==0,"zero tail has mirror noise");
    require(Ba662Noise::outputMirrorCurrentPsd(1,-1)==2*Ba662Noise::outputMirrorCurrentPsd(1,1),"branch polarity routing lost");
    std::cout<<"ten-source transistor/level-shift KCL relative error="<<worst<<"; quiet input +6.0206dB combined collector PSD\n";}
std::unique_ptr<YouKnowEngine> make(bool mirrors,double fs=48000,int quality=1,bool aa=false){auto e=std::make_unique<YouKnowEngine>();EngineParameters p;
    p.enableOtaShotNoise=true;p.enableBa662OutputMirrorNoise=mirrors;p.enableVoiceVcaAntialias=aa;p.enableVoiceVcaTemperature=false;p.calibration=1;
    p.enableVcfEarlyEffect=false;p.enableCardJohnsonFloor=false;p.chorus=ChorusMode::Off;p.chorusNoise=p.noiseLevel=0;
    e->setParameters(p);e->prepare(fs,128,quality);return e;}
void engineDensity(){constexpr double q=1.602176634e-19,R=47000,convert=1./2.6;double worst=0;
    const double full=YouKnowEngine::VoiceVcaSignalLaw::fullControlTailAmps,service=YouKnowEngine::VoiceVcaControlLaw::gain(4064.f/4095.f);
    for(double host:{8000.,44100.,48000.,96000.,192000.})for(int quality:{1,2,4}){
        auto e=make(true,host,quality);const double fs=YouKnowTestAccess::rate(*e);double power=0;constexpr int count=32768;
        for(int i=0;i<count;++i){const double x=YouKnowTestAccess::vca(*e,.4f);power+=x*x/count;}
        const double expected=8*q*(full/service*.4f)*R*R*fs/2*convert*convert;
        worst=std::max(worst,std::abs(power/expected-1));require(std::abs(power/expected-1)<.035,"actual VCA mirror density/rate is wrong");
        for(int i=0;i<30;++i)require(YouKnowTestAccess::vca(*e,0)==0,"mirror source bypassed closed VCA current");
        for(double y:{-.8,0.,.8}){auto raw=make(false,host,quality),mirrored=make(true,host,quality);constexpr double k=1.2,H=2*.026*(68000+560)/560,loopH=2*.026*100000/1500;
            const double difference=YouKnowTestAccess::resonanceDiffusion(*mirrored,k,y)-YouKnowTestAccess::resonanceDiffusion(*raw,k,y);
            const double sensitivity=1-std::pow(std::tanh(-k*loopH*y/H),2),w=2*std::numbers::pi*248;
            const double expectedRes=nodal(k*loopH/68000,-y)*std::pow(w*sensitivity*68000,2)/(2*fs);
            require(std::abs(difference/expectedRes-1)<2e-6,"resonance physical sign/current/first-pair sensitivity is wrong");}}
    for(int quality:{1,2,4}){auto e=make(true,48000,quality,true);auto random=YouKnowTestAccess::random(*e);const auto kernel=YouKnowTestAccess::kernel(*e);const double fs=YouKnowTestAccess::rate(*e);
        constexpr int count=900;std::vector<float> gains(count),noise(count*kernel.factor);for(int i=0;i<count;++i)gains[i]=i<150?0:i<350?.1f:i<500?.8f:i<650?.01f:0;
        for(int i=0;i<count;++i){for(int phase=0;phase<kernel.factor;++phase){double gain=gains[i];if(kernel.factor>1){const int at=i-24;const double a=at>=0?gains[at]:0,b=at+1>=0?gains[at+1]:0;gain=a+static_cast<double>(phase)/kernel.factor*(b-a);}
                noise[i*kernel.factor+phase]=static_cast<float>(random.next()*R*std::sqrt(4*q*(full/service*gain)*fs*kernel.factor)/YouKnowEngine::VoiceVcaSignalLaw::serviceGain());}
            double expected=noise[i*kernel.factor];if(kernel.factor>1){expected=0;for(int tap=0;tap<kernel.taps;++tap){const int at=i*kernel.factor-tap;if(at>=0)expected+=kernel.decimation[tap]*noise[at];}}
            expected*=YouKnowEngine::VoiceVcaSignalLaw::serviceGain()*convert;const double actual=YouKnowTestAccess::vca(*e,gains[i]);require(std::abs(actual-expected)<3e-11,"mirror noise/control FIR timestamp is wrong");}}
    std::cout<<"actual VCA variance relative="<<worst<<"; all rates/quality, RES polarity, gating, FIR current timeline passed\n";}
struct Render{std::vector<float>audio;decltype(YouKnowTestAccess::streams(std::declval<YouKnowEngine&>()))streams;};
Render render(bool mirrors,bool ota,float character,double fs,int quality,int block){auto e=std::make_unique<YouKnowEngine>();EngineParameters p;ProductFidelityProfile::applyTo(p);
    p.enableBa662OutputMirrorNoise=mirrors;p.enableOtaShotNoise=ota;p.calibration=character;p.chorus=ChorusMode::Off;p.chorusNoise=p.noiseLevel=0;
    p.attack=p.release=0;p.sustain=1;p.cutoff=.55f;p.resonance=.3f;p.polyphony=16;e->setParameters(p);e->prepare(fs,block,quality);e->noteOn(60,1);
    Render result;result.audio.resize(2048);std::vector<float> right(2048);int at=0;
    for(int segment=0;segment<4;++segment){if(segment==1)e->allNotesOff();if(segment==2)for(int i=0;i<16;++i)e->noteOn(48+i,1);if(segment==3){e->allNotesOff();e->setOversamplingFactor(quality==1?4:1);}
        const int end=(segment+1)*512;while(at<end){const int n=std::min(block,end-at);guard=true;e->process(result.audio.data()+at,right.data()+at,n);guard=false;at+=n;}}
    for(float x:result.audio)require(std::isfinite(x),"mirror lifecycle produced nonfinite audio");result.streams=YouKnowTestAccess::streams(*e);return result;}
void lifecycle(){require(!EngineParameters{}.enableBa662OutputMirrorNoise,"raw mirror flag changed");EngineParameters p;ProductFidelityProfile::applyTo(p);require(p.enableBa662OutputMirrorNoise,"product omitted mirror floor");
    for(double rate:{8000.,44100.,48000.,96000.,192000.})for(int quality:{1,2,4}){const auto zero=render(false,true,0,rate,quality,128),onZero=render(true,true,0,rate,quality,128);require(zero.audio==onZero.audio&&zero.streams==onZero.streams,"mirror changed Character0 reference");
        const auto off=render(false,true,1,rate,quality,137),on=render(true,true,1,rate,quality,137),single=render(true,true,1,rate,quality,1);
        require(on.audio==single.audio&&on.streams==single.streams,"mirror block partition changed");require(off.streams==on.streams,"mirror changed device/established stream chronology");require(off.audio!=on.audio,"mirror component did not reach engine");
        require(render(false,false,1,rate,quality,128).audio==render(true,false,1,rate,quality,128).audio,"mirror ignored OTA parent flag");}
    auto used=make(true),fresh=make(true);for(int i=0;i<35;++i)(void)YouKnowTestAccess::vca(*used,1);used->reset();require(YouKnowTestAccess::streams(*used)==YouKnowTestAccess::streams(*fresh),"mirror hard-reset chronology failed");
    require(allocations==0,"mirror process allocated");std::cout<<"Character0/parent-off exact, RNG/reset/retirement/idle/quality/block/noalloc passed\n";}
double cost(bool mirrors,int voices,int quality){auto e=std::make_unique<YouKnowEngine>();EngineParameters p;ProductFidelityProfile::applyTo(p);p.enableBa662OutputMirrorNoise=mirrors;p.chorus=ChorusMode::Off;p.chorusNoise=p.noiseLevel=0;p.polyphony=voices;p.attack=0;p.sustain=1;p.cutoff=.55f;p.resonance=.4f;p.calibration=.7f;
    e->setParameters(p);e->prepare(48000,128,quality);for(int i=0;i<voices;++i)e->noteOn(48+2*i,1);std::array<float,128>left{},right{};
    for(int i=0;i<30;++i)e->process(left.data(),right.data(),128);const auto start=std::chrono::steady_clock::now();for(int i=0;i<150;++i)e->process(left.data(),right.data(),128);
    return std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();}
void costs(){for(int voices:{6,16})for(int quality:{1,4}){double before=0,after=0;for(int repeat=0;repeat<3;++repeat){before+=cost(false,voices,quality);after+=cost(true,voices,quality);}
    std::cout<<"CPU "<<voices<<" voices quality"<<quality<<": "<<before/3<<" -> "<<after/3<<" s/.4s, ratio="<<after/before<<"\n";}}
}
int main(){try{transistorOracle();engineDensity();lifecycle();costs();}catch(const std::exception&e){guard=false;std::cerr<<e.what()<<'\n';return 1;}}
