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
    static void setup(YouKnowEngine& engine, double rate, float capacitor, float resistor)
    {
        engine.prepare(rate, 64, false);
        EngineParameters parameters;
        parameters.calibration = 1.0f;
        parameters.range = DcoRange::Eight;
        engine.setParameters(parameters);
        engine.panelGlidePrimed_ = true;
        for (auto& card : engine.cards_)
        {
            card.dcoComponents.capacitorDraw = capacitor;
            card.dcoComponents.resistorDraw = { -resistor, resistor, 0.5f * resistor };
        }
        engine.refreshVoiceRampCurrentScales();
        for (int slot = 0; slot < 6; ++slot)
        {
            auto& voice = engine.voices_[static_cast<std::size_t>(slot)];
            auto& dco = voice.dco;
            dco.reset();
            dco.divider = 7675;
            dco.periodSamples = rate * 7675.0 / 2000000.0;
            dco.pitState = YouKnowEngine::Dco::PitState::running;
            dco.pitClocksToEvent = 50000.0;
            voice.dcoCv = voice.dcoCvTarget = 256.0f;
            engine.beginDcoCharge(voice, 0.0f, false);
            dco.rampValue = -0.25;
            dco.saw.prime(static_cast<float>(
                (dco.rampValue + 1.0) * dco.renderScale * voice.rampCurrentScale - 1.0));
        }
    }

    static std::array<double, 4> state(YouKnowEngine& engine, int slot, float pwm)
    {
        auto& voice = engine.voices_[static_cast<std::size_t>(slot)];
        engine.pwmVolts_ = pwm;
        engine.updatePulseComparator(voice, engine.activeParameters_);
        const auto& d = voice.dco;
        return { 6.0 * d.renderScale * voice.rampCurrentScale * (d.rampValue + 1.0),
                 6.0 * d.renderScale * voice.rampCurrentScale * d.rampSlopePerSecond,
                 voice.pulseThresholdVolts, voice.pulseDuty };
    }

    static void range(YouKnowEngine& engine, DcoRange range)
    {
        auto parameters = engine.activeParameters_;
        parameters.range = range;
        engine.setParameters(parameters);
    }

    static void advance(YouKnowEngine& engine)
    {
        for (int slot = 0; slot < 6; ++slot)
        {
            auto& voice = engine.voices_[static_cast<std::size_t>(slot)];
            engine.advanceDcoPitAndRamp(voice, engine.activeParameters_.range,
                voice.pulseThresholdVolts, voice.pulseThresholdVolts, false, false, true);
        }
        engine.advanceRangeClock(engine.activeParameters_.range);
    }

    static void character(YouKnowEngine& engine, float character)
    {
        auto parameters = engine.activeParameters_;
        parameters.calibration = character;
        engine.setParameters(parameters);
    }
};
}

namespace
{
using youknow::DcoComponentTolerance;
using youknow::DcoRange;
using youknow::YouKnowEngine;
using Probe = youknow::YouKnowTestAccess;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void checkComponentBounds()
{
    for (float capacitor : { -1.0f, 0.0f, 1.0f })
    {
        for (float resistor : { -1.0f, 0.0f, 1.0f })
        {
            DcoComponentTolerance parts;
            parts.capacitorDraw = capacitor;
            parts.resistorDraw.fill(resistor);
            const double expected = 1.0 / ((1.0 + capacitor * 0.02)
                                         * (1.0 + resistor * 0.01));
            for (std::size_t range = 0; range < 3; ++range)
            {
                require(std::abs(parts.chargingScale(range, 1.0f) - expected) < 6e-8,
                        "component response failed reciprocal R*C corner oracle");
                require(parts.chargingScale(range, 0.0f) == 1.0f,
                        "Character zero retained a component error");
            }
        }
    }
    DcoComponentTolerance parts;
    parts.capacitorDraw = 1.0f;
    const float shared = parts.chargingScale(0, 1.0f);
    require(parts.chargingScale(1, 1.0f) == shared
                && parts.chargingScale(2, 1.0f) == shared,
            "one capacitor did not create correlated range errors");
    parts.resistorDraw[1] = 1.0f;
    require(parts.chargingScale(0, 1.0f) == shared
                && parts.chargingScale(2, 1.0f) == shared
                && parts.chargingScale(1, 1.0f) < shared,
            "a selected resistor changed another range's identity");
    require(parts.chargingScale(1, 2.0f) < parts.chargingScale(1, 1.0f),
            "component bounds silently disabled the Character exaggeration range");
}

void checkLiveRangesAndTrim()
{
    int cases = 0;
    for (double rate : { 8000.0, 48000.0, 192000.0 })
    {
        for (float capacitor : { -1.0f, 1.0f })
        {
            for (float resistor : { -1.0f, 1.0f })
            {
                auto engine = std::make_unique<YouKnowEngine>();
                Probe::setup(*engine, rate, capacitor, resistor);
                std::array<double, 6> threshold {};
                for (int slot = 0; slot < 6; ++slot)
                {
                    const auto mid = Probe::state(*engine, slot, 6.0f);
                    const auto end = Probe::state(*engine, slot, 0.6f);
                    require(mid[3] >= 0.48 - 1e-7 && mid[3] <= 0.52 + 1e-7
                                && end[3] >= 0.93 - 1e-7 && end[3] <= 0.97 + 1e-7,
                            "joint R/C/comparator draw failed a service acceptance point");
                    if (slot == 0)
                        require(std::abs(mid[3] - 0.5) < 1e-7,
                                "shared PWM trim did not put CH1 at 50 percent");
                    threshold[static_cast<std::size_t>(slot)] = mid[2];
                }
                for (DcoRange range : { DcoRange::Sixteen, DcoRange::Four, DcoRange::Eight })
                {
                    const auto before = Probe::state(*engine, 0, 6.0f);
                    Probe::range(*engine, range);
                    const auto after = Probe::state(*engine, 0, 6.0f);
                    require(std::abs(before[0] - after[0]) < 1e-12,
                            "component range selection moved retained capacitor voltage");
                    const double actualResistance = range == DcoRange::Sixteen
                        ? 399000.0 * (1.0 - resistor * 0.01)
                        : range == DcoRange::Four ? 100000.0 * (1.0 + resistor * 0.005)
                        : 200000.0 * (1.0 + resistor * 0.01);
                    constexpr double sourceVoltage = (12e-9 / (7675.0 / 2000000.0 - 2.2e-6))
                                                    * 200000.0;
                    const double expectedSlope = sourceVoltage
                        / (actualResistance * 1e-9 * (1.0 + capacitor * 0.02));
                    require(std::abs(after[1] - expectedSlope) < 0.001,
                            "engine did not use the selected card R*C current");
                    for (int slot = 0; slot < 6; ++slot)
                        require(Probe::state(*engine, slot, 6.0f)[2]
                                    == threshold[static_cast<std::size_t>(slot)],
                                "RANGE silently re-trimmed the comparator");
                    Probe::advance(*engine);
                    require(std::abs(Probe::state(*engine, 0, 6.0f)[0]
                                - (after[0] + expectedSlope / rate)) < 2e-7,
                            "selected component current failed independent charge integration");
                    ++cases;
                }
                const auto beforeCharacter = Probe::state(*engine, 0, 6.0f);
                Probe::character(*engine, 0.0f);
                require(std::abs(Probe::state(*engine, 0, 6.0f)[0] - beforeCharacter[0]) < 1e-12,
                        "Character morph introduced an artificial capacitor-voltage step");
            }
        }
    }
    std::cout << cases << " live component/range cases and both six-card service points verified\n";
}

std::vector<float> render(int block)
{
    auto engine = std::make_unique<YouKnowEngine>();
    engine->prepare(48000.0, 128, false);
    youknow::EngineParameters parameters;
    parameters.calibration = 1.0f;
    parameters.sawEnabled = true;
    parameters.pulseEnabled = true;
    engine->setParameters(parameters);
    for (int note : { 48, 52, 55, 60, 64, 67 })
        engine->noteOn(note, 1.0f);
    std::vector<float> output(4096), right(128);
    for (int offset = 0; offset < 4096;)
    {
        if (offset == 2048)
        {
            parameters.range = DcoRange::Four;
            engine->setParameters(parameters);
        }
        const int boundary = offset < 2048 ? 2048 : 4096;
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
        checkComponentBounds();
        checkLiveRangesAndTrim();
        const auto output = render(127);
        require(output == render(1), "component-aware range audio depends on block partition");
        require(std::all_of(output.begin(), output.end(), [](float x) { return std::isfinite(x); })
                    && std::any_of(output.begin(), output.end(), [](float x) { return std::abs(x) > 1e-5f; }),
                "component-aware range qualification rendered invalid or silent audio");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
