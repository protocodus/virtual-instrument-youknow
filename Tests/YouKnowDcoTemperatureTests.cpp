// The named CSA8.00MTZ curve is a component proxy, not measured KMFC drift.
// Exercise its published coordinates, reference normalization, and the shared
// thermal/clock boundary without treating temperature as six detune controls.
#include "DSP/YouKnowDcoTemperature.h"
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace youknow
{
struct YouKnowTestAccess
{
    static double warmupSeconds(const YouKnowEngine& engine)
    {
        return engine.thermalWarmupSeconds_;
    }

    static double warmupFraction(const YouKnowEngine& engine)
    {
        return engine.thermalWarmupFraction_;
    }

    static void advanceWarmup(YouKnowEngine& engine, double seconds)
    {
        const auto frames = static_cast<std::uint64_t>(
            std::llround(seconds * engine.oversampledRate_));
        for (std::uint64_t frame = 0; frame < frames; ++frame)
            engine.advanceThermalWarmup();
        engine.refreshDcoMasterClock();
    }

    static void setWarmupFraction(YouKnowEngine& engine, float fraction)
    {
        engine.thermalWarmupFraction_ = fraction;
        engine.refreshDcoMasterClock();
    }

    static bool switchQualityAtMutedBoundary(YouKnowEngine& engine, int factor)
    {
        // Enter the actual rebuild at its supported zero-gain boundary;
        // reaching that boundary through the safety fade is covered by the
        // engine suite and would add elapsed audio time to this comparison.
        engine.oversamplingRequested_ = factor;
        engine.oversamplingIdleSamples_ = engine.oversamplingQuietSamples_;
        engine.rateTransition_ = YouKnowEngine::RateTransition::FadingOut;
        engine.rateTransitionGain_ = 0.0f;
        return engine.applyPendingOversamplingIfIdle();
    }

    static void seedPhysicalStates(YouKnowEngine& engine)
    {
        engine.rangeClockClocksToFallingEdge_ = 0.375;
        engine.rangeClockClocksToReload_ = 1.625;
        engine.rangeClockTransitionPending_ = true;
        for (int card = 0; card < 6; ++card)
        {
            auto& voice = engine.voices_[static_cast<std::size_t>(card)];
            auto& dco = voice.dco;
            dco.divider = static_cast<std::uint32_t>(4545 + card);
            dco.pendingDivider = static_cast<std::uint32_t>(4500 + card);
            dco.pendingDividerValid = true;
            dco.periodSamples = (4545.0 + card)
                              / YouKnowEngine::rangeClockHz(DcoRange::Eight)
                              * engine.oversampledRate_;
            dco.pitState = YouKnowEngine::Dco::PitState::running;
            dco.pitClocksToEvent = 317.25 + card;
            dco.pitOutHigh = card % 2 == 0;
            dco.pitWriteState = YouKnowEngine::Dco::PitWriteState::awaitingMsb;
            dco.cpuStatesToWrite = 3.125 + card;
            dco.rampValue = -0.75 + 0.25 * card;
            // Include charging, active finite reset and positive rail hold.
            dco.rampSlopePerSecond = card % 3 == 0 ? 923.5
                                    : card % 3 == 1 ? -123456.75 : 0.0;
            dco.resetSecondsRemaining = card % 3 == 1 ? 3.5e-6 : 0.0;
            dco.positiveRailHeld = card % 3 == 2;
            dco.renderScale = 0.9f + 0.01f * card;
            dco.subState = card % 2 == 0 ? 1.0f : -1.0f;
            dco.pulseState = card % 2 == 0 ? -1.0f : 1.0f;
            voice.dcoCv = 0.5f + 0.01f * card;
            voice.dcoCvTarget = 0.6f + 0.01f * card;
            voice.rampCurrentScale = 1.0f + 0.01f * card;
        }
    }

    static std::vector<double> physicalState(const YouKnowEngine& engine)
    {
        std::vector<double> result {
            engine.rangeClockClocksToFallingEdge_,
            engine.rangeClockClocksToReload_,
            static_cast<double>(engine.rangeClockTransitionPending_),
            engine.controlScanPhase_ };
        for (int card = 0; card < 6; ++card)
        {
            const auto& voice = engine.voices_[static_cast<std::size_t>(card)];
            const auto& dco = voice.dco;
            result.insert(result.end(), {
                static_cast<double>(dco.divider),
                static_cast<double>(dco.pendingDivider),
                static_cast<double>(dco.pendingDividerValid),
                dco.periodSamples, static_cast<double>(dco.pitState),
                dco.pitClocksToEvent, static_cast<double>(dco.pitOutHigh),
                static_cast<double>(dco.pitWriteState), dco.cpuStatesToWrite,
                dco.rampValue, dco.rampSlopePerSecond,
                dco.resetSecondsRemaining,
                static_cast<double>(dco.positiveRailHeld), dco.renderScale,
                dco.subState, dco.pulseState, voice.dcoCv, voice.dcoCvTarget,
                voice.rampCurrentScale });
        }
        return result;
    }
};
}

namespace
{
using youknow::Csa8MtzTemperatureProxy;
using youknow::EngineParameters;
using youknow::YouKnowEngine;
using Probe = youknow::YouKnowTestAccess;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void near(double actual, double expected, double tolerance, const char* message)
{
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance)
    {
        std::cerr << message << ": actual=" << actual
                  << ", expected=" << expected << '\n';
        throw std::runtime_error(message);
    }
}

EngineParameters parameters(float character = 1.0f)
{
    EngineParameters result;
    result.calibration = character;
    result.aging = 0.0f;
    result.pulseEnabled = true;
    result.vcfTanhMode = youknow::VcfTanhMode::PolyZoned;
    result.vcfFastEarlyMode = youknow::VcfFastEarlyMode::Cubic;
    result.vcfSolverMode = youknow::VcfSolverMode::Rk4Single;
    return result;
}

std::unique_ptr<YouKnowEngine> makeEngine(double rate = 48000.0, int factor = 1,
                                        bool settled = false,
                                        double reference = 25.0,
                                        float character = 1.0f)
{
    auto engine = std::make_unique<YouKnowEngine>();
    require(engine->configureDcoTemperatureProxy(true, reference),
            "proxy configuration failed");
    require(engine->configureThermalStart(settled), "thermal start failed");
    engine->setParameters(parameters(character));
    engine->prepare(rate, 128, factor);
    return engine;
}

void checkPublishedCurveAndReference()
{
    // Independently read ordinate: +0.0629 percent at 40 C in Murata P16E-9,
    // printed p.10. At 25 C, interpolate a quarter of the 20..40 C segment:
    // -0.0152 + (0.0629 + 0.0152)/4 = +0.004325 percent.
    near(Csa8MtzTemperatureProxy::frequencyFactor(40.0), 1.000629,
         1.0e-14, "40 C source ordinate changed");
    near(Csa8MtzTemperatureProxy::frequencyFactor(25.0), 1.00004325,
         1.0e-14, "25 C source interpolation changed");
    near(Csa8MtzTemperatureProxy::frequencyRatio(40.0, 25.0),
         1.0005857246674, 5.0e-14, "40/25 C normalized ratio changed");
    near(Csa8MtzTemperatureProxy::frequencyRatio(25.0, 40.0),
         1.00004325 / 1.000629, 1.0e-14,
         "reference temperature did not normalize the shape");
    for (double temperature : { -20.0, 0.0, 25.0, 40.0, 80.0 })
        near(Csa8MtzTemperatureProxy::frequencyRatio(temperature, temperature),
             1.0, 0.0, "frequency differs at its own reference temperature");

    auto atForty = makeEngine(48000.0, 1, true, 40.0);
    near(atForty->dcoMasterClockHz(), 8000000.0, 1.0e-8,
         "reference Hz was not anchored at the declared 40 C");
    auto coldAtForty = makeEngine(48000.0, 1, false, 40.0);
    near(coldAtForty->dcoMasterClockHz(),
         8000000.0 * 1.00004325 / 1.000629, 1.0e-8,
         "cold frequency ignored its nonambient reference");
}

void checkConfigurationAndDefaults()
{
    auto engine = std::make_unique<YouKnowEngine>();
    for (double invalid : { -20.0001, 80.0001,
                            std::numeric_limits<double>::infinity(),
                            -std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::quiet_NaN() })
    {
        require(!engine->configureDcoTemperatureProxy(true, invalid),
                "invalid proxy reference was accepted");
        near(engine->dcoMasterClockHz(), 8000000.0, 0.0,
             "rejected configuration changed the clock");
    }
    for (double boundary : { -20.0, 80.0, 25.0 })
        require(engine->configureDcoTemperatureProxy(true, boundary),
                "valid proxy reference boundary was rejected");
    require(engine->configureDcoMasterClockHz(8012345.0),
            "valid calibrated reference clock was rejected");
    require(engine->configureThermalStart(true), "settled start rejected");
    engine->setParameters(parameters());
    engine->prepare(48000.0, 128, false);
    const double retained = engine->dcoMasterClockHz();
    require(!engine->configureDcoTemperatureProxy(false, 25.0),
            "proxy configuration changed a prepared engine");
    require(!engine->configureThermalStart(false),
            "thermal configuration changed a prepared engine");
    require(!engine->configureDcoMasterClockHz(8000000.0),
            "reference clock changed a prepared engine");
    near(engine->dcoMasterClockHz(), retained, 0.0,
         "rejected live configuration changed the actual clock");
    near(retained, 8012345.0 * 1.000629 / 1.00004325, 1.0e-8,
         "configured Hz was not interpreted at the reference temperature");
    engine->reset();
    near(engine->dcoMasterClockHz(), retained, 0.0,
         "reset discarded the configured thermal start or reference clock");
    engine->prepare(96000.0, 64, 2);
    near(engine->dcoMasterClockHz(), retained, 0.0,
         "prepare discarded the configured thermal start or reference clock");

    auto raw = std::make_unique<YouKnowEngine>();
    raw->prepare(48000.0, 128, false);
    raw->setParameters(parameters());
    Probe::setWarmupFraction(*raw, 1.0f);
    near(raw->dcoMasterClockHz(), 8000000.0, 0.0,
         "raw engine silently enabled the component proxy");

    auto product = std::make_unique<YouKnowEngine>();
    youknow::ProductFidelityProfile::configureBeforePrepare(*product);
    product->prepare(48000.0, 128, false);
    product->setParameters(parameters());
    Probe::setWarmupFraction(*product, 1.0f);
    near(product->dcoMasterClockHz(), 8000000.0 * 1.000629 / 1.00004325,
         1.0e-8, "product profile did not enable the named proxy at 25 C");
}

void checkThermalTrajectory()
{
    near(YouKnowEngine::thermalWarmupTimeConstantSeconds, 3.0, 0.0,
         "user-selected startup time constant changed");
    auto engine = makeEngine();
    near(engine->getDisplayTemperatureC(), 25.0, 0.0,
         "cold engine did not start at ambient temperature");
    for (int multiple : { 1, 3 })
    {
        Probe::advanceWarmup(*engine, multiple == 1 ? 3.0 : 6.0);
        const double expected = 1.0 - std::exp(-static_cast<double>(multiple));
        near(Probe::warmupSeconds(*engine), 3.0 * multiple, 1.0e-6,
             "warmup elapsed time changed");
        near(Probe::warmupFraction(*engine), expected, 1.0e-7,
             "warmup did not reach the requested exponential fraction");
        near(engine->getDisplayTemperatureC(), 25.0 + 15.0 * expected,
             3.0e-6, "display and shared chassis temperature diverged");
        near(engine->dcoMasterClockHz(), 8000000.0
                 * Csa8MtzTemperatureProxy::frequencyRatio(
                     engine->getDisplayTemperatureC(), 25.0),
             1.0e-8, "clock did not follow the displayed shared temperature");
    }

    auto settled = makeEngine(48000.0, 1, true);
    near(settled->getDisplayTemperatureC(), 40.0, 0.0,
         "settled startup did not initialize the common temperature");
    const double settledClock = settled->dcoMasterClockHz();
    Probe::advanceWarmup(*settled, 9.0);
    near(settled->getDisplayTemperatureC(), 40.0, 0.0,
         "settled startup heated a second time");
    near(settled->dcoMasterClockHz(), settledClock, 0.0,
         "settled startup changed the master clock");

    auto neutral = makeEngine(48000.0, 1, false, 25.0, 0.0f);
    Probe::advanceWarmup(*neutral, 9.0);
    near(neutral->getDisplayTemperatureC(), 25.0, 0.0,
         "Unit Character zero heated the displayed chassis");
    near(neutral->dcoMasterClockHz(), 8000000.0, 0.0,
         "Unit Character zero generated pitch drift");
}

void checkPhysicalContinuity()
{
    auto engine = makeEngine();
    Probe::seedPhysicalStates(*engine);
    const auto initial = Probe::physicalState(*engine);
    for (float fraction : { 0.25f, 1.0f, 0.5f, 0.0f })
    {
        Probe::setWarmupFraction(*engine, fraction);
        require(Probe::physicalState(*engine) == initial,
                "temperature refresh reset PIT phase, C54 charge/current, or CPU state");
        near(engine->dcoMasterClockHz(), 8000000.0
                 * Csa8MtzTemperatureProxy::frequencyRatio(
                     25.0 + 15.0 * fraction, 25.0),
             1.0e-8, "refresh did not update the one shared master frequency");
    }
}

void checkRateAndQualityInvariance()
{
    double referenceClock = 0.0;
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        for (int factor : { 1, 2, 4 })
        {
            auto engine = makeEngine(rate, factor);
            Probe::advanceWarmup(*engine, 3.0);
            near(Probe::warmupSeconds(*engine), 3.0, 1.0e-6,
                 "rate or quality changed physical warmup time");
            near(Probe::warmupFraction(*engine), 1.0 - std::exp(-1.0),
                 1.0e-7, "rate or quality changed warmup fraction");
            if (referenceClock == 0.0)
                referenceClock = engine->dcoMasterClockHz();
            near(engine->dcoMasterClockHz(), referenceClock, 0.01,
                 "rate or quality changed the temperature-derived clock");
        }
    }

    auto changing = makeEngine();
    Probe::advanceWarmup(*changing, 1.0);
    const double seconds = Probe::warmupSeconds(*changing);
    const double clock = changing->dcoMasterClockHz();
    require(Probe::switchQualityAtMutedBoundary(*changing, 4),
            "quality change at its muted boundary was deferred");
    near(Probe::warmupSeconds(*changing), seconds, 0.0,
         "live quality change reset chassis elapsed time");
    near(changing->dcoMasterClockHz(), clock, 0.0,
         "live quality change reset the shared master clock");
    Probe::advanceWarmup(*changing, 2.0);
    near(Probe::warmupFraction(*changing), 1.0 - std::exp(-1.0), 1.0e-7,
         "quality change changed the three-second trajectory");
}

void checkAudioBlockAndHostStop()
{
    constexpr int samples = 6144;
    auto reference = makeEngine();
    auto partitioned = makeEngine();
    reference->noteOn(60, 1.0f);
    partitioned->noteOn(60, 1.0f);
    std::array<float, samples> referenceLeft {}, referenceRight {};
    std::array<float, samples> splitLeft {}, splitRight {};
    const auto render = [](YouKnowEngine& engine, float* left, float* right,
                           int block)
    {
        for (int offset = 0; offset < samples; offset += block)
            engine.process(left + offset, right + offset,
                           std::min(block, samples - offset));
    };
    render(*reference, referenceLeft.data(), referenceRight.data(), 128);
    render(*partitioned, splitLeft.data(), splitRight.data(), 17);
    require(referenceLeft == splitLeft && referenceRight == splitRight,
            "thermal coupling depends on host block boundaries");
    require(Probe::physicalState(*reference) == Probe::physicalState(*partitioned),
            "host block partition changed clock or analog ramp state");
    require(std::all_of(referenceLeft.begin(), referenceLeft.end(),
                       [](float sample) { return std::isfinite(sample); }),
            "temperature-coupled render produced nonfinite audio");
    near(Probe::warmupSeconds(*reference), samples / 48000.0, 1.0e-8,
         "audio rendering did not advance shared warmup once per internal frame");
    const double elapsed = Probe::warmupSeconds(*reference);
    const double fraction = Probe::warmupFraction(*reference);
    const double temperature = reference->getDisplayTemperatureC();
    const double clock = reference->dcoMasterClockHz();
    require(clock > 8000000.0, "audio rendering did not update the thermal clock");
    reference->resetForHostStop();
    near(Probe::warmupSeconds(*reference), elapsed, 0.0,
         "host stop power-cycled the thermal model");
    near(Probe::warmupFraction(*reference), fraction, 0.0,
         "host stop discarded the shared thermal fraction");
    near(reference->getDisplayTemperatureC(), temperature, 0.0,
         "host stop changed displayed temperature");
    near(reference->dcoMasterClockHz(), clock, 0.0,
         "host stop left the clock at the cold startup frequency");
    reference->reset();
    near(reference->getDisplayTemperatureC(), 25.0, 0.0,
         "explicit cold reset retained the previous temperature");
    near(reference->dcoMasterClockHz(), 8000000.0, 0.0,
         "explicit cold reset retained the previous clock frequency");
}
}

int main()
{
    try
    {
        checkPublishedCurveAndReference();
        checkConfigurationAndDefaults();
        checkThermalTrajectory();
        checkPhysicalContinuity();
        checkRateAndQualityInvariance();
        checkAudioBlockAndHostStop();
        std::cout << "DCO temperature proxy checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
