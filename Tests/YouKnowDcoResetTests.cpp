// Explicit numerical reset fixtures, not measurements or factory defaults.
#include "DSP/YouKnowEngine.h"

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
    static void cell(YouKnowEngine& engine, double rate, float code = 256.0f,
                     bool periodic = false, std::uint32_t count = 5000)
    {
        engine.prepare(rate, 128, false);
        EngineParameters parameters;
        parameters.calibration = 0.0f;
        parameters.range = DcoRange::Eight;
        parameters.pulseEnabled = true;
        parameters.sawEnabled = true;
        engine.setParameters(parameters);
        engine.panelGlidePrimed_ = true;
        for (int slot = 0; slot < 6; ++slot)
        {
            auto& voice = engine.voices_[static_cast<std::size_t>(slot)];
            auto& d = voice.dco;
            d.reset();
            d.divider = d.pendingDivider = count;
            d.pitState = YouKnowEngine::Dco::PitState::running;
            d.pitOutHigh = false;
            d.pitClocksToEvent = periodic ? 0.0 : 5000000.0;
            d.periodSamples = rate * count / 2000000.0;
            voice.dcoCv = voice.dcoCvTarget = code;
            voice.rampCurrentScale = 1.0f;
            voice.rampServiceScale = 1.0f;
            d.renderScale = 1.0f;
            d.rampValue = 10.0 / 6.0 - 1.0;
            d.rampSlopePerSecond = 0.0;
            d.saw.prime(static_cast<float>(d.rampValue));
            d.pulse.prime(1.0f);
            d.pulseState = 1.0f;
            voice.pulseThresholdVolts = 6.0f;
        }
    }

    static void resetEdge(YouKnowEngine& engine, bool corrections = true)
    {
        engine.beginDcoDischarge(engine.voices_[0], 1.0f, corrections);
    }

    static std::array<double, 4> state(const YouKnowEngine& engine, int slot = 0)
    {
        const auto& v = engine.voices_[static_cast<std::size_t>(slot)];
        const auto& d = v.dco;
        const double scale = 6.0 * d.renderScale * v.rampCurrentScale;
        return { scale * (d.rampValue + 1.0), scale * d.rampSlopePerSecond,
                 d.resetSecondsRemaining, d.physicalResetActive ? 1.0 : 0.0 };
    }

    static void acquire(YouKnowEngine& engine, float code)
    {
        engine.updateDcoHeldCv(engine.voices_[0], code);
    }

    static void advance(YouKnowEngine& engine, bool corrections = true,
                        float from = 6.0f, float to = 6.0f, bool all = false)
    {
        for (int slot = 0; slot < (all ? 6 : 1); ++slot)
            engine.advanceDcoPitAndRamp(engine.voices_[static_cast<std::size_t>(slot)],
                engine.activeParameters_.range, from, to, from < 0.0f, to < 0.0f, corrections);
        engine.advanceRangeClock(engine.activeParameters_.range);
    }

    static bool pulse(const YouKnowEngine& engine)
    {
        return engine.voices_[0].dco.pulseState > 0.0f;
    }

    static double pulseMean(YouKnowEngine& engine)
    {
        return engine.steadyDcoPulseDuty(engine.voices_[0]);
    }

    static double sawMean(YouKnowEngine& engine)
    {
        return engine.steadyDcoSawMean(engine.voices_[0]);
    }

    static std::array<double, 48> correction(const YouKnowEngine& engine)
    {
        return engine.voices_[0].dco.resetSawCorrection;
    }

    static std::array<float, 3073> table()
    {
        return YouKnowEngine::correctionTables().slopeResidual;
    }

    static double pulseEventError(YouKnowEngine& engine, const std::array<double, 2>& roots)
    {
        YouKnowEngine::BandlimitedTrack expected;
        engine.addStep(expected, -2.0f, static_cast<float>(1.0 - roots[0] * 48000.0));
        engine.addStep(expected, 2.0f, static_cast<float>(1.0 - roots[1] * 48000.0));
        double error = 0.0;
        for (std::size_t slot = 0; slot < expected.ring.size(); ++slot)
            error = std::max(error, std::abs(static_cast<double>(expected.ring[slot])
                - engine.voices_[0].dco.pulse.ring[slot]));
        return error;
    }

    static void range(YouKnowEngine& engine, DcoRange range)
    {
        auto parameters = engine.activeParameters_;
        parameters.range = range;
        engine.setParameters(parameters);
    }

    static double couplingRippleBound(const YouKnowEngine& engine)
    {
        // A stable first-order state fed a zero-mean periodic waveform can
        // move no more than its input range times one period's pole loss.
        // Nominal fixture: <=15 V saw range +12 V pulse range, 400 Hz.
        const double g = engine.moduleCouplingG_;
        const double pole = (1.0 - g) / (1.0 + g);
        return 27.0 * (1.0 - std::pow(pole, 48000.0 / 400.0));
    }

    static void frontEnd(YouKnowEngine& engine, bool exact)
    {
        auto& voice = engine.voices_[0];
        engine.activeParameters_.vcfTanhMode = exact ? VcfTanhMode::Exact : VcfTanhMode::PolyZoned;
        engine.updatePulseComparator(voice, engine.activeParameters_);
        static_cast<void>(engine.prepareVoiceFilter(voice, engine.activeParameters_, 0.0f));
        engine.advanceRangeClock(DcoRange::Eight);
    }

    static double coupling(const YouKnowEngine& engine)
    {
        return engine.voices_[0].moduleCoupling.state;
    }
};
}

namespace
{
using youknow::DcoResetCircuit;
using youknow::YouKnowEngine;
using Probe = youknow::YouKnowTestAccess;
using Calibration = DcoResetCircuit::Calibration;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

double chargingSlope(double code)
{
    return (12.0 / (7675.0 / 2000000.0 - 2.2e-6)) * code / 256.0;
}

double integrateReset(double initial, double code, const Calibration& c, double seconds)
{
    // Independent RK4 of KCL using amperes and farads, not the production
    // exponential helper. The selected 1 nF capacitor is nominal here.
    const double current = chargingSlope(code) * 1e-9;
    const auto derivative = [&](double volts) {
        return (current - (volts - c.clampVolts) / c.dischargeOhms) / 1e-9;
    };
    const double dt = seconds / 4096.0;
    double voltage = initial;
    for (int step = 0; step < 4096; ++step)
    {
        const double k1 = derivative(voltage);
        const double k2 = derivative(voltage + 0.5 * dt * k1);
        const double k3 = derivative(voltage + 0.5 * dt * k2);
        const double k4 = derivative(voltage + dt * k3);
        voltage += dt * (k1 + 2.0 * k2 + 2.0 * k3 + k4) / 6.0;
    }
    return voltage;
}

void checkConfiguration()
{
    auto engine = std::make_unique<YouKnowEngine>();
    require(!engine->usesDcoResetCircuit(), "shipping selected an uncalibrated reset model");
    require(!engine->configureDcoResetCircuit({}), "missing reset calibration was accepted");
    for (const Calibration c : { Calibration { 0.0, 100.0, 0.0 },
            { 2e-3, 100.0, 0.0 }, { 1e-6, 0.0, 0.0 }, { 1e-6, 1e8, 0.0 },
            { 1e-6, 100.0, -0.1 }, { 1e-6, 100.0, 16.0 },
            { 1e-6, std::numeric_limits<double>::infinity(), 0.0 } })
        require(!engine->configureDcoResetCircuit(c) && !engine->usesDcoResetCircuit(),
                "invalid reset input mutated configuration");
    require(engine->configureDcoResetCircuit({ 3e-6, 1200.0, 0.02 }),
            "explicit valid reset circuit rejected");
    engine->prepare(48000.0, 64, false);
    require(!engine->configureDcoResetCircuit({ 2e-6, 1000.0, 0.03 }),
            "prepared engine allowed a live reset-circuit replacement");
    engine->reset();
    engine->prepare(44100.0, 64, false);
    require(engine->usesDcoResetCircuit(), "reset/prepare forgot explicit circuit configuration");
}

void checkRetainedCharge()
{
    double maximumError = 0.0;
    for (double rate : { 8000.0, 48000.0, 192000.0, 768000.0 })
    {
        for (const Calibration c : { Calibration { 3e-6, 1200.0, 0.02 },
                                     Calibration { 8e-6, 5000.0, 0.05 } })
        {
            auto engine = std::make_unique<YouKnowEngine>();
            require(engine->configureDcoResetCircuit(c), "reset fixture rejected");
            Probe::cell(*engine, rate);
            Probe::resetEdge(*engine);
            require(Probe::state(*engine)[0] == 10.0, "reset edge stepped capacitor voltage");
            double expected = 10.0;
            double remaining = c.gateSeconds;
            float code = 256.0f;
            for (int step = 0; step < 16 && remaining > 0.0; ++step)
            {
                if (step == 1)
                {
                    const double before = Probe::state(*engine)[0];
                    code = 512.0f;
                    Probe::acquire(*engine, code);
                    require(Probe::state(*engine)[0] == before,
                            "current change during reset stepped capacitor voltage");
                }
                const double dischargeTime = std::min(remaining, 1.0 / rate);
                expected = integrateReset(expected, code, c, dischargeTime);
                expected = std::min(15.0, expected + chargingSlope(code) * (1.0 / rate - dischargeTime));
                remaining = std::max(0.0, remaining - 1.0 / rate);
                Probe::advance(*engine);
                maximumError = std::max(maximumError, std::abs(Probe::state(*engine)[0] - expected));
                require(std::abs(Probe::state(*engine)[0] - expected) < 2e-10,
                        "physical reset disagrees with independent current/R/C integration");
            }
            require(Probe::state(*engine)[3] == 0.0 && Probe::state(*engine)[0] > 0.01,
                    "gate opening erased residual capacitor voltage");
        }
    }
    std::cout << "retained-reset maximum RK4-oracle error " << maximumError << " V\n";
}

double kernel(const std::array<float, 3073>& table, int slot, double age)
{
    const double x = (slot + age) * 64.0;
    const auto lower = static_cast<std::size_t>(std::min(3071.0, std::floor(x)));
    return table[lower] + (static_cast<double>(table[lower + 1]) - table[lower]) * (x - lower);
}

void checkCurvatureReconstruction()
{
    constexpr double rate = 48000.0;
    constexpr double duration = 1.0 / rate;
    const Calibration c { 4e-6, 1800.0, 0.03 };
    auto engine = std::make_unique<YouKnowEngine>();
    require(engine->configureDcoResetCircuit(c), "curvature fixture rejected");
    Probe::cell(*engine, rate);
    Probe::resetEdge(*engine);
    Probe::advance(*engine);
    const auto actual = Probe::correction(*engine);
    const auto table = Probe::table();
    const double tau = c.dischargeOhms * 1e-9;
    const double target = c.clampVolts + chargingSlope(256.0) * tau;
    const double derivative = (target - 10.0) / tau / 6.0;
    const double endDerivative = derivative * std::exp(-c.gateSeconds / tau);
    double error = 0.0;
    for (int slot = 0; slot < 48; ++slot)
    {
        double expected = derivative * duration * kernel(table, slot, 1.0);
        expected += (chargingSlope(256.0) / 6.0 - endDerivative) * duration
            * kernel(table, slot, 1.0 - c.gateSeconds / duration);
        // Independent dense midpoint quadrature, not the production centroid
        // rule. Includes distributed d²V/dt², which two corner BLAMPs omit.
        constexpr int divisions = 32768;
        const double dt = c.gateSeconds / divisions;
        for (int i = 0; i < divisions; ++i)
        {
            const double time = (i + 0.5) * dt;
            const double curvature = -derivative / tau * std::exp(-time / tau);
            expected += curvature * dt * duration * kernel(table, slot, 1.0 - time / duration);
        }
        error = std::max(error, std::abs(actual[static_cast<std::size_t>(slot)] - expected));
    }
    require(error < 2e-7, "reset reconstruction omitted or misplaced exponential curvature");
    std::cout << "curvature-kernel quadrature maximum residual error " << error << '\n';
}

void checkSteadyMeans()
{
    constexpr double rate = 768000.0;
    for (const Calibration c : { Calibration { 3.1e-6, 2200.0, 0.02 },
                                 Calibration { 0.5e-6, 100000.0, 0.0 } })
    {
        for (float code : { 256.0f, 768.0f })
        {
            auto engine = std::make_unique<YouKnowEngine>();
            require(engine->configureDcoResetCircuit(c)
                        && engine->configureDcoMasterClockHz(8800000.0), "steady fixture rejected");
            Probe::cell(*engine, rate, code, true);
            for (int i = 0; i < 76800; ++i)
                Probe::advance(*engine, false);
            double volts = 0.0;
            int high = 0;
            constexpr int samples = 76800;
            for (int i = 0; i < samples; ++i)
            {
                // Comparator logic is assessed directly from the same voltage
                // state; corrections are irrelevant to physical occupancy.
                Probe::advance(*engine, false);
                const double v = Probe::state(*engine)[0];
                require(v >= 0.0 && v <= 15.0000001 && std::isfinite(v),
                        "periodic physical reset escaped supply bounds");
                volts += v;
                high += v >= 6.0;
            }
            require(std::abs(Probe::pulseMean(*engine) - static_cast<double>(high) / samples)
                        < 2.0 * 440.0 / rate + 1e-6,
                    "physical reset PWM mean disagrees with sampled occupancy");
            // Uniform sampling error <= total variation*dt/period. This is a
            // mathematical quadrature bound, not a hardware tolerance.
            require(std::abs(Probe::sawMean(*engine) - (volts / samples - 6.0))
                        < 15.0 * 440.0 / rate + 1e-5,
                    "physical reset saw mean disagrees with integrated waveform");
        }
    }
}

void checkMovingComparatorAndRange()
{
    const Calibration c { 30e-6, 10000.0, 0.0 };
    auto engine = std::make_unique<YouKnowEngine>();
    require(engine->configureDcoResetCircuit(c), "moving comparator fixture rejected");
    Probe::cell(*engine, 48000.0);
    Probe::resetEdge(*engine);
    // An exponentially falling ramp can cross a linearly falling threshold
    // twice in one sample. Find both crossings by an independent fine grid
    // and secant interpolation, without the production stationary-point split.
    const double tau = c.dischargeOhms * 1e-9;
    const double target = chargingSlope(256.0) * tau;
    const auto difference = [&](double t) {
        return target + (10.0 - target) * std::exp(-t / tau)
            - (8.0 - 7.0 * t * 48000.0);
    };
    std::array<double, 2> roots {};
    int found = 0;
    constexpr int steps = 262144;
    double before = difference(0.0);
    for (int i = 1; i <= steps; ++i)
    {
        const double t = static_cast<double>(i) / (48000.0 * steps);
        const double after = difference(t);
        if ((before > 0.0) != (after > 0.0))
        {
            require(found < 2, "independent comparator fixture has too many roots");
            roots[static_cast<std::size_t>(found++)] = t
                - after / (after - before) / (48000.0 * steps);
        }
        before = after;
    }
    require(found == 2, "independent moving comparator fixture lacks two roots");
    Probe::advance(*engine, true, 8.0f, 1.0f);
    require(Probe::pulse(*engine) && Probe::pulseEventError(*engine, roots) < 2e-6,
            "moving comparator lost or mistimed a within-reset crossing");

    // Live PF selection changes current during the still-active gate. Voltage
    // and its original remaining gate time survive; the derivative obeys KCL.
    const auto previous = Probe::state(*engine);
    Probe::range(*engine, youknow::DcoRange::Four);
    const auto current = Probe::state(*engine);
    require(current[0] == previous[0] && current[2] == previous[2] && current[3] == 1.0,
            "range selection restarted or stepped a retained reset");
    const double expected = chargingSlope(512.0) - current[0] / tau;
    require(std::abs(current[1] - expected) < 1e-7,
            "range selection left the physical reset charging current unchanged");
}

void checkComparisonDomain()
{
    for (double rate : { 8000.0, 48000.0, 192000.0 })
    {
        for (const Calibration c : { Calibration { 1e-9, 1.0, 0.0 },
                { 1e-3, 1.0, 0.0 }, { 1e-9, 1e7, 15.0 }, { 1e-3, 1e7, 15.0 } })
        {
            for (float code : { 0.0f, 4095.0f })
            {
                auto engine = std::make_unique<YouKnowEngine>();
                require(engine->configureDcoResetCircuit(c), "comparison corner rejected");
                Probe::cell(*engine, rate, code, true, 8);
                for (int sample = 0; sample < 128; ++sample)
                {
                    Probe::advance(*engine, true, 6.0f, 6.0f, true);
                    for (int slot = 0; slot < 6; ++slot)
                    {
                        const auto state = Probe::state(*engine, slot);
                        require(std::isfinite(state[0]) && std::isfinite(state[1])
                                    && state[0] >= -1e-7 && state[0] <= 15.0000001,
                                "comparison domain escaped finite capacitor supply bounds");
                        require(state == Probe::state(*engine),
                                "simultaneous identical cards acquired independent clock/reset phases");
                    }
                }
                require(std::isfinite(Probe::sawMean(*engine))
                            && std::isfinite(Probe::pulseMean(*engine)),
                        "gate overlap or zero current made an invalid idle mean");
                for (double residual : Probe::correction(*engine))
                    require(std::isfinite(residual), "comparison corner poisoned reconstruction");
            }
        }
    }
}

std::vector<float> audio(bool physical, int block)
{
    auto engine = std::make_unique<YouKnowEngine>();
    if (physical)
        require(engine->configureDcoResetCircuit({ 3.1e-6, 2200.0, 0.02 }), "audio fixture rejected");
    engine->prepare(48000.0, 128, false);
    youknow::EngineParameters parameters;
    parameters.calibration = 0.0f;
    parameters.pulseEnabled = true;
    parameters.sawEnabled = true;
    parameters.dcoLfoDepth = 0.1f;
    engine->setParameters(parameters);
    for (int note : { 48, 52, 55, 60, 64, 67 })
        engine->noteOn(note, 1.0f);
    std::vector<float> result(4096), right(128);
    for (int offset = 0; offset < 4096;)
    {
        if (offset == 2048)
        {
            parameters.range = youknow::DcoRange::Four;
            engine->setParameters(parameters);
        }
        const int boundary = offset < 2048 ? 2048 : 4096;
        const int count = std::min(block, boundary - offset);
        engine->process(result.data() + offset, right.data(), count);
        offset += count;
    }
    return result;
}

void checkAudioAndFreewheel()
{
    const auto physical = audio(true, 127);
    require(physical == audio(true, 1), "physical-reset sound depends on host block partition");
    require(physical != audio(false, 127), "configured reset is disconnected from rendered sound");
    require(std::all_of(physical.begin(), physical.end(), [](float x) { return std::isfinite(x); }),
            "physical reset produced nonfinite audio");
    auto exact = std::make_unique<YouKnowEngine>();
    auto fast = std::make_unique<YouKnowEngine>();
    for (auto* engine : { exact.get(), fast.get() })
    {
        require(engine->configureDcoResetCircuit({ 3.1e-6, 2200.0, 0.02 }), "freewheel fixture rejected");
        Probe::cell(*engine, 48000.0, 256.0f, true);
    }
    for (int i = 0; i < 12000; ++i)
    {
        Probe::frontEnd(*exact, true);
        Probe::frontEnd(*fast, false);
        require(Probe::state(*exact) == Probe::state(*fast),
                "fast physical reset changed capacitor trajectory");
    }
    require(std::abs(Probe::coupling(*exact) - Probe::coupling(*fast))
                < Probe::couplingRippleBound(*fast),
            "freewheel discarded physical-reset WAVE-node charge");
    const double held = Probe::coupling(*fast);
    fast->noteOn(60, 1.0f);
    require(Probe::coupling(*fast) == held, "physical-reset resume cleared coupling charge");
}
}

int main()
{
    try
    {
        checkConfiguration();
        checkRetainedCharge();
        checkCurvatureReconstruction();
        checkSteadyMeans();
        checkMovingComparatorAndRange();
        checkComparisonDomain();
        checkAudioAndFreewheel();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
