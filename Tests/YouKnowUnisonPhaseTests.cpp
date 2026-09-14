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
    struct Timer
    {
        std::uint32_t count;
        double remaining;
        bool high;
        bool running;
        float midi;
        bool operator==(const Timer&) const = default;
    };

    static std::array<Timer, 6> timers(const YouKnowEngine& engine)
    {
        std::array<Timer, 6> result {};
        for (std::size_t slot = 0; slot < result.size(); ++slot)
        {
            const auto& voice = engine.voices_[slot];
            const auto& d = voice.dco;
            result[slot] = { d.divider, d.pitClocksToEvent, d.pitOutHigh,
                            d.pitState == YouKnowEngine::Dco::PitState::running,
                            voice.currentMidi };
        }
        return result;
    }

    static std::array<double, 6> volts(const YouKnowEngine& engine)
    {
        std::array<double, 6> result {};
        for (std::size_t slot = 0; slot < result.size(); ++slot)
        {
            const auto& voice = engine.voices_[slot];
            result[slot] = 6.0 * voice.dco.renderScale * voice.rampCurrentScale
                * (voice.dco.rampValue + 1.0);
        }
        return result;
    }

    static bool everyResetRequest(const YouKnowEngine& engine, bool expected)
    {
        for (std::size_t slot = 0; slot < 6; ++slot)
            if (engine.voices_[slot].dcoResetPending != expected)
                return false;
        return true;
    }
};
}

namespace
{
using youknow::YouKnowEngine;
using Probe = youknow::YouKnowTestAccess;
constexpr double rate = 48000.0;
// This suite qualifies the currently shipped logical command boundary and
// chart placement. It does not calibrate A5/B2 serial command-service times.
constexpr auto timing = YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void prepare(YouKnowEngine& engine)
{
    engine.selectConverterTimingProfile(timing);
    require(engine.configureDcoTemperatureProxy(true, 40.0), "common temperature fixture rejected");
    engine.prepare(rate, 128, false);
}

youknow::EngineParameters patch(float character = 0.0f)
{
    youknow::EngineParameters p;
    p.keyMode = youknow::KeyMode::Unison;
    p.calibration = character;
    p.sustain = 1.0f;
    p.cutoff = 1.0f;
    p.resonance = 0.0f;
    p.envDepth = 0.0f;
    return p;
}

void render(YouKnowEngine& engine, int samples, int blockSize = 128)
{
    std::array<float, 128> left {}, right {};
    while (samples > 0)
    {
        const int block = std::min(samples, blockSize);
        engine.process(left.data(), right.data(), block);
        samples -= block;
    }
}

struct Observation
{
    std::array<std::vector<double>, 6> edges;
    double maximumSteadyPhaseError { 0.0 };
    double maximumCountSpreadCents { 0.0 };
    int differingCountSamples { 0 };
};

double phase(const Probe::Timer& timer)
{
    // Direct count coordinate, including the odd-count long high half.
    const double clocks = timer.high ? (timer.count + 1u) / 2u : timer.count;
    return (clocks - timer.remaining) / timer.count;
}

double wrap(double cycles)
{
    return cycles - std::round(cycles);
}

Observation observe(YouKnowEngine& engine, int samples, bool equalCounts)
{
    Observation result;
    const auto initial = Probe::timers(engine);
    std::array<double, 6> initialDifference {};
    for (std::size_t slot = 0; slot < 6; ++slot)
        initialDifference[slot] = wrap(phase(initial[slot]) - phase(initial[0]));
    for (int sample = 0; sample < samples; ++sample)
    {
        const auto before = Probe::timers(engine);
        const double clock = engine.dcoMasterClockHz() / 4.0;
        float left = 0.0f, right = 0.0f;
        engine.process(&left, &right, 1);
        require(std::isfinite(left) && std::isfinite(right), "unison produced nonfinite sound");
        const auto after = Probe::timers(engine);
        std::uint32_t minimum = after[0].count, maximum = after[0].count;
        for (std::size_t slot = 0; slot < 6; ++slot)
        {
            require(after[slot].running, "settled unison timer stopped running");
            minimum = std::min(minimum, after[slot].count);
            maximum = std::max(maximum, after[slot].count);
            if (equalCounts)
            {
                require(after[slot].count == after[0].count,
                        "static common pitch acquired a per-card divider offset");
                result.maximumSteadyPhaseError = std::max(result.maximumSteadyPhaseError,
                    std::abs(wrap(phase(after[slot]) - phase(after[0])
                                  - initialDifference[slot])));
            }
            // At these low notes there is at most one OUT transition per
            // host sample. The retained CE countdown gives its exact physical
            // timestamp; no waveform fit or random initial phase is involved.
            if (!before[slot].high && after[slot].high)
            {
                require(before[slot].remaining / clock <= 1.0 / rate + 1e-12,
                        "rising edge did not belong to the observed sample");
                result.edges[slot].push_back(sample / rate + before[slot].remaining / clock);
            }
        }
        result.differingCountSamples += minimum != maximum;
        result.maximumCountSpreadCents = std::max(result.maximumCountSpreadCents,
            1200.0 * std::log2(static_cast<double>(maximum) / minimum));
    }
    return result;
}

double edgeLagSpan(const Observation& observation)
{
    const auto& first = observation.edges[0];
    const auto& last = observation.edges[5];
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -minimum;
    // Pair each CH1 edge with the nearest CH6 edge and express lag in local
    // CH1 cycles. Circular unwrapping prevents a half-cycle crossing from
    // being misreported as a one-cycle physical jump.
    double previous = 0.0, accumulated = 0.0;
    bool primed = false;
    for (std::size_t i = 1; i + 1 < first.size(); ++i)
    {
        const auto next = std::lower_bound(last.begin(), last.end(), first[i]);
        if (next == last.begin() || next == last.end())
            continue;
        const double edge = std::abs(*next - first[i]) < std::abs(*(next - 1) - first[i])
            ? *next : *(next - 1);
        const double lag = (edge - first[i]) / (first[i + 1] - first[i]);
        accumulated += primed ? wrap(lag - previous) : lag;
        previous = lag;
        primed = true;
        minimum = std::min(minimum, accumulated);
        maximum = std::max(maximum, accumulated);
    }
    require(primed, "unison observer saw too few rising edges");
    return maximum - minimum;
}

void checkSteadyAndCommonWarmup()
{
    for (float character : { 0.0f, 1.0f })
    {
        auto engine = std::make_unique<YouKnowEngine>();
        prepare(*engine);
        engine->setParameters(patch(character));
        engine->noteOn(57, 1.0f);
        render(*engine, 12000);
        const auto observation = observe(*engine, 48000, true);
        require(observation.maximumSteadyPhaseError < 2e-10,
                "shared warmup clock moved the relative phase of equal counters");
        const auto timers = Probe::timers(*engine);
        require(std::abs(wrap(phase(timers[5]) - phase(timers[0]))) > 1e-4,
                "unison phases were collapsed onto one coherent timer");
        double minimumHz = std::numeric_limits<double>::infinity(), maximumHz = 0.0;
        for (const auto& edges : observation.edges)
        {
            const double frequency = (edges.size() - 1.0) / (edges.back() - edges.front());
            minimumHz = std::min(minimumHz, frequency);
            maximumHz = std::max(maximumHz, frequency);
        }
        // Character0 is an exactly fixed common clock. At Character1 the
        // asynchronously phased finite windows sample slightly different
        // sections of a common thermal trajectory; that is not pitch spread.
        if (character == 0.0f)
            require(maximumHz - minimumHz < 1e-8, "steady equal counters have unequal frequencies");
        std::cout << "steady Character " << character << ": relative-phase error "
                  << observation.maximumSteadyPhaseError << " cycles; finite-window Hz spread "
                  << maximumHz - minimumHz << '\n';
    }
}

void checkSharedModulation()
{
    for (bool glide : { false, true })
    {
        auto engine = std::make_unique<YouKnowEngine>();
        require(engine->configureThermalStart(true), "settled fixture rejected");
        prepare(*engine);
        auto parameters = patch();
        parameters.portamento = glide ? 0.75f : 0.0f;
        parameters.dcoLfoDepth = glide ? 0.0f : 0.6f;
        parameters.lfoRate = 0.55f;
        engine->setParameters(parameters);
        engine->noteOn(48, 1.0f);
        render(*engine, 12000);
        if (glide)
            engine->noteOn(60, 1.0f);
        const auto observation = observe(*engine, 48000, false);
        const double span = edgeLagSpan(observation);
        require(observation.differingCountSamples > 0 && span > 1e-4,
                "shared modulation incorrectly locked every active count/phase together");
        std::cout << (glide ? "glide" : "shared LFO") << ": CH1/CH6 lag span "
                  << span << " cycles; transient active-count spread "
                  << observation.maximumCountSpreadCents << " cents; differing samples "
                  << observation.differingCountSamples << "/48000\n";
    }
}

void checkPriorHistoryAndLegato()
{
    auto engine = std::make_unique<YouKnowEngine>();
    prepare(*engine);
    auto parameters = patch();
    parameters.keyMode = youknow::KeyMode::Poly1;
    engine->setParameters(parameters);
    for (int note : { 36, 40, 43, 48, 52, 55 })
        engine->noteOn(note, 1.0f);
    render(*engine, 12000);
    parameters.portamento = 0.9f;
    parameters.keyMode = youknow::KeyMode::Unison;
    engine->setParameters(parameters);
    render(*engine, 2400);
    const auto inherited = Probe::timers(*engine);
    require(inherited[0].midi != inherited[5].midi,
            "entering Unison erased distinct physical portamento histories");
    const auto voltage = Probe::volts(*engine);
    engine->noteOn(60, 1.0f);
    require(Probe::volts(*engine) == voltage, "unison legato reset capacitor charge at the host event");
    parameters.portamento = 0.0f;
    engine->setParameters(parameters);
    render(*engine, 4800);
    const auto settled = observe(*engine, 12000, true);
    require(settled.maximumSteadyPhaseError < 2e-10,
            "prior-history unison kept detuning after every word reached the same target");
}

void checkResetPolicyAndBlocks()
{
    auto single = std::make_unique<YouKnowEngine>();
    auto blocked = std::make_unique<YouKnowEngine>();
    for (auto* engine : { single.get(), blocked.get() })
    {
        prepare(*engine);
        engine->setParameters(patch());
        engine->noteOn(48, 1.0f);
    }
    render(*single, 12000, 1);
    render(*blocked, 12000, 127);
    require(Probe::timers(*single) == Probe::timers(*blocked)
                && Probe::volts(*single) == Probe::volts(*blocked),
            "unison timer/capacitor state depends on host block partition");
    for (auto* engine : { single.get(), blocked.get() })
    {
        const auto before = Probe::timers(*engine);
        engine->noteOn(60, 1.0f);
        const auto after = Probe::timers(*engine);
        require(Probe::everyResetRequest(*engine, false),
                "held unison legato invented six Mode-3 reset requests");
        for (std::size_t slot = 0; slot < 6; ++slot)
            require(before[slot].count == after[slot].count
                        && before[slot].remaining == after[slot].remaining
                        && before[slot].high == after[slot].high,
                    "logical legato assignment reset active timer phase before its write");
    }
    render(*single, 12000, 1);
    render(*blocked, 12000, 127);
    require(Probe::timers(*single) == Probe::timers(*blocked),
            "count-only unison retarget depends on host blocks");
    single->releaseAllNotes();
    single->noteOn(60, 1.0f);
    require(Probe::everyResetRequest(*single, false),
            "released same-pitch unison invented a Mode-3 reset");
    single->releaseAllNotes();
    const auto voltage = Probe::volts(*single);
    single->noteOn(61, 1.0f);
    require(Probe::everyResetRequest(*single, true),
            "released different-pitch unison omitted the firmware reset requests");
    require(Probe::volts(*single) == voltage,
            "requesting Mode-3 reset erased capacitor charge at the host boundary");
}
}

int main()
{
    try
    {
        checkSteadyAndCommonWarmup();
        checkSharedModulation();
        checkPriorHistoryAndLegato();
        checkResetPolicyAndBlocks();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
