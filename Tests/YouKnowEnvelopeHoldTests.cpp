#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowVcaControl.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace youknow
{
struct YouKnowTestAccess
{
    static bool configured(const YouKnowEngine& e) { return e.envelopeHoldsConfigured_; }
    static double resistance(const YouKnowEngine& e)
    { return e.envelopeHoldConfiguration_[0].onResistanceOhms; }
    static double held(const YouKnowEngine& e, int slot = 0)
    { return e.envelopeHolds_[static_cast<std::size_t>(slot)].volts(); }
    static bool selected(const YouKnowEngine& e, int slot = 0)
    { return e.envelopeHolds_[static_cast<std::size_t>(slot)].selected(); }
    static double control(const YouKnowEngine& e, int slot = 0)
    { return e.voices_[static_cast<std::size_t>(slot)].vcaControl; }
    static double target(const YouKnowEngine& e, int slot = 0)
    { return e.voices_[static_cast<std::size_t>(slot)].vcaControlTarget; }
    static double span() { return YouKnowEngine::VoiceVcaControlLaw::controlFullScaleVolts; }
    static double offset() { return YouKnowEngine::VoiceVcaSignalLaw::holdStandoffVolts; }
    static double capacitor(double u)
    { return offset() + span() * YouKnowEngine::voiceVcaControlCircuit().capacitorCoordinate(u); }
    static void seed(YouKnowEngine& e, double value, float target)
    {
        e.converterPassEnvelopeUpdated_.fill(true);
        for (std::size_t slot = 0; slot < e.envelopeHolds_.size(); ++slot)
        {
            e.envelopeHolds_[slot].reset(offset() + span() * value);
            e.voices_[slot].vcaControl = value;
            e.voices_[slot].vcaControlTarget = static_cast<float>(value);
            e.voices_[slot].envelope.value = target;
        }
    }
    static void select(YouKnowEngine& e, int slot, float target)
    { e.beginEnvelopeHoldAcquisition(slot, target); }
    static void inhibit(YouKnowEngine& e) { e.inhibitEnvelopeHold(); }
    static void advance(YouKnowEngine& e, double seconds)
    { e.advanceEnvelopeHolds(seconds, e.activeParameters_); }
    static void commitLatched(YouKnowEngine& e, int slot, float target)
    {
        e.performConverterWrite({YouKnowEngine::ConverterDestination::VoiceVca, slot},
                                e.activeParameters_, &target);
    }
    static double atEnable(YouKnowEngine& e, int slot, double position)
    {
        const auto& writes = YouKnowEngine::converterWriteOrder();
        const auto it = std::find_if(writes.begin(), writes.end(), [slot](const auto& w) {
            return w.destination == YouKnowEngine::ConverterDestination::VoiceVca && w.voice == slot;
        });
        const auto ordinal = static_cast<std::size_t>(it - writes.begin());
        const double delta = YouKnowEngine::controlScanHz / e.oversampledRate_;
        e.controlScanPhase_ = e.converterEventPhases_[ordinal] - position * delta;
        e.nextConverterWrite_ = ordinal;
        e.passiveHoldEventLatch_ = {};
        return (e.converterEventPhases_[ordinal] - e.controlScanPhase_) / delta;
    }
    static double atInhibit(YouKnowEngine& e, double position)
    {
        // VCF2 follows VCA1. Its DAC inhibit must close VCA1 before its own
        // later enable, even though VCF does not use this comparison bank.
        constexpr std::size_t ordinal = 12;
        const double delta = YouKnowEngine::controlScanHz / e.oversampledRate_;
        e.controlScanPhase_ = e.envelopeMuxInhibitPhase(ordinal) - position * delta;
        e.nextConverterWrite_ = ordinal;
        e.passiveHoldEventLatch_ = {};
        return (e.envelopeMuxInhibitPhase(ordinal) - e.controlScanPhase_) / delta;
    }
    static void advanceFirmwareToCursor(YouKnowEngine& e)
    { e.advanceFirmwareControlEvents(e.controlScanPhase_); }
    static float firmwareVcaTarget(const YouKnowEngine& e, int slot)
    {
        const auto ordinal = static_cast<std::size_t>(11 + 2 * slot);
        return static_cast<float>(e.firmwareConverterCodes_[ordinal]) / 4095.0f;
    }
    static double atFirmwareWrap(YouKnowEngine& e, bool oldHold, double statesBeforeEnd)
    {
        e.setSustainPedal(oldHold);
        e.refreshFirmwareControlTrace();
        const double firstInhibitStates = e.converterInhibitPhases_[0]
            * YouKnowEngine::voiceCpuStateHz / YouKnowEngine::controlScanHz;
        e.controlScanPhase_ = e.converterPassEndPhase_
            - statesBeforeEnd * YouKnowEngine::controlScanHz / YouKnowEngine::voiceCpuStateHz;
        e.advanceFirmwareControlEvents(e.controlScanPhase_);
        e.nextConverterWrite_ = YouKnowEngine::converterWritesPerPass;
        e.passiveHoldEventLatch_ = {};
        return firstInhibitStates;
    }
};
} // namespace youknow

namespace
{
using Engine = youknow::YouKnowEngine;
using Hold = youknow::EnvelopeHoldCircuit;
using Access = youknow::YouKnowTestAccess;
using Configuration = std::array<Hold::Configuration, 6>;

void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
void near(double a, double b, double tolerance, const char* message)
{
    if (std::abs(a - b) > tolerance)
    {
        std::cerr << message << ": " << a << " vs " << b << " error=" << a-b << '\n';
        throw std::runtime_error(message);
    }
}
Configuration configuration(double resistance)
{
    Configuration result {};
    for (auto& channel : result) channel.onResistanceOhms = resistance;
    return result;
}
youknow::EngineParameters parameters(bool coupled = true)
{
    youknow::EngineParameters p;
    p.calibration = p.aging = p.chorusNoise = p.noiseLevel = p.envDepth = 0;
    p.vcfTanhMode = youknow::VcfTanhMode::PolyZoned;
    p.enableCoupledVoiceVcaControl = coupled;
    p.attack = p.decay = 0;
    p.sustain = 1;
    p.release = .2f;
    p.cutoff = .85f;
    return p;
}
void process(Engine& e, int count = 1)
{
    std::vector<float> l(static_cast<std::size_t>(count)), r(l.size());
    e.process(l.data(), r.data(), count);
    for (float value : l) require(std::isfinite(value), "non-finite rendered audio");
}
std::unique_ptr<Engine> make(double resistance, double rate = 48000, bool coupled = true)
{
    auto engine = std::make_unique<Engine>();
    if (resistance > 0)
        require(engine->configureEnvelopeHolds(configuration(resistance)), "configuration rejected");
    engine->setParameters(parameters(coupled));
    engine->prepare(rate, 256, 1);
    return engine;
}

void configurationTest()
{
    auto engine = std::make_unique<Engine>();
    require(!Access::configured(*engine), "finite acquisition silently became default");
    for (double invalid : {0., -1., 1e10, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()})
        require(!engine->configureEnvelopeHolds(configuration(invalid)), "invalid R accepted");
    auto c = configuration(280);
    c[3].inputBiasAmps = std::numeric_limits<double>::quiet_NaN();
    require(!engine->configureEnvelopeHolds(c), "invalid parasitic accepted");
    require(!Access::configured(*engine), "rejection partially configured circuit");
    require(engine->configureEnvelopeHolds(configuration(280)), "valid config rejected");
    c = configuration(80); c[5].offLeakageAmps = 1;
    require(!engine->configureEnvelopeHolds(c), "partially valid replacement accepted");
    near(Access::resistance(*engine), 280, 0, "rejected replacement changed existing configuration");
    engine->prepare(48000, 1, 1);
    require(!engine->configureEnvelopeHolds(configuration(80)), "live configuration accepted");
    engine->reset();
    require(Access::configured(*engine), "reset lost configuration");
    near(Access::held(*engine), Access::offset(), 0, "reset retained old hold charge");
    engine->prepare(96000, 1, 1);
    require(Access::configured(*engine), "reprepare lost configuration");
}

void signedDomainTest()
{
    for (double sign : {-1., 1.})
    {
        auto c = configuration(280);
        for (auto& channel : c)
        {
            channel.inputBiasAmps = -sign * 30e-12;
            channel.offLeakageAmps = -sign * 10e-12;
            channel.turnOffChargeCoulombs = sign * 1e-9;
        }
        auto engine = std::make_unique<Engine>();
        require(engine->configureEnvelopeHolds(c), "signed configuration rejected");
        engine->setParameters(parameters()); engine->prepare(48000, 1, 1);
        const double endpoint = sign < 0 ? 0 : 1;
        Access::seed(*engine, endpoint, static_cast<float>(endpoint));
        Access::select(*engine, 0, static_cast<float>(endpoint));
        Access::inhibit(*engine);
        Access::advance(*engine, 1e-5);
        const double expected = Access::offset() + Access::span() * endpoint
            + sign * (.1 + 40e-12 * 1e-5 / 1e-8);
        near(Access::held(*engine), expected, 2e-14, "signed physical overrange was discarded");
        near(Access::control(*engine), endpoint, 0, "C58 escaped its declared calibrated domain");
    }
}

void circuitTest()
{
    for (double resistance : {80., 280., 1e5})
        for (double multiples : {.1, 1., 9.})
        {
            Hold hold;
            const auto c = configuration(resistance)[0];
            hold.reset(.4); hold.select(9.6);
            near(hold.volts(), .4, 0, "enable changed capacitor charge instantaneously");
            const double seconds = resistance * 1e-8 * multiples;
            long double reference = .4L;
            const long double dt = seconds / 4000;
            const auto f = [resistance](long double v) { return (9.6L-v)/(resistance*1e-8L); };
            for (int i=0;i<4000;++i)
            {
                const auto a=f(reference), b=f(reference+dt*a/2), d=f(reference+dt*b/2);
                const auto e=f(reference+dt*d);
                reference += dt*(a+2*b+2*d+e)/6;
            }
            hold.advance(hold.trajectory(c), seconds);
            near(hold.volts(), static_cast<double>(reference), 2e-11, "hold differs from independent KCL RK4");
            hold.inhibit(c);
            const double retained = hold.volts();
            hold.advance(hold.trajectory(c), .01);
            near(hold.volts(), retained, 0, "inhibited hold lost charge without leakage");
        }
    auto c = configuration(280)[0];
    c.inputBiasAmps=30e-12; c.offLeakageAmps=10e-12; c.turnOffChargeCoulombs=7e-12;
    Hold hold; hold.reset(4); hold.select(4); hold.inhibit(c);
    near(hold.volts(),4.0007,1e-14,"Q/C injection missing");
    hold.inhibit(c);
    near(hold.volts(),4.0007,1e-14,"same inhibit injected twice");
    hold.advance(hold.trajectory(c),.0042);
    near(hold.volts(),4.0007-16.8e-6,1e-14,"signed I*t/C droop incorrect");
    hold.select(4); hold.advance(hold.trajectory(c),.01);
    near(hold.volts(),4-30e-12*280,1e-14,"selected bias equilibrium incorrect");

    // Independent two-capacitor RK4, including equal and separated poles.
    for(double tau : {2.8e-6, .000687, .001})
    {
        Hold::Trajectory trajectory{.8,-.5,0,tau};
        double h=.3,u=.2;
        const double dt=1e-7, seconds=.001;
        const auto derivative=[tau](double hv,double uv){
            return std::array<double,2>{(.8-hv)/tau,(hv-uv)/.000687};
        };
        for(int i=0;i<10000;++i)
        {
            const auto a=derivative(h,u);
            const auto b=derivative(h+dt*a[0]/2,u+dt*a[1]/2);
            const auto c2=derivative(h+dt*b[0]/2,u+dt*b[1]/2);
            const auto d=derivative(h+dt*c2[0],u+dt*c2[1]);
            h+=dt*(a[0]+2*b[0]+2*c2[0]+d[0])/6;
            u+=dt*(a[1]+2*b[1]+2*c2[1]+d[1])/6;
        }
        near(trajectory.throughOnePole(.2,seconds,.000687),u,2e-10,"linear cascade oracle failed");
    }
}

// Independent transistor/voltage-domain oracle, retaining the existing knee
// prior. Integrates actual C58 KCL; never calls production q(u), table or RK4.
constexpr long double vt = static_cast<long double>(.026f);
constexpr long double span = 5.0L*2.0210925562118116L*4095/4096;
constexpr long double offset=.26L;
const long double logIs=std::log(vt/32000)-(offset+static_cast<long double>(.015f)*9.921875L)/vt;
long double current(long double node,long double resistance)
{
    long double log=std::log(std::max(node/resistance,1e-20L));
    for(int i=0;i<30;++i)
    {
        const auto value=std::exp(log);
        const auto step=(resistance*value+vt*(log-logIs)-node)/(resistance*value+vt);
        log-=step;
        if(std::abs(step)<1e-17L)break;
    }
    return std::exp(log);
}
long double equilibrium(long double control)
{
    const auto supply=offset+span*control;
    return supply-10000*current(supply,32000);
}
long double oracle(double u0,double target,double resistance,double seconds,double enable)
{
    long double node=equilibrium(u0);
    const int steps=std::max(4000,static_cast<int>(std::ceil(seconds/(resistance*1e-8/40))));
    const long double dt=seconds/steps;
    const auto derivative=[=](long double t,long double v) {
        const long double control=t<enable ? u0
            : target+(u0-target)*std::exp(-(t-enable)/(resistance*1e-8L));
        return ((offset+span*control-v)/10000-current(v,22000))/.1e-6L;
    };
    for(int i=0;i<steps;++i)
    {
        const long double t=i*dt;
        const auto a=derivative(t,node),b=derivative(t+dt/2,node+dt*a/2);
        const auto c=derivative(t+dt/2,node+dt*b/2),d=derivative(t+dt,node+dt*c);
        node+=dt*(a+2*b+2*c+d)/6;
    }
    return node;
}

void fractionalEngineTest()
{
    double largestError=0;
    for(double rate : {8000.,48000.,192000.})
        for(double resistance : {80.,280.,10000.})
            for(int slot : {0,5})
            for(const auto& direction : {std::pair{.006,.8f},std::pair{.8,.006f}})
            {
                const auto [initial, target] = direction;
                auto e=make(resistance,rate);
                Access::seed(*e,initial,target);
                const double position=Access::atEnable(*e,slot,.37);
                const double before=Access::held(*e,slot);
                process(*e);
                const double actualTarget=static_cast<double>(target);
                const double expected=Access::offset()+Access::span()
                    *(actualTarget+(initial-actualTarget)*std::exp(-(1-position)/rate/(resistance*1e-8)));
                near(Access::held(*e,slot),expected,2e-10,"fractional physical enable misplaced");
                require(std::abs(Access::held(*e,slot)-before)>1e-9,"real callback never acquired hold");
                near(Access::held(*e,slot==0?1:4),before,0,"unselected card acquired another channel");
                const double reference=static_cast<double>(oracle(initial,actualTarget,resistance,1/rate,position/rate));
                const double error=std::abs(Access::capacitor(Access::control(*e,slot))-reference);
                largestError=std::max(largestError,error);
                require(error<1.5e-5,"upstream hold did not drive nonlinear C58 trajectory");
                // The later target commit must not reset the capacitor or add
                // a second acquisition/injection. Its endpoint keeps moving
                // toward the written code in either direction.
                const double held=Access::held(*e,slot);
                process(*e);
                const double finalVolts=Access::offset()+Access::span()*actualTarget;
                require(std::abs(Access::held(*e,slot)-finalVolts)
                    <=std::abs(held-finalVolts)+1e-12,"latched write reset held charge");
                near(Access::target(*e,slot),actualTarget,1e-8,"latched target did not commit");
            }
    std::cout<<"maximum independent C58 voltage error="<<largestError<<" V\n";
}

void windowAndRestartTest()
{
    for(double rate : {8000.,48000.,192000.})
    {
        auto e=make(10000,rate,false);
        Access::seed(*e,.2,.9f);
        Access::select(*e,0,.9f);
        const double fraction=Access::atInhibit(*e,.43);
        process(*e);
        const double expected=Access::offset()+Access::span()
            *(static_cast<double>(.9f)+(.2-static_cast<double>(.9f))*std::exp(-fraction/rate/1e-4));
        near(Access::held(*e),expected,2e-10,"acquisition continued through mux inhibit");
        require(!Access::selected(*e),"mux inhibit left envelope connected");
        const double held=Access::held(*e);
        process(*e);
        near(Access::held(*e),held,0,"disconnected hold continued toward DAC target");
    }
    auto e=make(10000);
    Access::seed(*e,.3,.9f);
    Access::select(*e,4,.9f);
    const double charge=Access::held(*e,4);
    e->noteOn(60,1);
    near(Access::held(*e,4),charge,0,"note allocation changed physical capacitor");
    require(Access::selected(*e,4),"logical scan restart invented a mux edge");
    process(*e);
    require(!Access::selected(*e,4),"replacement pass did not inhibit old selection");

    auto injection = std::make_unique<Engine>();
    auto c = configuration(10000);
    for(auto& channel:c)channel.turnOffChargeCoulombs=7e-12;
    require(injection->configureEnvelopeHolds(c),"injection comparison rejected");
    injection->setParameters(parameters(false));
    injection->prepare(8000,1,1);
    Access::seed(*injection,.2,.8f);
    Access::select(*injection,1,.8f);
    const double position=Access::atEnable(*injection,0,.5);
    process(*injection);
    const double inhibitSeconds=position/8000.0-75.0/4000000.0;
    const double oldExpected=Access::offset()+Access::span()
        *(static_cast<double>(.8f)+(.2-static_cast<double>(.8f))*std::exp(-inhibitSeconds/1e-4))
        +7e-12/1e-8;
    near(Access::held(*injection,1),oldExpected,2e-10,"physical inhibit did not inject into old channel");
    const double newHeld=Access::held(*injection);
    Access::commitLatched(*injection,0,.8f);
    near(Access::held(*injection),newHeld,0,"fractional target commit injected charge twice");
    require(Access::selected(*injection),"fractional target commit interrupted ongoing acquisition");
}

std::unique_ptr<Engine> makeFirmwareHold(double rate = 32000, int factor = 1,
                                       bool coupled = false)
{
    auto e = std::make_unique<Engine>();
    auto c = configuration(10000);
    for (auto& channel : c) channel.turnOffChargeCoulombs = 7e-12;
    require(e->configureEnvelopeHolds(c), "firmware hold configuration rejected");
    e->selectConverterTimingProfile(Engine::ConverterTimingProfile::FirmwareControlNoInterrupt);
    auto p = parameters(coupled);
    p.keyMode = youknow::KeyMode::Unison;
    p.sustain = .6f;
    e->setParameters(p);
    e->prepare(rate, 256, factor);
    e->noteOn(60, 1);
    return e;
}

void firmwareHoldHandoffTest()
{
    constexpr double resistance = 10000, capacitance = 1e-8, injection = 7e-12;
    constexpr double initial = .2;
    const auto acquiredVolts = [](double target, double seconds) {
        return Access::offset() + Access::span()
            * (target + (initial - target) * std::exp(-seconds / (resistance * capacitance)));
    };
    for (double rate : {32000., 48000., 192000.})
    {
        for (int slot : {0, 5})
        {
            auto e = makeFirmwareHold(rate);
            const double position = Access::atEnable(*e, slot, .37);
            Access::advanceFirmwareToCursor(*e);
            const float target = Access::firmwareVcaTarget(*e, slot);
            require(target > .3f, "firmware acquisition witness has no target step");
            Access::seed(*e, initial, .03f); // traced code must win over this later scalar
            process(*e);
            near(Access::held(*e, slot), acquiredVolts(target, (1-position)/rate), 2e-10,
                 "firmware fractional enable did not acquire the traced DAC code");
            require(Access::selected(*e, slot), "firmware enable did not open the real hold");
            process(*e);
            near(Access::held(*e, slot), acquiredVolts(target, (2-position)/rate), 2e-10,
                 "firmware target commit interrupted or reinjected the acquired hold");
            near(Access::target(*e, slot), target, 0, "firmware target commit lost its captured code");
        }
        auto e = makeFirmwareHold(rate);
        const double position = Access::atInhibit(*e, .43);
        Access::advanceFirmwareToCursor(*e);
        Access::seed(*e, initial, .9f);
        Access::select(*e, 0, .9f);
        process(*e);
        const double expected = acquiredVolts(static_cast<double>(.9f), position/rate)
                              + injection/capacitance;
        near(Access::held(*e), expected, 2e-10, "same-pass firmware inhibit missed its actual timestamp");
        require(!Access::selected(*e), "same-pass firmware inhibit left the hold selected");
        process(*e);
        near(Access::held(*e), expected, 2e-10, "later VCF enable injected a second turn-off charge");
    }

    // At 32 kHz an interval spans125 nominal CPU states. Place its beginning
    // five states before the real pass end. The next RES inhibit is inside
    // this interval for HOLDoff (5+111), but outside for HOLDon (5+129).
    // Its enable is outside either way. Check both snapshot-change directions.
    for (bool oldHold : {false, true})
        for (bool nextHold : {false, true})
        {
            auto e = makeFirmwareHold();
            near(Access::atFirmwareWrap(*e, oldHold, 5), oldHold ? 129 : 111, 1e-10,
                 "independent first-inhibit instruction sum differs from current trace");
            Access::seed(*e, initial, .9f);
            // This deliberately selected capacitor is a circuit witness for
            // the inhibit edge, independent of the ordinary NOISE selection.
            Access::select(*e, 5, .9f);
            e->setSustainPedal(nextHold);
            const double inhibitSeconds = (5.0 + (nextHold ? 129.0 : 111.0))/4000000.0;
            process(*e);
            const double firstSeconds = std::min(1.0/32000, inhibitSeconds);
            const double expectedFirst = acquiredVolts(static_cast<double>(.9f), firstSeconds)
                + (nextHold ? 0 : injection/capacitance);
            near(Access::held(*e, 5), expectedFirst, 2e-10,
                 "next-pass inhibit used a normalized period or stale HOLD snapshot");
            require(Access::selected(*e, 5) == nextHold,
                    "next-pass inhibit/enable ownership is incorrect at the wrap");
            process(*e);
            const double expectedFinal = acquiredVolts(static_cast<double>(.9f), inhibitSeconds)
                                       + injection/capacitance;
            near(Access::held(*e, 5), expectedFinal, 2e-10,
                 "next-pass inhibit did not preserve the exact acquisition prefix");
            require(!Access::selected(*e, 5), "next-pass RES inhibit never closed the hold");
            process(*e); // commit the already consumed fractional RES enable
            near(Access::held(*e, 5), expectedFinal, 2e-10,
                 "next-pass RES target commit injected turn-off charge twice");
        }
}

struct FirmwareHoldRender
{
    std::vector<float> audio;
    std::array<double, 6> holds {}, controls {};
};
FirmwareHoldRender renderFirmwareHold(int block, int factor = 1)
{
    auto e = makeFirmwareHold(32000.0/factor, factor, true);
    // Event times are expressed on the common 32 kHz internal clock and
    // divisible by four, so 8k/4x and 32k/1x receive the same physical inputs.
    constexpr std::array eventFrames {256, 512, 768, 900, 1020, 1476, 2560};
    FirmwareHoldRender result;
    result.audio.resize(2560/static_cast<std::size_t>(factor));
    std::vector<float> right(result.audio.size());
    int at = 0;
    for (std::size_t event = 0; event < eventFrames.size(); ++event)
    {
        const int end = eventFrames[event]/factor;
        while (at < end)
        {
            const int count = std::min(block, end-at);
            e->process(result.audio.data()+at, right.data()+at, count);
            at += count;
        }
        if (event == 0 || event == 2) e->setSustainPedal(true);
        else if (event == 1 || event == 4) e->setSustainPedal(false);
        else if (event == 3) e->noteOff(60);
        else if (event == 5) e->noteOn(67, 1);
    }
    for (int slot = 0; slot < 6; ++slot)
    {
        result.holds[static_cast<std::size_t>(slot)] = Access::held(*e, slot);
        result.controls[static_cast<std::size_t>(slot)] = Access::control(*e, slot);
    }
    return result;
}

void firmwareHoldBlockTest()
{
    const auto reference = renderFirmwareHold(1);
    for (int block : {97, 256})
    {
        const auto other = renderFirmwareHold(block);
        require(reference.audio == other.audio && reference.holds == other.holds
                && reference.controls == other.controls,
                "firmware/hold audio or state depends on callback partition");
    }
    double energy = 0;
    for (float value : reference.audio)
    {
        require(std::isfinite(value), "firmware/hold audio is non-finite");
        energy += value*value;
    }
    require(energy > 1e-5, "firmware/hold comparison did not reach the audio path");
    const auto oversampled = renderFirmwareHold(97, 4);
    require(reference.holds == oversampled.holds && reference.controls == oversampled.controls,
            "equal physical clocks changed firmware/hold state across host rates");
}

std::vector<float> render(double resistance, int block, double rate=48000, int factor=1,
                          bool coupled=true)
{
    auto e=make(resistance,rate,coupled);
    if(factor!=1)e->prepare(rate,256,factor);
    e->noteOn(60,1);
    const int length=static_cast<int>(rate*.08),off=static_cast<int>(rate*.027);
    std::vector<float> result(static_cast<std::size_t>(length)),right(result.size());
    for(int i=0;i<length;)
    {
        if(i==off)e->noteOff(60);
        const int count=std::min({block,length-i,i<off?off-i:length-i});
        e->process(result.data()+i,right.data()+i,count);
        i+=count;
    }
    for(float value:result)require(std::isfinite(value),"audio comparison non-finite");
    return result;
}
void audioAndRateTest()
{
    const auto reference=render(0,97),finite=render(10000,97),fine=render(1,97);
    require(finite==render(10000,1),"finite hold audio depends on host block size");
    require(finite==render(10000,256),"finite hold audio depends on long host blocks");
    double difference=0,limitDifference=0,energy=0;
    for(std::size_t i=0;i<reference.size();++i)
    {
        difference+=std::pow(finite[i]-reference[i],2);
        limitDifference+=std::pow(fine[i]-reference[i],2);
        energy+=reference[i]*reference[i];
    }
    require(difference>1e-8 && energy>1e-4,"configured physical model failed to reach audio");
    require(limitDifference<difference*1e-5,"ideal-acquisition limit does not approach baseline");
    std::cout<<"audio finite-versus-ideal residual="<<10*std::log10(difference/energy)
             <<" dBr; 1-ohm limit="<<10*std::log10(limitDifference/energy)<<" dBr\n";
    for(double resistance:{80.,280.})
    {
        const auto conditional=render(resistance,97);
        double residual=0;
        for(std::size_t i=0;i<reference.size();++i)
            residual+=std::pow(conditional[i]-reference[i],2);
        std::cout<<"conditional "<<resistance<<"-ohm audio residual="
                 <<10*std::log10(residual/energy)<<" dBr (not installed calibration)\n";
    }
    std::array<double,6> expected {};
    for(double rate : {8000.,48000.,96000.,192000.})
    {
        auto e=make(10000,rate,false);
        e->noteOn(60,1); process(*e,static_cast<int>(rate*.020));
        for(int slot=0;slot<6;++slot)
        {
            const double value=Access::control(*e,slot);
            if(rate==8000)expected[static_cast<std::size_t>(slot)]=value;
            else near(value,expected[static_cast<std::size_t>(slot)],2e-9,"physical hold/C58 seconds depend on rate");
        }
    }
    // Equal internal rates are the same circuit clock even at different
    // host/quality combinations. Audio differs only in downstream decimation.
    auto a=make(10000,48000,false),b=make(10000,96000,false);
    a->prepare(48000,256,2); a->noteOn(60,1); b->noteOn(60,1);
    process(*a,960); process(*b,1920);
    near(Access::control(*a),Access::control(*b),0,"quality changed physical hold rate");
}
} // namespace

int main()
{
    try
    {
        configurationTest(); signedDomainTest(); circuitTest(); fractionalEngineTest();
        windowAndRestartTest(); firmwareHoldHandoffTest(); firmwareHoldBlockTest(); audioAndRateTest();
        std::cout<<"Envelope hold circuit tests passed\n";
    }
    catch(const std::exception& error)
    { std::cerr<<error.what()<<'\n'; return 1; }
}
