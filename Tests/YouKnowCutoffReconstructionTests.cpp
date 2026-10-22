#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowCutoffReconstruction.h"
#include "DSP/YouKnowActiveProductFidelity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <ctime>
#include <string_view>

namespace youknow
{
struct YouKnowTestAccess
{
    using Cascade = YouKnowEngine::OtaCascade;
    static constexpr double headroom = YouKnowEngine::otaHeadroomVolts;
    static constexpr double loopHeadroom =
        YouKnowEngine::VoicedResonanceCompatibilityProfile::loopHeadroomVolts;
    static constexpr double tau = YouKnowEngine::vcfHoldSlewSeconds;
    static bool checkWiring(const YouKnowEngine& engine, int& curved)
    {
        bool correct=true;
        for(int card=0;card<6;++card)
        {
            const auto& voice=engine.voices_[static_cast<std::size_t>(card)];
            const auto& reconstruction=voice.islandCutoffReconstruction;
            if(voice.filterIsland.frames==0 || !reconstruction.ready
                || engine.exactVcfControlInterval_[static_cast<std::size_t>(card)]) continue;
            const auto& context=voice.filterIsland.contexts[static_cast<std::size_t>(
                (voice.filterIsland.frames-1)&31)];
            const float expected=static_cast<float>(reconstruction.at(
                reconstruction.history[1],reconstruction.history[0],.5));
            const float linear=static_cast<float>(reconstruction.history[1]
                +.5*(reconstruction.history[0]-reconstruction.history[1]));
            correct=correct && context.omega[3]==expected;
            if(expected!=linear) ++curved;
        }
        return correct;
    }
};
}

namespace
{
using Access = youknow::YouKnowTestAccess;
constexpr double pi = 3.141592653589793238462643383279502884;
int failures = 0;
void expect(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; ++failures; }
}

// Independently reduced declared anti-log/current-limit equation. It calls
// neither the production mapping nor the reconstruction. Constants retain
// the existing model's coordinates, rather than fitting a hardware capture.
double frequency(double counts)
{
    const double raw = static_cast<double>(youknow::YouKnowEngine::vcfBaseFrequencyHz)
        * std::exp2(std::clamp(counts, -2000.0, 20000.0)
            / youknow::YouKnowEngine::vcfCountsPerOctave);
    const double p = youknow::YouKnowEngine::vcfControlSaturationExponent;
    const double inverse = std::pow(1.0 + std::pow(
        raw / youknow::YouKnowEngine::vcfControlSaturationHz, p), -1.0 / p);
    return std::min(50000.0, raw * inverse);
}
struct Segment
{
    double initial, target;
    double counts(double seconds) const
    { return target + (initial - target) * std::exp(-seconds / Access::tau); }
    double hz(double seconds) const { return frequency(counts(seconds)); }
};

void controlContracts()
{
    double linearError = 0, cubicError = 0, maxLinear = 0, maxCubic = 0;
    double standardLinear=0,standardCubic=0;
    int cases = 0;
    for (const double rate : {8000., 44100., 48000., 88200., 96000., 192000.})
        for (const double initial : {0., 4000., 8000., 12000., 16256.})
            for (const double target : {0., 4000., 8000., 12000., 16256.})
            {
                Segment curve {initial, target};
                youknow::CutoffReconstruction reconstruction;
                double before = static_cast<float>(curve.hz(0));
                reconstruction.advance(before, static_cast<float>(target), true);
                const int length = static_cast<int>(std::ceil(rate * .01));
                for (int sample = 1; sample <= length; ++sample)
                {
                    const double after = static_cast<float>(curve.hz(sample / rate));
                    reconstruction.advance(after, static_cast<float>(target), false);
                    for (int point = 0; point <= 24; ++point)
                    {
                        const double t = point / 24.;
                        const double truth = curve.hz((sample - 1 + t) / rate);
                        const double linear = before + t * (after - before);
                        const double cubic = reconstruction.at(before, after, t);
                        expect(cubic >= std::min(before, after) && cubic <= std::max(before, after),
                            "cutoff reconstruction escaped physical endpoint bounds");
                        if (point == 0) expect(cubic == before, "cutoff start changed");
                        if (point == 24) expect(cubic == after, "cutoff end changed");
                        const double a = (linear - truth) / std::max(1., truth);
                        const double b = (cubic - truth) / std::max(1., truth);
                        linearError += a*a; cubicError += b*b;
                        if(rate>=44100) {standardLinear+=a*a;standardCubic+=b*b;}
                        maxLinear = std::max(maxLinear, std::abs(a));
                        maxCubic = std::max(maxCubic, std::abs(b));
                    }
                    before = after;
                }
                ++cases;
            }
    std::cout << "control cases=" << cases << " relative squared-error ratio="
        << cubicError/linearError << " max linear=" << maxLinear
        << " max bounded cubic=" << maxCubic << '\n';
    std::cout << "standard-rate relative squared-error ratio=" << standardCubic/standardLinear << '\n';
    expect(cubicError < linearError, "bounded cutoff curve failed aggregate exponential-control admission");
    expect(standardCubic < standardLinear*.8, "bounded cutoff curve failed standard-rate exponential-control admission");

    // A boundary discards all older slopes, while settled values stay exact.
    youknow::CutoffReconstruction c;
    for (double value : {1., 2., 3., 4.}) c.advance(value, 1.f, false);
    c.advance(20., 2.f, false);
    expect(c.at(4.,20.,.25)==8., "target edit retained stale cutoff slope");
    c.advance(30., 2.f, true);
    expect(c.at(20.,30.,.25)==22.5, "converter event retained stale cutoff slope");
    c.reset();
    for (int i=0;i<8;++i) c.advance(8.,1.f,false);
    expect(c.at(8.,8.,.37)==8., "settled cutoff was altered");
}

using State = std::array<double,4>;
State plus(State state, const State& slope, double h)
{ for (int i=0;i<4;++i) state[i]+=h*slope[i]; return state; }
State rhs(const State& state, double time, const Segment& control, double feedback)
{
    State result;
    const double input=.6*std::sin(2*pi*220*time);
    const double omega=2*pi*control.hz(time);
    double previous=input-feedback*Access::loopHeadroom*std::tanh(state[3]/Access::loopHeadroom);
    for (int i=0;i<4;++i)
    {
        result[i]=omega*Access::headroom*std::tanh((previous-state[i])/Access::headroom);
        previous=state[i];
    }
    return result;
}
void referenceStep(State& state, double time, double h, const Segment& control, double feedback)
{
    const auto a=rhs(state,time,control,feedback);
    const auto b=rhs(plus(state,a,h/2),time+h/2,control,feedback);
    const auto c=rhs(plus(state,b,h/2),time+h/2,control,feedback);
    const auto d=rhs(plus(state,c,h),time+h,control,feedback);
    for(int i=0;i<4;++i) state[i]+=h/6*(a[i]+2*b[i]+2*c[i]+d[i]);
}

void audioContracts()
{
    for (const double rate : {44100.,48000.,96000.})
        for (const int factor : {2,4})
        for (const double feedback : {0.,3.5})
            for (const Segment curve : {Segment{5000,12000}, Segment{12000,5000}})
            {
                Access::Cascade linear, cubic;
                youknow::CutoffReconstruction reconstruction;
                State reference {}, fine {};
                const double baseOmega=2*pi*curve.hz(0)/rate;
                linear.previousOmegaStep=cubic.previousOmegaStep=baseOmega/factor;
                linear.previousFeedback=cubic.previousFeedback=feedback;
                linear.previousHeadroom=cubic.previousHeadroom=Access::headroom;
                linear.parameterHistoryPrimed=cubic.parameterHistoryPrimed=true;
                reconstruction.advance(static_cast<float>(baseOmega),static_cast<float>(curve.target),true);
                double before=static_cast<float>(baseOmega), aError=0, bError=0, refError=0, energy=0;
                const int length=static_cast<int>(rate*.012);
                for (int sample=1;sample<=length;++sample)
                {
                    const double current=static_cast<float>(2*pi*curve.hz(sample/rate)/rate);
                    reconstruction.advance(current,static_cast<float>(curve.target),false);
                    Access::Cascade::ControlTrajectory a,b;
                    for(std::size_t point=0;point<a.omegaStep.size();++point)
                    {
                        const double t=Access::Cascade::controlNodePositions[point];
                        a.omegaStep[point]=before+t*(current-before);
                        b.omegaStep[point]=reconstruction.at(before,current,t);
                        a.feedback[point]=b.feedback[point]=feedback;
                        a.headroom[point]=b.headroom[point]=Access::headroom;
                    }
                    // Reproduce the island's existing piecewise-linear read
                    // of the seven stored base-grid control values. The ODE
                    // oracle below evaluates the physical exponential at each
                    // fine RK stage instead, so this probe includes that
                    // remaining interpolation boundary rather than claiming
                    // the cubic is consumed continuously by production.
                    const auto sampleAt=[](const std::array<double,7>& values,double t) {
                        const auto& positions=Access::Cascade::controlNodePositions;
                        const auto right=std::lower_bound(positions.begin()+1,positions.end(),t);
                        const auto index=static_cast<std::size_t>(right-positions.begin());
                        const double u=(t-positions[index-1])/(positions[index]-positions[index-1]);
                        return values[index-1]+u*(values[index]-values[index-1]);
                    };
                    double left=0,right=0;
                    for(int phase=0;phase<factor;++phase)
                    {
                        Access::Cascade::ControlTrajectory localA,localB;
                        for(std::size_t point=0;point<7;++point)
                        {
                            const double t=(phase+Access::Cascade::controlNodePositions[point])/factor;
                            localA.omegaStep[point]=sampleAt(a.omegaStep,t)/factor;
                            localB.omegaStep[point]=sampleAt(b.omegaStep,t)/factor;
                            localA.feedback[point]=localB.feedback[point]=feedback;
                            localA.headroom[point]=localB.headroom[point]=Access::headroom;
                        }
                        const double time=(sample-1+(phase+1.)/factor)/rate;
                        const float input=static_cast<float>(.6*std::sin(2*pi*220*time));
                        left=linear.process(input,localA.omegaStep.back(),feedback,Access::headroom,false,0,&localA,
                            youknow::VcfTanhMode::Exact,youknow::VcfSolverMode::MersonHalfSteps);
                        right=cubic.process(input,localB.omegaStep.back(),feedback,Access::headroom,false,0,&localB,
                            youknow::VcfTanhMode::Exact,youknow::VcfSolverMode::MersonHalfSteps);
                    }
                    for(int step=0;step<64;++step)
                        referenceStep(reference,(sample-1+step/64.)/rate,1/(rate*64),curve,feedback);
                    for(int step=0;step<128;++step)
                        referenceStep(fine,(sample-1+step/128.)/rate,1/(rate*128),curve,feedback);
                    aError+=(left-fine[3])*(left-fine[3]);
                    bError+=(right-fine[3])*(right-fine[3]);
                    refError+=(reference[3]-fine[3])*(reference[3]-fine[3]);
                    energy+=fine[3]*fine[3];
                    before=current;
                }
                std::cout << "audio rate=" << rate << " feedback=" << feedback
                    << " island=" << factor
                    << " direction=" << (curve.initial<curve.target?"up":"down")
                    << " linear dBr=" << 10*std::log10(aError/energy)
                    << " cubic dBr=" << 10*std::log10(bError/energy)
                    << " oracle dBr=" << 10*std::log10(refError/energy) << '\n';
                expect(bError<aError,"cutoff curve failed audio admission against continuous-control oracle");
                expect(refError < bError*1e-4,"fine-step VCF oracle not converged below candidate error");
            }
}

void productWiring()
{
    auto engine=std::make_unique<youknow::YouKnowEngine>();
    youknow::ActiveProductFidelityProfile::configureBeforePrepare(*engine);
    engine->prepare(48000.,64,1);
    youknow::EngineParameters parameters;
    youknow::ActiveProductFidelityProfile::applyTo(parameters);
    parameters.enableVcfSettlingReconstruction=true;
    parameters.enableVoiceVcaControlReconstruction=false;
    parameters.enableContinuousChorusMuteLoading=false;
    parameters.calibration=0;
    parameters.cutoff=.15f;
    parameters.envDepth=.8f;
    parameters.attack=.1f;
    engine->setParameters(parameters);
    engine->noteOn(60,1);
    int curved=0;
    float left=0,right=0;
    for(int i=0;i<2000;++i)
    {
        engine->process(&left,&right,1);
        expect(Access::checkWiring(*engine,curved),"product island bypassed bounded cutoff reconstruction");
    }
    expect(curved>100,"product cutoff probe did not exercise settling curvature");
    std::cout << "product curved contexts=" << curved << '\n';
}

void benchmark()
{
    const auto run=[](bool enabled) {
        auto engine=std::make_unique<youknow::YouKnowEngine>();
        youknow::ActiveProductFidelityProfile::configureBeforePrepare(*engine);
        engine->prepare(48000.,64,1);
        engine->setInitialCircuitRate(youknow::CircuitRate::Normal);
        youknow::EngineParameters p;
        youknow::ActiveProductFidelityProfile::applyTo(p);
        p.enableVcfSettlingReconstruction=enabled;
        p.enableVoiceVcaControlReconstruction=false;
        p.enableContinuousChorusMuteLoading=false;
        p.vcfTanhMode=youknow::VcfTanhMode::PolyZoned;
        p.vcfFastEarlyMode=youknow::VcfFastEarlyMode::Cubic;
        p.vcfSolverMode=youknow::VcfSolverMode::Rk4Single;
        p.polyphony=6;p.cutoff=.4f;p.resonance=.6f;p.envDepth=.6f;p.vcfLfoDepth=.4f;
        p.attack=.1f;p.sustain=.8f;
        p.chorus=youknow::ChorusMode::Off;
        engine->setParameters(p);
        for(int note : {36,48,55,60,64,67}) engine->noteOn(note,.8f);
        std::array<float,64> left{},right{};
        for(int block=0;block<256;++block) engine->process(left.data(),right.data(),64);
        const auto start=std::clock();
        double energy=0;
        for(int block=0;block<3000;++block)
        {
            engine->process(left.data(),right.data(),64);
            for(int i=0;i<64;++i) energy+=left[i]*left[i]+right[i]*right[i];
        }
        return std::array<double,2>{double(std::clock()-start)/CLOCKS_PER_SEC,energy};
    };
    std::array<double,5> ratios{};
    for(int pair=0;pair<5;++pair)
    {
        const bool reverse=pair%2;
        const auto first=run(reverse),second=run(!reverse);
        const auto& baseline=reverse?second:first;
        const auto& candidate=reverse?first:second;
        ratios[static_cast<std::size_t>(pair)]=candidate[0]/baseline[0];
        std::cout << "pair=" << pair << " baseline CPU=" << baseline[0]
            << " candidate CPU=" << candidate[0]
            << " ratio=" << candidate[0]/baseline[0]
            << " raw RMS change dB=" << 10*std::log10(candidate[1]/baseline[1]) << std::endl;
    }
    std::sort(ratios.begin(),ratios.end());
    std::cout << "median CPU ratio=" << ratios[2] << '\n';
}
}

int main(int argc,char** argv)
{
    std::cout << std::setprecision(9);
    if(argc==2 && std::string_view(argv[1])=="--benchmark") { benchmark();return 0; }
    controlContracts();
    audioContracts();
    productWiring();
    return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
