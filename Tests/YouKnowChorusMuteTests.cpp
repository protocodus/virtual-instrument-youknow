// Independent nodal oracle for the jack-board mute drive (service notes p.15).
// The production path uses a prepared matrix exponential. This reference
// integrates resistor currents with RK4 and independently transcribed parts.
// It validates the passive/threshold model, not original-unit transistor
// parameters or the separate 5 ms plug-in wet-return fade.
#include "../Source/DSP/YouKnowChorus.h"
#include "../Source/DSP/YouKnowEngine.h"
#include "../Source/DSP/YouKnowProductFidelity.h"

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
    const double sink = muted ? 0.0 : (v[0] + 15.0) / 330.0;
    return { ((15.0 - v[0]) / 10000.0 - between - sink) / 2.2e-6,
             (between - (v[1] + 15.0) / (560000.0 + 39000.0)) / 1.0e-6 };
}

State conductingEquilibrium()
{
    // Independent literal nodal DC solve, including R50's current while
    // Tr5 conducts. The production equilibrium helper is deliberately unused.
    const double pullUp = 1.0 / 10000.0;
    const double pullDown = 1.0 / 330.0;
    const double between = 1.0 / 150000.0;
    const double holdDown = 1.0 / 599000.0;
    const double a = pullUp + pullDown + between;
    const double b = between + holdDown;
    const double inputA = 15.0 * (pullUp - pullDown);
    const double inputB = -15.0 * holdDown;
    const double determinant = a * b - between * between;
    return { (b * inputA + between * inputB) / determinant,
             (between * inputA + a * inputB) / determinant };
}

State offset(State value, const State& slope, double dt)
{
    for (std::size_t i = 0; i < value.size(); ++i)
        value[i] += dt * slope[i];
    return value;
}

void oracleStep(State& v, bool muted, double seconds, double maxStep)
{
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
    State reference = conductingEquilibrium();
    State finer = reference;
    // A settled Off/On pair followed by interrupted charging in both
    // directions. The very short command also exercises a single-frame
    // finite Tr5 sink at the lowest supported rate.
    constexpr std::array<std::pair<bool, double>, 9> sequence {{
        { true, 2.0 }, { false, 2.0 }, { true, 0.06 },
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
        {
            openMs = 1000.0 * (actualSwitch + 1) / rate;
            const auto actual = youknow::YouKnowTestAccess::volts(chorus);
            const auto rest = conductingEquilibrium();
            require(std::abs(actual[0] - rest[0]) < 2.0e-5
                    && std::abs(actual[1] - rest[1]) < 2.0e-4,
                    "conducting Tr5 did not approach the finite-R46 loaded equilibrium");
            require(actual[0] > -14.1 && actual[0] < -14.0,
                    "R50 current through R46 no longer lifts C16 above the negative rail");
            require(openMs > 120.0 && openMs < 123.0,
                    "finite R46 no longer contributes the derived opening delay");
        }
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

void checkRateInvariance()
{
    // Every command edge is representable on all three grids. Check the
    // same elapsed-time capacitor trajectory, not just threshold timestamps.
    // A hard node clamp or a per-sample linear ramp fails the nodal oracle;
    // an incorrectly cached rate fails this independent grid comparison.
    std::array<youknow::Chorus, 3> chorus;
    constexpr std::array rates { 48000.0, 192000.0, 768000.0 };
    for (std::size_t i = 0; i < chorus.size(); ++i)
    {
        chorus[i].prepare(rates[i]);
        float left {}, right {};
        chorus[i].process(0.0f, youknow::ChorusMode::One, 0.0f, left, right,
                          false, false, 1.0f, false, true, true);
    }
    constexpr std::array<std::pair<bool, double>, 5> sequence {{
        { true, 0.0875 }, { false, 0.0125 }, { true, 0.225 },
        { false, 0.05 }, { true, 0.1 }
    }};
    double maximumError = 0.0;
    for (const auto [muted, seconds] : sequence)
    {
        for (std::size_t i = 0; i < chorus.size(); ++i)
            for (int frame = 0; frame < std::llround(seconds * rates[i]); ++frame)
                youknow::YouKnowTestAccess::advance(chorus[i], muted);
        const auto reference = youknow::YouKnowTestAccess::volts(chorus.back());
        for (const auto& candidate : chorus)
        {
            const auto actual = youknow::YouKnowTestAccess::volts(candidate);
            for (std::size_t node = 0; node < actual.size(); ++node)
                maximumError = std::max(maximumError, std::abs(actual[node] - reference[node]));
        }
    }
    require(maximumError < 1.0e-8, "finite sink trajectory depends on numerical sample grid");
    std::cout << "48/192/768 kHz elapsed-time node agreement " << maximumError << " V\n";
}

void checkEffectiveProfileIsolation()
{
    using youknow::Chorus;
    using youknow::ChorusMode;
    using youknow::ChorusTimingProfile;
    require(youknow::EngineParameters {}.chorusTimingProfile
                == youknow::ChorusTimingProfile::Shipping,
            "effective A11 timing was enabled in shipping defaults");
    // Every comparison candidate must differ from shipping in Mode I and in
    // nothing else: these exist to let OQ-01 be decided by ear, not to invent a
    // second chorus. The blend alone also moves Mode II, by the owner's
    // 2026-09-22 choice checked below.
    for (const auto profile : { ChorusTimingProfile::A11Spectral,
                                ChorusTimingProfile::A11ClickTiming,
                                ChorusTimingProfile::DerivedNominal,
                                ChorusTimingProfile::OwnerBlend })
    {
        for (const auto mode : { ChorusMode::Off, ChorusMode::One,
                                 ChorusMode::Two, ChorusMode::OneTwo })
        {
            const auto shipping = Chorus::settingsFor(mode);
            const auto candidate = Chorus::settingsFor(mode, profile);
            require(shipping.wetGain == candidate.wetGain,
                    "a timing candidate changed chorus gain");
            const auto same = shipping.centreDelaySeconds == candidate.centreDelaySeconds
                           && shipping.sweepSeconds == candidate.sweepSeconds
                           && shipping.rateHz == candidate.rateHz;
            const auto owned = mode == ChorusMode::One
                || (mode == ChorusMode::Two && profile == ChorusTimingProfile::OwnerBlend);
            require(owned ? !same : same,
                    owned ? "a timing candidate left its mode where shipping has it"
                          : "a timing candidate reached a mode it was not chosen for");
        }
    }

    // The owner's blend is exactly the 1:2:1 mean of A, B and C in Mode I,
    // and it is what the product selects (Docs/decisions.md, 2026-09-17).
    {
        const auto a = Chorus::settingsFor(ChorusMode::One);
        const auto b = Chorus::settingsFor(ChorusMode::One, ChorusTimingProfile::A11Spectral);
        const auto c = Chorus::settingsFor(ChorusMode::One, ChorusTimingProfile::A11ClickTiming);
        const auto blend = Chorus::settingsFor(ChorusMode::One, ChorusTimingProfile::OwnerBlend);
        const auto mean = [](double x, double y, double z) { return 0.25 * (x + 2.0 * y + z); };
        require(std::abs(blend.centreDelaySeconds
                         - mean(a.centreDelaySeconds, b.centreDelaySeconds, c.centreDelaySeconds)) < 1.0e-9
                    && std::abs(blend.sweepSeconds
                                - mean(a.sweepSeconds, b.sweepSeconds, c.sweepSeconds)) < 1.0e-9
                    && std::abs(blend.rateHz - mean(a.rateHz, b.rateHz, c.rateHz)) < 1.0e-7,
                "the owner's blend is not the 1:2:1 mean of A, B and C");
        require(std::abs(blend.centreDelaySeconds - 0.00349) < 1.0e-5
                    && std::abs(blend.sweepSeconds - 0.00204) < 1.0e-5
                    && std::abs(blend.rateHz - 0.5248) < 1.0e-4,
                "the owner's blend left its recorded 3.49 ms / 2.04 ms / 0.5248 Hz");
        // Mode II keeps the blend's excursion and follows the rate-only mode
        // line at shipping's II/I ratio: 0.852 Hz (Docs/decisions.md, 2026-09-22).
        const auto two = Chorus::settingsFor(ChorusMode::Two, ChorusTimingProfile::OwnerBlend);
        const auto shippingTwo = Chorus::settingsFor(ChorusMode::Two);
        require(two.centreDelaySeconds == blend.centreDelaySeconds
                    && two.sweepSeconds == blend.sweepSeconds
                    && std::abs(two.rateHz - blend.rateHz * shippingTwo.rateHz / a.rateHz) < 1.0e-6
                    && std::abs(two.rateHz - 0.852) < 1.0e-3,
                "the blend's Mode II left its rate-only 0.852 Hz");
        youknow::EngineParameters product;
        youknow::ProductFidelityProfile::applyTo(product);
        require(product.chorusTimingProfile == ChorusTimingProfile::OwnerBlend,
                "the product does not select the owner's blend");
    }

    for (const auto mode : { ChorusMode::Off, ChorusMode::One, ChorusMode::Two, ChorusMode::OneTwo })
    {
        const auto nominal = Chorus::settingsFor(mode);
        const auto effective =
            Chorus::settingsFor(mode, ChorusTimingProfile::A11Spectral);
        require(nominal.wetGain == effective.wetGain,
                "timing comparison also changed chorus gain");
        if (mode != ChorusMode::One)
            require(nominal.centreDelaySeconds == effective.centreDelaySeconds
                    && nominal.sweepSeconds == effective.sweepSeconds
                    && nominal.rateHz == effective.rateHz,
                    "the Mode-I identification invented a different chorus mode");
        else
            require(std::abs(effective.centreDelaySeconds - 0.00338027575) < 1.0e-9
                    && std::abs(effective.sweepSeconds - 0.00176176683) < 1.0e-9
                    && std::abs(effective.rateHz - 0.5159334275) < 1.0e-7,
                    "comparison profile no longer matches the identified effective timing");
        Chorus ordinary, candidate;
        ordinary.prepare(48000.0);
        candidate.prepare(48000.0);
        double error = 0.0;
        for (int frame = 0; frame < 4800; ++frame)
        {
            const float input = static_cast<float>(0.1 * std::sin(2.0 * 3.141592653589793 * 173.0 * frame / 48000.0));
            float leftA {}, rightA {}, leftB {}, rightB {};
            ordinary.process(input, mode, 0.0f, leftA, rightA, false, false,
                             1.0f, false, true, true, true,
                             ChorusTimingProfile::Shipping);
            candidate.process(input, mode, 0.0f, leftB, rightB, false, false,
                              1.0f, false, true, true, true,
                              ChorusTimingProfile::A11Spectral);
            require(ordinary.muteDriveMuted() == candidate.muteDriveMuted(),
                    "timing-profile comparison changed the mute circuit state");
            error = std::max({ error, std::abs(static_cast<double>(leftA) - leftB),
                              std::abs(static_cast<double>(rightA) - rightB) });
        }
        require(mode == ChorusMode::One ? error > 1.0e-4 : error == 0.0,
                "comparison flag did not isolate the Mode-I timing audio path");
    }
    std::cout << "A11 effective timing changes Mode I only; gains/mute and shipping default preserved\n";
}
}

int main()
{
    for (const double rate : { 8000.0, 44100.0, 48000.0, 176400.0, 192000.0, 768000.0 })
        check(rate);
    checkRateInvariance();
    checkEffectiveProfileIsolation();
}
