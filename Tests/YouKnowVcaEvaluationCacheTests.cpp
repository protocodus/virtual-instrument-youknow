// Exact reuse of an already computed Tr20 current. The reference forces a
// result-cache miss while retaining the original two-knot cache, so both
// paths use precisely the established interpolation and operation order.
#include "DSP/YouKnowEvidenceVca.h"
#include "DSP/YouKnowControlDac.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{
using Circuit=youknow::VcaJunctionTemperatureCircuit;
using Cache=Circuit::CurrentCache;
constexpr double span=static_cast<float>(youknow::ControlDac::positiveSpanVolts(
    youknow::ControlDac::maximumCode));

void require(bool condition,const char* message)
{ if(!condition)throw std::runtime_error(message); }
bool sameBits(double a,double b)
{ return std::memcmp(&a,&b,sizeof(a))==0; }

double forcedMiss(const Circuit& circuit,double charge,double temperature,Cache& cache)
{
    cache.evaluationValid=false;
    return circuit.emitterAmpsAtCharge(charge,temperature,&cache);
}

void gridAndLifecycle()
{
    const auto circuit=std::make_unique<Circuit>(span);
    const auto different=std::make_unique<Circuit>(span*1.02);
    Cache actual,reference;
    const std::array temperatures{-std::numeric_limits<double>::infinity(),-0.0,0.0,24.0,
        25.0,std::nextafter(25.0,26.0),25.37,39.73,40.0,63.7,64.0,65.0,
        std::numeric_limits<double>::infinity()};
    constexpr double width=(Circuit::maximumCharge-Circuit::minimumCharge)/Circuit::tableSteps;
    std::uint64_t calls=0;
    for(double temperature:temperatures)
        for(int cell=0;cell<=Circuit::tableSteps;++cell)
        {
            const double knot=Circuit::minimumCharge+cell*width;
            for(double charge:{std::nextafter(knot,-std::numeric_limits<double>::infinity()),
                knot,std::nextafter(knot,std::numeric_limits<double>::infinity()),
                knot+.125*width,knot+.875*width})
            {
                const auto check=[&](const Circuit& selected) {
                    const auto expected=forcedMiss(selected,charge,temperature,reference);
                    for(int repeat=0;repeat<3;++repeat)
                    {
                        const auto observed=selected.emitterAmpsAtCharge(charge,temperature,&actual);
                        require(sameBits(observed,expected),"exact current result cache changed interpolation bits");
                        ++calls;
                    }
                    reference.evaluationValid=false;
                    require(sameBits(selected.controlAtCharge(charge,temperature,&actual),
                        selected.controlAtCharge(charge,temperature,&reference)),
                        "result cache changed equivalent control bits");
                };
                check(*circuit);
                if(cell%128==0){check(*different);check(*circuit);}
            }
            if(cell%257==0){actual={};reference={};}
            if(cell%131==0){auto copied=actual;actual=copied;}
        }
    std::cout<<"Exact VCA cache comparisons="<<calls<<" cache bytes="<<sizeof(Cache)<<'\n';
}

void hitAndInvalidationSemantics()
{
    const auto circuit=std::make_unique<Circuit>(span);
    constexpr double charge=.02003,temperature=39.73;
    Cache warmed;
    const double expected=circuit->emitterAmpsAtCharge(charge,temperature,&warmed);
    require(warmed.valid&&warmed.evaluationValid&&warmed.evaluatedCharge==charge
        &&sameBits(warmed.evaluatedCurrent,expected),"current evaluation was not recorded");

    // Make reading the knot pair observable. An exact-key hit must reuse
    // the finished current rather than repeat its cell interpolation.
    auto poisonedKnots=warmed;
    poisonedKnots.first=poisonedKnots.difference=std::numeric_limits<double>::quiet_NaN();
    require(sameBits(circuit->emitterAmpsAtCharge(charge,temperature,&poisonedKnots),expected),
        "identical current query did not take the result-cache hit");
    poisonedKnots.valid=false;
    require(sameBits(circuit->emitterAmpsAtCharge(charge,temperature,&poisonedKnots),expected),
        "invalid knot cache reused a current evaluation");

    // A NaN key is never an exact match, even if the result-cache validity
    // bit was copied. The physical query remains valid; unlike table keys,
    // NaN physical charge/temperature are outside this solver's input domain.
    const double nan=std::numeric_limits<double>::quiet_NaN();
    for(int key=0;key<3;++key)
    {
        auto invalid=warmed;
        invalid.evaluatedCurrent=nan;
        if(key==0)invalid.evaluatedCharge=nan;
        if(key==1)invalid.celsius=nan;
        if(key==2)invalid.span=nan;
        require(sameBits(circuit->emitterAmpsAtCharge(charge,temperature,&invalid),expected),
            "NaN current-cache key hit or failed to recover");
    }
    auto disabled=warmed;disabled.evaluationValid=false;disabled.evaluatedCurrent=nan;
    require(sameBits(circuit->emitterAmpsAtCharge(charge,temperature,&disabled),expected),
        "invalid current evaluation was reused");

    for(double zero:{-0.0,0.0})
    {
        Cache actual,reference;
        const double first=forcedMiss(*circuit,zero,temperature,reference);
        require(sameBits(circuit->emitterAmpsAtCharge(zero,temperature,&actual),first),
            "signed-zero current query changed output bits");
        require(sameBits(circuit->emitterAmpsAtCharge(-zero,temperature,&actual),first),
            "signed-zero equivalent current query changed output bits");
    }
    for(double extreme:{-std::numeric_limits<double>::infinity(),-1.0,2.0,
        std::numeric_limits<double>::infinity()})
    {
        Cache actual,reference;
        const double expectedExtreme=forcedMiss(*circuit,extreme,temperature,reference);
        require(sameBits(circuit->emitterAmpsAtCharge(extreme,temperature,&actual),expectedExtreme)
            &&sameBits(circuit->emitterAmpsAtCharge(extreme,temperature,&actual),expectedExtreme),
            "clamped charge query changed established bits");
    }
}

void trajectoryBits()
{
    const auto circuit=std::make_unique<Circuit>(span);
    for(double rate:{8000.,44100.,48000.,192000.,768000.})
        for(double initial:{-.02,0.,.02,.07999,.08,.5,1.02})
        {
            Cache actual,reference;
            double q=initial,r=initial;
            constexpr std::array targets{0.0,.005,.02,1.0,.05,-.2,1.2};
            constexpr std::array temperatures{25.0,25.37,40.0,63.7,64.0,24.0,65.0};
            for(int sample=0;sample<420;++sample)
            {
                const double target=targets[static_cast<std::size_t>(sample/17)%targets.size()];
                const double temperature=temperatures[static_cast<std::size_t>(sample/13)%temperatures.size()];
                const double seconds=(sample%5 ? 1.0 : .03125)/rate;
                reference.evaluationValid=false;
                q=circuit->advance(q,target,seconds,temperature,&actual);
                r=circuit->advance(r,target,seconds,temperature,&reference);
                require(sameBits(q,r),"exact current reuse changed RK4 charge bits");
                reference.evaluationValid=false;
                require(sameBits(circuit->controlAtCharge(q,temperature,&actual),
                    circuit->controlAtCharge(r,temperature,&reference)),
                    "exact current reuse changed RK4 equivalent-control bits");
                if(sample==97){actual={};reference={};}
            }
        }
}

void benchmark()
{
    const auto circuit=std::make_unique<Circuit>(span);
    // A full six-query control/gain/RK pattern: the preceding endpoint is
    // queried at the first RK stage, then four RK stage positions, followed
    // by two identical final-current reads. Disable only the finished-result
    // reuse in the reference; both variants keep the existing knot cache.
    constexpr int frames=400000;
    std::array<std::vector<double>,2> times;
    volatile double witness=0;
    for(int run=0;run<10;++run)for(int offset=0;offset<2;++offset)
    {
        const bool enabled=((run+offset)&1)!=0;
        Cache cache;
        const auto start=std::chrono::steady_clock::now();
        double sum=0,previous=.4;
        for(int frame=0;frame<frames;++frame)
        {
            const double temperature=25.0+double((frame/128)%1500)/100;
            const double current=.4+double(frame%4000)*.000001;
            for(double charge:{previous,previous+.3*(current-previous),previous+.6*(current-previous),
                previous+.9*(current-previous),current,current})
            {
                if(!enabled)cache.evaluationValid=false;
                sum+=circuit->emitterAmpsAtCharge(charge,temperature,&cache);
            }
            previous=current;
        }
        const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        times[enabled].push_back(elapsed);witness=sum;
    }
    for(auto& t:times)std::sort(t.begin(),t.end());
    const double a=.5*(times[0][4]+times[0][5]),b=.5*(times[1][4]+times[1][5]);
    std::cout<<"VCA same-cell six-query median "<<a<<" -> "<<b
        <<" seconds, ratio="<<b/a<<" witness="<<witness<<'\n';
}
}

int main(int argc,char** argv)
{
    try {
        if(argc==2&&std::string_view(argv[1])=="--benchmark"){benchmark();return 0;}
        gridAndLifecycle();hitAndInvalidationSemantics();trajectoryBits();
        std::cout<<"Exact VCA evaluation cache tests passed\n";return 0;
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
