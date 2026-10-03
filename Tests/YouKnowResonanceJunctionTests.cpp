#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace youknow {
struct YouKnowTestAccess {
    static float resonanceHold(const YouKnowEngine& e) { return e.resonanceCv_; }
    static auto randomState(const YouKnowEngine& e) {
        return std::array<std::uint32_t, 3> {e.noiseState_,
            std::bit_cast<std::uint32_t>(e.noiseGaussianSpare_),
            static_cast<std::uint32_t>(e.noiseGaussianSpareValid_)};
    }
};
}
namespace {
using namespace youknow;
using Law=YouKnowEngine::CircuitDerivedResonanceProfile;
void require(bool b,const char* s) { if(!b) throw std::runtime_error(s); }
constexpr long double vt=1.380649e-23L*298.15L/1.602176634e-19L;
constexpr long double alpha=200.L/201.L;
long double referenceIc() {
    // Independently solve the two already accepted PNP followers' DC KCL.
    const long double a=1+44000.L/(201.L*22000.L),b=-1/201.L;
    const long double c=44000.L/201.L,d=10000.L+c;
    return alpha*(((15.L-.61L)/22000.L)*d-b*(15.L-2*.61L))/(a*d-b*c);
}
long double saturationCurrent() { return referenceIc()/std::expm1(.61L/vt); }
long double seriesResistance() {
    // The legacy model's float service constants define its coordinate.
    const long double ic=static_cast<long double>(4.504f)*static_cast<long double>(2.f*.026f*(100000.f/1500.f))/68000.L;
    return (static_cast<long double>(.26f)+static_cast<long double>(10.026514f)
        -vt*std::log1p(ic/saturationCurrent()))*alpha/ic;
}
long double oracleCurrent(long double hold) {
    // Bisect the original emitter KCL; no Wright omega/table under test.
    long double lo=0,hi=hold;
    for(int i=0;i<100;++i) {
        const auto ve=(lo+hi)/2;
        const auto residual=(hold-ve)/seriesResistance()
            -saturationCurrent()*std::expm1(ve/vt)/alpha;
        if(residual>0) lo=ve; else hi=ve;
    }
    return saturationCurrent()*std::expm1(((lo+hi)/2)/vt);
}
long double oracleGain(long double control) {
    const auto off=oracleCurrent(static_cast<long double>(.26f));
    const auto full=oracleCurrent(static_cast<long double>(.26f)+static_cast<long double>(10.026514f));
    return static_cast<long double>(4.504f)*(oracleCurrent(static_cast<long double>(.26f)
        +static_cast<long double>(10.026514f)*control)-off)/(full-off);
}
void currentOracle() {
    require(Law::junctionSeriesOhms()>27000&&Law::junctionSeriesOhms()<47000,
        "service-derived Tr18 resistance escaped the drawn trimmer range");
    require(std::abs(Law::junctionSeriesOhms()/seriesResistance()-1)<2e-8L,
        "Tr18 service resistance differs from independent endpoint solve");
    double worstCurrent=0,worstTable=0;float previous=-1;
    for(int i=0;i<=32768;++i) {
        const float control=static_cast<float>(i)/32768;
        const double hold=static_cast<double>(.26f)+static_cast<double>(10.026514f)*control;
        worstCurrent=std::max(worstCurrent,std::abs(Law::junctionCollectorCurrent(hold)
            /static_cast<double>(oracleCurrent(hold))-1));
        const auto gain=Law::junctionLoopGain(control);
        require(std::isfinite(gain)&&gain>=previous&&gain>=0&&gain<=4.504f,"invalid Tr18 table");previous=gain;
        worstTable=std::max(worstTable,std::abs(gain-static_cast<double>(oracleGain(control))));
    }
    require(worstCurrent<3e-8,"Tr18 current disagrees with independent emitter KCL");
    require(worstTable<3e-6,"Tr18 interpolation exceeds its absolute loop-gain error budget");
    require(Law::junctionLoopGain(0)==0&&Law::junctionLoopGain(1)==4.504f,"Tr18 exact endpoints changed");
    for(float v:{-1.f,std::numeric_limits<float>::quiet_NaN(),-std::numeric_limits<float>::infinity()})
        require(Law::junctionLoopGain(v)==0,"Tr18 invalid input was not sanitized");
    require(Law::junctionLoopGain(4.f/127)>0&&Law::loopGain(4.f/127)==0,"soft onset remained a hard corner");
    std::cout<<"Tr18 Rs="<<Law::junctionSeriesOhms()<<" ohm; current relative="<<worstCurrent
        <<"; table absolute="<<worstTable<<'\n';
}
struct Render { std::vector<float> audio,holds;std::array<std::uint32_t,3> rng; };
Render render(bool enabled,float resonance,int block,double rate=48000,int quality=0,bool move=false,bool circuit=true) {
    auto e=std::make_unique<YouKnowEngine>();EngineParameters p;
    p.calibration=0;p.chorus=ChorusMode::Off;p.chorusNoise=0;p.noiseLevel=0;
    p.sawEnabled=true;p.pulseEnabled=false;p.subLevel=0;p.attack=0;p.decay=.1f;p.sustain=1;
    p.cutoff=.27f;p.resonance=resonance;p.enableResonanceSoftJunction=enabled;
    p.useCircuitDerivedResonanceShape=circuit;
    e->setOversamplingFactor(1<<quality);
    e->setParameters(p);e->prepare(rate,block);e->noteOn(60,1);
    Render r;r.audio.resize(static_cast<std::size_t>(rate*.16));std::vector<float> right(r.audio.size());
    std::size_t at=0;
    for(int section=0;section<4;++section) {
        if(move) {p.resonance=std::array<float,4>{0,4.f/127,.2f,0}[section];e->setParameters(p);}
        const auto end=(section+1)*r.audio.size()/4;
        while(at<end) {const int n=std::min(block,static_cast<int>(end-at));e->process(r.audio.data()+at,right.data()+at,n);at+=n;
            if(block==1) r.holds.push_back(YouKnowTestAccess::resonanceHold(*e));}
    }
    for(float sample:r.audio) require(std::isfinite(sample),"Tr18 produced nonfinite engine audio");
    r.rng=YouKnowTestAccess::randomState(*e);return r;
}
void engineChecks() {
    require(!EngineParameters{}.enableResonanceSoftJunction,"raw Tr18 reference changed");
    EngineParameters p;ProductFidelityProfile::applyTo(p);require(p.enableResonanceSoftJunction,"product omitted Tr18 junction");
    for(double rate:{8000.,44100.,48000.,96000.,192000.})for(int quality=0;quality<3;++quality) {
        for(float res:{0.f,1.f}) {const auto raw=render(false,res,128,rate,quality),corrected=render(true,res,128,rate,quality);
            require(raw.audio==corrected.audio,"Tr18 moved zero/full endpoint audio");require(raw.rng==corrected.rng,"Tr18 moved RNG chronology");}
        const auto one=render(true,.1f,1,rate,quality,true),blocked=render(true,.1f,137,rate,quality,true);
        const auto old=render(false,.1f,1,rate,quality,true);
        require(one.audio==blocked.audio&&one.rng==blocked.rng,"Tr18 depends on block partition");
        require(one.holds==old.holds&&one.rng==old.rng,"Tr18 changed converter holds or RNG");
        require(one.audio!=old.audio,"Tr18 soft onset did not reach actual engine");
    }
    require(render(false,.2f,128,48000,0,false,false).audio==render(true,.2f,128,48000,0,false,false).audio,
        "voiced-resonance compatibility does not bypass Tr18 circuit law");
    std::cout<<"Tr18 actual engine: five rates/three qualities; endpoint/control/RNG/block invariance passed\n";
}
}
int main(){try{currentOracle();engineChecks();}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}return 0;}
