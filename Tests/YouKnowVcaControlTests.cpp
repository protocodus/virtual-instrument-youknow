#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowVcaControl.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace youknow
{
struct YouKnowTestAccess
{
    static const VcaControlCircuit& productionCircuit()
    {
        return YouKnowEngine::voiceVcaControlCircuit();
    }
    static void seedQuietHold(YouKnowEngine& engine)
    {
        engine.voices_[0].vcaControl=.015;
        engine.voices_[0].vcaControlTarget=0;
    }
    static double control(const YouKnowEngine& engine, int slot = 0)
    {
        return engine.voices_[static_cast<std::size_t>(slot)].vcaControl;
    }
    static double seedFractionalWrite(YouKnowEngine& engine, int slot,
                                      double requestedPosition)
    {
        for (int index = 0; index < YouKnowEngine::hardwareVoices; ++index)
        {
            auto& voice = engine.voices_[static_cast<std::size_t>(index)];
            voice.vcaControl = .009;
            voice.vcaControlTarget = .006f;
            voice.envelope.value = .8f;
        }
        const auto& writes = YouKnowEngine::converterWriteOrder();
        const auto found = std::find_if(writes.begin(), writes.end(), [slot](const auto& write) {
            return write.destination == YouKnowEngine::ConverterDestination::VoiceVca
                && write.voice == slot;
        });
        if (found == writes.end())
            throw std::runtime_error("voice VCA write missing from the converter schedule");
        const auto ordinal = static_cast<std::size_t>(found - writes.begin());
        const double delta = YouKnowEngine::controlScanHz / engine.oversampledRate_;
        const double event = engine.converterEventPhases_[ordinal];
        engine.controlScanPhase_ = event - requestedPosition * delta;
        engine.nextConverterWrite_ = ordinal;
        engine.passiveHoldEventLatch_ = {};
        engine.assignmentRescanPending_ = false;
        engine.assignmentRescanPassArmed_ = false;
        return std::clamp((event - engine.controlScanPhase_) / delta, 0.0, 1.0);
    }
    static float target(const YouKnowEngine& engine, int slot)
    {
        return engine.voices_[static_cast<std::size_t>(slot)].vcaControlTarget;
    }
    static void changeEnvelope(YouKnowEngine& engine, int slot)
    {
        engine.voices_[static_cast<std::size_t>(slot)].envelope.value = .3f;
    }
    static void seedInactiveAudioCell(YouKnowEngine& engine)
    {
        auto& voice = engine.voices_[0];
        voice.active = false;
        voice.vcaControl = voice.vcaControlTarget = .5f;
        voice.vcaGain = 0.0f;
        voice.cutoffCounts = voice.cutoffCountsTarget = 6000.0f;
        voice.filterOmegaStep = 0.0f;
        voice.cutoffChainCounts = -1.0e30f;
        voice.pulseThresholdVolts = -100.0f;
        // No converter write is due during this one-sample callback.
        engine.controlScanPhase_ = -1.0;
        engine.nextConverterWrite_ = 0;
        engine.passiveHoldEventLatch_ = {};
    }
    static bool inactiveAudioCellUpdated(const YouKnowEngine& engine)
    {
        const auto& voice = engine.voices_[0];
        return !voice.active && voice.vcaGain > 0.0f
            && voice.filterOmegaStep > 0.0f
            && voice.pulseThresholdVolts > -100.0f;
    }
};
}

namespace
{
// Independent voltage-domain oracle: solve Tr20's exponential junction at
// R105 and integrate C58 KCL with substepped long-double RK4. It never calls
// the production charge table, RK4 helper or effective-CV equation.
constexpr long double vt = static_cast<long double>(0.026f);
constexpr long double span = 9.921875L;
constexpr long double knee = static_cast<long double>(0.015f);
constexpr long double standoff = 0.26L;
const long double logIs = std::log(vt / 32000.0L) - (standoff + knee * span) / vt;

long double emitterCurrent(long double node, long double resistance)
{
    long double logarithm = std::log(std::max(node / resistance, 1.0e-20L));
    for (int iteration = 0; iteration < 30; ++iteration)
    {
        const long double current = std::exp(logarithm);
        const long double residual = resistance * current + vt * (logarithm - logIs) - node;
        const long double step = residual / (resistance * current + vt);
        logarithm -= step;
        if (std::abs(step) < 1.0e-17L)
            break;
    }
    return std::exp(logarithm);
}

long double equilibrium(long double control)
{
    const long double supply = standoff + span * control;
    return supply - 10000.0L * emitterCurrent(supply, 32000.0L);
}

long double advanceOracle(long double node, long double target, long double dt)
{
    const auto derivative = [target](long double voltage) {
        return ((standoff + span * target - voltage) / 10000.0L
                - emitterCurrent(voltage, 22000.0L)) / 0.1e-6L;
    };
    const auto a = derivative(node);
    const auto b = derivative(node + dt * a / 2);
    const auto c = derivative(node + dt * b / 2);
    const auto d = derivative(node + dt * c);
    return node + dt * (a + 2*b + 2*c + d) / 6;
}

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

long double advanceFineOracle(long double node, long double target, long double seconds)
{
    for (int substep = 0; substep < 64; ++substep)
        node = advanceOracle(node, target, seconds / 64);
    return node;
}

double capacitorError(const youknow::VcaControlCircuit& circuit,
                       double control, long double reference)
{
    return std::abs(static_cast<double>(standoff
        + span * circuit.capacitorCoordinate(control) - reference));
}

double errorBudget(int rate)
{
    // Includes the single-step side of the bounded-substep cutoff, which
    // is substantially harder than the ordinary envelope transition sweep.
    return .003 * std::pow(8000.0 / rate, 2) + 5e-6;
}

void processOne(youknow::YouKnowEngine& engine)
{
    float left = 0, right = 0;
    engine.process(&left, &right, 1);
    require(std::isfinite(left) && std::isfinite(right),
            "VCA callback fixture produced non-finite audio");
}

void testSubstepBoundary(const youknow::VcaControlCircuit& circuit)
{
    // Deliberately approach the branch on both sides. At the boundary the
    // solver takes one RK4 step even for a large upward target change.
    const double boundary = static_cast<double>(knee)
        + 8.0 * static_cast<double>(vt) / static_cast<double>(span);
    for (int rate : {8000, 44100, 48000, 192000, 768000})
    {
        double maximumError = 0;
        for (double initial : {std::nextafter(boundary, 0.0), boundary,
                               std::nextafter(boundary, 1.0)})
        {
            double control = initial;
            long double reference = equilibrium(initial);
            for (int frame = 0; frame < rate / 500; ++frame)
            {
                control = circuit.advance(control, 1.0, 1.0 / rate);
                reference = advanceFineOracle(reference, 1.0, 1.0L / rate);
                maximumError = std::max(maximumError,
                    capacitorError(circuit, control, reference));
            }
        }
        std::cout << rate << "Hz substep cutoff: max C58 error "
                  << maximumError * 1e6 << "uV\n";
        require(maximumError < errorBudget(rate),
                "VCA substep cutoff exceeded its circuit error budget");
    }
}

void testFractionalCallbacks(const youknow::VcaControlCircuit& circuit)
{
    for (int rate : {8000, 48000, 192000})
    {
        double maximumError = 0;
        for (int slot = 0; slot < 6; ++slot)
        {
            for (double requestedPosition : {0.0, .001, .17, .5, .89, .999, 1.0})
            {
                auto engine = std::make_unique<youknow::YouKnowEngine>();
                engine->prepare(rate, 1, 1);
                youknow::EngineParameters parameters;
                parameters.calibration = 0;
                parameters.velocityDepth = 0;
                parameters.vcaMode = youknow::VcaMode::Envelope;
                parameters.enableCoupledVoiceVcaControl = true;
                engine->setParameters(parameters);
                const double position = youknow::YouKnowTestAccess::seedFractionalWrite(
                    *engine, slot, requestedPosition);
                const long double dt = 1.0L / rate;
                long double reference = advanceFineOracle(
                    equilibrium(.009), static_cast<long double>(.006f), dt * position);
                reference = advanceFineOracle(
                    reference, static_cast<long double>(.8f), dt * (1 - position));
                processOne(*engine);
                maximumError = std::max(maximumError, capacitorError(circuit,
                    youknow::YouKnowTestAccess::control(*engine, slot), reference));
                // The event belongs to exactly one physical card.
                const auto otherReference = advanceFineOracle(
                    equilibrium(.009), static_cast<long double>(.006f), dt);
                for (int other = 0; other < 6; ++other)
                    if (other != slot)
                        require(capacitorError(circuit,
                            youknow::YouKnowTestAccess::control(*engine, other),
                            otherReference) < 5e-6,
                            "fractional VCA write reached a different card");

                // A right-edge peek must commit its saved payload at the next
                // callback, including when the envelope has changed meanwhile.
                youknow::YouKnowTestAccess::changeEnvelope(*engine, slot);
                processOne(*engine);
                reference = advanceFineOracle(reference, static_cast<long double>(.8f), dt);
                require(youknow::YouKnowTestAccess::target(*engine, slot) == .8f,
                        "fractional VCA write did not commit its captured target");
                maximumError = std::max(maximumError, capacitorError(circuit,
                    youknow::YouKnowTestAccess::control(*engine, slot), reference));
            }
        }
        std::cout << rate << "Hz actual fractional callbacks: max C58 error "
                  << maximumError * 1e6 << "uV\n";
        require(maximumError < errorBudget(rate),
                "fractional engine VCA write misses continuous C58 KCL");
    }
}

void testInactiveCoupledMixer()
{
    auto engine = std::make_unique<youknow::YouKnowEngine>();
    // Synthetic, explicitly supplied circuit fixture; no claim that these
    // unmeasured mixer source parameters are hardware calibration readings.
    require(engine->configureCoupledMixer({10000, 47000, .5, 0, .6, .1}),
            "coupled mixer fixture rejected its valid calibration");
    engine->prepare(48000, 1, 1);
    youknow::EngineParameters parameters;
    parameters.calibration = 0;
    parameters.vcfTanhMode = youknow::VcfTanhMode::PolyZoned;
    parameters.enablePulseOffWaveNodeCoupling = false;
    engine->setParameters(parameters);
    youknow::YouKnowTestAccess::seedInactiveAudioCell(*engine);
    processOne(*engine);
    require(youknow::YouKnowTestAccess::inactiveAudioCellUpdated(*engine),
            "inactive coupled mixer card consumed stale VCA/filter/comparator coefficients");
}
}

int main()
{
    try
    {
        const auto& circuit = youknow::YouKnowTestAccess::productionCircuit();
        for (double level : {0.0, .004, .015, .02, .1, .5, 1.0})
        {
            require(circuit.advance(level, level, 1.0/48000) == level,
                    "settled control changed");
            const double voltage = static_cast<double>(standoff + span * circuit.capacitorCoordinate(level));
            require(std::abs(voltage - static_cast<double>(equilibrium(level))) < 5.0e-6,
                    "C58 equilibrium disagrees with the independent junction solve");
        }
        constexpr std::array targets { .02, .006, .0, .1, 1.0, .015, .0 };
        for (int rate : {8000, 44100, 48000, 192000, 768000})
        {
            const double dt = 1.0 / rate;
            double control = 0.0;
            long double reference = equilibrium(0);
            double maximumError = 0;
            double maximumHalfStepError = 0;
            for (double target : targets)
            {
                for (int frame = 0; frame < rate / 500; ++frame)
                {
                    const double half = circuit.advance(circuit.advance(control, target, dt/2), target, dt/2);
                    const double next = circuit.advance(control, target, dt);
                    require(next >= std::min(control,target) && next <= std::max(control,target),
                            "passive control overshot its target");
                    control = next;
                    for (int substep = 0; substep < 8; ++substep)
                        reference = advanceOracle(reference, target, static_cast<long double>(dt)/8);
                    const double voltage = static_cast<double>(standoff + span*circuit.capacitorCoordinate(control));
                    maximumError = std::max(maximumError, std::abs(voltage-static_cast<double>(reference)));
                    maximumHalfStepError = std::max(maximumHalfStepError,
                        std::abs(circuit.capacitorCoordinate(half)-circuit.capacitorCoordinate(control))*static_cast<double>(span));
                }
            }
            std::cout << rate << "Hz: max C58 error " << maximumError*1e6
                      << "uV, local half-step change " << maximumHalfStepError*1e6 << "uV\n";
            // Conservative temporal budget plus a 5uV interpolation allowance.
            // 48kHz acceptance is <90uV on a ~10V full-scale control rail;
            // the independent RK4 oracle has eight times finer steps.
            require(maximumError < errorBudget(rate),
                    "coupled VCA hold exceeded its circuit error budget");
        }
        testSubstepBoundary(circuit);
        testFractionalCallbacks(circuit);
        testInactiveCoupledMixer();
        for(double position:{0.0,.001,.17,.5,.89,.999,1.0})
        {
            const double dt=1.0/48000;
            double control=.009;
            long double reference=equilibrium(control);
            control=circuit.advance(control,.006,dt*position);
            control=circuit.advance(control,.8,dt*(1-position));
            for(int step=0;step<16;++step)
                reference=advanceOracle(reference,.006,dt*position/16);
            for(int step=0;step<16;++step)
                reference=advanceOracle(reference,.8,dt*(1-position)/16);
            require(std::abs(static_cast<double>(standoff+span*circuit.capacitorCoordinate(control)-reference))<90e-6,
                    "fractional converter write misses continuous C58 KCL");
        }
        for(bool coupled:{false,true})
        {
            auto engine=std::make_unique<youknow::YouKnowEngine>();
            engine->prepare(48000,1,1);
            youknow::EngineParameters parameters;
            parameters.enableCoupledVoiceVcaControl=coupled;
            parameters.calibration=0;
            engine->setParameters(parameters);
            youknow::YouKnowTestAccess::seedQuietHold(*engine);
            float left=0,right=0;
            engine->process(&left,&right,1);
            const double actual=youknow::YouKnowTestAccess::control(*engine);
            if(coupled)
            {
                long double node=equilibrium(.015);
                for(int step=0;step<16;++step)node=advanceOracle(node,0,1.0L/(48000*16));
                require(std::abs(static_cast<double>(standoff+span*circuit.capacitorCoordinate(actual)-node))<5e-6,
                        "engine callback does not consume the coupled C58 circuit");
            }
            else
                require(std::abs(actual-.015*std::exp(-1.0/(48000*static_cast<double>(687e-6f))))<1e-14,
                        "linear comparison no longer preserves its prior hold");
        }
        // This interval targets the missing finite transistor impedance.
        // A fixed 687us hold must disagree appreciably with the real node.
        double state=.015, old=.015;
        for(int frame=0;frame<48;++frame)
        {
            state=circuit.advance(state,0,1.0/48000);
            old*=std::exp(-1.0/(48000*.000687));
        }
        std::cout << "After 1ms quiet release: coupled " << state << ", fixed RC " << old << '\n';
        require(state > old*1.2, "oracle did not exercise transistor unloading");
        std::cout << "coupled voice-VCA circuit tests passed\n";
    }
    catch(const std::exception& error)
    {
        std::cerr<<error.what()<<'\n'; return 1;
    }
}
