// Independent capacitor-current oracle for Roland jack-board p.15 C15/C16/C13.
// Parts/topology are schematic anchored; 0.6 V diode/transistor thresholds are
// explicit nominal priors, and this is not an original-unit timing measurement.
#include "../Source/DSP/YouKnowChorus.h"
#include "../Source/DSP/YouKnowProductFidelity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <utility>

namespace youknow
{
struct YouKnowTestAccess
{
    static void advance(Chorus& c, bool muted) { c.advanceMuteDrive(muted); }
    static std::array<double, 3> volts(const Chorus& c)
    { return { c.muteDriveNodeVolts_, c.muteDriveHoldVolts_, c.clockMuteVolts_ }; }
    static void setVolts(Chorus& c, const std::array<double, 3>& v)
    { c.muteDriveNodeVolts_=v[0]; c.muteDriveHoldVolts_=v[1]; c.clockMuteVolts_=v[2]; }
    static auto buckets(const Chorus& c) { return c.lineA_.cells; }
    static auto rng(const Chorus& c) { return c.lineA_.noiseState; }
    static auto phase(const Chorus& c) { return c.lineA_.clockPhase; }
    static auto index(const Chorus& c) { return c.lineA_.writeIndex; }
    static std::array<double, 12> support(const Chorus& c)
    {
        const auto& s=c.inputSupport_;
        return {s.exactState[0],s.exactState[1],s.exactState[2],s.exactState[3],
                s.exactState[4],s.exactState[5],s.couplingState,s.passiveState,
                s.antiAliasFirst.s1,s.antiAliasFirst.s2,
                s.antiAliasSecond.s1,s.antiAliasSecond.s2};
    }
    static auto runningMode(const Chorus& c) { return c.runningMode_; }
    static auto rate(const Chorus& c) { return c.rateHz_; }
    static auto builds(const Chorus& c) { return c.supportBuildCount_; }
};
}

namespace
{
using State = std::array<double, 3>;
using youknow::Chorus;
using youknow::ChorusMode;
using Access = youknow::YouKnowTestAccess;
constexpr double muteThreshold = -15.0 + 0.6 * 599000.0 / 39000.0;
constexpr double clockThreshold = -15.0 + 0.6 * 363000.0 / 33000.0;

void require(bool pass, const char* message)
{
    if (!pass) { std::cerr << message << '\n'; std::exit(1); }
}

void process(Chorus& c, float input, ChorusMode mode, float& left, float& right)
{
    c.process(input, mode, 1.0f, left, right, 1.0f, true, true, false,
              youknow::ChorusTimingProfile::Shipping, true);
}

State equilibrium(bool muted)
{
    // Collapse the independently transcribed resistor paths only at DC.
    // D3 is reverse biased at both equilibria, so it carries no current.
    if (muted)
    {
        // Each base is clamped to its -15 V emitter plus the nominal 0.6 V.
        // R42/R131/R145 then draw from that junction; they no longer belong
        // in series with the capacitors' upper load resistors.
        const double base=-14.4, wetPath=150000.0+560000.0;
        const double clockPath=330000.0+330000.0/2.0;
        const double node=(15.0/10000.0+base/wetPath+base/clockPath)
                         /(1.0/10000.0+1.0/wetPath+1.0/clockPath);
        return {node,base+(node-base)*560000.0/wetPath,
                     base+(node-base)*(330000.0/2.0)/clockPath};
    }
    const double sink = 1.0 / 749000.0 + 1.0 / 511500.0
                      + (muted ? 0.0 : 1.0 / 330.0);
    const double aboveRail = 30.0 / (1.0 + 10000.0 * sink);
    return { -15.0 + aboveRail, -15.0 + aboveRail * 599000.0 / 749000.0,
             -15.0 + aboveRail * 181500.0 / 511500.0 };
}

State currents(const State& v, bool muted)
{
    const double i48 = (v[0] - v[1]) / 150000.0;
    const double i47 = (v[2] - v[0]) / 330000.0;
    const double diode = std::max(v[2] - v[0] - 0.6, 0.0) / 10000.0;
    const double sink = muted ? 0.0 : (v[0] + 15.0) / 330.0;
    // Solve the three actual base nodes, including their resistor-to-emitter
    // paths and the constant-Vbe junction inequality. This independent current
    // reference does not select regions or use the prepared matrix equations.
    const double wetBase=std::min(-15.0+(v[1]+15.0)*39000.0/599000.0,-14.4);
    const double clockBase=std::min(-15.0+(v[2]+15.0)*33000.0/363000.0,-14.4);
    const double wetBaseCurrent=(v[1]-wetBase)/560000.0;
    const double firstClockCurrent=(v[2]-clockBase)/330000.0;
    const double secondClockCurrent=(v[2]-clockBase)/330000.0;
    return { ((15.0-v[0])/10000.0 - sink - i48 + i47 + diode) / 2.2e-6,
             (i48 - wetBaseCurrent) / 1.0e-6,
             (-i47 - diode - firstClockCurrent - secondClockCurrent) / 2.2e-6 };
}

State offset(State v, const State& slope, double dt)
{
    for (std::size_t i=0; i<3; ++i) v[i] += dt*slope[i];
    return v;
}

void oracleStep(State& v, bool muted, double seconds, double maxStep)
{
    const int steps = static_cast<int>(std::ceil(seconds / maxStep));
    const double dt = seconds / steps;
    for (int s=0; s<steps; ++s)
    {
        const auto a=currents(v,muted), b=currents(offset(v,a,dt*0.5),muted);
        const auto c=currents(offset(v,b,dt*0.5),muted), d=currents(offset(v,c,dt),muted);
        for (std::size_t i=0; i<3; ++i) v[i] += dt*(a[i]+2*b[i]+2*c[i]+d[i])/6.0;
    }
}

void checkOracle(double rate)
{
    Chorus chorus;
    chorus.prepareSupportRates(48000.0);
    chorus.prepare(rate);
    float l{},r{};
    process(chorus,0.0f,ChorusMode::One,l,r);
    State oracle=equilibrium(false), finer=oracle;
    constexpr std::array<std::pair<bool,double>,8> sequence {{
        {true,2.0},{false,2.0},{true,0.06},{false,0.025},
        {true,0.25},{false,0.035},{true,0.15},{false,0.05}
    }};
    double error=0.0, convergence=0.0;
    int segment=0;
    for (auto [muted,seconds] : sequence)
    {
        int muteFrame=-1,clockFrame=-1,expectedMute=-1,expectedClock=-1;
        const int frames=static_cast<int>(std::llround(seconds*rate));
        for (int frame=0; frame<frames; ++frame)
        {
            Access::advance(chorus,muted);
            oracleStep(oracle,muted,1.0/rate,1.0/500000.0);
            oracleStep(finer,muted,1.0/rate,1.0/1000000.0);
            const auto actual=Access::volts(chorus);
            for (std::size_t i=0; i<3; ++i)
            {
                error=std::max(error,std::abs(actual[i]-finer[i]));
                convergence=std::max(convergence,std::abs(oracle[i]-finer[i]));
                require(actual[i]>=-15.000001 && actual[i]<=15.000001,
                        "clock-mute capacitor escaped supply rails");
            }
            if (chorus.muteDriveMuted()==muted && muteFrame<0) muteFrame=frame;
            if (chorus.clocksStopped()==muted && clockFrame<0) clockFrame=frame;
            if ((finer[1]>=muteThreshold)==muted && expectedMute<0) expectedMute=frame;
            if ((finer[2]>=clockThreshold)==muted && expectedClock<0) expectedClock=frame;
        }
        require((muteFrame<0 && expectedMute<0) || std::abs(muteFrame-expectedMute)<=1,
                "coupled wet mute differed from nodal oracle by >1 sample");
        require((clockFrame<0 && expectedClock<0) || std::abs(clockFrame-expectedClock)<=1,
                "clock clamp differed from nodal oracle by >1 sample");
        if (segment<2)
        {
            const auto actual=Access::volts(chorus), rest=equilibrium(muted);
            for (std::size_t i=0; i<3; ++i)
                require(std::abs(actual[i]-rest[i])<0.006,
                        "three-node circuit did not approach independent DC solution");
            std::cout << rate << " Hz " << (muted?"Off":"On")
                      << ": wet switch " << 1000.0*(muteFrame+1)/rate
                      << " ms, clock switch " << 1000.0*(clockFrame+1)/rate << " ms\n";
            if (muted) require(clockFrame>muteFrame,"clock stopped before wet return muted");
            else require(clockFrame<muteFrame,"clock restarted after wet return opened");
        }
        ++segment;
    }
    require(convergence<1.0e-7,"independent switched RK4 reference did not converge");
    require(error<2.0e-6,"three-node prepared circuit disagrees with nodal RK4");
    const auto before=Access::volts(chorus);
    const auto builds=Access::builds(chorus);
    const double nextRate=rate==48000.0?192000.0:48000.0;
    chorus.prepare(nextRate,true);
    require(Access::volts(chorus)==before,"rate change reset one of the three capacitors");
    require(Access::builds(chorus)==builds,"cached rate change rebuilt clock circuit");
    Access::advance(chorus,true);
    oracleStep(finer,true,1.0/nextRate,1.0/1000000.0);
    for (std::size_t i=0; i<3; ++i)
        require(std::abs(Access::volts(chorus)[i]-finer[i])<2.0e-6,
                "three-node circuit lost charge across numerical grid change");
    std::cout << "max node error " << error << " V; step-halving " << convergence << " V\n";
}

void checkJunctionCorners(double rate)
{
    Chorus chorus;
    chorus.prepare(rate);
    float l{},r{};
    process(chorus,0.0f,ChorusMode::Off,l,r);
    const auto rest=equilibrium(true);
    for(std::size_t i=0;i<3;++i)
        require(std::abs(Access::volts(chorus)[i]-rest[i])<2.0e-8,
                "initial Off did not use both clamped-base DC loads");
    for(const auto& start : {
            State{clockThreshold-0.6002,muteThreshold+0.0001,clockThreshold+0.0001},
            State{clockThreshold-0.6,muteThreshold,clockThreshold},
            State{clockThreshold-0.5998,muteThreshold-0.0001,clockThreshold-0.0001}})
        for(const bool muted : {true,false})
        {
            Access::setVolts(chorus,start);
            State oracle=start, finer=start;
            for(int n=0;n<16;++n)
            {
                Access::advance(chorus,muted);
                oracleStep(oracle,muted,1.0/rate,1.0/2000000.0);
                oracleStep(finer,muted,1.0/rate,1.0/4000000.0);
                for(std::size_t i=0;i<3;++i)
                {
                    require(std::abs(oracle[i]-finer[i])<1.0e-8,
                            "junction-corner RK4 reference did not converge");
                    require(std::abs(Access::volts(chorus)[i]-finer[i])<2.0e-7,
                            "multiple/simultaneous junction crossings lost charge");
                }
            }
        }
    std::cout << rate << " Hz simultaneous/multiple junction crossings agree\n";
}

void checkStoppedMemory()
{
    Chorus chorus;
    chorus.prepare(48000.0);
    float l{},r{};
    for(int n=0;n<4800;++n) process(chorus,0.2f*std::sin(n*0.031f),ChorusMode::One,l,r);
    for(int n=0;n<48000;++n) process(chorus,0.2f*std::sin(n*0.031f),ChorusMode::Off,l,r);
    require(chorus.clocksStopped(),"Off never stopped the BBD clocks");
    const auto buckets=Access::buckets(chorus);
    const auto rng=Access::rng(chorus);
    const auto phase=Access::phase(chorus);
    const auto index=Access::index(chorus);
    const auto support=Access::support(chorus);
    for(int n=0;n<1000;++n)
    {
        require(!chorus.processBypassedWhenSettled(0.25f,l,r),
                "old fast bypass discarded stopped physical state");
        process(chorus,0.25f,ChorusMode::Off,l,r);
    }
    require(Access::buckets(chorus)==buckets && Access::rng(chorus)==rng
            && Access::phase(chorus)==phase && Access::index(chorus)==index,
            "stopped clock shifted buckets, advanced phase, or generated edge noise");
    require(Access::support(chorus)!=support,"stopped clock froze analog input support");
    process(chorus,0.0f,ChorusMode::One,l,r);
    require(Access::buckets(chorus)==buckets && Access::rng(chorus)==rng,
            "On command cleared stored BBD charge before its clocks restarted");
    int restart=-1,open=-1;
    for(int n=0;n<10000;++n)
    {
        process(chorus,0.0f,ChorusMode::One,l,r);
        if(!chorus.clocksStopped() && restart<0) restart=n;
        if(!chorus.muteDriveMuted() && open<0) open=n;
        require(std::isfinite(l)&&std::isfinite(r),"clock restart produced nonfinite output");
    }
    require(restart>=0 && open>restart,"wet return opened before clock restart");
    require(Access::buckets(chorus)!=buckets && Access::rng(chorus)!=rng,
            "BBD clock failed to resume sampling after On");
    std::cout << "Stopped buckets/RNG/phase retained; support and resumed clocks advance\n";
}

// The two IC40 outputs shown on Roland service p.13 control mute and mode
// separately. Changing I/II while muted must preserve stored charge and mute
// capacitor trajectories, while the continuously running LFO changes speed.
void checkHardwareModeSelection()
{
    for (const double sampleRate : {44100.0, 48000.0, 192000.0})
    {
        Chorus first, second;
        first.prepare(sampleRate);
        second.prepare(sampleRate);
        float al{}, ar{}, bl{}, br{};
        process(first, 0.15f, ChorusMode::Off, al, ar);
        process(second, 0.15f, ChorusMode::Off, bl, br);
        require(first.clocksStopped() && second.clocksStopped(),
                "fixture did not start with stopped BBD clocks");
        const auto buckets = Access::buckets(second);
        const auto rng = Access::rng(second);
        const auto bucketPhase = Access::phase(second);
        const auto lfoPhase = second.getLfoPhase();
        const auto muteVolts = Access::volts(second);
        second.setHardwareModeSelection(ChorusMode::Two);
        require(Access::buckets(second) == buckets && Access::rng(second) == rng
                && Access::phase(second) == bucketPhase
                && second.getLfoPhase() == lfoPhase && Access::volts(second) == muteVolts,
                "mode-select pin reset oscillator, BBD charge or mute capacitors");
        const double expectedPhase = lfoPhase
            + static_cast<double>(Chorus::settingsFor(ChorusMode::Two).rateHz) / sampleRate;
        process(first, 0.15f, ChorusMode::Off, al, ar);
        process(second, 0.15f, ChorusMode::Off, bl, br);
        require(second.getLfoPhase() == expectedPhase,
                "muted mode II did not advance the LFO at its selected rate");
        require(Access::runningMode(second) == ChorusMode::Two
                && second.muteDriveMuted() && second.clocksStopped(),
                "mode-select pin incorrectly opened the wet return or BBD clocks");
        require(Access::volts(first) == Access::volts(second) && al == bl && ar == br,
                "mode-select pin altered the independent mute circuit or dry audio");
        require(Access::buckets(second) == buckets && Access::rng(second) == rng
                && Access::phase(second) == bucketPhase,
                "muted selector change discarded stopped BBD history");
        // The command argument now only supplies mute; the physical selector
        // remains authoritative when the wet return is enabled again.
        process(second, 0.15f, ChorusMode::One, bl, br);
        require(Access::runningMode(second) == ChorusMode::Two,
                "unmute overwrote the separate hardware mode-select pin");
        second.setHardwareModeSelection(ChorusMode::Off);
        process(second, 0.15f, ChorusMode::One, bl, br);
        require(Access::runningMode(second) == ChorusMode::One,
                "disabling hardware selection did not restore host mode control");
        second.setHardwareModeSelection(ChorusMode::Two);
        second.reset();
        process(second, 0.15f, ChorusMode::One, bl, br);
        require(Access::runningMode(second) == ChorusMode::One,
                "reset leaked hardware selection into an ordinary host session");
    }
    std::cout << "Independent IC40 mode/mute signals preserve phase and stopped charge\n";
}

void checkSteadyIsolation()
{
    for (const auto mode : {ChorusMode::One,ChorusMode::Two,ChorusMode::OneTwo})
    {
        Chorus legacy,candidate;
        legacy.prepare(48000.0);
        candidate.prepare(48000.0);
        for (int n=0;n<4800;++n)
        {
            const float input=0.2f*std::sin(n*0.031f);
            float la{},ra{},lb{},rb{};
            legacy.process(input,mode,1.0f,la,ra,1.0f,true,true,false,
                           youknow::ChorusTimingProfile::Shipping);
            process(candidate,input,mode,lb,rb);
            require(la==lb && ra==rb,"clock-mute candidate changed steady engaged tone/noise");
        }
    }
    std::cout << "Steady I/II/I+II audio and edge noise remain bit-identical\n";
}
}

int main()
{
    youknow::EngineParameters parameters;
    require(!parameters.enableChorusClockMuteCircuit,
            "raw reference unexpectedly enables the clock-mute circuit");
    youknow::ProductFidelityProfile::applyTo(parameters);
    require(parameters.enableChorusMuteDrive && parameters.enableChorusClockMuteCircuit,
            "product omitted the complete chorus switching circuit");
    std::cout << std::setprecision(10);
    for(double rate : {8000.0,44100.0,48000.0,192000.0,768000.0})
    {
        checkOracle(rate);
        checkJunctionCorners(rate);
    }
    checkStoppedMemory();
    checkSteadyIsolation();
    checkHardwareModeSelection();
}
