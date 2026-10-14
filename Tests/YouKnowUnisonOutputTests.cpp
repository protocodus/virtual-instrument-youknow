// The requested 0.8 digital output trim is a product policy, not a circuit
// calibration. A single assigned voice gives an independent audio oracle:
// Poly1, Poly2 and Unison render the same circuit state before the final trim.
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowActiveProductFidelity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace youknow {
struct YouKnowTestAccess {
    static float gain(const YouKnowEngine& e) { return e.unisonOutputGain_; }
    static bool primed(const YouKnowEngine& e) { return e.panelGlidePrimed_; }
    static float filter(const YouKnowEngine& e) { return e.voices_[0].filter.state[3]; }
    static double phase(const YouKnowEngine& e) { return e.voices_[0].dco.pitClocksToEvent; }
    static KeyMode selected(const YouKnowEngine& e) { return e.activeParameters_.keyMode; }
    static KeyMode mirror(const YouKnowEngine& e) { return e.firmwareSerialCircuitParameters_.keyMode; }
    static void retainMirrorMode(YouKnowEngine& e, KeyMode mode) { e.firmwareSerialCircuitParameters_.keyMode=mode; }
};
}

namespace {
using namespace youknow;
using Access = YouKnowTestAccess;
unsigned assertions = 0;
void require(bool ok, const char* message) {
    ++assertions;
    if (!ok) { std::fprintf(stderr, "Unison output: %s\n", message); std::exit(1); }
}
EngineParameters patch(KeyMode mode) {
    EngineParameters p;
    ActiveProductFidelityProfile::applyTo(p);
    p.keyMode=mode; p.polyphony=1; p.portamento=0; p.aging=0;
    p.chorus=ChorusMode::One; p.vcaMode=VcaMode::Gate;
    p.volume=.7f; p.cutoff=.8f; p.resonance=.3f;
    p.vcfSolverMode=VcfSolverMode::Rk4Single;
    return p;
}
auto create(const EngineParameters& p, double rate=48000, int factor=1,
            bool original=false) {
    auto e=std::make_unique<YouKnowEngine>();
    require(ActiveProductFidelityProfile::tryConfigureBeforePrepare(*e), "product configuration");
    require(e->configureSingleVoiceLastNotePriority(true), "mono policy");
    e->setParameters(p);
    e->setOriginalPerformanceMode(original);
    e->prepare(rate,256,factor);
    return e;
}
void advance(YouKnowEngine& e, int count, int block=64) {
    std::array<float,256> left{},right{};
    while(count>0) {
        const int n=std::min({count,block,256});
        e.process(left.data(),right.data(),n);
        for(int i=0;i<n;++i) require(std::isfinite(left[i]) && std::isfinite(right[i]), "finite audio");
        count-=n;
    }
}
void invalidCalls(YouKnowEngine& e) {
    const float gain=Access::gain(e);
    const bool primed=Access::primed(e);
    float left=12, right=13;
    e.process(&left,&right,0); e.process(&left,&right,-1);
    e.process(nullptr,&right,32); e.process(&left,nullptr,32);
    require(Access::gain(e)==gain && Access::primed(e)==primed,
            "invalid/empty calls advanced or primed gain");
    require(left==12 && right==13,"invalid calls wrote output");
}
void startupAndReset() {
    auto unprepared=std::make_unique<YouKnowEngine>();
    float left=1,right=1;
    unprepared->process(&left,&right,1);
    require(left==0 && right==0 && !Access::primed(*unprepared), "unprepared output/priming");
    for(auto mode:{KeyMode::Poly1,KeyMode::Poly2,KeyMode::Unison}) {
        auto p=patch(mode);
        auto e=create(p);
        invalidCalls(*e);
        advance(*e,1);
        require(Access::gain(*e)==(mode==KeyMode::Unison?.8f:1.f), "startup gain did not prime selected mode");
        for(auto next:{KeyMode::Unison,KeyMode::Poly1}) {
            e->reset(); p.keyMode=next; e->setParameters(p);
            invalidCalls(*e); advance(*e,1);
            require(Access::gain(*e)==(next==KeyMode::Unison?.8f:1.f), "reset failed to re-prime selected mode");
        }
        e->prepare(44100,64,2); p.keyMode=KeyMode::Unison; e->setParameters(p);
        advance(*e,1);
        require(Access::gain(*e)==.8f,"reprepare failed to prime gain");
    }
}
void slewTiming() {
    for(double rate:{8000.,44100.,48000.,96000.,192000.}) for(int factor:{1,2,4}) {
        auto p=patch(KeyMode::Poly2); p.calibration=0; p.chorus=ChorusMode::Off;
        auto e=create(p,rate,factor); advance(*e,1);
        const int duration=static_cast<int>(std::ceil(rate*.005));
        const double step=.2/duration;
        for(auto next:{KeyMode::Unison,KeyMode::Poly2}) {
            p.keyMode=next; e->setParameters(p); invalidCalls(*e);
            float previous=Access::gain(*e);
            for(int i=1;i<=duration;++i) {
                advance(*e,1);
                const float actual=Access::gain(*e);
                const double expected=next==KeyMode::Unison
                    ? std::max(.8,1.-i*step) : std::min(1.,.8+i*step);
                require(actual>=.8f && actual<=1.f,"slew exceeded endpoints");
                require(std::abs(actual-expected)<3e-5,"slew is not host-rate five milliseconds");
                // Endpoint snapping may consume accumulated float rounding;
                // the same absolute tolerance bounds the trajectory above.
                require(std::abs(actual-previous)<=step+3e-5,"slew contains an output-gain jump");
                previous=actual;
            }
            if(Access::gain(*e)!=(next==KeyMode::Unison?.8f:1.f))
                std::fprintf(stderr,"rate %.0f factor %d frames %d gain %.9g\n",rate,factor,duration,Access::gain(*e));
            require(Access::gain(*e)==(next==KeyMode::Unison?.8f:1.f),"slew failed to land at the five-millisecond endpoint");
        }
        p.keyMode=KeyMode::Unison; e->setParameters(p);
        advance(*e,static_cast<int>(rate*.002));
        float previous=Access::gain(*e);
        require(previous>.8f && previous<1.f,"reversal fixture did not stop mid-slew");
        p.keyMode=KeyMode::Poly1; e->setParameters(p);
        require(Access::gain(*e)==previous,"mode reversal jumped before rendering");
        for(int i=0;i<duration;++i) {
            advance(*e,1); const float actual=Access::gain(*e);
            require(actual>=previous && actual<=1.f && actual-previous<=step+3e-5,
                    "reversal overshot or jumped");
            previous=actual;
        }
        require(previous==1.f,"reversal did not settle at unity");
        p.keyMode=KeyMode::Unison; e->setParameters(p); advance(*e,duration);
        p.keyMode=KeyMode::Poly1; e->setParameters(p); advance(*e,duration/2);
        p.keyMode=KeyMode::Poly2; e->setParameters(p);
        // This API reports completion, not acceptance; the safety fade may
        // still be pending while the independent mode-gain ramp finishes.
        (void)e->setOversamplingFactor(factor==1?2:1);
        advance(*e,duration-duration/2);
        require(Access::gain(*e)==1.f,"Poly-to-Poly or quality change restarted the same-target slew");
    }
}
void naturalAudioOracle() {
    for(auto route:{HeadphoneOutput::Route::Line,HeadphoneOutput::Route::Headphones})
    for(double rate:{44100.,48000.,96000.}) for(int factor:{1,2,4}) {
        auto p=patch(KeyMode::Poly1); p.outputRoute=route;
        auto poly1=create(p,rate,factor);
        p.keyMode=KeyMode::Poly2; auto poly2=create(p,rate,factor);
        p.keyMode=KeyMode::Unison; auto unison=create(p,rate,factor);
        for(auto* e:{poly1.get(),poly2.get(),unison.get()}) e->noteOn(60,1);
        std::array<float,64> a{},b{},c{},d{},u{},v{};
        double energy=0,stereoDifference=0;
        for(int block=0;block<64;++block) {
            poly1->process(a.data(),b.data(),64);
            poly2->process(c.data(),d.data(),64);
            unison->process(u.data(),v.data(),64);
            require(Access::filter(*poly1)==Access::filter(*unison),"trim changed internal filter drive");
            require(Access::phase(*poly1)==Access::phase(*unison),"single-voice oracle lost identical oscillator phase");
            for(int i=0;i<64;++i) {
                require(a[i]==c[i] && b[i]==d[i],"Poly modes no longer have unity-relative output");
                require(u[i]==a[i]*.8f && v[i]==b[i]*.8f,"Unison is not exactly0.8 on both output channels");
                energy+=double(a[i])*a[i]+double(b[i])*b[i];
                stereoDifference+=std::abs(double(a[i])-b[i]);
            }
        }
        require(energy>1e-6,"audio oracle rendered silence");
        require(stereoDifference>1e-6,"audio oracle did not exercise distinct stereo channels");
    }
}
std::vector<float> partitioned(int block) {
    auto p=patch(KeyMode::Poly2); auto e=create(p); e->noteOn(60,1);
    std::vector<float> result;
    std::array<float,256> left{},right{};
    for(auto mode:{KeyMode::Poly2,KeyMode::Unison,KeyMode::Poly1}) {
        p.keyMode=mode;e->setParameters(p);
        for(int done=0;done<511;) {
            const int n=std::min(block,511-done);
            e->process(left.data(),right.data(),n);
            for(int i=0;i<n;++i) {result.push_back(left[i]);result.push_back(right[i]);}
            done+=n;
        }
        result.push_back(Access::gain(*e));
    }
    return result;
}
void originalSelectedMode() {
    auto p=patch(KeyMode::Poly1);p.polyphony=6;
    auto e=create(p,48000,1,true);advance(*e,1);
    p.keyMode=KeyMode::Unison;e->setParameters(p);
    require(Access::selected(*e)==KeyMode::Unison,"original adapter lost selected mode");
    // Supply a retained circuit mirror deliberately; current parameter
    // translation may update it immediately. The product trim must follow
    // the selected host mode even when that mirror holds an older image.
    Access::retainMirrorMode(*e,KeyMode::Poly1);
    advance(*e,1);
    require(Access::mirror(*e)==KeyMode::Poly1,"original fixture did not exercise the retained mirror");
    require(Access::gain(*e)<1.f,"original gain waited for firmware mode instead of selected mode");
    advance(*e,239);
    require(Access::gain(*e)==.8f,"original selected Unison did not settle");
    require(e->originalPerformanceHealthy(),"original performance path became unhealthy");
    p.keyMode=KeyMode::Poly2;e->setParameters(p);
    Access::retainMirrorMode(*e,KeyMode::Unison);advance(*e,240);
    require(Access::gain(*e)==1.f,"original selected Poly did not return to unity");
}
}
int main() {
    startupAndReset();
    slewTiming();
    naturalAudioOracle();
    const auto reference=partitioned(1);
    for(int block:{17,64,256}) require(partitioned(block)==reference,"gain/audio depends on host block partition");
    originalSelectedMode();
    std::printf("PASS %u Unison output assertions: stereo/routes, startup/reset, five-ms slew, reversal, partitions, original mode\n",assertions);
}
