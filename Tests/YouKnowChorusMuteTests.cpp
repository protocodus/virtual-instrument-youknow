// Independent nodal oracle for the jack-board mute drive (service notes p.15).
// The production path uses a prepared matrix exponential. This reference
// integrates resistor currents with RK4 and independently transcribed parts.
// It validates the passive/threshold model, not original-unit transistor
// parameters or the separate 5 ms plug-in wet-return fade.
#include "../Source/DSP/YouKnowChorus.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <utility>

namespace youknow
{
struct YouKnowTestAccess
{
    static void advance(Chorus& chorus, bool muted)
    {
        chorus.advanceMuteDrive(muted);
    }
    static std::array<double, 2> volts(const Chorus& chorus)
    {
        return { chorus.muteDriveNodeVolts_, chorus.muteDriveHoldVolts_ };
    }
    static std::uint64_t supportBuilds(const Chorus& chorus)
    {
        return chorus.supportBuildCount_;
    }
};
}

namespace
{
using State = std::array<double, 2>;
constexpr double threshold = -15.0 + 0.6 * (560000.0 + 39000.0) / 39000.0;

State currents(const State& v, bool muted)
{
    const double between = (v[0] - v[1]) / 150000.0;
    return { muted ? ((15.0 - v[0]) / 10000.0 - between) / 2.2e-6 : 0.0,
             (between - (v[1] + 15.0) / (560000.0 + 39000.0)) / 1.0e-6 };
}

State offset(State value, const State& slope, double dt)
{
    for (std::size_t i = 0; i < value.size(); ++i)
        value[i] += dt * slope[i];
    return value;
}

void oracleStep(State& v, bool muted, double seconds, double maxStep)
{
    if (!muted)
        v[0] = -15.0;
    const int steps = static_cast<int>(std::ceil(seconds / maxStep));
    const double dt = seconds / steps;
    for (int step = 0; step < steps; ++step)
    {
        const auto k1 = currents(v, muted);
        const auto k2 = currents(offset(v, k1, 0.5 * dt), muted);
        const auto k3 = currents(offset(v, k2, 0.5 * dt), muted);
        const auto k4 = currents(offset(v, k3, dt), muted);
        for (std::size_t i = 0; i < v.size(); ++i)
            v[i] += dt * (k1[i] + 2.0 * k2[i] + 2.0 * k3[i] + k4[i]) / 6.0;
    }
}

void require(bool pass, const char* message)
{
    if (!pass)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void check(double rate)
{
    youknow::Chorus chorus;
    chorus.prepareSupportRates(48000.0);
    chorus.prepare(rate);
    float left {}, right {};
    chorus.process(0.0f, youknow::ChorusMode::One, 0.0f, left, right,
                   false, false, 1.0f, false, true, true);
    State reference { -15.0, -15.0 };
    State finer = reference;
    // A settled Off/On pair followed by interrupted charging in both
    // directions. The very short command also exercises a single-frame
    // Tr5 clamp at the lowest supported rate.
    constexpr std::array<std::pair<bool, double>, 9> sequence {{
        { true, 2.0 }, { false, 0.5 }, { true, 0.06 },
        { false, 0.025 }, { true, 0.25 }, { false, 0.035 },
        { true, 0.15 }, { false, 0.000125 }, { true, 0.2 }
    }};
    double maxError = 0.0;
    double convergence = 0.0;
    double muteMs = -1.0, openMs = -1.0;
    int intervalIndex = 0;
    for (const auto [muted, seconds] : sequence)
    {
        const int frames = static_cast<int>(std::llround(seconds * rate));
        int actualSwitch = -1, expectedSwitch = -1;
        for (int frame = 0; frame < frames; ++frame)
        {
            youknow::YouKnowTestAccess::advance(chorus, muted);
            oracleStep(reference, muted, 1.0 / rate, 1.0 / 500000.0);
            oracleStep(finer, muted, 1.0 / rate, 1.0 / 1000000.0);
            const auto actual = youknow::YouKnowTestAccess::volts(chorus);
            for (std::size_t i = 0; i < actual.size(); ++i)
            {
                maxError = std::max(maxError, std::abs(actual[i] - finer[i]));
                convergence = std::max(convergence, std::abs(reference[i] - finer[i]));
                require(actual[i] >= -15.000001 && actual[i] <= 15.000001,
                        "passive mute network escaped its supply rails");
            }
            if (chorus.muteDriveMuted() == muted && actualSwitch < 0)
                actualSwitch = frame;
            if ((finer[1] >= threshold) == muted && expectedSwitch < 0)
                expectedSwitch = frame;
        }
        require(actualSwitch < 0 ? expectedSwitch < 0
                                : expectedSwitch >= 0 && std::abs(actualSwitch - expectedSwitch) <= 1,
                "mute crossing differs from independent nodal oracle by more than one sample");
        if (intervalIndex == 0)
        {
            muteMs = 1000.0 * (actualSwitch + 1) / rate;
            // DC check from the literal four-resistor series current.
            const double current = 30.0 / (10000.0 + 150000.0 + 560000.0 + 39000.0);
            const auto actual = youknow::YouKnowTestAccess::volts(chorus);
            require(std::abs(actual[0] - (15.0 - current * 10000.0)) < 2.0e-5
                    && std::abs(actual[1] - (-15.0 + current * 599000.0)) < 2.0e-4,
                    "open Tr5 did not approach the loaded series-resistor equilibrium");
        }
        if (intervalIndex == 1)
            openMs = 1000.0 * (actualSwitch + 1) / rate;
        ++intervalIndex;
    }
    // Halving the reference step verifies the numerical integration margin.
    // 2 uV additionally covers production's float-stored component values
    // versus the independently transcribed decimal schematic coordinates.
    require(convergence < 1.0e-8, "nodal oracle did not converge under step halving");
    require(maxError < 2.0e-6, "prepared mute transition disagrees with nodal oracle");

    // A rate change is a new numerical grid, not a reset of either capacitor.
    const auto before = youknow::YouKnowTestAccess::volts(chorus);
    const auto builds = youknow::YouKnowTestAccess::supportBuilds(chorus);
    const double nextRate = rate == 48000.0 ? 192000.0 : 48000.0;
    chorus.prepare(nextRate, true);
    require(youknow::YouKnowTestAccess::volts(chorus) == before,
            "prepare(preserveState) reset the mute capacitors");
    require(youknow::YouKnowTestAccess::supportBuilds(chorus) == builds,
            "a prepared quality change rebuilt support inside the live transition");
    youknow::YouKnowTestAccess::advance(chorus, true);
    oracleStep(finer, true, 1.0 / nextRate, 1.0 / 1000000.0);
    const auto after = youknow::YouKnowTestAccess::volts(chorus);
    require(std::abs(after[0] - finer[0]) < 2.0e-6
            && std::abs(after[1] - finer[1]) < 2.0e-6,
            "the preserved mute state did not advance on the new cached grid");
    std::cout << std::setprecision(10) << rate << " Hz: max node error "
              << maxError << " V; RK4 convergence " << convergence
              << " V; mute " << muteMs << " ms; open " << openMs << " ms\n";
}
}

int main()
{
    for (const double rate : { 8000.0, 44100.0, 48000.0, 176400.0, 192000.0, 768000.0 })
        check(rate);
}
