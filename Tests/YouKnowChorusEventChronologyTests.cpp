// Independent continuous-clock oracle: Gauss quadrature of 128/delay(t), then
// bisection for each physical half-cycle. It deliberately uses neither the
// production logarithmic integral nor its exponential inverse. Verify the
// actual capture values and output-event ages, not only a helper identity.
#include "DSP/YouKnowChorus.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace youknow {
struct YouKnowTestAccess {
    using Clock = Chorus::ClockTrajectory;
    using Line = Chorus::Line;
    using Capture = Chorus::InputCaptureInterval;
    static float transfer(float value) { return Chorus::bbdTransfer(value); }
    static double residual(double value) { return Chorus::bbdPolyBlepResidual(value); }
};
}
namespace {
using namespace youknow;
using Access = YouKnowTestAccess;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Oracle {
    long double phase, rate, centre, sweep, sampleRate, side;
    long double value(long double sample) const {
        auto p = phase + rate * sample / sampleRate;
        p -= std::floor(p);
        const auto triangle = 4 * std::min(p, 1-p) - 1;
        return 128 / (sampleRate * (centre + side * sweep * triangle));
    }
    long double integratePiece(long double a, long double b) const {
        // Eight-point Gauss-Legendre rule on a smooth flank.
        constexpr std::array<long double,4> nodes {
            .18343464249564980494L, .52553240991632898582L,
            .79666647741362673959L, .96028985649753623168L};
        constexpr std::array<long double,4> weights {
            .36268378337836198297L, .31370664587788728734L,
            .22238103445337447054L, .10122853629037625915L};
        long double sum=0;
        for (std::size_t i=0;i<nodes.size();++i)
            sum += weights[i] * (value((a+b)/2 + (b-a)*nodes[i]/2)
                                +value((a+b)/2 - (b-a)*nodes[i]/2));
        return (b-a)*sum/2;
    }
    long double count(long double end) const {
        const auto cusp = ((phase < .5L ? .5L : 1.L)-phase)*sampleRate/rate;
        return cusp < end ? integratePiece(0,cusp)+integratePiece(cusp,end)
                          : integratePiece(0,end);
    }
    long double time(long double count) const {
        long double a=0,b=3;
        for(int i=0;i<64;++i) {
            const auto mid=(a+b)/2;
            if(this->count(mid)<count) a=mid; else b=mid;
        }
        return (a+b)/2;
    }
};
void chronology() {
    for(double rate:{0.,1.}) {
        const auto clock=Access::Clock::make(.49999,rate,.003,0.,48000.,1.);
        for(double sample:{.125,1.,3.}) {
            require(std::isfinite(clock.cyclesAt(sample))
                && std::abs(clock.sampleAt(clock.cyclesAt(sample))-sample)<2e-14,
                "zero-depth or stopped-LFO clock is not finite/constant");
        }
    }
    double worstCount=0,worstAge=0,worstLegacyAge=0,worstPrediction=0;
    int cases=0,edges=0;
    for(double sampleRate:{8000.,44100.,48000.,192000.})
    for(auto profile:{ChorusTimingProfile::Shipping,ChorusTimingProfile::A11Spectral,
        ChorusTimingProfile::A11ClickTiming,ChorusTimingProfile::DerivedNominal,
        ChorusTimingProfile::OwnerBlend,ChorusTimingProfile::HardwareEvidence})
    for(auto mode:{ChorusMode::One,ChorusMode::Two,ChorusMode::OneTwo}) {
        const auto setting=Chorus::settingsFor(mode,profile);
        // These fixed programs never reach the MN3009 clock clamps. Should a
        // future program do so, the piecewise clock needs extra clamp segments.
        require(128./(setting.centreDelaySeconds+setting.sweepSeconds)>=Chorus::minimumClockHz
            && 128./(setting.centreDelaySeconds-setting.sweepSeconds)<=Chorus::maximumClockHz,
            "fixed clock program exceeds the physical clock interval");
        for(double boundary:{.5,1.})
        for(double cusp:{.125,.75,1.25,2.75,10.})
        for(double side:{-1.,1.}) {
            const double phase=boundary-cusp*setting.rateHz/sampleRate;
            const Oracle oracle{phase,setting.rateHz,setting.centreDelaySeconds,
                setting.sweepSeconds,sampleRate,side};
            const auto clock=Access::Clock::make(phase,setting.rateHz,
                setting.centreDelaySeconds,setting.sweepSeconds,sampleRate,side);
            for(double t:{.125,.5,1.,1.5,2.,3.}) {
                worstCount=std::max(worstCount,std::abs(clock.cyclesAt(t)-double(oracle.count(t))));
                const double recovered=clock.sampleAt(double(oracle.count(t)));
                require(std::abs(recovered-t)<2e-10,"moving clock inverse disagrees with quadrature");
            }
            for(double initialPhase:{.11,.49,.88}) {
                Access::Line line;
                line.reset(12345);
                line.clockPhase=initialPhase;
                for(int i=0;i<128;++i)line.cells[std::size_t(i)]=.001f*float(i+1);
                Access::Capture capture;
                capture.enabled=true;
                capture.initialOutput=.1;
                capture.finalOutput=.3;
                capture.outputByPower[0]=.1;
                capture.outputByPower[1]=.2;
                const auto result=line.processClockedCore(.3f,0,float(sampleRate),0,
                    &capture,true,&clock);
                require(std::isfinite(result),"moving clock core is nonfinite");
                const double increment=double(oracle.count(1));
                const double end=initialPhase+increment;
                int input=0,output=0;
                for(int event=initialPhase<.5?1:2;event*.5<=end;++event) {
                    const double target=event*.5-initialPhase;
                    const double when=double(oracle.time(target));
                    const double age=1-when;
                    worstLegacyAge=std::max(worstLegacyAge,std::abs(age-(1-target/increment)));
                    if(event&1) {
                        require(output<line.outputEventCount,"moving clock lost an output edge");
                        const double error=std::abs(line.outputEvents[std::size_t(output++)].ageInSamples-age);
                        worstAge=std::max(worstAge,error);
                        require(error<2e-10,"output edge retained interval-average clock timing");
                    } else {
                        ++input;
                        const float expected=Access::transfer(float(.1+.2*when));
                        require(std::abs(line.cells[std::size_t(input)]-expected)<3e-8f,
                            "input capture did not use continuous clock time");
                    }
                    ++edges;
                }
                require(output==line.outputEventCount,"moving clock created an extra output edge");
                require(input==line.writeIndex,"moving clock capture ledger disagrees");
                require(std::abs(line.clockPhase-(end-std::floor(end)))<2e-12,
                    "moving clock phase disagrees with integrated physical frequency");
                // Check the changed low-grid path too: predict future held
                // values from already-filled buckets, but obtain each event
                // time independently by quadrature/bisection across the cusp.
                double expectedCorrection=0;
                for(int i=0;i<line.pastBlepEventCount;++i) {
                    const auto& event=line.pastBlepEvents[std::size_t(i)];
                    expectedCorrection+=event.jump*Access::residual(event.ageInSamples);
                }
                float transfer=line.transferState,held=line.held;
                int slot=(line.writeIndex+(line.clockPhase<.5?1:2))%128;
                for(double edge=std::floor(end-.5)+1.5;;edge+=1,slot=(slot+1)%128) {
                    const double when=double(oracle.time(edge-initialPhase));
                    // Bisection is bounded at three samples. Count rejects the
                    // edge past that horizon rather than confusing it with 3.
                    if(edge-initialPhase>=double(oracle.count(3)))break;
                    transfer+=.8654743f*(line.cells[std::size_t(slot)]-transfer);
                    const float next=transfer*line.signalInsertionGain;
                    expectedCorrection-=double(next-held)*Access::residual(when-1);
                    held=next;
                }
                const auto unchanged=line;
                const double predicted=line.deterministicBlepCorrection(clock.intervalCycles,0,&clock);
                const double error=std::abs(predicted-expectedCorrection);
                worstPrediction=std::max(worstPrediction,error);
                require(error<2e-11,"BLEP predictor retained interval-average future timing");
                require(line.noiseState==unchanged.noiseState
                    && line.transferNoiseState==unchanged.transferNoiseState
                    && line.previousTransferNoise==unchanged.previousTransferNoise
                    && line.clockPhase==unchanged.clockPhase && line.cells==unchanged.cells
                    && line.transferState==unchanged.transferState && line.held==unchanged.held,
                    "BLEP prediction changed physical state or noise histories");
                ++cases;
            }
        }
    }
    require(worstCount<2e-11,"clock count disagrees with quadrature");
    require(worstLegacyAge>1e-5,"oracle did not expose the old event-age error");
    std::cout<<cases<<" cases / "<<edges<<" physical edges; worst integral error "
             <<worstCount<<" cycles, BLEP error "<<worstPrediction<<", event error "<<worstAge
             <<" samples; old average-rate event error "<<worstLegacyAge<<" samples\n";
}
}
int main() {
    try {chronology();return 0;}
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
