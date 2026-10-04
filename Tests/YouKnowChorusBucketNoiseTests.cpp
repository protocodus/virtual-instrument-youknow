// Independent covariance and continuous-staircase oracles. No SciPy or
// hardware fitting is required by the realtime regression target.
#include "DSP/YouKnowChorus.h"
#include "DSP/YouKnowProductFidelity.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <complex>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace { thread_local bool counting=false; thread_local unsigned allocations=0; }
void* operator new(std::size_t n) {if(counting)++allocations;if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n) {return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace youknow {
struct YouKnowTestAccess {
    static void seed(Chorus& c,unsigned s) {c.lineA_.reset(s);}
    static void mix(Chorus& c,double r,double correlation=1) {
        c.lineA_.bucketNoise=ChorusBucketNoise::coefficients(r,correlation);
    }
    static float core(Chorus& c,float clock,float rate) {return c.lineA_.processClockedCore(0,clock,rate,1);}
    static auto history(const Chorus& c) {return std::tuple{c.lineA_.noiseState,c.lineA_.transferNoiseState,c.lineA_.previousTransferNoise,c.lineA_.held,c.lineA_.clockPhase};}
    static auto engineHistory(const YouKnowEngine& e) {return std::array{e.outputNoiseStateLeft_,e.outputNoiseStateRight_,e.commonVcaNoiseState_,e.chorus_.lineA_.noiseState,e.chorus_.lineB_.noiseState};}
    static void nominalRatio(Chorus& c) {c.wetInputConductanceRatio_=ChorusMuteDrive::conductanceRatio(0);}
    static const auto& transition(Chorus& c) {return c.finiteWetTransition();}
    static float output(Chorus& c,float clock,float rate,double r) {
        auto q=ChorusBucketNoise::coefficients(r,1);
        ChorusBucketNoise::normalize(q,c.bucketNoiseCosineMoment(clock));c.lineA_.bucketNoise=q;
        return c.lineA_.process(0,clock,rate,c.finiteWetTransition(),1);
    }
}; }
namespace {
using namespace youknow;
constexpr double pi=3.14159265358979323846;
constexpr unsigned seed=0x1234567u;
void require(bool b,const char* m) {if(!b)throw std::runtime_error(m);}
unsigned next(unsigned s) {s^=s<<13;s^=s>>17;s^=s<<5;return s;}
float draw(unsigned s) {return float(s&0xffffffu)*(2.f/16777215.f)-1.f;}
double kernel(double t) {double x=std::abs(t);if(x<1)return (x+1)*(x-1)*(x-2)/2;if(x<2)return -(x-1)*(x-2)*(x-3)/6;return 0;}
double staircase(double time,double period,const std::vector<float>& values) {
    // Exact Gaussian integration of the held packets against an independent
    // Lagrange kernel. Split at both packet edges and kernel boundaries.
    std::vector<double> knots{time-2,time-1,time,time+1,time+2};
    for(auto k=static_cast<long long>(std::floor((time-2)/period+.5));k<=std::ceil((time+2)/period+.5);++k)
        if((k-.5)*period>time-2&&(k-.5)*period<time+2)knots.push_back((k-.5)*period);
    std::sort(knots.begin(),knots.end());double sum=0;
    for(std::size_t j=1;j<knots.size();++j) {
        const double mid=(knots[j-1]+knots[j])/2,h=(knots[j]-knots[j-1])/2;
        const auto k=static_cast<long long>(std::floor(mid/period+.5));const double v=k<0?0:values.at(static_cast<std::size_t>(k));
        constexpr double a=.774596669241483377;
        sum+=v*h*(5./9*kernel(time-mid+h*a)+8./9*kernel(time-mid)+5./9*kernel(time-mid-h*a));
    }return sum;
}
void covariance() {
    constexpr int count=2000000;double maxError=0;
    for(auto [r,c]:std::array<std::pair<double,double>,5>{{{0,1},{.91711,1},{1,1},{.7,.3},{1,0}}}) {
        auto coefficients=ChorusBucketNoise::coefficients(r,c);
        ChorusBucketNoise::normalize(coefficients,.6);
        unsigned u=seed,v=(seed^0xd1b54a35u)|1u;float previous=0;std::array<double,4> sums{};std::array<float,3> history{};
        for(int n=0;n<count;++n) {
            u=next(u);v=next(v);float x=ChorusBucketNoise::step(draw(u),draw(v),previous,coefficients);previous=draw(v);
            sums[0]+=x*x;for(int j=0;j<3;++j)sums[j+1]+=x*history[j];history={x,history[0],history[1]};
        }
        // The existing 24-bit uniform source has variance (N+2)/(3N).
        const double variance=(16777215.+2)/(3*16777215.)*coefficients.aWeightedScale*coefficients.aWeightedScale;
        const double eta=2*r*c/(1+c*c);
        for(int lag=0;lag<4;++lag) {
            double expected=lag==0?variance:(lag==1?-.5*eta*variance:0);
            const double error=std::abs(sums[lag]/count-expected)/variance;maxError=std::max(maxError,error);
            require(error<.004,"packet covariance failed independent uniform-source oracle");
        }
        // Wiener-Khinchin: Fourier transform the measured covariance, rather
        // than compare against another production mix implementation.
        for(double w:{.1,.4,1.,2.,3.}) {
            double measured=sums[0];for(int lag=1;lag<4;++lag)measured+=2*sums[lag]*std::cos(w*lag);
            require(std::abs(measured/count-variance*(1-eta*std::cos(w)))<.018*variance,"physical-event PSD failed covariance Fourier oracle");
        }
    }
    auto q=ChorusBucketNoise::coefficients(NAN,NAN);require(q.storage==1&&q.transfer==0,"nonfinite source controls were not sanitized");
    require(ChorusBucketNoise::coefficients(2,-1).eta==0,"bounded covariance controls failed");
    std::cout<<"covariance maximum relative absolute error "<<maxError<<"\n";
}
void reconstruction() {
    double maximum=0;
    for(auto [fs,clock]:std::array<std::pair<float,float>,6>{{{48000,12000},{48000,37001},{44100,80000},{192000,37001},{8000,200000},{768000,200000}}}) {
        auto chorus=std::make_unique<Chorus>();chorus->prepare(fs);
        const auto originalResetHistory=YouKnowTestAccess::history(*chorus);
        YouKnowTestAccess::seed(*chorus,seed);YouKnowTestAccess::mix(*chorus,.91711,.8);
        const double period=double(fs)/clock;unsigned u=seed,v=(seed^0xd1b54a35u)|1u;float previous=0;
        std::vector<float> values{0};std::vector<unsigned> us{u},vs{v};std::vector<float> draws{0};
        for(std::size_t n=1;n<static_cast<std::size_t>(std::ceil(515/period))+2;++n) {
            u=next(u);v=next(v);
            // Direct algebraic reference, not the production helper.
            const float x=float(std::sqrt(1-.91711)*draw(u)+std::sqrt(.91711/(1+.8*.8))*(draw(v)-.8*previous));previous=draw(v);
            values.push_back(x*Chorus::independentLineRandomAmplitude);us.push_back(u);vs.push_back(v);draws.push_back(previous);
        }
        for(int sample=1;sample<=512;++sample) {
            maximum=std::max(maximum,std::abs(YouKnowTestAccess::core(*chorus,clock,fs)-staircase(sample,period,values)));
            const auto numerator=static_cast<unsigned long long>(sample)*static_cast<unsigned>(clock);
            const auto denominator=static_cast<unsigned>(fs);
            auto events=(2*numerator+denominator)/(2*denominator);const bool boundary=(2*numerator+denominator)%(2*denominator)==0;
            auto [liveU,liveV,prev,held,phase]=YouKnowTestAccess::history(*chorus);
            if(boundary&&events&&liveU==us.at(events-1))--events;
            require(liveU==us.at(events)&&liveV==vs.at(events)&&prev==draws.at(events),"BLEP prediction changed physical source chronology");
            require(held==values.at(events),"physical held packet failed independent event ledger");
        }
        const auto history=YouKnowTestAccess::history(*chorus);
        for(int n=0;n<100;++n)YouKnowTestAccess::core(*chorus,0,fs);
        require(history==YouKnowTestAccess::history(*chorus),"stopped clock advanced packet noise");
        chorus->prepare(fs==48000?192000:48000,true);
        require(history==YouKnowTestAccess::history(*chorus),"rate change lost physical packet histories");
        chorus->reset();
        require(YouKnowTestAccess::history(*chorus)==originalResetHistory,"hard reset retained correlated packet history");
    }
    const double bound=(2*Chorus::maximumBlepEvents+4)*std::numeric_limits<float>::epsilon()*Chorus::independentLineRandomAmplitude*2;
    require(maximum<bound,"colored held source failed independent continuous-staircase quadrature");
    std::cout<<"colored continuous-staircase error "<<maximum<<", bound "<<bound<<"\n";
}
std::complex<double> response(const Chorus::SupportChain::ExactTransition& t,double frequency,double fs) {
    // Independent z-domain solve of the prepared high-rate linear circuit.
    const auto z=std::exp(std::complex<double>(0,-2*pi*frequency/fs));
    std::array<std::array<std::complex<double>,7>,6> m{};
    for(int i=0;i<6;++i) {for(int j=0;j<6;++j)m[i][j]=(i==j?1.:0.)-z*t.stateByColumn[j][i];for(int j=0;j<4;++j)m[i][6]+=std::pow(z,j)*t.driveBySample[j][i];}
    for(int j=0;j<6;++j) {int pivot=j;for(int i=j+1;i<6;++i)if(std::norm(m[i][j])>std::norm(m[pivot][j]))pivot=i;std::swap(m[j],m[pivot]);auto d=m[j][j];for(int k=j;k<7;++k)m[j][k]/=d;for(int i=0;i<6;++i)if(i!=j){auto s=m[i][j];for(int k=j;k<7;++k)m[i][k]-=s*m[j][k];}}
    std::complex<double> result=t.outputDirect;for(int i=0;i<6;++i)result+=t.outputByState[i]*m[i][6];return result;
}
double weighting(double f) {double q=f*f;return std::pow(12194.*12194.*q*q/((q+20.6*20.6)*std::sqrt((q+107.7*107.7)*(q+737.9*737.9))*(q+12194.*12194.))/.79434639,2);}
void normalization() {
    double maxMoment=0,maxLevel=0;
    for(auto profile:{ChorusSupportProfile::IdealFollowers,ChorusSupportProfile::Nominal2SA1015,ChorusSupportProfile::Nominal2SA1015Nonlinear}) {
        auto c=std::make_unique<Chorus>();require(c->configureSupportProfile(profile),"support selection rejected");c->prepare(768000);YouKnowTestAccess::nominalRatio(*c);const auto& t=YouKnowTestAccess::transition(*c);
        std::array<double,4096> powers{},frequencies{};
        for(int k=0;k<4096;++k){double f=20+(k+.5)*19980/4096;frequencies[k]=f;powers[k]=weighting(f)*std::norm(response(t,f,768000));}
        for(double clock:{10000.,20000.,23001.,37001.,80000.,100000.,199983.}) {
            double white=0,colored=0,moment=0;auto q=ChorusBucketNoise::coefficients(.91711,1);ChorusBucketNoise::normalize(q,c->bucketNoiseCosineMoment(clock));
            for(int k=0;k<4096;++k){double x=pi*frequencies[k]/clock,w=powers[k]*std::pow(std::sin(x)/x,2);white+=w;moment+=w*std::cos(2*x);colored+=w*(1-q.eta*std::cos(2*x))*q.aWeightedScale*q.aWeightedScale;}
            maxMoment=std::max(maxMoment,std::abs(moment/white-c->bucketNoiseCosineMoment(clock)));maxLevel=std::max(maxLevel,std::abs(10*std::log10(colored/white)));
        }
    }
    require(maxMoment<.0002&&maxLevel<.004,"A-weighted budget failed independent high-rate circuit quadrature");
    std::cout<<"normalization max moment error "<<maxMoment<<", output level error "<<maxLevel<<" dB\n";
}
std::vector<float> render(int block,bool enabled,float fraction,std::array<unsigned,5>& rng,double& elapsed,float noise=Chorus::defaultNoiseScale,int seconds=1,int quality=1,ChorusMode mode=ChorusMode::One,bool musical=false) {
    auto engine=std::make_unique<YouKnowEngine>();ProductFidelityProfile::configureBeforePrepare(*engine);engine->prepare(48000,173,quality);
    EngineParameters p;ProductFidelityProfile::applyTo(p);p.enableChorusCorrelatedNoise=enabled;p.chorusNoiseTransferFraction=fraction;p.chorusNoiseTransferCorrelation=1;p.chorus=mode;p.chorusNoise=noise;p.sawEnabled=musical;p.pulseEnabled=false;p.subLevel=0;p.noiseLevel=0;p.calibration=1;
    engine->setParameters(p);if(musical)for(int note:{48,55,60,64,67,72})engine->noteOn(note,1);
    std::vector<float> l(48000*seconds),r(l.size());const auto begin=std::chrono::steady_clock::now();counting=true;
    for(std::size_t cursor=0;cursor<l.size();) {const auto n=std::min<std::size_t>(block,l.size()-cursor);engine->process(l.data()+cursor,r.data()+cursor,int(n));cursor+=n;}
    counting=false;elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();rng=YouKnowTestAccess::engineHistory(*engine);l.insert(l.end(),r.begin(),r.end());return l;
}
void engine() {
    require(!EngineParameters{}.enableChorusCorrelatedNoise,"raw source default changed");
    EngineParameters product;ProductFidelityProfile::applyTo(product);
    require(product.enableChorusCorrelatedNoise&&product.chorusNoiseTransferFraction==ProductFidelityProfile::chorusNoiseEffectiveCovariance&&product.chorusNoiseTransferCorrelation==1,"product covariance selection absent");
    std::array<unsigned,5> a{},b{},c{},d{};double ta,tb,tc,td;const auto x=render(1,true,.91711,a,ta),y=render(173,true,.91711,b,tb),old=render(173,false,0,c,tc),zero=render(173,true,0,d,td);
    require(x==y&&a==b,"colored source changed with callback partition");require(old==zero&&c==d,"zero covariance was not exact legacy source");require(a==c,"colored source consumed unrelated/legacy RNG draws");require(allocations==0,"colored callback allocated");
    const double idleRawCost=tc;
    require(render(173,false,0,c,tc,0,1,1,ChorusMode::One,true)==render(173,true,.91711,d,td,0,1,1,ChorusMode::One,true),"zero-noise six-voice signal path changed with covariance model");
    double delta=0,power=0;for(std::size_t i=0;i<x.size();++i){delta+=std::pow(x[i]-old[i],2);power+=old[i]*old[i];}
    require(delta>0&&std::isfinite(delta),"colored source had no finite audible-path effect");
    std::cout<<"idle engine delta "<<10*std::log10(delta/power)<<" dBc, callback cost "<<tb/idleRawCost<<"x; block/RNG/noalloc passed\n";
    std::array<double,3> rawCost{},coloredCost{};
    for(int k=0;k<3;++k){render(173,false,0,c,rawCost[k],Chorus::defaultNoiseScale,1,1,ChorusMode::One,true);render(173,true,.91711,d,coloredCost[k],Chorus::defaultNoiseScale,1,1,ChorusMode::One,true);}
    std::sort(rawCost.begin(),rawCost.end());std::sort(coloredCost.begin(),coloredCost.end());
    std::cout<<"six-voice median callback cost "<<coloredCost[1]/rawCost[1]<<"x ("<<rawCost[1]<<" -> "<<coloredCost[1]<<" seconds per audio second)\n";
}
void engineMeasure(const std::filesystem::path& directory) {
    require(!std::filesystem::exists(directory),"engine measurement directory already exists");std::filesystem::create_directories(directory);
    std::ofstream index(directory/"index.csv");index<<"file,sample_rate,quality,mode,fraction,seconds,elapsed\n"<<std::setprecision(17);
    for(int quality:{1,4})for(auto mode:{ChorusMode::One,ChorusMode::Two,ChorusMode::OneTwo})for(double r:{0.,double(ProductFidelityProfile::chorusNoiseEffectiveCovariance)}) {
        std::array<unsigned,5> states{};double elapsed;auto samples=render(173,r!=0,float(r),states,elapsed,Chorus::defaultNoiseScale,8,quality,mode);
        const auto name="engine-"+std::to_string(quality)+"-"+std::to_string(int(mode))+"-"+(r==0?"raw":"colored")+".f32";
        std::ofstream f(directory/name,std::ios::binary);f.write(reinterpret_cast<const char*>(samples.data()),samples.size()*sizeof(float));index<<name<<",48000,"<<quality<<','<<int(mode)<<','<<r<<",8,"<<elapsed<<'\n';
    }
}
void exportSupport() {
    auto c=std::make_unique<Chorus>();require(c->configureSupportProfile(ChorusSupportProfile::Nominal2SA1015Nonlinear),"support selection rejected");c->prepare(768000);YouKnowTestAccess::nominalRatio(*c);const auto& t=YouKnowTestAccess::transition(*c);
    const auto rows=[](const auto& values){std::cout<<'[';bool first=true;for(const auto& row:values){if(!first)std::cout<<',';first=false;std::cout<<'[';bool fv=true;for(auto v:row){if(!fv)std::cout<<',';fv=false;std::cout<<v;}std::cout<<']';}std::cout<<']';};
    std::cout<<std::setprecision(17)<<"{\"sample_rate\":768000,\"state_by_column\":";rows(t.stateByColumn);std::cout<<",\"drive_by_sample\":";rows(t.driveBySample);std::cout<<",\"output_by_state\":[";
    for(int i=0;i<6;++i){if(i)std::cout<<',';std::cout<<t.outputByState[i];}std::cout<<"],\"output_direct\":"<<t.outputDirect<<",\"timing\":{";
    for(auto mode:{ChorusMode::One,ChorusMode::Two}) {auto s=Chorus::settingsFor(mode,ChorusTimingProfile::OwnerBlend);if(mode==ChorusMode::Two)std::cout<<',';std::cout<<'"'<<(mode==ChorusMode::One?"I":"II")<<"\":{\"centre\":"<<s.centreDelaySeconds<<",\"sweep\":"<<s.sweepSeconds<<",\"rate\":"<<s.rateHz<<'}';}std::cout<<"}}\n";
}
void measure(const std::filesystem::path& directory) {
    require(!std::filesystem::exists(directory),"measurement directory already exists");std::filesystem::create_directories(directory);std::ofstream index(directory/"index.csv");index<<"file,sample_rate,clock_hz,fraction,source_amplitude\n"<<std::setprecision(17);
    for(int rate:{48000,192000,768000})for(int clock:{20000,37001,80000,100000})for(double r:{0.,double(ProductFidelityProfile::chorusNoiseEffectiveCovariance)}) {
        auto c=std::make_unique<Chorus>();require(c->configureSupportProfile(ChorusSupportProfile::Nominal2SA1015Nonlinear),"support selection rejected");c->prepare(rate);YouKnowTestAccess::nominalRatio(*c);YouKnowTestAccess::seed(*c,seed);std::vector<float> output(4*rate);
        for(auto& sample:output)sample=YouKnowTestAccess::output(*c,float(clock),float(rate),r);
        const auto name="noise-"+std::to_string(rate)+"-"+std::to_string(clock)+"-"+(r==0?"raw":"colored")+".f32";std::ofstream f(directory/name,std::ios::binary);f.write(reinterpret_cast<const char*>(output.data()),output.size()*sizeof(float));index<<name<<','<<rate<<','<<clock<<','<<r<<','<<Chorus::independentLineRandomAmplitude<<'\n';
    }
}
}
int main(int argc,char** argv) {try {if(argc==2&&std::string(argv[1])=="--support"){exportSupport();return 0;}if(argc==3&&std::string(argv[1])=="--measure"){measure(argv[2]);return 0;}if(argc==3&&std::string(argv[1])=="--engine-measure"){engineMeasure(argv[2]);return 0;}require(argc==1,"usage: [--support | --measure NEWDIR | --engine-measure NEWDIR]");covariance();reconstruction();normalization();engine();return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
