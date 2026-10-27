#pragma STDC FENV_ACCESS ON

#include "DSP/YouKnowFinite.h"
#include "DSP/YouKnowEngine.h"

#include <array>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace youknow
{
struct YouKnowTestAccess
{
    using Cascade=YouKnowEngine::OtaCascade;
    using Trajectory=Cascade::ControlTrajectory;
    static double clamp(double value) {return Cascade::clampOmegaStep(value);}
};
}

namespace
{
void require(bool value,const char* message)
{if(!value)throw std::runtime_error(message);}

template<typename Value,typename Bits>
Value fromBits(Bits bits)
{
    static_assert(sizeof(Value)==sizeof(Bits));
    Value value;
    std::memcpy(&value,&bits,sizeof(value));
    return value;
}

template<typename Value,typename Bits>
void check(Bits bits)
{
    const auto value=fromBits<Value>(bits);
    require(youknow::finiteValue(value)==std::isfinite(value),
        "finite predicate differs from original standard predicate");
    require(youknow::finite_detail::representation<Bits>(value)==bits,
        "classification changed the supplied object representation");
}

void classifications()
{
    std::uint64_t checks=0;
    // Every exponent, both signs, and mantissas covering zeros, subnormals,
    // normal endpoints, infinities, signaling NaNs and quiet NaNs. The finite
    // decision is exponent-only; payload sweeps below add non-boundary bits.
    for(std::uint32_t exponent=0;exponent<256;++exponent)
        for(std::uint32_t fraction:{0u,1u,0x003fffffu,0x00400000u,0x007fffffu})
            for(std::uint32_t sign:{0u,0x80000000u})
            {check<float>(sign|(exponent<<23)|fraction);++checks;}
    for(std::uint64_t exponent=0;exponent<2048;++exponent)
        for(std::uint64_t fraction:{0ull,1ull,0x0007ffffffffffffull,
                0x0008000000000000ull,0x000fffffffffffffull})
            for(std::uint64_t sign:{0ull,0x8000000000000000ull})
            {check<double>(sign|(exponent<<52)|fraction);++checks;}
    std::uint64_t state=0x82a2b175229d6a5bull;
    for(int i=0;i<1000000;++i)
    {
        state^=state<<13;state^=state>>7;state^=state<<17;
        check<double>(state);check<float>(static_cast<std::uint32_t>(state));checks+=2;
    }
    for(std::uint32_t payload=1;payload<0x00800000;payload+=7919)
    {
        check<float>(0x7f800000u|payload);check<float>(0xff800000u|payload);checks+=2;
    }
    std::cout<<"finite classification checks="<<checks<<'\n';
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
bool classifyFloat(float value) {return youknow::finiteValue(value);}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
bool classifyDouble(double value) {return youknow::finiteValue(value);}

void exceptionFlags()
{
    // Classification must not perform an ordered floating comparison on a
    // NaN. In particular, signaling payloads remain nonfinite without adding
    // FE_INVALID; preexisting status flags must also survive untouched.
    for(std::uint32_t bits:{0u,0x80000000u,1u,0x00800000u,0x7f7fffffu,
            0x7f800000u,0xff800000u,0x7f800001u,0xff800001u,0x7fc00001u,0xffc00001u})
    {
        const float value=fromBits<float>(bits);
        std::feclearexcept(FE_ALL_EXCEPT);
        volatile bool result=classifyFloat(value);(void)result;
        require(std::fetestexcept(FE_ALL_EXCEPT)==0,"float classification raised floating exception");
        std::feraiseexcept(FE_DIVBYZERO);
        result=classifyFloat(value);
        require(std::fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO,"float classification changed status flags");
    }
    for(std::uint64_t bits:{0ull,0x8000000000000000ull,1ull,0x0010000000000000ull,
            0x7fefffffffffffffull,0x7ff0000000000000ull,0xfff0000000000000ull,
            0x7ff0000000000001ull,0xfff0000000000001ull,
            0x7ff8000000000001ull,0xfff8000000000001ull})
    {
        const double value=fromBits<double>(bits);
        std::feclearexcept(FE_ALL_EXCEPT);
        volatile bool result=classifyDouble(value);(void)result;
        require(std::fetestexcept(FE_ALL_EXCEPT)==0,"double classification raised floating exception");
        std::feraiseexcept(FE_DIVBYZERO);
        result=classifyDouble(value);
        require(std::fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO,"double classification changed status flags");
    }
    std::feclearexcept(FE_ALL_EXCEPT);
}

void cascadeFallbacks()
{
    using Access=youknow::YouKnowTestAccess;
    const auto make=[] {
        Access::Cascade c;
        c.state={.1,.05,.02,.01};c.inputHistory={.1,-.1,.2};c.inputHistoryCount=2;
        return c;
    };
    const auto same=[](const Access::Cascade& a,const Access::Cascade& b,float x,float y) {
        require(youknow::finite_detail::representation<std::uint32_t>(x)
            ==youknow::finite_detail::representation<std::uint32_t>(y),"VCF invalid-parameter fallback output changed");
        require(std::memcmp(a.state.data(),b.state.data(),sizeof(a.state))==0,
            "VCF invalid-parameter fallback state changed");
    };
    for(std::uint32_t bits:{0x7f800000u,0xff800000u,0x7f800001u,0xff800001u,0x7fc00001u,0xffc00001u})
        for(int field=0;field<5;++field)
        {
            auto a=make(),b=make();
            std::array<float,5> actual{.3f,.05f,3.5f,6.f,.5f},expected=actual;
            actual[static_cast<std::size_t>(field)]=fromBits<float>(bits);
            expected[static_cast<std::size_t>(field)]=field==3?1e-5f:0.f;
            const auto run=[](Access::Cascade& c,const std::array<float,5>& p) {
                return c.process(p[0],p[1],p[2],p[3],true,p[4],nullptr,
                    youknow::VcfTanhMode::Exact,youknow::VcfSolverMode::MersonHalfSteps);
            };
            same(a,b,run(a,actual),run(b,expected));
        }
    for(std::uint64_t bits:{0x7ff0000000000000ull,0xfff0000000000000ull,
            0x7ff0000000000001ull,0xfff0000000000001ull,
            0x7ff8000000000001ull,0xfff8000000000001ull})
    {
        const double invalid=fromBits<double>(bits);
        require(Access::clamp(invalid)==0,"VCF nonfinite omega fallback changed");
        for(int field=0;field<3;++field)for(std::size_t node=0;node<7;++node)
        {
            auto a=make(),b=make();Access::Trajectory actual,expected;
            actual.omegaStep.fill(.05);actual.feedback.fill(3.5);actual.headroom.fill(6);
            expected=actual;
            auto& bad=field==0?actual.omegaStep:field==1?actual.feedback:actual.headroom;
            auto& replacement=field==0?expected.omegaStep:field==1?expected.feedback:expected.headroom;
            bad[node]=invalid;replacement[node]=field==2?1e-5:0;
            const auto run=[](Access::Cascade& c,const Access::Trajectory& t) {
                return c.process(.3f,.05f,3.5f,6.f,true,.5f,&t,
                    youknow::VcfTanhMode::Exact,youknow::VcfSolverMode::MersonHalfSteps);
            };
            same(a,b,run(a,actual),run(b,expected));
        }
    }
}
}

int main()
{
    try
    {
        classifications();exceptionFlags();cascadeFallbacks();
        std::cout<<"finite predicates and VCF invalid-input fallbacks passed\n";return 0;
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
