// C54 range-switch qualification against a separate constant-current charge
// integral. The service-note p.9 topology and p.13 resistor values specify
// current ratios, not the custom IC's absolute gain or reset waveform:
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf
#include "DSP/YouKnowEngine.h"

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
    struct Ramp
    {
        double chargeCoordinate, slope, scale, resetTime;
        bool railHeld, pulseHigh;
    };

    static void configure(YouKnowEngine& engine, double rate, DcoRange range,
                          double slope = 500.0, bool held = false)
    {
        engine.prepare(rate, 64, false);
        EngineParameters p;
        p.range = range;
        p.calibration = 0.0f;
        engine.setParameters(p);
        engine.panelGlidePrimed_ = true;
        auto& voice = engine.voices_[0];
        auto& dco = voice.dco;
        dco.reset();
        dco.divider = 60000;
        dco.periodSamples = rate * 60000.0 / YouKnowEngine::rangeClockHz(range);
        dco.pitState = YouKnowEngine::Dco::PitState::running;
        dco.pitOutHigh = true;
        dco.pitClocksToEvent = 50000.25;
        dco.rampValue = -0.3;
        dco.rampSlopePerSecond = slope;
        dco.renderScale = 0.91f;
        dco.resetSecondsRemaining = slope < 0 ? 0.01 : 0.0;
        dco.positiveRailHeld = held;
        dco.pulseState = -1.0f;
        voice.rampCurrentScale = 1.0f;
    }

    static Ramp ramp(const YouKnowEngine& engine)
    {
        const auto& d = engine.voices_[0].dco;
        return { d.rampValue, d.rampSlopePerSecond, d.renderScale,
                 d.resetSecondsRemaining, d.positiveRailHeld, d.pulseState > 0 };
    }

    static void switchTo(YouKnowEngine& engine, DcoRange range)
    {
        auto p = engine.activeParameters_;
        p.range = range;
        engine.setParameters(p);
    }

    static void advance(YouKnowEngine& engine, DcoRange range, float threshold)
    {
        engine.advanceDcoPitAndRamp(engine.voices_[0], range,
            threshold, threshold, false, false, true);
        engine.advanceRangeClock(range);
    }

    static double launchScale(YouKnowEngine& engine, float code)
    {
        auto& voice = engine.voices_[0];
        voice.dcoPitchTransactionValid = false;
        voice.dcoCvTarget = code;
        return engine.dcoLaunchScale(voice);
    }
};
}

namespace
{
using youknow::DcoRange;
using youknow::YouKnowEngine;
using Probe = youknow::YouKnowTestAccess;

void require(bool pass, const char* message)
{
    if (!pass)
        throw std::runtime_error(message);
}

void checkSwitches()
{
    constexpr std::array ranges { DcoRange::Sixteen, DcoRange::Eight, DcoRange::Four };
    // Independent physical part values; do not obtain expected ratios from
    // a production helper. Capacitance is C54's printed .001 uF.
    constexpr std::array resistance { 399000.0, 200000.0, 100000.0 };
    constexpr double capacitance = 1e-9;
    int cases = 0;
    double maximumChargeError = 0;
    for (double rate : { 8000.0, 44100.0, 48000.0, 96000.0, 192000.0, 768000.0 })
    {
        for (std::size_t a = 0; a < ranges.size(); ++a)
        {
            for (std::size_t b = 0; b < ranges.size(); ++b)
            {
                if (a == b)
                    continue;
                auto engine = std::make_unique<YouKnowEngine>();
                Probe::configure(*engine, rate, ranges[a]);
                const auto before = Probe::ramp(*engine);
                const double voltsPerCoordinate = 6.0 * before.scale;
                const double sourceVolts = before.slope * voltsPerCoordinate
                    * capacitance * resistance[a];
                const double currentAfter = sourceVolts / resistance[b];
                const double expectedSlope = currentAfter / capacitance;
                Probe::switchTo(*engine, ranges[b]);
                const auto after = Probe::ramp(*engine);
                require(after.chargeCoordinate == before.chargeCoordinate
                        && after.scale == before.scale,
                        "RANGE stepped C54 charge or its coordinate scale");
                require(std::abs(after.slope * voltsPerCoordinate - expectedSlope) < 1e-9,
                        "RANGE did not change charging current at the PF write");

                // Put the threshold between old-current and new-current
                // endpoint predictions. The pulse must follow the new slope
                // during this same partial cycle, before any PIT edge/reset.
                const double voltageBefore = (before.chargeCoordinate + 1.0)
                                           * voltsPerCoordinate;
                const float threshold = static_cast<float>(voltageBefore
                    + 0.5 * (expectedSlope + before.slope * voltsPerCoordinate) / rate);
                Probe::advance(*engine, ranges[b], threshold);
                const auto advanced = Probe::ramp(*engine);
                const double voltageAfter = (advanced.chargeCoordinate + 1.0)
                                          * voltsPerCoordinate;
                const double expectedVoltage = voltageBefore + currentAfter / (capacitance * rate);
                maximumChargeError = std::max(maximumChargeError,
                                              std::abs(voltageAfter - expectedVoltage));
                require(std::abs(voltageAfter - expectedVoltage) < 1e-11,
                        "C54 integration differs from independent Q=Q0+I*dt");
                require(advanced.pulseHigh == (resistance[a] > resistance[b]),
                        "pulse comparator retained the old range current for a partial cycle");

                // A second selection before another audio sample cannot
                // accumulate a level step or leave a stale charging ratio.
                const double retainedCharge = advanced.chargeCoordinate;
                Probe::switchTo(*engine, ranges[a]);
                const auto restored = Probe::ramp(*engine);
                require(restored.chargeCoordinate == retainedCharge
                        && std::abs(restored.slope - before.slope) < 1e-10,
                        "rapid RANGE roundtrip changed charge or failed to restore current");

                for (double slope : { -500.0, 0.0 })
                {
                    Probe::configure(*engine, rate, ranges[a], slope, slope == 0.0);
                    const auto inactive = Probe::ramp(*engine);
                    Probe::switchTo(*engine, ranges[b]);
                    const auto switched = Probe::ramp(*engine);
                    require(switched.slope == inactive.slope
                            && switched.chargeCoordinate == inactive.chargeCoordinate
                            && switched.resetTime == inactive.resetTime
                            && switched.railHeld == inactive.railHeld,
                            "charging resistor altered discharge timing or the rail hold");
                }
                ++cases;
            }
        }
    }
    std::cout << cases << " range/rate cases; max charge-integral error "
              << maximumChargeError << " V\n";
}

void checkSteadyRangeHeight()
{
    for (float code : { 0.01f, 20.0f, 2000.0f })
    {
        std::array<double, 3> scales {};
        constexpr std::array ranges { DcoRange::Sixteen, DcoRange::Eight, DcoRange::Four };
        for (std::size_t i = 0; i < ranges.size(); ++i)
        {
            auto engine = std::make_unique<YouKnowEngine>();
            Probe::configure(*engine, 48000.0, ranges[i]);
            scales[i] = Probe::launchScale(*engine, code);
        }
        require(std::abs(scales[0] / scales[1] - 400.0 / 399.0) < 2e-7,
                "16-foot launch height omits printed 399k charging resistor");
        require(scales[1] == scales[2], "8/4-foot RC-clock products disagree");
    }
}
}

int main()
{
    try
    {
        checkSwitches();
        checkSteadyRangeHeight();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
