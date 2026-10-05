// Conditional same-part thermal estimate, not an installed Juno capture.
// Roland p13 supplies R106/R105/C58 and p18 the .26V standoff. Toshiba's
// 2SA1015 p2 -25/25/100C VBE curves support the named -1.6mV/C estimate.
// The oracle below solves physical junction voltage and capacitor KCL;
// it does not share the production log-current solver or prepared tables.
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowControlDac.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace youknow
{
struct YouKnowTestAccess
{
    static const VcaJunctionTemperatureCircuit& circuit()
    { return YouKnowEngine::voiceVcaJunctionCircuit(); }
    static void temperature(YouKnowEngine& engine, float fraction)
    {
        engine.thermalWarmupFraction_ = fraction;
        engine.refreshVoiceCardThermalScales();
    }
    static double celsius(const YouKnowEngine& engine, int slot)
    { return engine.cards_[slot].vcaJunctionCelsius; }
    static std::array<double, 3> service(const YouKnowEngine& engine, int slot)
    {
        const auto& c = engine.cards_[slot];
        return {c.vcaJunctionServiceEmitterAmps, c.vcaJunctionHeadroomVolts,
                c.vcaJunctionServiceGain};
    }
    static void seed(YouKnowEngine& engine, int slot, double control)
    {
        auto& voice = engine.voices_[slot];
        engine.cards_[slot].vcaGainError = 0;
        engine.refreshVoiceVcaCoupling();
        voice.active = voice.keyDown = true;
        voice.vcaControl = voice.vcaControlTarget = control;
        voice.vcaJunctionCharge = circuit().chargeAtControl(control, celsius(engine, slot));
        voice.vcaJunctionChargeInitialised = true;
        voice.vcaInputCoupling.reset();
        engine.updateVoiceAudio(voice, engine.activeParameters_);
    }
    static double gain(YouKnowEngine& engine, int slot)
    {
        engine.updateVoiceAudio(engine.voices_[slot], engine.activeParameters_);
        return engine.voices_[slot].vcaGain;
    }
    static double charge(const YouKnowEngine& engine, int slot)
    { return engine.voices_[slot].vcaJunctionCharge; }
    static double finish(YouKnowEngine& engine, int slot, float volts)
    { return engine.finishVoiceFilter(engine.voices_[slot], volts) * YouKnowEngine::internalVoltsPerUnit; }
    static double input(const YouKnowEngine& engine, int slot)
    { return engine.voices_[slot].vcaInputVolts; }
    static double fraction(const YouKnowEngine& engine)
    { return engine.thermalWarmupFraction_; }
    static void changeRate(YouKnowEngine& engine, double rate, int factor)
    {
        engine.sampleRate_ = rate;
        engine.inverseSampleRate_ = static_cast<float>(1/rate);
        engine.oversamplingApplied_ = factor;
        engine.updateProcessingRate(true);
    }
};
}

namespace
{
using Engine = youknow::YouKnowEngine;
using Probe = youknow::YouKnowTestAccess;
constexpr long double span = static_cast<float>(
    youknow::ControlDac::positiveSpanVolts(youknow::ControlDac::maximumCode));
constexpr long double referenceIe = .0006509655627366167L * 201.0L/200.0L;
constexpr long double alpha = 200.0L/201.0L;
constexpr long double referenceVt = 1.380649e-23L*298.15L/1.602176634e-19L;

void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
void near(double actual, double expected, double tolerance, const char* message)
{
    if (!std::isfinite(actual) || std::abs(actual-expected)>tolerance)
    {
        std::cerr << message << ": " << actual << " vs " << expected
                  << " (limit " << tolerance << ")\n";
        throw std::runtime_error(message);
    }
}
long double emitter(long double voltage, long double resistance, long double temperature)
{
    const long double vt = referenceVt*(temperature+273.15L)/298.15L;
    const long double referenceVbe = .61L-.0016L*(temperature-25.0L);
    // A voltage-domain convex Newton solve from above; all screened physical
    // currents are positive and no production table or Is solver is reused.
    long double junction = referenceVbe;
    for (int iteration=0; iteration<80; ++iteration)
    {
        const long double current = referenceIe*std::exp((junction-referenceVbe)/vt);
        const long double step=(junction+resistance*current-voltage)/(1+resistance*current/vt);
        junction-=step;
        if (std::abs(step)<1e-19L) break;
    }
    return referenceIe*std::exp((junction-referenceVbe)/vt);
}
long double currentAtControl(long double control, long double temperature)
{ return emitter(.26L+span*control, 32000.0L, temperature); }
long double currentAtCharge(long double charge, long double temperature)
{ return emitter(.26L+span*charge, 22000.0L, temperature); }
long double capacitorDerivative(long double charge, long double target, long double temperature)
{ return (target-charge)/.001L-currentAtCharge(charge,temperature)/(span*.1e-6L); }
long double oracleStep(long double q, long double target, long double dt, long double temperature)
{
    const auto a=capacitorDerivative(q,target,temperature);
    const auto b=capacitorDerivative(q+dt*a/2,target,temperature);
    const auto c=capacitorDerivative(q+dt*b/2,target,temperature);
    const auto d=capacitorDerivative(q+dt*c,target,temperature);
    return q+dt*(a+2*b+2*c+d)/6;
}
std::unique_ptr<Engine> makeEngine(float character=1, bool spatial=false,
                                  bool settled=false, double rate=48000, int factor=1)
{
    auto engine=std::make_unique<Engine>();
    require(engine->configureThermalStart(settled), "thermal start rejected");
    require(engine->configureServiceDerivedVcaCoupling(true), "C59 service configuration rejected");
    youknow::EngineParameters p;
    p.calibration=character;
    p.aging=0;
    p.enableEvidenceVcaCalibration=true;
    p.enableVoiceVcaJunctionTemperature=true;
    p.enableSpatialThermalGradient=spatial;
    p.enableOtaShotNoise=false;
    p.enableVoiceVcaAntialias=false;
    p.vcfTanhMode=youknow::VcfTanhMode::PolyZoned;
    p.vcfFastEarlyMode=youknow::VcfFastEarlyMode::Cubic;
    p.vcfSolverMode=youknow::VcfSolverMode::Rk4Single;
    p.chorus=youknow::ChorusMode::Off;
    engine->setParameters(p);
    engine->prepare(rate,128,factor);
    return engine;
}
void checkPreparedCurrentAccuracy()
{
    const auto& circuit=Probe::circuit();
    double maximumRelative=0;
    for (double temperature : {25.0,25.37,29.5,39.73,40.0,54.25,63.0,64.0})
    {
        for (int sample=0; sample<=1024; ++sample)
        {
            const double u=static_cast<double>(sample)/1024;
            for (double coordinate : {u, -.01+1.02*u})
            {
                const double expected=static_cast<double>(currentAtCharge(coordinate,temperature));
                const double actual=circuit.emitterAmpsAtCharge(coordinate,temperature);
                const double relative=std::abs(actual-expected)/expected;
                maximumRelative=std::max(maximumRelative,relative);
                // At 8192 knots, the quiet exponential has a bounded small
                // interpolation error; full-current DC is much more linear.
                near(actual,expected,expected*8e-4+1e-20,
                     "prepared C58 current missed independent junction solve");
            }
            const double expected=static_cast<double>(currentAtControl(u,temperature));
            near(circuit.emitterAmpsAtControl(u,temperature),expected,
                 expected*8e-4+1e-20,"prepared hold current missed junction solve");
            const double q=circuit.chargeAtControl(u,temperature);
            near(circuit.controlAtCharge(q,temperature),u,2e-7,
                 "physical C58/equivalent hold coordinates disagree");
        }
    }
    std::cout << "Tr20 prepared-grid maximum relative current error " << maximumRelative << '\n';
}
void checkPhysicalCapacitorTrajectory()
{
    const auto& circuit=Probe::circuit();
    for (int rate : {8000,48000,96000})
    {
    double maximumChargeError=0;
    for (double temperature : {25.0,25.37,40.0,63.7})
        for (const auto& transition : std::array<std::array<double,2>,8>{{
            {0,.005},{.005,.02},{.02,.005},{.02,1},{1,0},{.5,.05},{.01,.01},{0,1}}})
        {
            long double reference=transition[0]-10000*currentAtControl(transition[0],temperature)/span;
            double actual=static_cast<double>(reference);
            const double dt=1.0/rate;
            for (int sample=0;sample<rate/100;++sample)
            {
                actual=circuit.advance(actual,transition[1],dt,temperature);
                for (int sub=0;sub<32;++sub)
                    reference=oracleStep(reference,transition[1],dt/32,temperature);
                maximumChargeError=std::max(maximumChargeError,std::abs(actual-static_cast<double>(reference)));
                // A single RK4 step at8kHz has dt/tau up to.182. This
                // screened physical-voltage budget follows fourth-order
                // convergence plus a table floor; it is not a proved global
                // error bound. The explicit48/96k screens retain their much
                // tighter node limits. These are
                // physical C58 coordinates (multiply span for node volts),
                // not an audio-match tolerance or a change to old oracles.
                const double budget=1e-5*std::pow(8000.0/rate,4)+2e-7;
                near(actual,static_cast<double>(reference),budget,
                     "thermal C58 trajectory missed independent physical KCL");
            }
            const double gainActual=circuit.emitterAmpsAtCharge(actual,temperature);
            const double gainExpected=static_cast<double>(currentAtCharge(reference,temperature));
            near(gainActual,gainExpected,gainExpected*.002+1e-16,
                 "quiet-tail current missed refined physical KCL");
        }
    std::cout << "Tr20 " << rate << "Hz maximum C58 voltage error "
              << maximumChargeError*static_cast<double>(span)*1e6 << " uV\n";
    }
    long double reference=.02L-10000*currentAtControl(.02L,40.37L)/span;
    long double refined=reference;
    double maximumRefinement=0;
    for (int sample=0;sample<80;++sample)
    {
        constexpr long double dt=1.0L/8000;
        for (int sub=0;sub<32;++sub) reference=oracleStep(reference,1,dt/32,40.37L);
        for (int sub=0;sub<64;++sub) refined=oracleStep(refined,1,dt/64,40.37L);
        maximumRefinement=std::max(maximumRefinement,
            std::abs(static_cast<double>(reference-refined))*static_cast<double>(span));
    }
    std::cout << "Tr20 physical KCL32-to-64 refinement " << maximumRefinement*1e6 << " uV\n";
    // 5nV is over80x below the screened production-grid node error and
    // over5000x below the coarse8kHz error; this screen measures reference
    // separation, rather than asserting an unproved exact-solution bound.
    require(maximumRefinement<5e-9,"independent physical KCL reference did not converge");
}
void checkFixedServiceAndPhysicalPair()
{
    for (float character : {0.0f,1.0f,2.0f})
        for (bool spatial : {false,true})
        {
            auto engine=makeEngine(character,spatial,true);
            for (int card=0;card<6;++card)
            {
                const double serviceTemperature=25+character*(15+(spatial?4*std::exp(-card/2.5):0));
                const auto fixed=Probe::service(*engine,card);
                const double peak=static_cast<double>(currentAtControl(1,serviceTemperature));
                const double sustain=static_cast<double>(currentAtControl(4064.0L/4095.0L,serviceTemperature));
                const double headroom=2.4/std::atanh(3/(47000*static_cast<double>(alpha)*sustain));
                near(fixed[0],peak,peak*1e-7,"fixed service current missed settled card temperature");
                near(fixed[1],headroom,headroom*2e-7,"service divider was not recalibrated coherently");
                for (float fraction : {1.0f,0.0f,.5f,1.0f})
                {
                    Probe::temperature(*engine,fraction);
                    require(Probe::service(*engine,card)==fixed,"warmup re-normalized fixed service coefficients");
                    const double temperature=Probe::celsius(*engine,card);
                    Probe::seed(*engine,card,.02);
                    const double gainExpected=static_cast<double>(currentAtControl(.02,temperature))/peak;
                    near(Probe::gain(*engine,card),gainExpected,gainExpected*.001,
                         "engine gain did not use absolute thermal current with fixed reference");
                    const double output=Probe::finish(*engine,card,2.4f);
                    const double actualInput=Probe::input(*engine,card);
                    const double expected=47000*static_cast<double>(alpha)
                        *static_cast<double>(currentAtControl(.02,temperature))
                        *std::tanh(actualInput/headroom*(serviceTemperature+273.15)/(temperature+273.15));
                    near(output,expected,std::abs(expected)*.001+2e-7,
                         "physical pair gain doubled or omitted thermal scaling");
                }
                Probe::temperature(*engine,1);
                Probe::seed(*engine,card,4064.0/4095.0);
                const double output=Probe::finish(*engine,card,2.4f);
                const double actualInput=Probe::input(*engine,card);
                near(output,3*std::tanh(actualInput/headroom)/std::tanh(2.4/headroom),2e-6,
                     "warm fixed trim no longer gives 4.8-to-6Vpp service gain");
            }
        }
    const double fullRatio=static_cast<double>(currentAtControl(1,40)/currentAtControl(1,25));
    const double quietRatio=static_cast<double>(currentAtControl(.02,40)/currentAtControl(.02,25));
    require(20*std::log10(fullRatio)<.03 && 20*std::log10(quietRatio)>4.5,
            "thermal estimate failed its near-full versus quiet-current consequence");
    std::cout << "Tr20 25-to-40C current change: full " << 20*std::log10(fullRatio)
              << " dB; 2% " << 20*std::log10(quietRatio) << " dB\n";
}
void checkChargeAndLifecycle()
{
    auto engine=makeEngine();
    Probe::seed(*engine,0,.02);
    const double retained=Probe::charge(*engine,0);
    const auto fixed=Probe::service(*engine,0);
    Probe::temperature(*engine,1);
    require(Probe::charge(*engine,0)==retained,"temperature reset actual C58 charge");
    Probe::changeRate(*engine,96000,4);
    require(Probe::charge(*engine,0)==retained,"quality rebuild reset actual C58 charge");
    require(Probe::service(*engine,0)==fixed,"quality rebuild retuned service current");
    for (double rate : {48000.0,96000.0})
        for (int factor : {1,4})
        {
            auto whole=makeEngine(1,true,false,rate,factor);
            auto split=makeEngine(1,true,false,rate,factor);
            whole->noteOn(60,1); split->noteOn(60,1);
            const int count=static_cast<int>(rate*.025);
            std::vector<float> l(count),r(count),sl(count),sr(count);
            for (int offset=0;offset<count;offset+=128)
                whole->process(l.data()+offset,r.data()+offset,std::min(128,count-offset));
            for (int offset=0;offset<count;offset+=17)
                split->process(sl.data()+offset,sr.data()+offset,std::min(17,count-offset));
            require(l==sl && r==sr,"thermal current depends on host block partition");
            require(std::any_of(l.begin(),l.end(),[](float x){return std::abs(x)>1e-7;}),
                    "thermal trajectory render was silent");
            near(Probe::fraction(*whole),1-std::exp(-.025/3),2e-7,
                 "rate/quality changed existing chassis temperature trajectory");
        }
}
}
int main()
{
    try
    {
        checkPreparedCurrentAccuracy();
        checkPhysicalCapacitorTrajectory();
        checkFixedServiceAndPhysicalPair();
        checkChargeAndLifecycle();
        std::cout << "Voice VCA junction temperature checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    { std::cerr<<error.what()<<'\n';return 1; }
}
