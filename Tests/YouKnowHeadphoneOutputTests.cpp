#include "DSP/YouKnowHeadphoneOutput.h"
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"

#include <chrono>

#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace
{
using Phones=youknow::HeadphoneOutput;
using C=std::complex<long double>;
constexpr long double pi=std::numbers::pi_v<long double>;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
struct Reference { C signal {}, impedance {}; long double noise {}; };

// Independent full component-node MNA: keep R54, both pot legs, all three
// LINE ladder legs, IC7's two input and two feedback resistors, C17, C26 and
// R80. The ideal opamp is a voltage constraint H=F with an unknown supply
// current into O, rather than the production's precomputed gain. Every
// resistor injects an independent4kT/R Norton source. No Thevenin reduction,
// shelf factor or production coefficient appears in this oracle.
Reference reference(double volume,double load,double frequency,bool impedance=false,bool finiteAmplifier=false)
{
    enum { A,T,W,M,L,H,F,O,K,J,Ground,Count=10 };
    std::array<int,Count+1> parent {};
    for(int i=0;i<=Count;++i) parent[i]=i;
    const auto root=[&](int n) { while(parent[n]!=n)n=parent[n];return n; };
    const auto join=[&](int a,int b) { parent[root(a)]=root(b); };
    if(volume==0)join(W,Ground);
    if(volume==1)join(T,W);
    std::array<int,Count+1> nodes {}, assigned {}; assigned.fill(-2);
    assigned[root(Ground)]=-1;
    int count=0;
    for(int i=0;i<=Count;++i) {const int r=root(i);if(assigned[r]==-2)assigned[r]=count++;nodes[i]=assigned[r];}
    const int supply=count++;
    std::array<std::array<C,24>,12> matrix {};
    struct R { int p,q; long double ohms; };
    std::vector<R> resistors;
    const auto y=[&](int a,int b,C admittance) {
        const int p=nodes[a],q=nodes[b];
        if(p>=0)matrix[p][p]+=admittance;
        if(q>=0)matrix[q][q]+=admittance;
        if(p>=0&&q>=0){matrix[p][q]-=admittance;matrix[q][p]-=admittance;}
    };
    const auto r=[&](int a,int b,long double resistance,bool internal=true) {
        if(resistance==0)return;
        y(a,b,1.0L/resistance);
        if(internal)resistors.push_back({nodes[a],nodes[b],resistance});
    };
    r(A,T,1500);r(T,W,(1-volume)*10000);r(W,Ground,volume*10000);
    r(W,M,33000);r(M,L,6800);r(L,Ground,1500);
    r(W,H,1000);r(H,Ground,100000);
    r(O,F,39000);r(F,Ground,15000);r(K,J,220);
    const C s{0,2*pi*frequency};
    y(A,Ground,s*10e-6L);y(O,K,s*47e-6L);
    if(!impedance)r(J,Ground,load,false);
    matrix[nodes[O]][supply]=1;
    matrix[supply][nodes[H]]=1;matrix[supply][nodes[F]]=-1;
    // Independent dominant-pole open-loop device constraint O=(2pi*7MHz/s)*(H-F).
    if(finiteAmplifier)matrix[supply][nodes[O]]-=s/(2*pi*7e6L);
    for(int row=0;row<count;++row)matrix[row][count+row]=1;
    for(int col=0;col<count;++col)
    {
        int pivot=col;
        for(int row=col+1;row<count;++row)
            if(std::abs(matrix[row][col])>std::abs(matrix[pivot][col]))pivot=row;
        require(std::abs(matrix[pivot][col])>0,"singular full PHONES MNA");
        std::swap(matrix[pivot],matrix[col]);
        const auto divisor=matrix[col][col];
        for(int j=col;j<2*count;++j)matrix[col][j]/=divisor;
        for(int row=0;row<count;++row)if(row!=col)
        {
            const auto factor=matrix[row][col];
            for(int j=col;j<2*count;++j)matrix[row][j]-=factor*matrix[col][j];
        }
    }
    const int output=nodes[J];
    Reference result;
    result.signal=matrix[output][count+nodes[A]]*s*10e-6L;
    result.impedance=matrix[output][count+nodes[J]];
    for(const auto& resistor:resistors)
    {
        const auto transfer=(resistor.p>=0?matrix[output][count+resistor.p]:C{})
                           -(resistor.q>=0?matrix[output][count+resistor.q]:C{});
        result.noise+=4*1.380649e-23L*298.15L/resistor.ohms*std::norm(transfer);
    }
    return result;
}

void testAnalog()
{
    double worstSignal=0,worstNoise=0,worstImpedance=0;
    for(double volume:{0.,.0001,.25,.5,1.})
        for(double load:{1.,32.,80.,300.,600.,1e9})
        {
            Phones phones;require(phones.prepare(48000,load)&&phones.setVolume(volume),"valid PHONES rejected");
            for(double frequency:{0.,.01,.2,1.,5.,20.,100.,1000.,20000.,100000.})
            {
                const auto expected=reference(volume,load,frequency);
                const auto response=phones.analogResponse(frequency);
                const C actual{response.real(),response.imag()};
                const auto error=double(std::abs(actual-expected.signal)/std::max(1e-18L,std::abs(expected.signal)));
                worstSignal=std::max(worstSignal,error);
                require(error<2e-9,"PHONES signal gain/phase differs from full MNA");
                if(frequency>0)
                {
                    const auto noiseError=std::abs(phones.analogNoisePsd(frequency,298.15)/double(expected.noise)-1);
                    worstNoise=std::max(worstNoise,noiseError);
                    require(noiseError<2e-9,"PHONES noise differs from individual-resistor MNA");
                    const auto z=Phones::analogOutputImpedance(frequency);
                    const auto zi=reference(volume,load,frequency,true).impedance;
                    const auto ze=double(std::abs(C{z.real(),z.imag()}-zi)/std::abs(zi));
                    worstImpedance=std::max(worstImpedance,ze);
                    require(ze<2e-9,"PHONES output impedance differs from independent current injection");
                }
                else require(phones.analogNoisePsd(0,298.15)==0,"PHONES passed DC noise");
            }
        }
    std::cout<<"PHONES MNA errors signal="<<worstSignal<<" noise="<<worstNoise<<" impedance="<<worstImpedance<<'\n';
    for(double load:{32.,80.,300.,600.})
    {
        Phones p;require(p.prepare(48000,load),"declared load rejected");
        std::cout<<"load="<<load<<" pole="<<p.coefficients().headphonePoleHz
                 <<"Hz gain="<<p.coefficients().passbandGain<<'\n';
    }
}

std::complex<double> transform(const std::vector<double>& impulse,double f,double fs,double p,double q)
{
    const auto z=std::polar(1.,-2*std::numbers::pi*f/fs);
    std::complex<double> result{},power{1,0};
    for(const auto value:impulse){result+=value*power;power*=z;}
    // Exact sum of the two TPT-pole tails measured from the final two samples.
    const auto n=impulse.size();
    const double a=(impulse[n-1]-q*impulse[n-2])/(p-q),b=impulse[n-2]-a;
    return result+power*(a*p*p/(1.-p*z)+b*q*q/(1.-q*z));
}
void testDigital()
{
    double worstDb=0,worstPhase=0,worstNoiseDb=0;
    for(double rate:{8000.,44100.,48000.,96000.,192000.,768000.})
        for(double load:{32.,80.,300.,600.})
            for(double volume:{0.,.5,1.})
            {
                Phones p;require(p.prepare(rate,load)&&p.setVolume(volume),"digital setup rejected");
                std::vector<double> signal(2048),noise(2048);
                for(std::size_t i=0;i<signal.size();++i)
                {const auto out=p.process(i==0?1:0,0);signal[i]=out[0];require(out[1]==0,"PHONES stereo leaked");}
                p.reset();require(p.setNoise(298.15,1),"noise setup rejected");
                for(std::size_t i=0;i<noise.size();++i)noise[i]=p.process(0,0,i==0?1:0,0)[0];
                const auto co=p.coefficients();
                const double g=std::tan(std::numbers::pi*co.inputPoleHz/rate);
                const double h=std::tan(std::numbers::pi*co.headphonePoleHz/rate);
                const double a=(1-g)/(1+g),b=(1-h)/(1+h);
                for(double f:{.2,1.,5.,20.,100.,1000.,std::min(20000.,rate*.45)})
                {
                    const auto expected=reference(volume,load,f);
                    const auto actual=transform(signal,f,rate,a,b);
                    if(std::abs(expected.signal)>1e-15L)
                    {
                        const double error=std::abs(20*std::log10(std::abs(actual)/double(std::abs(expected.signal))));
                        const double phase=std::abs(std::arg(actual/std::complex<double>{double(expected.signal.real()),double(expected.signal.imag())}))*180/std::numbers::pi;
                        worstDb=std::max(worstDb,error);worstPhase=std::max(worstPhase,phase);
                        require(error<.02&&phase<.5,"PHONES TPT source exceeds qualified gain/phase bound");
                    }
                    const auto actualNoise=transform(noise,f,rate,a,b);
                    const double error=std::abs(10*std::log10(2/rate*std::norm(actualNoise)/double(expected.noise)));
                    worstNoiseDb=std::max(worstNoiseDb,error);
                    require(error<.02,"PHONES TPT noise exceeds qualified bound");
                }
            }
    std::cout<<"PHONES realized errors signal="<<worstDb<<"dB noise="<<worstNoiseDb<<"dB phase="<<worstPhase<<"deg\n";
}

void testFiniteAmplifier()
{
    double worstMagnitude=0,worstPhase=0,worstNoise=0;
    for(double rate:{8000.,44100.,48000.,96000.,192000.,768000.})
        for(double load:{32.,80.,300.,600.})
            for(double volume:{0.,.25,1.})
            {
                Phones p;require(p.prepare(rate,load)&&p.setVolume(volume),"finite amplifier setup rejected");
                p.setAmplifierDynamics(true);
                require(p.setNoise(298.15,1),"finite amplifier noise setup rejected");
                std::vector<double> signal(2048),inputNoise(2048),outputNoise(2048);
                for(std::size_t i=0;i<signal.size();++i)
                    signal[i]=p.process(i==0?.001:0,0)[0]*1000;
                p.reset();
                for(std::size_t i=0;i<inputNoise.size();++i)
                    inputNoise[i]=p.process(0,0,i==0?1:0,0)[0];
                p.reset();
                for(std::size_t i=0;i<outputNoise.size();++i)
                    outputNoise[i]=p.process(0,0,0,0,i==0?1:0,0)[0];
                const auto co=p.coefficients();
                const double g=std::tan(std::numbers::pi*co.inputPoleHz/rate);
                const double h=std::tan(std::numbers::pi*co.headphonePoleHz/rate);
                const double a=(1-g)/(1+g),b=(1-h)/(1+h);
                for(double f:{.2,1.,5.,20.,100.,1000.,std::min(20000.,rate*.45)})
                {
                    const auto expected=reference(volume,load,f,false,true);
                    const auto analog=p.analogResponse(f);
                    require(std::abs(C{analog.real(),analog.imag()}-expected.signal)
                        <2e-9L*std::max(1e-16L,std::abs(expected.signal)),
                        "finite amplifier response differs from independent open-loop nodal constraint");
                    require(std::abs(p.analogNoisePsd(f,298.15)/double(expected.noise)-1)<2e-9,
                        "finite amplifier noise differs from each independent resistor source");
                    if(std::abs(expected.signal)>1e-15L)
                    {
                        const auto actual=transform(signal,f,rate,a,b);
                        const double db=std::abs(20*std::log10(std::abs(actual)/double(std::abs(expected.signal))));
                        const double phase=std::abs(std::arg(actual/std::complex<double>{double(expected.signal.real()),double(expected.signal.imag())}))*180/std::numbers::pi;
                        worstMagnitude=std::max(worstMagnitude,db);worstPhase=std::max(worstPhase,phase);
                        require(db<.02&&phase<.7,"matched amplifier exceeds qualified audio-band magnitude/phase screen");
                    }
                    const auto ni=transform(inputNoise,f,rate,a,b),no=transform(outputNoise,f,rate,a,b);
                    const double db=std::abs(10*std::log10(2/rate*(std::norm(ni)+std::norm(no))/double(expected.noise)));
                    worstNoise=std::max(worstNoise,db);
                    require(db<.02,"finite amplifier resistor-noise source routing differs from nodal PSD");
                }
            }
    // Physically independent amplifier output node: slew precedes C26 and
    // attenuation, so changing the declared load cannot change this trajectory.
    for(double rate:{8000.,44100.,48000.,96000.,192000.,768000.})
    {
        Phones p,q;require(p.prepare(rate,32)&&q.prepare(rate,600),"slew setup failed");
        p.setAmplifierDynamics(true);q.setAmplifierDynamics(true);
        double previous=0;bool limited=false;
        for(int i=0;i<400;++i)
        {
            const double input=(i/50)%2?100:-100;
            (void)p.process(input,0);(void)q.process(input,0);
            const double voltage=p.amplifierVoltages()[0];
            const double step=std::abs(voltage-previous),limit=2.2e6/rate;
            require(step<=limit*(1+2e-14),"IC7 sampled slew exceeded original2.2V/us prior");
            require(std::abs(voltage-q.amplifierVoltages()[0])<1e-10,"headphone load incorrectly scaled amplifier slew");
            limited|=step>limit*.999;previous=voltage;
        }
        require(limited,"strong diagnostic steps never engaged slew");
        auto copy=p;require(p.prepare(rate,32),"repeat prepare failed");
        require(p.process(.5,0)==copy.process(.5,0),"prepare lost retained response/slew state");
        copy=p;p.setAmplifierDynamics(false);p.setAmplifierDynamics(true);
        require(p.process(.5,0)==copy.process(.5,0),"option toggles reset capacitor or amplifier history");
        p.reset();require(p.process(0,0)==std::array<double,2>{0,0},"finite amplifier reset retained a tail");
    }
    std::cout<<"finite PHONES response screen max="<<worstMagnitude<<"dB phase="<<worstPhase
             <<"deg independent resistor PSD="<<worstNoise<<"dB\n";
}

void testGuardsAndState()
{
    Phones p;
    require(p.prepare(48000,32)&&p.setNoise(298.15,0),"guard setup failed");
    for(double bad:{0.,-1.,1e10,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
        require(!p.setLoad(bad)&&!p.prepare(48000,bad)&&p.loadOhms()==32,"invalid load changed PHONES state");
    require(!p.setVolume(std::numeric_limits<double>::quiet_NaN()),"nonfinite volume accepted");
    for(int i=0;i<64;++i)(void)p.process(1,.5);
    auto copy=p;
    require(p.setLoad(32)&&p.setVolume(1)&&p.prepare(48000,32),"unchanged setup rejected");
    require(p.process(1,.5)==copy.process(1,.5),"reprepare/reset coefficient update lost histories");
    require(p.setLoad(600)&&p.setVolume(.5),"live load update rejected");
    for(int i=0;i<10000;++i)
    {const auto out=p.process(0,0);require(std::isfinite(out[0])&&std::isfinite(out[1]),"live load update unstable");}
    p.reset();require(p.process(0,0)==std::array<double,2>{0,0},"PHONES reset retained audio");
    const auto hostile=p.process(std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN());
    require(hostile==std::array<double,2>{0,0},"PHONES failed to sanitize source");
}

std::vector<float> engineTake(Phones::Route route,double load,bool changedLine=false,bool original=false,bool dynamics=false)
{
    auto engine=std::make_unique<youknow::YouKnowEngine>();
    youknow::EngineParameters p;p.outputRoute=route;p.headphoneLoadOhms=float(load);
    p.enableHeadphoneAmplifierDynamics=dynamics;
    p.calibration=0;p.chorus=youknow::ChorusMode::Off;p.highPass=youknow::HighPassMode::One;
    p.vcfTanhMode=youknow::VcfTanhMode::PolyZoned;p.vcfSolverMode=youknow::VcfSolverMode::Rk4Single;
    if(changedLine){p.outputSelector=youknow::OutputNetwork::Selector::Low;p.outputLoadOhms=10000;p.outputCapacitancePf=5000;p.outputMono=true;}
    engine->setParameters(p);engine->setOriginalPerformanceMode(original);
    engine->prepare(48000,97,1);engine->noteOn(24,1);
    std::array<float,97> left{},right{};std::vector<float> out;
    for(int i=0;i<120;++i){engine->process(left.data(),right.data(),97);out.insert(out.end(),left.begin(),left.end());}
    require(engine->originalPerformanceHealthy(),"PHONES render lost Original firmware support");
    return out;
}
std::vector<float> detailedTake(bool dynamics,Phones::Route route,int block,int quality,bool switchRoute=false)
{
    auto e=std::make_unique<youknow::YouKnowEngine>();
    youknow::EngineParameters p;youknow::ProductFidelityProfile::applyTo(p);
    p.enableHeadphoneAmplifierDynamics=dynamics;p.outputRoute=route;
    p.volume=.9f;p.calibration=.7f;p.cutoff=.9f;p.resonance=.1f;
    p.chorus=youknow::ChorusMode::Off;p.highPass=youknow::HighPassMode::One;
    p.vcfTanhMode=youknow::VcfTanhMode::PolyZoned;p.vcfSolverMode=youknow::VcfSolverMode::Rk4Single;
    youknow::ProductFidelityProfile::configureBeforePrepare(*e);
    require(e->configureThermalStart(true),"cannot settle test board temperature");
    e->setParameters(p);e->prepare(48000,256,quality);e->noteOn(108,1);
    std::array<float,256> l{},r{};std::vector<float> result;
    for(int i=0;i<3072;)
    {
        if(switchRoute&&i==1024){p.outputRoute=Phones::Route::Headphones;e->setParameters(p);}
        const int next=i<1024?1024:3072;
        const int count=std::min({block,3072-i,next-i});
        e->process(l.data(),r.data(),count);
        for(int j=0;j<count;++j){result.push_back(l[j]);result.push_back(r[j]);}
        i+=count;
    }
    return result;
}
void testContinuousRouteAndBlocks()
{
    for(int quality:{1,4})
    {
        const auto before=detailedTake(false,Phones::Route::Line,128,quality);
        require(before==detailedTake(true,Phones::Route::Line,128,quality),
            "IC7 dynamics/RNG changed product LINE including upstream noise");
        const auto phones=detailedTake(true,Phones::Route::Headphones,128,quality);
        require(phones==detailedTake(true,Phones::Route::Headphones,47,quality),
            "PHONES response/noise chronology depends on host block size");
        require(phones==detailedTake(true,Phones::Route::Headphones,128,quality),
            "reset PHONES sequence is nondeterministic");
        const auto switched=detailedTake(true,Phones::Route::Line,128,quality,true);
        require(std::equal(phones.begin()+2048,phones.end(),switched.begin()+2048),
            "LINE-to-PHONES route switch resurrected stale amplifier/capacitor/RNG history");
        require(phones!=detailedTake(false,Phones::Route::Headphones,128,quality),
            "selected dynamics did not reach PHONES product audio");
    }
}

void testEngine()
{
    youknow::EngineParameters product;youknow::ProductFidelityProfile::applyTo(product);
    require(product.enableHeadphoneAmplifierDynamics,"product omitted IC7 dynamics");
    const auto line=engineTake(Phones::Route::Line,32);
    require(line==engineTake(Phones::Route::Line,32,false,false,true),"IC7 dynamics changed LINE audio");
    require(line==engineTake(Phones::Route::Line,600),"unused headphone load altered LINE");
    require(line==engineTake(static_cast<Phones::Route>(99),32),"bad route did not fall back to LINE");
    const auto phones=engineTake(Phones::Route::Headphones,32);
    require(phones==engineTake(Phones::Route::Headphones,32,true),"LINE controls altered independent PHONES route");
    require(phones==engineTake(Phones::Route::Headphones,std::numeric_limits<double>::quiet_NaN()),"invalid headphone load did not restore32Ohm");
    require(phones!=line&&phones!=engineTake(Phones::Route::Headphones,600),"PHONES route/load did not reach audio");
    double energy=0;for(float x:phones){require(std::isfinite(x),"PHONES engine not finite");energy+=x*x;}
    require(energy>1e-6,"PHONES engine remained silent");
    const auto originalPhones=engineTake(Phones::Route::Headphones,32,false,true);
    require(originalPhones!=engineTake(Phones::Route::Line,32,false,true),
            "PHONES route did not reach Original-mode audio");
    double originalEnergy=0;for(float x:originalPhones)originalEnergy+=x*x;
    require(originalEnergy>1e-6,"Original-mode PHONES render stayed silent");
}
}
int main()
{
    try {testAnalog();testDigital();testFiniteAmplifier();testGuardsAndState();testEngine();testContinuousRouteAndBlocks();std::cout<<"PHONES output checks passed\n";}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
