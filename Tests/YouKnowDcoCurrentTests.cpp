// A separate capacitor-charge oracle for the live DCO CV. The reference
// volts/code calibration is model policy; I=V/R and Q=C*V are circuit laws.
#include "DSP/YouKnowEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace youknow
{
struct YouKnowTestAccess
{
    static void cell(YouKnowEngine& engine, double rate, DcoRange range,
                     std::uint32_t count, float code)
    {
        engine.prepare(rate, 64, false);
        EngineParameters parameters;
        parameters.calibration = 0.0f;
        parameters.range = range;
        engine.setParameters(parameters);
        engine.panelGlidePrimed_ = true;
        auto& voice = engine.voices_[0];
        voice.dco.reset();
        voice.dco.divider = count;
        voice.dco.periodSamples = rate * count / YouKnowEngine::rangeClockHz(range);
        voice.dco.pitState = YouKnowEngine::Dco::PitState::running;
        voice.dco.pitClocksToEvent = 500000.0;
        voice.dcoCv = voice.dcoCvTarget = code;
        voice.rampCurrentScale = 1.0f;
        engine.beginDcoCharge(voice, 0.0f, false);
        voice.dco.saw.prime(-1.0f);
    }

    static std::array<double, 4> state(const YouKnowEngine& engine)
    {
        const auto& v = engine.voices_[0];
        const auto& d = v.dco;
        return { 6.0 * d.renderScale * (d.rampValue + 1.0),
                 6.0 * d.renderScale * d.rampSlopePerSecond,
                 d.pitClocksToEvent, d.resetSecondsRemaining };
    }

    static void future(YouKnowEngine& engine, float code)
    {
        auto& v = engine.voices_[0];
        v.dcoPitchTransactionValid = true;
        v.dcoPitchTransactionCvTarget = code;
        v.dcoCvTarget = code;
    }

    static void acquire(YouKnowEngine& engine, float code)
    {
        engine.updateDcoHeldCv(engine.voices_[0], code);
    }

    static void advance(YouKnowEngine& engine, float threshold = 6.0f)
    {
        engine.advanceDcoPitAndRamp(engine.voices_[0], engine.activeParameters_.range,
            threshold, threshold, false, false, true);
        engine.advanceRangeClock(engine.activeParameters_.range);
    }

    static void discharge(YouKnowEngine& engine)
    {
        engine.beginDcoDischarge(engine.voices_[0], 1.0f, true);
    }

    static bool pulse(const YouKnowEngine& engine)
    {
        return engine.voices_[0].dco.pulseState > 0.0f;
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

double slope(float code, DcoRange range)
{
    constexpr double capacitance = 1e-9;
    constexpr double referenceSeconds = 7675.0 / 2000000.0 - 2.2e-6;
    const double resistance = range == DcoRange::Sixteen ? 399000.0
                            : range == DcoRange::Eight ? 200000.0 : 100000.0;
    const double referenceCurrent = capacitance * 12.0 / referenceSeconds;
    const double sourceVoltage = referenceCurrent * 200000.0 * code / 256.0;
    return sourceVoltage / (resistance * capacitance);
}

void checkCausalCharge()
{
    int cases = 0;
    double maximumError = 0.0;
    for (double rate : { 8000.0, 44100.0, 48000.0, 192000.0, 768000.0 })
    {
        for (DcoRange range : { DcoRange::Sixteen, DcoRange::Eight, DcoRange::Four })
        {
            for (std::uint32_t count : { 479u, 7675u, 60000u })
            {
                auto engine = std::make_unique<YouKnowEngine>();
                Probe::cell(*engine, rate, range, count, 96.0f);
                const auto initial = Probe::state(*engine);
                require(std::abs(initial[1] - slope(96.0f, range)) < 1e-9,
                        "fixed-CV charging current depends on active PIT count");
                Probe::future(*engine, 768.0f);
                Probe::advance(*engine);
                const double first = slope(96.0f, range) / rate;
                require(std::abs(Probe::state(*engine)[0] - first) < 1e-11,
                        "future captured CV changed charge before T");
                Probe::acquire(*engine, 192.0f);
                require(std::abs(Probe::state(*engine)[0] - first) < 1e-11,
                        "held-CV acquisition stepped capacitor voltage");
                const float threshold = static_cast<float>(first
                    + 0.75 * slope(192.0f, range) / rate);
                Probe::advance(*engine, threshold);
                const double expected = first + slope(192.0f, range) / rate;
                maximumError = std::max(maximumError,
                    std::abs(Probe::state(*engine)[0] - expected));
                require(std::abs(Probe::state(*engine)[0] - expected) < 1e-11,
                        "live hold failed independent piecewise Q=I*dt oracle");
                require(Probe::pulse(*engine), "PWM did not follow the new current at T");
                Probe::acquire(*engine, 0.0f);
                const double retained = Probe::state(*engine)[0];
                Probe::advance(*engine);
                require(Probe::state(*engine)[0] == retained,
                        "zero held CV continued charging the capacitor");
                Probe::acquire(*engine, 96.0f);
                require(std::abs(Probe::state(*engine)[1] - initial[1]) < 1e-9,
                        "current failed to resume after zero held CV");
                Probe::discharge(*engine);
                const auto reset = Probe::state(*engine);
                Probe::acquire(*engine, 384.0f);
                require(Probe::state(*engine) == reset,
                        "CV write changed the compatibility reset or capacitor charge");
                for (int i = 0; i < 4 && Probe::state(*engine)[3] > 0.0; ++i)
                    Probe::advance(*engine);
                require(std::abs(Probe::state(*engine)[1] - slope(384.0f, range)) < 1e-8,
                        "post-reset charging ignored the CV acquired during reset");
                ++cases;
            }
        }
    }
    std::cout << cases << " current/range/rate cases, maximum charge error "
              << maximumError << " V\n";
}

std::vector<float> audio(int block)
{
    auto engine = std::make_unique<YouKnowEngine>();
    engine->prepare(48000.0, 128, false);
    youknow::EngineParameters parameters;
    parameters.calibration = 0.0f;
    parameters.chorus = youknow::ChorusMode::Off;
    parameters.sawEnabled = true;
    parameters.pulseEnabled = true;
    parameters.dcoLfoDepth = 0.5f;
    engine->setParameters(parameters);
    engine->noteOn(60, 1.0f);
    std::vector<float> output(4800), right(128);
    for (int offset = 0; offset < 4800;)
    {
        if (offset == 2400)
            engine->noteOn(72, 1.0f);
        const int boundary = offset < 2400 ? 2400 : 4800;
        const int count = std::min(block, boundary - offset);
        engine->process(output.data() + offset, right.data(), count);
        offset += count;
    }
    return output;
}
}

int main()
{
    try
    {
        checkCausalCharge();
        const auto one = audio(1);
        const auto varied = audio(127);
        require(one == varied, "live-CV audio depends on host block partition");
        require(std::any_of(one.begin(), one.end(), [](float x) { return std::abs(x) > 1e-5f; }),
                "audio qualification rendered silence");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
