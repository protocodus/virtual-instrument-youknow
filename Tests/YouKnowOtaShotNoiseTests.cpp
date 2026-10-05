#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace {bool guardAllocation=false;unsigned allocations=0;}
void* operator new(std::size_t n) {if(guardAllocation)++allocations;if(void*p=std::malloc(n))return p;throw std::bad_alloc{};}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void*p) noexcept {std::free(p);}
void operator delete[](void*p) noexcept {std::free(p);}
void operator delete(void*p,std::size_t) noexcept {std::free(p);}
void operator delete[](void*p,std::size_t) noexcept {std::free(p);}

namespace youknow {
struct YouKnowTestAccess {
    static constexpr double h=2.*.026*(68000.+560.)/560.;
    static double rate(const YouKnowEngine&e){return e.oversampledRate_;}
    static auto establishedRandom(const YouKnowEngine&e) {
        std::array<std::uint32_t,YouKnowEngine::maxVoices> cards{};
        for(int i=0;i<YouKnowEngine::maxVoices;++i)cards[i]=e.voices_[i].noiseState;
        return std::tuple{e.noiseState_,e.noiseGaussianSpare_,e.noiseGaussianSpareValid_,cards};
    }
    static auto deviceRandom(const YouKnowEngine&e) {
        std::array<std::tuple<std::uint32_t,double,bool>,YouKnowEngine::maxVoices*2> cards{};
        for(int i=0;i<YouKnowEngine::maxVoices;++i){const auto&a=e.voices_[i].filterShotRandom;const auto&b=e.voices_[i].vcaShotRandom;
            cards[2*i]={a.state,a.spare,a.hasSpare};cards[2*i+1]={b.state,b.spare,b.hasSpare};}
        return cards;
    }
    static double filter(YouKnowEngine&e,double fc,double feedback=0) {
        auto&v=e.voices_[0];auto&f=v.filter;
        f.offsetVoltage.fill(0);f.resonanceOffsetVolts=0;f.inputCompensationCoefficient=0;
        f.resonanceHeadroomFollowsStage=false;f.gScale.fill(1);
        YouKnowEngine::VoiceFilterFrame frame;
        frame.shotNoiseOrigin=f.state;frame.shotNoiseInput=0;
        frame.shotNoiseOmega=2*std::numbers::pi*fc/e.oversampledRate_;
        frame.shotNoiseFeedback=feedback;frame.shotNoiseHeadroom=h;
        const auto out=f.process<false>(0,frame.shotNoiseOmega,feedback,h,false,0);
        return e.applyVoiceFilterShotNoise(v,frame,out);
    }
    static double impulse(YouKnowEngine&e,double fc,double feedback,OtaShotNoise::Vector&state) {
        auto&v=e.voices_[0];v.filter.state={};v.filter.offsetVoltage.fill(0);
        v.filter.resonanceOffsetVolts=0;v.filter.inputCompensationCoefficient=0;
        v.filter.resonanceHeadroomFollowsStage=false;v.filter.gScale.fill(1);
        YouKnowEngine::VoiceFilterFrame frame;frame.shotNoiseHeadroom=h;
        frame.shotNoiseOmega=2*std::numbers::pi*fc/e.oversampledRate_;frame.shotNoiseFeedback=feedback;
        const auto out=e.applyVoiceFilterShotNoise(v,frame,0);state=v.filter.state;return out;
    }
    static double vca(YouKnowEngine&e,float gain) {
        auto&v=e.voices_[0];v.active=true;v.vca=gain;v.vcaInputTrim=1;
        return e.finishVoiceFilter(v,0);
    }
    static auto vcaRandom(const YouKnowEngine&e) {return e.voices_[0].vcaShotRandom;}
    static auto kernel(const YouKnowEngine&e) {return e.voiceVcaAntialiasKernel_;}
    static void retire(YouKnowEngine&e,int slot){e.silenceVoice(e.voices_[slot]);}
    static void freewheel(YouKnowEngine&e){e.freewheelVoiceCard(e.voices_[0]);}
    static auto cache(const YouKnowEngine&e){return e.voices_[0].filterShotCache;}
    static void disturbExtension(YouKnowEngine&e){(void)e.voices_[6].filterShotRandom.next();(void)e.voices_[6].vcaShotRandom.next();e.voices_[6].filterShotCache.primed=true;}
    static bool extensionCacheEmpty(const YouKnowEngine&e){return !e.voices_[6].filterShotCache.primed;}
    static bool rebuild(YouKnowEngine&e,int factor){e.anyVoiceActive_=false;e.oversamplingIdleSamples_=e.oversamplingQuietSamples_;
        e.rateTransition_=YouKnowEngine::RateTransition::FadingOut;e.rateTransitionGain_=0;return e.setOversamplingFactor(factor);}
    static auto saturated(YouKnowEngine&e,double fc,double k,const OtaShotNoise::Vector&state) {
        auto&v=e.voices_[0];v.filter.reset();v.filter.state=state;
        v.filter.offsetVoltage.fill(0);v.filter.resonanceOffsetVolts=0;
        v.filter.inputCompensationCoefficient=0;v.filter.resonanceHeadroomFollowsStage=false;
        YouKnowEngine::VoiceFilterFrame frame;frame.shotNoiseOrigin=state;frame.shotNoiseHeadroom=h;
        frame.shotNoiseOmega=2*std::numbers::pi*fc/e.oversampledRate_;frame.shotNoiseFeedback=k;
        (void)e.applyVoiceFilterShotNoise(v,frame,static_cast<float>(state[3]));
        return v.filterShotCache;
    }
};
}

namespace {
using namespace youknow;
void require(bool b,const char* text) {if(!b) throw std::runtime_error(text);}
using Matrix=OtaShotNoise::Matrix;
using Vector=OtaShotNoise::Vector;
using WideMatrix=std::array<std::array<long double,4>,4>;
WideMatrix ode(const Matrix&a,const Vector&d,const WideMatrix&q) {
    WideMatrix result{};
    for(int i=0;i<4;++i)for(int j=0;j<4;++j) {
        result[i][j]=i==j?d[i]:0;
        for(int k=0;k<4;++k)result[i][j]+=a[i][k]*q[k][j]+q[i][k]*a[j][k];
    }
    return result;
}
WideMatrix add(const WideMatrix&q,const WideMatrix&k,long double step) {
    auto out=q;for(int i=0;i<4;++i)for(int j=0;j<4;++j)out[i][j]+=step*k[i][j];return out;
}
// Independent continuous Lyapunov ODE integration in long double. It neither
// uses the implementation's power series nor its scaling/doubling formula.
WideMatrix oracle(const Matrix&a,const Vector&d,int steps=1024) {
    WideMatrix q{};const long double step=1.L/steps;
    for(int n=0;n<steps;++n) {
        const auto k1=ode(a,d,q),k2=ode(a,d,add(q,k1,step/2));
        const auto k3=ode(a,d,add(q,k2,step/2)),k4=ode(a,d,add(q,k3,step));
        for(int i=0;i<4;++i)for(int j=0;j<4;++j)q[i][j]+=step/6*(k1[i][j]+2*k2[i][j]+2*k3[i][j]+k4[i][j]);
    }
    return q;
}
template<class Reference> double error(const Matrix&a,const Reference&b) {
    long double difference=0,power=0;
    for(int i=0;i<4;++i)for(int j=0;j<4;++j){const auto delta=a[i][j]-b[i][j];difference+=delta*delta;power+=b[i][j]*b[i][j];}
    return power>0?std::sqrt(difference/power):difference==0?0:std::numeric_limits<double>::infinity();
}
// Frozen pre-optimization positive quadrature. Keep this recurrence explicit
// and independent of the shared-power/Horner implementation: it pins the
// same Taylor8 polynomial, quadrature nodes, covariance doubling and PSD
// behavior without altering the continuous ODE oracle or its tolerances.
Matrix frozenPositiveCovariance(Matrix a,Vector d) {
    double norm=0;for(const auto&row:a){double sum=0;for(double x:row)sum+=std::abs(x);norm=std::max(norm,sum);}
    int squares=0;while(norm>.25&&squares<12){norm*=.5;++squares;}
    const double scale=std::ldexp(1.0,-squares);
    for(auto&row:a)for(double&x:row)x*=scale;
    for(double&x:d)x*=scale;
    constexpr Vector nodes{.06943184420297371,.33000947820757187,.6699905217924281,.9305681557970262};
    constexpr Vector weights{.1739274225687269,.3260725774312731,.3260725774312731,.1739274225687269};
    const auto exponential=[&](double position,int terms) {
        Matrix f{},term{};for(std::size_t i=0;i<4;++i)f[i][i]=term[i][i]=1;
        for(int order=1;order<=terms;++order){Matrix next{};
            for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<4;++j) {
                next[i][j]=(a[i][i]*term[i][j]+a[i][(i+3)&3]*term[(i+3)&3][j])*position/order;
                f[i][j]+=next[i][j];
            }
            term=next;
        }return f;
    };
    Matrix q{};
    for(std::size_t node=0;node<4;++node) {
        const auto f=exponential(nodes[node],8);
        for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<=i;++j) {
            double value=0;for(std::size_t k=0;k<4;++k)value+=f[i][k]*d[k]*f[j][k];
            q[i][j]+=weights[node]*value;
        }
    }
    for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<i;++j)q[j][i]=q[i][j];
    if(squares>0){auto f=exponential(1,8);
        for(int square=0;square<squares;++square){const auto propagated=OtaShotNoise::multiply(OtaShotNoise::multiply(f,q),OtaShotNoise::transpose(f));
            for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<4;++j)q[i][j]+=propagated[i][j];
            f=OtaShotNoise::multiply(f,f);}
        for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<i;++j)q[i][j]=q[j][i]=.5*(q[i][j]+q[j][i]);
    }return q;
}
void sharedPositiveCovarianceParity() {
    double worstCovariance=0,worstProjection=0,worstInnovation=0;
    std::size_t cases=0;
    const auto screen=[&](const Matrix&a,const Vector&d) {
        const auto expected=frozenPositiveCovariance(a,d);
        const auto actual=OtaShotNoise::positiveCovariance(a,d);
        const double covarianceError=error(actual,expected);
        worstCovariance=std::max(worstCovariance,covarianceError);
        require(covarianceError<1e-10,"shared Taylor8 covariance differs from the frozen polynomial");
        bool expectedValid=false,actualValid=false;
        const auto expectedL=OtaShotNoise::factor(expected,&expectedValid);
        const auto actualL=OtaShotNoise::factor(actual,&actualValid);
        require(actualValid==expectedValid,"shared Taylor8 changed positive Cholesky validity");
        for(std::size_t i=0;i<4;++i) {
            require(std::isfinite(actual[i][i]),"shared Taylor8 produced nonfinite node variance");
            if(expected[i][i]>0) {
                const double relative=std::abs(actual[i][i]/expected[i][i]-1);
                worstProjection=std::max(worstProjection,relative);
                require(relative<1e-10,"shared Taylor8 changed a tiny projected-node variance");
            } else require(actual[i][i]==0,"shared Taylor8 introduced an unreachable node variance");
        }
        if(actualValid) {
            const auto realized=OtaShotNoise::multiply(actualL,OtaShotNoise::transpose(actualL));
            require(error(realized,actual)<1e-12,"shared Taylor8 covariance lost Cholesky reconstruction");
            // Basis vectors expose every Cholesky column; mixed vectors
            // also screen a realized innovation. Normalize each node by its
            // own standard deviation, not the dominant covariance norm.
            constexpr std::array<Vector,6> normals{{Vector{1,0,0,0},Vector{0,1,0,0},
                Vector{0,0,1,0},Vector{0,0,0,1},Vector{.3,-.7,1.1,-.4},Vector{-1,.5,-.25,1.5}}};
            for(const auto&normal:normals) {
                const auto reference=OtaShotNoise::applyFactor(expectedL,normal);
                const auto result=OtaShotNoise::applyFactor(actualL,normal);
                for(std::size_t i=0;i<4;++i) {
                    if(expected[i][i]>0) {
                        const double normalized=std::abs(result[i]-reference[i])/std::sqrt(expected[i][i]);
                        worstInnovation=std::max(worstInnovation,normalized);
                        require(normalized<1e-8,"shared Taylor8 changed a normalized sparse-node innovation");
                    } else require(result[i]==reference[i],"shared Taylor8 changed an unreachable innovation");
                }
            }
        }
        ++cases;
    };
    // Normalize to actual interval row norms, spanning physical low/high
    // cutoff cases and several additional covariance-doubling levels.
    constexpr std::array<Vector,6> spreads{{Vector{1,1,1,1},Vector{.95,1.05,.2,1.7},
        Vector{1,1e-6,.02,.8},Vector{1,0,1,1},Vector{0,1,1,1},Vector{1,0,0,1}}};
    for(const auto&spread:spreads)
    for(double intervalNorm:{.0001,.0625,.125,.25,.5,1.,3.,12.,32.,64.})
    for(double feedback:{0.,4.504,8.}) {
        Matrix a{};double norm=0;
        for(std::size_t i=0;i<4;++i) {
            a[i][i]=-spread[i];a[i][(i+3)&3]=spread[i]*(i==0?-feedback:1);
            norm=std::max(norm,std::abs(a[i][i])+std::abs(a[i][(i+3)&3]));
        }
        for(auto&row:a)for(double&value:row)value*=intervalNorm/norm;
        screen(a,{});
        for(int source=0;source<4;++source)
        for(double floor:{0.,1e-30,1e-20,.009,.011}) {
            Vector d{};for(int i=0;i<4;++i)d[i]=i==source?1:floor;
            screen(a,d);
        }
        screen(a,{1e-7,1e-12,2e-12,.7e-12});
    }
    // The existing Q33 regression needs relative precision even though its
    // variance is tiny. Keep its independently pinned tolerance unchanged.
    Matrix critical{};for(int i=0;i<4;++i){critical[i][i]=-.0625;if(i)critical[i][i-1]=.0625;}
    screen(critical,{1,0,0,0});
    const auto q=OtaShotNoise::positiveCovariance(critical,{1,0,0,0});
    require(std::abs(q[3][3]/2.120410e-10-1)<1e-6,"shared Taylor8 lost the fourth-stage sparse regression");
    screen({},{});screen({},{1,0,0,0});screen({},{1e-30,1e-20,1e-12,1});
    std::cout<<"shared Taylor8 frozen parity cases="<<cases<<"; covariance="<<worstCovariance
        <<"; projected="<<worstProjection<<"; normalized innovation="<<worstInnovation<<"\n";
}
void currentAndCovariance() {
    constexpr double q=1.602176634e-19,C=240e-12,H=YouKnowTestAccess::h;
    for(double current:{0.,1e-9,1e-6,.000302079})for(double y:{0.,.1,.5,.9,1.})
        require(std::abs(OtaShotNoise::currentPsd(current,y)-2*q*current*(1-y*y))<1e-36,"collector PSD disagrees with Schottky projection");
    double worst=0,refinement=0;
    for(double fs:{8000.,44100.,48000.,96000.,192000.,768000.})for(double fc:{248.,1000.,std::min(18000.,fs*.45)})for(double k:{0.,.2,4.504}) {
        const double step=2*std::numbers::pi*fc/fs;
        Matrix a{};Vector d{};
        for(int i=0;i<4;++i){const double spread=std::array<double,4>{.95,1.05,1.02,.98}[i];
            a[i][i]=-step*spread;if(i>0)a[i][i-1]=step*spread;else a[0][3]=-step*spread*k;
            const double tail=2*std::numbers::pi*fc*C*H,cap=C/spread;
            d[i]=2*q*tail/(2*cap*cap*fs);}
        // Resonance current through the first pair's incremental gm.
        const double resTail=k*(2*.026*100000/1500)/68000;
        d[0]+=2*q*resTail*std::pow(step*.95*fs*68000,2)/(2*fs);
        const auto reference=oracle(a,d),fine=oracle(a,d,2048);
        refinement=std::max(refinement,error(OtaShotNoise::cascadeCovariance(a,d),fine)
            -error(OtaShotNoise::cascadeCovariance(a,d),reference));
        const auto result=OtaShotNoise::cascadeCovariance(a,d);
        worst=std::max(worst,error(result,fine));
        require(error(result,OtaShotNoise::covariance(a,d))<1e-5,"sparse covariance differs from unstructured circuit solve");
        const auto reconstructed=OtaShotNoise::multiply(OtaShotNoise::factor(result),OtaShotNoise::transpose(OtaShotNoise::factor(result)));
        require(error(reconstructed,result)<1e-12,"noise covariance failed positive Cholesky reconstruction");
    }
    require(worst<1e-5&&refinement<1e-7,"physical interval covariance differs from independent ODE oracle");
    OtaShotNoise::Cache cache;double worstReuse=0;
    for(int n=0;n<3000;++n) {
        const double step=.13*(1+.07*std::sin(n*.008)),feedback=4.504*(.5+.5*std::sin(n*.004));
        Matrix a{};Vector d{};
        for(int i=0;i<4;++i){const double s=1-std::pow(.3*std::sin(n*.012+i),2);
            a[i][i]=-step*s;a[i][(i+3)&3]=step*s*(i==0?-feedback:1);d[i]=1e-8*step*s;}
        (void)cache.next(a,d,{});
        const auto actual=OtaShotNoise::multiply(cache.l,OtaShotNoise::transpose(cache.l));
        worstReuse=std::max(worstReuse,error(actual,OtaShotNoise::covariance(a,d)));
    }
    require(worstReuse<.003,"factor reuse exceeded .3% covariance budget");
    // Reuse is separately screened on stiff/near-onset, high feedback and
    // vanishing diffusion, including abrupt step and exact-zero intervals.
    for(double scale:{.0001,.01,.1,1.,3.})for(double level:{1e-30,1e-12,1e-7}) {
        OtaShotNoise::Cache edge;
        for(int n=0;n<160;++n) {
            Matrix a{};Vector d{};
            const double k=n==80?0:4.504*(.5+.5*std::cos(n*.03));
            for(int i=0;i<4;++i){const double sensitivity=std::pow(.8+.19*std::sin(n*.008+i),2);
                const double step=scale*(n<80?1:1.1)*sensitivity;
                a[i][i]=-step;a[i][(i+3)&3]=step*(i==0?-k:1);
                d[i]=n==79?0:level*sensitivity;}
            (void)edge.next(a,d,{});const auto actual=OtaShotNoise::multiply(edge.l,OtaShotNoise::transpose(edge.l));
            worstReuse=std::max(worstReuse,error(actual,OtaShotNoise::covariance(a,d)));
        }
    }
    require(worstReuse<.003,"factor reuse failed stiff/low-diffusion transition screen");
    std::cout<<"covariance oracle relative="<<worst<<"; reuse="<<worstReuse<<"\n";
}
void randomChecks() {
    OtaShotNoise::Random random;random.seed(0x106);constexpr int n=1<<20;
    long double sum=0,power=0,fourth=0;int tail=0;
    for(int i=0;i<n;++i){const double x=random.next();sum+=x;power+=x*x;fourth+=x*x*x*x;if(std::abs(x)>3)++tail;}
    require(std::abs(sum/n)<.005&&std::abs(power/n-1)<.01&&std::abs(fourth/n-3)<.05,"bounded Gaussian sampler failed moment screen");
    require(std::abs(static_cast<double>(tail)/n-.002699796)<.0003,"bounded Gaussian sampler lost tails");
}
void sparseDiffusion() {
    double worst=0;
    for(double scale:{.0001,.0625,.125,1.,3.})for(double k:{0.,4.504,8.})for(int source=0;source<4;++source)for(double floor:{0.,1e-20,.009,.011}) {
        Matrix a{};Vector d{};
        for(int i=0;i<4;++i){const double spread=std::array<double,4>{.95,1.05,.2,1.7}[i];a[i][i]=-scale*spread;a[i][(i+3)&3]=scale*spread*(i==0?-k:1);d[i]=i==source?1:floor;}
        const auto actual=OtaShotNoise::cascadeCovariance(a,d);
        const auto expected=oracle(a,d,2048);
        bool valid;const auto l=OtaShotNoise::factor(actual,&valid);
        require(valid,"unequal/saturated diffusion produced an indefinite covariance");
        const auto realized=OtaShotNoise::multiply(l,OtaShotNoise::transpose(l));
        for(int i=0;i<4;++i)if(expected[i][i]>0) {
            const double relative=std::abs(realized[i][i]/expected[i][i]-1);worst=std::max(worst,relative);
            require(relative<1e-4,"sparse diffusion projected-node variance differs from independent ODE");
        } else require(realized[i][i]==0,"noise appeared in an unreachable quiet node");
    }
    // Explicit regression: the fast four-term Lyapunov series used to lose
    // Q33 for a source only at stage0 and then silently floor a negative pivot.
    Matrix a{};Vector d{1,0,0,0};for(int i=0;i<4;++i){a[i][i]=-.0625;if(i)a[i][i-1]=.0625;}
    const auto q=OtaShotNoise::cascadeCovariance(a,d);
    require(std::abs(q[3][3]/2.120410e-10-1)<1e-6,"single-source fourth-stage variance regression");
    std::cout<<"unequal/single-source/saturated projected variances="<<worst<<"\n";
}
std::unique_ptr<YouKnowEngine> make(double fs=48000,int quality=1,bool aa=false) {
    auto e=std::make_unique<YouKnowEngine>();EngineParameters p;
    p.enableOtaShotNoise=true;p.enableVoiceVcaAntialias=aa;p.calibration=1;
    p.enableVcfEarlyEffect=false;p.enableCardJohnsonFloor=false;
    p.chorus=ChorusMode::Off;p.chorusNoise=p.noiseLevel=0;
    e->setParameters(p);e->prepare(fs,128,quality);return e;
}
void fft(std::vector<std::complex<double>>&x) {
    const auto n=x.size();for(std::size_t i=1,j=0;i<n;++i){std::size_t bit=n/2;while(j&bit){j^=bit;bit/=2;}j^=bit;if(i<j)std::swap(x[i],x[j]);}
    for(std::size_t len=2;len<=n;len*=2){const auto wlen=std::polar(1.,-2*std::numbers::pi/len);
        for(std::size_t i=0;i<n;i+=len){std::complex<double>w=1;for(std::size_t j=0;j<len/2;++j){const auto a=x[i+j],b=x[i+j+len/2]*w;x[i+j]=a+b;x[i+j+len/2]=a-b;w*=wlen;}}}
}
void filterPsd() {
    constexpr double q=1.602176634e-19,C=240e-12,H=YouKnowTestAccess::h,fc=248;
    constexpr int count=65536;double worstVariance=0,worstPsd=0;
    const double variance=35*q*H/(32*C);
    for(double host:{8000.,44100.,48000.,96000.,192000.})for(int quality:{1,2,4}) {
        auto e=make(host,quality);const double fs=YouKnowTestAccess::rate(*e);
        std::vector<std::complex<double>> bins(count);double power=0;
        for(int i=0;i<4096;++i)(void)YouKnowTestAccess::filter(*e,fc);
        for(auto&x:bins){const double sample=YouKnowTestAccess::filter(*e,fc);x=sample;power+=sample*sample/count;}
        worstVariance=std::max(worstVariance,std::abs(power/variance-1));fft(bins);
        const double lambda=2*std::numbers::pi*fc,tail=lambda*C*H;
        for(auto [lo,hi]:{std::pair{70.,200.},std::pair{350.,900.},std::pair{1500.,3000.}}) {
            double measured=0,expected=0;int used=0;
            for(int bin=std::ceil(lo*count/fs);bin<=std::floor(hi*count/fs);++bin) {
                const double frequency=bin*fs/count;double psd=0;
                // Continuous white pair current is integrated by a capacitor.
                // Sampling its OU state folds the continuous tails. Account
                // for that sum explicitly, rather than fitting a rate factor.
                for(int alias=-100;alias<=100;++alias) {
                    const double w=2*std::numbers::pi*(frequency+alias*fs);
                    const double r=lambda*lambda/(lambda*lambda+w*w);
                    psd+=2*q*tail/(C*C*(lambda*lambda+w*w))*(1+r+r*r+r*r*r);
                }
                measured+=2*std::norm(bins[bin])/(fs*count);expected+=psd;++used;
            }
            if(used>20)worstPsd=std::max(worstPsd,std::abs(measured/expected-1));
        }
    }
    require(worstVariance<.22&&worstPsd<.3,"actual filter OTA PSD disagrees with analog pole-chain oracle");
    // Full engine-node innovation covariance, including the separate RES OTA.
    for(double k:{0.,.5,4.504}) {
        auto e=make();const double fs=YouKnowTestAccess::rate(*e),step=2*std::numbers::pi*fc/fs;
        Matrix a{},observed{};Vector d{};
        for(int i=0;i<4;++i){a[i][i]=-step;a[i][(i+3)&3]=step*(i==0?-k:1);d[i]=q*(2*std::numbers::pi*fc*C*H)/(C*C*fs);}
        d[0]+=q*(k*(2*.026*100000/1500)/68000)*std::pow(step*fs*68000,2)/fs;
        for(int n=0;n<count;++n){Vector sample;(void)YouKnowTestAccess::impulse(*e,fc,k,sample);
            for(int i=0;i<4;++i)for(int j=0;j<4;++j)observed[i][j]+=sample[i]*sample[j]/count;}
        require(error(observed,oracle(a,d))<.025,"engine RES/stage node noise current mapping differs from physical covariance");
    }
    std::cout<<"actual VCF: all five rates/three qualities; variance relative="<<worstVariance<<"; PSD bands="<<worstPsd<<"\n";
}
void vcaChecks() {
    // Independently restate the stored-service Tr20 current coordinate.
    // Its 302.079 uA is at sustain stored4064, whereas voice.vca is unity4095.
    constexpr double q=1.602176634e-19,R=47000,convert=1./2.6;
    const double full=YouKnowEngine::VoiceVcaSignalLaw::fullControlTailAmps;
    const double atService=YouKnowEngine::VoiceVcaControlLaw::gain(4064.f/4095.f);
    double worst=0;
    for(double host:{8000.,44100.,48000.,96000.,192000.})for(int quality:{1,2,4})for(float gain:{.01f,.1f,1.f}) {
        auto e=make(host,quality);const double fs=YouKnowTestAccess::rate(*e);
        double power=0;constexpr int n=32768;
        for(int i=0;i<n;++i){const double v=YouKnowTestAccess::vca(*e,gain);power+=v*v/n;}
        const double expected=2*q*(full/atService*gain)*R*R*fs/2*convert*convert;
        worst=std::max(worst,std::abs(power/expected-1));
        require(std::abs(power/expected-1)<.035,"VCA output-current noise density/gain/rate is wrong");
        for(int i=0;i<100;++i)require(YouKnowTestAccess::vca(*e,0)==0,"closed VCA leaks bare pair current noise");
    }
    // Exercise actual VCA stage + high-rate output-noise injection. The
    // independent convolution uses the matched delayed CONTROL timestamp;
    // adding current before the envelope or using today's gain fails this.
    for(int quality:{1,2,4}) {
        auto e=make(48000,quality,true);auto random=YouKnowTestAccess::vcaRandom(*e);
        const auto kernel=YouKnowTestAccess::kernel(*e);const double fs=YouKnowTestAccess::rate(*e);
        constexpr int n=900;std::vector<float> gains(n),noise(n*kernel.factor);
        for(int i=0;i<n;++i)gains[i]=i<150?0:i<350?.1f:i<500?.8f:i<650?.01f:0;
        double maximum=0;
        for(int i=0;i<n;++i) {
            for(int phase=0;phase<kernel.factor;++phase) {
                double gain=gains[i];
                if(kernel.factor>1){const int at=i-24;const double a=at>=0?gains[at]:0,b=at+1>=0?gains[at+1]:0;gain=a+static_cast<double>(phase)/kernel.factor*(b-a);}
                const double physical=random.next()*R*std::sqrt(q*(full/atService*gain)*fs*kernel.factor);
                noise[i*kernel.factor+phase]=static_cast<float>(physical/YouKnowEngine::VoiceVcaSignalLaw::serviceGain());
            }
            double expected=noise[i*kernel.factor];
            if(kernel.factor>1){expected=0;for(int tap=0;tap<kernel.taps;++tap){const int at=i*kernel.factor-tap;if(at>=0)expected+=kernel.decimation[tap]*noise[at];}}
            expected*=YouKnowEngine::VoiceVcaSignalLaw::serviceGain()*convert;
            const double actual=YouKnowTestAccess::vca(*e,gains[i]);maximum=std::max(maximum,std::abs(actual-expected));
            if(i>750)require(actual==0,"VCA noise tail did not drain after current closes");
        }
        require(maximum<2e-11,"VCA shot-noise control/FIR alignment differs from independent convolution");
    }
    std::cout<<"actual VCA current/rate variance="<<worst<<"; delayed control/current/FIR oracle passed\n";
}
double filterVariance(double fc,double k) {
    // Independent frequency-domain pole-chain integral, including the RES
    // pair as an additional first-node current source. tan(theta) maps the
    // entire physical frequency axis to a finite quadrature interval.
    constexpr double q=1.602176634e-19,C=240e-12,H=YouKnowTestAccess::h;
    const double lambda=2*std::numbers::pi*fc,tail=lambda*C*H;
    const double resTail=k*(2*.026*100000/1500)/68000;
    const double resRatio=resTail/tail*std::pow(lambda*68000*C,2);
    const auto integrand=[&](double theta) {
        if(theta>=std::numbers::pi/2)return 1.;
        const double x=std::tan(theta),r=1/(1+x*x);
        const std::complex<double> z=1./std::complex<double>{1,x};
        return (1+r+r*r+r*r*r*(1+resRatio))/std::norm(1.+k*z*z*z*z);
    };
    constexpr int n=8192;const double step=(std::numbers::pi/2)/n;
    double total=integrand(0)+integrand(std::numbers::pi/2);
    for(int i=1;i<n;++i)total+=(i&1?4:2)*integrand(i*step);
    return 2*q*tail/(C*C*lambda*lambda)*lambda/(2*std::numbers::pi)*total*step/3;
}
void highCutoffVariance() {
    double worst=0;
    for(double host:{8000.,48000.,192000.})for(int quality:{1,4})for(double k:{0.,3.5}) {
        auto e=make(host,quality);const double fs=YouKnowTestAccess::rate(*e);
        for(double fc:{1000.,fs*.3}) {
            e->reset();for(int i=0;i<8192;++i)(void)YouKnowTestAccess::filter(*e,fc,k);
            double power=0;constexpr int n=65536;
            for(int i=0;i<n;++i){const auto x=YouKnowTestAccess::filter(*e,fc,k);power+=x*x/n;}
            const auto expected=filterVariance(fc,k);worst=std::max(worst,std::abs(power/expected-1));
            require(std::abs(power/expected-1)<.25,"actual high-cutoff/high-res noise variance differs from analog transfer");
        }
    }
    std::cout<<"actual high cutoff/RES3.5: analog total variance relative="<<worst<<"\n";
    double sparseWorst=0;
    for(double host:{8000.,48000.,192000.})for(int quality:{1,4})for(double k:{0.,4.504,8.}) {
        auto e=make(host,quality);const double fs=YouKnowTestAccess::rate(*e);
        for(const Vector&state:{Vector{60,-60,60,-60},Vector{0,64,64,64},Vector{64,0,0,-64}}) {
            const auto cache=YouKnowTestAccess::saturated(*e,fs*.45,k,state);
            const auto actual=OtaShotNoise::multiply(cache.l,OtaShotNoise::transpose(cache.l));
            const auto expected=oracle(cache.a,cache.d,2048);
            for(int i=0;i<4;++i)if(expected[i][i]>0){const double relative=std::abs(actual[i][i]/expected[i][i]-1);sparseWorst=std::max(sparseWorst,relative);
                require(relative<1e-4,"actual saturated-node innovation variance is wrong");}
        }
    }
    std::cout<<"actual high cutoff/saturated covariance projected variance="<<sparseWorst<<"\n";
}
struct Render {std::vector<float> audio;decltype(YouKnowTestAccess::establishedRandom(std::declval<YouKnowEngine&>())) oldRandom;
    decltype(YouKnowTestAccess::deviceRandom(std::declval<YouKnowEngine&>())) deviceRandom;};
Render render(bool enabled,float character,double fs,int quality,int block) {
    auto e=std::make_unique<YouKnowEngine>();EngineParameters p;ProductFidelityProfile::applyTo(p);
    p.enableOtaShotNoise=enabled;p.calibration=character;p.chorus=ChorusMode::Off;p.chorusNoise=p.noiseLevel=0;
    p.sawEnabled=true;p.pulseEnabled=false;p.subLevel=0;p.attack=0;p.release=0;p.sustain=1;
    p.cutoff=.35f;p.resonance=.15f;p.polyphony=16;
    e->setParameters(p);e->prepare(fs,block,quality);e->noteOn(60,1);
    Render r;r.audio.resize(2048);std::vector<float> right(r.audio.size());
    int at=0;
    for(int segment=0;segment<4;++segment) {
        if(segment==1){e->noteOff(60);e->allNotesOff();}
        if(segment==2)for(int i=0;i<16;++i)e->noteOn(48+i,1);
        if(segment==3){e->allNotesOff();e->setOversamplingFactor(quality==1?4:1);}
        const int end=(segment+1)*512;
        while(at<end){const int size=std::min(block,end-at);guardAllocation=true;e->process(r.audio.data()+at,right.data()+at,size);guardAllocation=false;at+=size;}
    }
    for(float v:r.audio)require(std::isfinite(v),"OTA lifecycle generated nonfinite audio");
    r.oldRandom=YouKnowTestAccess::establishedRandom(*e);r.deviceRandom=YouKnowTestAccess::deviceRandom(*e);return r;
}
void lifecycle() {
    require(!EngineParameters{}.enableOtaShotNoise,"raw noise reference changed");
    EngineParameters p;ProductFidelityProfile::applyTo(p);require(p.enableOtaShotNoise,"product omitted OTA noise");
    for(double fs:{8000.,44100.,48000.,96000.,192000.})for(int quality:{1,2,4}) {
        const auto zero=render(false,0,fs,quality,128),enabledZero=render(true,0,fs,quality,128);
        require(zero.audio==enabledZero.audio,"shot noise changed Unit Character zero reference");
        const auto one=render(true,1,fs,quality,1),blocked=render(true,1,fs,quality,137),raw=render(false,1,fs,quality,137);
        require(one.audio==blocked.audio&&one.deviceRandom==blocked.deviceRandom,"OTA noise depends on host block partition");
        require(raw.oldRandom==blocked.oldRandom&&zero.oldRandom==enabledZero.oldRandom,"OTA noise disturbed established random streams");
        require(raw.audio!=blocked.audio,"OTA noise failed to reach actual engine");
    }
    require(allocations==0,"OTA process allocated memory");
    auto e=make(),fresh=make();for(int i=0;i<29;++i)(void)YouKnowTestAccess::filter(*e,1000);
    e->reset();require(YouKnowTestAccess::deviceRandom(*e)==YouKnowTestAccess::deviceRandom(*fresh),"hard reset did not restore device streams");
    require(!YouKnowTestAccess::cache(*e).primed,"hard reset retained cached covariance");
    const auto physical=YouKnowTestAccess::deviceRandom(*e);YouKnowTestAccess::retire(*e,0);
    require(physical==YouKnowTestAccess::deviceRandom(*e),"physical retirement reseeded device noise");
    auto active=make();for(int i=0;i<13;++i){YouKnowTestAccess::freewheel(*e);(void)YouKnowTestAccess::filter(*active,1000);(void)YouKnowTestAccess::vca(*active,0);}
    require(YouKnowTestAccess::deviceRandom(*e)==YouKnowTestAccess::deviceRandom(*active),"powered-card freewheel lost device draw chronology");
    const auto powered=YouKnowTestAccess::deviceRandom(*e);
    (void)YouKnowTestAccess::filter(*e,1000);const auto beforeGrid=YouKnowTestAccess::deviceRandom(*e);
    require(YouKnowTestAccess::rebuild(*e,4)&&!YouKnowTestAccess::cache(*e).primed,"quality rebuild retained cached covariance");
    require(beforeGrid==YouKnowTestAccess::deviceRandom(*e),"quality rebuild reset/advanced device streams");
    YouKnowTestAccess::disturbExtension(*e);YouKnowTestAccess::retire(*e,6);
    require(YouKnowTestAccess::deviceRandom(*e)[12]==powered[12]&&YouKnowTestAccess::deviceRandom(*e)[13]==powered[13]
        &&YouKnowTestAccess::extensionCacheEmpty(*e),"extension retirement retained frozen device history");
    std::cout<<"actual engine lifecycle: all rates/qualities; Character0, RNG, reset, idle, block, noalloc passed\n";
}
double cost(bool enabled,int voices,int quality) {
    auto e=std::make_unique<YouKnowEngine>();EngineParameters p;
    ProductFidelityProfile::applyTo(p);p.enableOtaShotNoise=enabled;
    p.chorus=ChorusMode::Off;p.chorusNoise=0;p.noiseLevel=0;p.polyphony=voices;
    p.cutoff=.55f;p.resonance=.4f;p.attack=0;p.sustain=1;p.calibration=.7f;
    e->setParameters(p);e->prepare(48000,128,quality);
    for(int i=0;i<voices;++i)e->noteOn(48+i*2,1);
    std::array<float,128> left{},right{};
    for(int i=0;i<30;++i)e->process(left.data(),right.data(),128);
    const auto begin=std::chrono::steady_clock::now();
    for(int i=0;i<150;++i)e->process(left.data(),right.data(),128);
    return std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
}
void costs(){for(int voices:{6,16})for(int q:{1,4}){
    const double before=cost(false,voices,q),after=cost(true,voices,q);
    std::cout<<"CPU "<<voices<<" voices quality="<<q<<": "<<before<<" -> "<<after
        <<" s / .4 s audio (ratio "<<after/before<<")\n";
}}
}
int main(){try{randomChecks();sharedPositiveCovarianceParity();currentAndCovariance();sparseDiffusion();filterPsd();highCutoffVariance();vcaChecks();lifecycle();costs();}catch(const std::exception&e){guardAllocation=false;std::cerr<<e.what()<<'\n';return 1;}return 0;}
