#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowCutoffReconstruction.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>

namespace youknow
{
struct YouKnowTestAccess
{
    using Cascade = YouKnowEngine::OtaCascade;
    static constexpr double headroom = YouKnowEngine::otaHeadroomVolts;
    static constexpr double loopHeadroom =
        YouKnowEngine::VoicedResonanceCompatibilityProfile::loopHeadroomVolts;
    static constexpr double tau = YouKnowEngine::vcfHoldSlewSeconds;
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
        for (const double feedback : {0.,3.5})
            for (const Segment curve : {Segment{5000,12000}, Segment{12000,5000}})
            {
                Access::Cascade linear, cubic;
                youknow::CutoffReconstruction reconstruction;
                State reference {}, fine {};
                const double baseOmega=2*pi*curve.hz(0)/rate;
                linear.previousOmegaStep=cubic.previousOmegaStep=baseOmega;
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
                    const float input=static_cast<float>(.6*std::sin(2*pi*220*sample/rate));
                    const double left=linear.process(input,current,feedback,Access::headroom,false,0,&a,
                        youknow::VcfTanhMode::Exact,youknow::VcfSolverMode::MersonHalfSteps);
                    const double right=cubic.process(input,current,feedback,Access::headroom,false,0,&b,
                        youknow::VcfTanhMode::Exact,youknow::VcfSolverMode::MersonHalfSteps);
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
                    << " direction=" << (curve.initial<curve.target?"up":"down")
                    << " linear dBr=" << 10*std::log10(aError/energy)
                    << " cubic dBr=" << 10*std::log10(bError/energy)
                    << " oracle dBr=" << 10*std::log10(refError/energy) << '\n';
                expect(bError<aError,"cutoff curve failed audio admission against continuous-control oracle");
                expect(refError < bError*1e-4,"fine-step VCF oracle not converged below candidate error");
            }
}
}

int main()
{
    std::cout << std::setprecision(9);
    controlContracts();
    audioContracts();
    return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
