// A configured, already-settled master clock changes elapsed PIT time. It
// does not change the B-2 pitch words or the current charging C54. Qualification
// below observes the event walker and integrates its physical waveform; the
// clock limits used here are an engineering test domain, not measured drift.
#include "DSP/YouKnowEngine.h"

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
    struct Frame
    {
        double volts, slopeVolts, clocksRemaining, resetRemaining;
        double nominalPeriod, scale, cpuStates;
        std::uint32_t count, pendingCount;
        float code;
        bool subHigh, pulseHigh, railHeld;
    };

    static void configureCell(YouKnowEngine& engine, double rate,
                              DcoRange range, std::uint32_t count,
                              float amplitude = 1.0f, float threshold = 6.0f)
    {
        engine.prepare(rate, 128, false);
        EngineParameters parameters;
        parameters.range = range;
        parameters.calibration = 0.0f;
        parameters.aging = 0.0f;
        parameters.pulseEnabled = true;
        parameters.sawEnabled = true;
        parameters.subLevel = 0.0f;
        engine.setParameters(parameters);
        engine.panelGlidePrimed_ = true;
        const double nominalClock = YouKnowEngine::rangeClockHz(range);
        const double nominalPeriod = count / nominalClock;
        const double reset = YouKnowEngine::resetFraction(nominalPeriod)
                           * nominalPeriod;
        const double actualClock = engine.actualRangeClockHz(range);
        engine.rangeClockClocksToFallingEdge_ =
            1.0 - std::fmod(reset * actualClock, 1.0);
        engine.rangeClockTransitionPending_ = false;
        engine.rangeClockClocksToReload_ = 0.0;
        for (int card = 0; card < 6; ++card)
        {
            auto& voice = engine.voices_[static_cast<std::size_t>(card)];
            auto& dco = voice.dco;
            dco.reset();
            dco.divider = count;
            dco.pendingDivider = count;
            dco.periodSamples = nominalPeriod * rate;
            dco.pitState = YouKnowEngine::Dco::PitState::running;
            dco.pitOutHigh = true;
            dco.pitClocksToEvent = (count + 1u) / 2u - reset * actualClock;
            voice.dcoPitchTransactionValid = false;
            // Arrange an amplitude through the real CV/count path. Expected
            // slope/peak values below are observed or integrated independently.
            voice.dcoCvTarget = amplitude * YouKnowEngine::dcoRampReferenceProduct
                / static_cast<float>(count);
            voice.dcoCv = voice.dcoCvTarget;
            voice.rampCurrentScale = 1.0f;
            voice.pulseThresholdVolts = threshold;
            voice.pulsePinnedHigh = threshold < 0.0f;
            engine.beginDcoCharge(voice, 0.0f, false);
            dco.pulseState = threshold <= 0.0f ? 1.0f : -1.0f;
        }
    }

    static Frame frame(const YouKnowEngine& engine, int card = 0)
    {
        const auto& voice = engine.voices_[static_cast<std::size_t>(card)];
        const auto& dco = voice.dco;
        const double voltsPerCoordinate = 6.0 * dco.renderScale
                                       * voice.rampCurrentScale;
        return { voltsPerCoordinate * (dco.rampValue + 1.0),
                 voltsPerCoordinate * dco.rampSlopePerSecond,
                 dco.pitClocksToEvent, dco.resetSecondsRemaining,
                 dco.periodSamples / engine.oversampledRate_, dco.renderScale,
                 dco.cpuStatesToWrite, dco.divider, dco.pendingDivider,
                 voice.dcoCvTarget, dco.subState > 0.0f,
                 dco.pulseState > 0.0f, dco.positiveRailHeld };
    }

    static void advance(YouKnowEngine& engine, bool allCards = false)
    {
        for (int card = 0; card < (allCards ? 6 : 1); ++card)
        {
            auto& voice = engine.voices_[static_cast<std::size_t>(card)];
            engine.advanceDcoPitAndRamp(voice, engine.activeParameters_.range,
                voice.pulseThresholdVolts, voice.pulseThresholdVolts,
                voice.pulsePinnedHigh, voice.pulsePinnedHigh, true);
        }
        engine.advanceRangeClock(engine.activeParameters_.range);
    }

    static double duty(const YouKnowEngine& engine)
    {
        return engine.steadyDcoPulseDuty(engine.voices_[0]);
    }

    static double sawMean(const YouKnowEngine& engine)
    {
        return engine.steadyDcoSawMean(engine.voices_[0]);
    }

    static double renderedSaw(const YouKnowEngine& engine)
    {
        const auto& voice = engine.voices_[0];
        // The rendered saw subtracts the nominal 6 V midpoint from the
        // physical 0..12 V coordinate; changing its peak must retain that DC.
        return 6.0 * voice.rampCurrentScale
             * (voice.dco.renderScale * (voice.dco.rampValue + 1.0) - 1.0);
    }

    static void stageCpuWrite(YouKnowEngine& engine)
    {
        auto& dco = engine.voices_[0].dco;
        dco.pitWriteState = YouKnowEngine::Dco::PitWriteState::awaitingLsb;
        dco.cpuStatesToWrite = 1000.0;
    }

    static void advanceFrontEnd(YouKnowEngine& engine, bool exact)
    {
        auto& voice = engine.voices_[0];
        engine.activeParameters_.vcfTanhMode = exact
            ? VcfTanhMode::Exact : VcfTanhMode::PolyZoned;
        voice.pulseDuty = engine.steadyDcoPulseDuty(voice);
        static_cast<void>(engine.prepareVoiceFilter(
            voice, engine.activeParameters_, 0.0f));
        engine.advanceRangeClock(engine.activeParameters_.range);
    }

    static double couplingState(const YouKnowEngine& engine)
    {
        return engine.voices_[0].moduleCoupling.state;
    }

    static double sharedClockPhase(const YouKnowEngine& engine)
    {
        return engine.rangeClockClocksToFallingEdge_;
    }

    static std::vector<double> firmwareState(const YouKnowEngine& engine)
    {
        std::vector<double> result {
            engine.controlScanPhase_, static_cast<double>(engine.nextConverterWrite_),
            static_cast<double>(engine.lfoAccumulator_), engine.lfoValue_,
            static_cast<double>(engine.converterPassPwmDacCode_) };
        for (int card = 0; card < 6; ++card)
        {
            const auto& voice = engine.voices_[static_cast<std::size_t>(card)];
            result.insert(result.end(), {
                static_cast<double>(voice.dco.pitWriteDivider), voice.dcoCvTarget,
                voice.dcoPitchTransactionCvTarget, voice.currentMidi,
                static_cast<double>(voice.envelope.level),
                voice.dco.cpuStatesToWrite });
        }
        return result;
    }
};
}

namespace
{
using youknow::DcoRange;
using youknow::YouKnowEngine;
using Probe = youknow::YouKnowTestAccess;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void checkConfiguration()
{
    auto engine = std::make_unique<YouKnowEngine>();
    require(engine->dcoMasterClockHz() == 8000000.0,
            "default DCO clock is not nominal 8 MHz");
    for (double invalid : { 0.0, -1.0, 7199999.0, 8800001.0,
                           std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN() })
    {
        require(!engine->configureDcoMasterClockHz(invalid),
                "invalid DCO clock was accepted");
        require(engine->dcoMasterClockHz() == 8000000.0,
                "rejected DCO clock changed configuration");
    }
    for (double accepted : { 7200000.0, 8000000.0, 8800000.0 })
        require(engine->configureDcoMasterClockHz(accepted)
                    && std::abs(engine->dcoMasterClockHz() - accepted) < 1e-8,
                "bounded configured DCO clock was not retained");
    engine->prepare(48000.0, 128, false);
    require(!engine->configureDcoMasterClockHz(8000000.0)
                && engine->dcoMasterClockHz() == 8800000.0,
            "prepared engine allowed a live master-clock edit");
    engine->reset();
    engine->prepare(44100.0, 32, false);
    require(engine->dcoMasterClockHz() == 8800000.0,
            "reset/prepare discarded the configured physical clock");
}

struct Measurement
{
    double slope, peak, duty, sawMean, period, reset;
};

double risingArea(double startVolts, double slope, double duration)
{
    if (!(slope > 0.0))
        return startVolts * duration;
    const double chargeSeconds = std::clamp((15.0 - startVolts) / slope,
                                          0.0, duration);
    return startVolts * chargeSeconds
         + 0.5 * slope * chargeSeconds * chargeSeconds
         + 15.0 * (duration - chargeSeconds);
}

Measurement measure(double clock, DcoRange range, double nominalFrequency,
                    float amplitude = 1.0f, float threshold = 6.0f)
{
    constexpr double rate = 768000.0;
    const double divisor = range == DcoRange::Sixteen ? 8.0
                         : range == DcoRange::Four ? 2.0 : 4.0;
    const auto count = static_cast<std::uint32_t>(
        std::llround(8000000.0 / (divisor * nominalFrequency)));
    const double actualPitClock = clock / divisor;
    auto engine = std::make_unique<YouKnowEngine>();
    require(engine->configureDcoMasterClockHz(clock), "clock fixture rejected");
    Probe::configureCell(*engine, rate, range, count, amplitude, threshold);
    const auto initial = Probe::frame(*engine);
    Measurement measured { initial.slopeVolts, 0.0, 0.0, 0.0, 0.0, 0.0 };
    // A 100 ms aperture contains an integer number of periods at every
    // 200/400 Hz x .9/1/1.1 test point. Occupancy and waveform sums thus do
    // not depend on arbitrary start phase. Bounds include one grid interval
    // per comparator edge rather than asserting sample-exact analogue edges.
    for (int sample = 0; sample < 7680; ++sample)
        Probe::advance(*engine);
    double firstEdge = 0.0;
    double lastEdge = 0.0;
    int edges = 0;
    constexpr int samples = 76800;
    for (int sample = 0; sample < samples; ++sample)
    {
        const auto before = Probe::frame(*engine);
        Probe::advance(*engine);
        const auto after = Probe::frame(*engine);
        require(std::isfinite(after.volts) && after.volts >= -1e-10
                    && after.volts <= 15.000001,
                "clock event walk left the physical ramp bounds");
        require(after.count == count && after.code == initial.code,
                "actual clock rewrote firmware divider or ramp-current CV");
        measured.duty += after.pulseHigh ? 1.0 : 0.0;
        // Integrate the observed piecewise-linear waveform, splitting the
        // quadrature at actual corners. The 2.2 us fall spans fewer than two
        // grid intervals, so an unsplit endpoint sum has avoidable DC error.
        double voltageArea = 0.5 * (before.volts + after.volts) / rate;
        if (after.subHigh != before.subHigh)
        {
            const double age = ((count + 1u) / 2u - after.clocksRemaining)
                             / actualPitClock;
            const double timeBeforeEdge = 1.0 / rate - age;
            const double peak = after.volts - after.slopeVolts * age;
            voltageArea = risingArea(before.volts, before.slopeVolts, timeBeforeEdge)
                        + 0.5 * (peak + after.volts) * age;
        }
        else if (before.resetRemaining > 0.0
                 && before.resetRemaining <= 1.0 / rate)
        {
            voltageArea = 0.5 * before.volts * before.resetRemaining
                        + 0.5 * after.volts * (1.0 / rate - before.resetRemaining);
        }
        else if (after.railHeld && !before.railHeld)
            voltageArea = risingArea(before.volts, before.slopeVolts, 1.0 / rate);
        measured.sawMean += voltageArea * rate
                         + Probe::renderedSaw(*engine) - after.volts;
        measured.peak = std::max(measured.peak, after.volts);
        if (after.subHigh != before.subHigh)
        {
            // At these rates the first sample after OUT rises is still in
            // the 2.2 us discharge. Recover its edge timestamp and starting
            // voltage from the remaining timer count and observed slope.
            const double age = ((count + 1u) / 2u - after.clocksRemaining)
                             / actualPitClock;
            const double edge = (sample + 1.0) / rate - age;
            if (edges == 0)
                firstEdge = edge;
            lastEdge = edge;
            ++edges;
            require(after.resetRemaining > 0.0,
                    "measurement grid missed the finite reset");
            measured.reset = after.resetRemaining + age;
            measured.peak = std::max(measured.peak,
                                    after.volts - after.slopeVolts * age);
        }
    }
    measured.duty /= samples;
    measured.sawMean /= samples;
    require(edges > 10, "clock observation lacks complete periods");
    measured.period = (lastEdge - firstEdge) / (edges - 1);
    const double actualFrequency = actualPitClock / count;
    require(std::abs(measured.period - 1.0 / actualFrequency) < 2e-12,
            "PIT OUT period does not follow configured clock/count");
    require(std::abs(Probe::duty(*engine) - measured.duty)
                <= 2.1 * actualFrequency / rate + 2e-6,
            "steady PWM mean disagrees with observed comparator occupancy");
    // Piecewise trapezoids integrate each linear section exactly. The helper
    // returns float, so retain only its rounding allowance in this DC gate.
    require(std::abs(Probe::sawMean(*engine) - measured.sawMean) < 2e-6,
            "steady saw mean disagrees with integrated rendered ramp");
    return measured;
}

void checkPhysicalClock()
{
    int cases = 0;
    for (DcoRange range : { DcoRange::Sixteen, DcoRange::Eight, DcoRange::Four })
    {
        for (double frequency : { 200.0, 400.0 })
        {
            const auto nominal = measure(8000000.0, range, frequency);
            for (double clock : { 7200000.0, 8800000.0 })
            {
                const auto actual = measure(clock, range, frequency);
                require(actual.slope == nominal.slope,
                        "configured clock changed current at fixed CV/range");
                require(std::abs(actual.reset - nominal.reset) < 2e-12,
                        "configured clock stretched the finite reset");
                // Independent charge integral: start at the discharged rail,
                // accumulate the observed nominal dV/dt for actual elapsed
                // charge time, and stop at the supply if it is reached.
                const double integral = nominal.slope
                                      * (actual.period - nominal.reset);
                require(std::abs(actual.peak - std::min(15.0, integral)) < 2e-7,
                        "actual ramp peak fails constant-current integration");
                require(clock < 8000000.0 ? actual.duty > nominal.duty
                                           : actual.duty < nominal.duty,
                        "faster clock has the wrong comparator-high duty sign");
                ++cases;
            }
        }
    }
    // The rail plateau has a different duty/mean from a triangular ramp
    // normalized to its clipped peak. Include nominal here to forbid a
    // compatibility discontinuity at exactly 8 MHz.
    for (double clock : { 7200000.0, 8000000.0, 8800000.0 })
    {
        const auto rail = measure(clock, DcoRange::Eight, 400.0, 1.6f);
        require(std::abs(rail.peak - 15.0) < 1e-7,
                "configured-clock rail plateau exceeds or misses supply");
        const auto off = measure(clock, DcoRange::Eight, 200.0, 1.0f, -0.8f);
        require(off.duty == 1.0, "configured clock unpinned Pulse Off");
    }
    std::cout << cases << " nonnominal range/frequency cases; rail, PWM and saw means verified\n";
}

void checkSharedClockAndCpu()
{
    for (double clock : { 7200000.0, 8000000.0, 8800000.0 })
    {
        auto engine = std::make_unique<YouKnowEngine>();
        require(engine->configureDcoMasterClockHz(clock), "clock fixture rejected");
        Probe::configureCell(*engine, 768000.0, DcoRange::Eight, 5000);
        const double initialPhase = Probe::sharedClockPhase(*engine);
        for (int sample = 0; sample < 4000; ++sample)
        {
            Probe::advance(*engine, true);
            const double expectedPhase = std::fmod(
                initialPhase - (sample + 1.0) * (clock / 4.0) / 768000.0, 1.0);
            const double phaseDifference = std::abs(std::remainder(
                Probe::sharedClockPhase(*engine) - expectedPhase, 1.0));
            require(phaseDifference < 2e-8,
                    "shared IC35 phase advanced at the wrong physical clock");
            const auto first = Probe::frame(*engine);
            for (int card = 1; card < 6; ++card)
            {
                const auto other = Probe::frame(*engine, card);
                require(other.clocksRemaining == first.clocksRemaining
                            && other.volts == first.volts
                            && other.pulseHigh == first.pulseHigh
                            && other.subHigh == first.subHigh,
                        "six equal cells do not consume one common clock");
            }
        }
        Probe::stageCpuWrite(*engine);
        for (int sample = 0; sample < 10; ++sample)
            Probe::advance(*engine);
        require(std::abs(Probe::frame(*engine).cpuStates
                    - (1000.0 - 10.0 * 4000000.0 / 768000.0)) < 1e-10,
                "DCO reference frequency changed independent 12 MHz CPU timing");
    }
    for (DcoRange range : { DcoRange::Sixteen, DcoRange::Eight, DcoRange::Four })
    {
        auto engine = std::make_unique<YouKnowEngine>();
        require(engine->configureDcoMasterClockHz(8800000.0), "clock fixture rejected");
        Probe::configureCell(*engine, 8000.0, range, 8);
        for (int sample = 0; sample < 1000; ++sample)
        {
            Probe::advance(*engine, true);
            const auto frame = Probe::frame(*engine);
            require(std::isfinite(frame.volts) && std::isfinite(frame.clocksRemaining)
                        && frame.volts >= -1e-8 && frame.volts <= 15.000001
                        && frame.clocksRemaining > 0.0,
                    "maximum configured clock exhausted the low-rate event walk");
        }
    }
}

void checkFreewheelAndResume()
{
    double maximumDifference = 0.0;
    for (double clock : { 7200000.0, 8800000.0 })
    {
        for (std::uint32_t count : { 40000u, 5000u })
        {
            auto exact = std::make_unique<YouKnowEngine>();
            auto fast = std::make_unique<YouKnowEngine>();
            require(exact->configureDcoMasterClockHz(clock)
                        && fast->configureDcoMasterClockHz(clock),
                    "freewheel clock fixture rejected");
            Probe::configureCell(*exact, 48000.0, DcoRange::Eight, count);
            Probe::configureCell(*fast, 48000.0, DcoRange::Eight, count);
            // Exercise the real rendered front end versus its retired-card
            // shortcut. The OTA solve is unnecessary here: C56 precedes it.
            // 45/55 Hz uses endpoint tracking; 360/440 Hz uses cycle means.
            for (int sample = 0; sample < 24000; ++sample)
            {
                Probe::advanceFrontEnd(*exact, true);
                Probe::advanceFrontEnd(*fast, false);
            }
            const auto exactRamp = Probe::frame(*exact);
            const auto fastRamp = Probe::frame(*fast);
            require(exactRamp.volts == fastRamp.volts
                        && exactRamp.clocksRemaining == fastRamp.clocksRemaining
                        && exactRamp.subHigh == fastRamp.subHigh,
                    "retired card changed the configured PIT/ramp trajectory");
            const double exactState = Probe::couplingState(*exact);
            const double fastState = Probe::couplingState(*fast);
            maximumDifference = std::max(maximumDifference,
                                        std::abs(exactState - fastState));
            // Endpoint/BLEP and omitted periodic capacitor ripple are bounded
            // approximations, not identical waveforms. This 25 mV bound is
            // tighter than the existing 50 mV low-note C56 qualification.
            require(std::abs(exactState - fastState) < 0.025,
                    "configured-clock freewheel lost C56 saw/pulse charge");
            fast->noteOn(60, 1.0f);
            exact->noteOn(60, 1.0f);
            require(Probe::couplingState(*fast) == fastState
                        && Probe::couplingState(*exact) == exactState,
                    "resuming configured-clock card reset C56 charge");
            // Resume through the rendered front end. This fixture deliberately
            // gave the fast card no rendered history, so its 24-sample delay
            // cannot null against Exact. Test capacitor continuity instead:
            // with this saw+pulse fixture the source, including reconstruction
            // overshoot, stays inside a conservative +/-20 V envelope. C56's
            // retained 0.330 s load time bounds its change in one interval.
            for (int sample = 0; sample < 128; ++sample)
            {
                const double exactBefore = Probe::couplingState(*exact);
                const double fastBefore = Probe::couplingState(*fast);
                Probe::advanceFrontEnd(*exact, true);
                Probe::advanceFrontEnd(*fast, false);
                require(std::isfinite(Probe::couplingState(*fast))
                            && std::abs(Probe::couplingState(*fast) - fastBefore)
                                < (20.0 + std::abs(fastBefore)) / (48000.0 * 0.330)
                            && std::abs(Probe::couplingState(*exact) - exactBefore)
                                < (20.0 + std::abs(exactBefore)) / (48000.0 * 0.330),
                        "configured-clock resume violates C56 charge continuity");
            }
        }
    }
    std::cout << "maximum fast/Exact C56 state difference "
              << maximumDifference << " V\n";
}

struct Render
{
    std::vector<float> left, right;
    std::vector<double> firmware;
};

Render render(double clock, int block)
{
    auto engine = std::make_unique<YouKnowEngine>();
    require(engine->configureDcoMasterClockHz(clock), "render clock rejected");
    engine->prepare(48000.0, 128, false);
    youknow::EngineParameters parameters;
    parameters.calibration = 0.0f;
    parameters.aging = 0.0f;
    parameters.pulseEnabled = true;
    parameters.pwmSource = youknow::PwmSource::Lfo;
    parameters.dcoLfoDepth = 0.08f;
    parameters.vcfTanhMode = youknow::VcfTanhMode::PolyZoned;
    parameters.vcfFastEarlyMode = youknow::VcfFastEarlyMode::Cubic;
    parameters.vcfSolverMode = youknow::VcfSolverMode::Rk4Single;
    engine->setParameters(parameters);
    for (int note : { 48, 52, 55, 60, 64, 67 })
        engine->noteOn(note, 1.0f);
    Render result;
    result.left.resize(8192);
    result.right.resize(result.left.size());
    for (int start = 0; start < 8192;)
    {
        if (start == 4096)
        {
            parameters.range = DcoRange::Four;
            engine->setParameters(parameters);
        }
        const int boundary = start < 4096 ? 4096 : 8192;
        const int count = std::min(block, boundary - start);
        engine->process(result.left.data() + start, result.right.data() + start, count);
        start += count;
    }
    result.firmware = Probe::firmwareState(*engine);
    for (std::size_t i = 0; i < result.left.size(); ++i)
        require(std::isfinite(result.left[i]) && std::isfinite(result.right[i]),
                "configured clock produced non-finite audio");
    return result;
}

void checkAudioAndFirmware()
{
    const auto nominal = render(8000000.0, 128);
    const auto actual = render(8008000.0, 128);
    const auto partitioned = render(8008000.0, 17);
    require(actual.left == partitioned.left && actual.right == partitioned.right,
            "configured common clock or range handoff depends on host block partition");
    require(actual.left != nominal.left, "configured clock did not reach rendered sound");
    require(actual.firmware == nominal.firmware,
            "common DCO clock changed firmware pitch, CV, envelope or LFO state");
}
}

int main()
{
    try
    {
        checkConfiguration();
        checkPhysicalClock();
        checkSharedClockAndCpu();
        checkFreewheelAndResume();
        checkAudioAndFirmware();
        std::cout << "PASS: configured DCO clock timing, fixed charging current, finite reset, shared phase, CPU isolation and block invariance\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
