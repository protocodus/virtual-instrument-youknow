// IC6's two output summers, Roland jack board p. 15: R70 100k feedback with
// R71 47k dry and R72 39k wet for the left output, R67 / R73 / R74 for the
// right. The oracle solves each inverting summer's virtual-earth current sum
// from the drawn parts, and the chorus is checked at its outputs.
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=15
#include "DSP/YouKnowChorus.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>

namespace
{
using namespace youknow;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// |Vout| for the dry and wet inputs: Rf * (Vdry / Rdry + Vwet / Rwet).
double summerVolts(double feedback, double dryLeg, double wetLeg,
                   double dry, double wet)
{
    return feedback * (dry / dryLeg + wet / wetLeg);
}

void legLaw()
{
    for (float calibration : { 0.0f, 1.0f, 2.0f })
    {
        const auto off = Chorus::summerLegScale(calibration, false);
        require(off.dryLeft == 1.0f && off.wetLeft == 1.0f
                && off.dryRight == 1.0f && off.wetRight == 1.0f,
                "disabled leg spread moved an output");
    }
    const auto zero = Chorus::summerLegScale(0.0f, true);
    require(zero.dryLeft == 1.0f && zero.wetLeft == 1.0f
            && zero.dryRight == 1.0f && zero.wetRight == 1.0f,
            "Unit Character zero did not restore the nominal summers");

    double previous = 0.0;
    for (float calibration : { 0.5f, 1.0f, 2.0f })
    {
        const auto scale = Chorus::summerLegScale(calibration, true);
        const auto drawn = [&](double ohms, std::uint32_t designator) {
            return Chorus::drawnResistance(ohms, designator, calibration);
        };
        const double r70 = drawn(100e3, 70u), r71 = drawn(47e3, 71u), r72 = drawn(39e3, 72u);
        const double r67 = drawn(100e3, 67u), r73 = drawn(47e3, 73u), r74 = drawn(39e3, 74u);
        // A unit dry or wet source alone: each pair keeps its drawn ratio,
        // and the pair's geometric mean stays at the nominal summer's gain.
        const auto expect = [](float left, float right, double leftVolts, double rightVolts) {
            const double mean = std::sqrt(leftVolts * rightVolts);
            return std::abs(left - leftVolts / mean) < 2e-7 && std::abs(right - rightVolts / mean) < 2e-7;
        };
        require(expect(scale.dryLeft, scale.dryRight,
                       summerVolts(r70, r71, r72, 1, 0), summerVolts(r67, r73, r74, 1, 0))
                && expect(scale.wetLeft, scale.wetRight,
                          summerVolts(r70, r71, r72, 0, 1), summerVolts(r67, r73, r74, 0, 1)),
                "an output leg is not its drawn summer's share of the pair");

        // Each factor is a ratio of two parts of the class.
        const double t = Chorus::carbonResistorToleranceClass * calibration;
        const double bound = std::log((1.0 + t) / (1.0 - t)) + 1e-6;
        double widest = 0.0;
        for (float factor : { scale.dryLeft, scale.wetLeft, scale.dryRight, scale.wetRight })
        {
            require(factor != 1.0f && std::abs(std::log(factor)) <= bound,
                    "an output leg left the drawn resistor class");
            widest = std::max(widest, std::abs(std::log(static_cast<double>(factor))));
        }
        require(scale.dryLeft != scale.dryRight && scale.wetLeft != scale.wetRight,
                "the two outputs drew the same summer");
        require(widest > previous, "leg spread does not grow with Unit Character");
        previous = widest;
    }
    const auto unit = Chorus::summerLegScale(1.0f, true);
    std::cout << "IC6 legs at Character 1: left dry x" << unit.dryLeft << " wet x" << unit.wetLeft
              << ", right dry x" << unit.dryRight << " wet x" << unit.wetRight << '\n';
}

void reachesTheOutputs()
{
    // Nothing upstream of IC6 reads the flag, so the same input gives the
    // same wet line voltage in both runs: the nominal run's output recovers
    // it, and the drawn run must be the drawn summer applied to it.
    constexpr double rate = 48000.0;
    auto nominal = std::make_unique<Chorus>();
    auto drawn = std::make_unique<Chorus>();
    nominal->prepare(rate);
    drawn->prepare(rate);
    const auto scale = Chorus::summerLegScale(1.0f, true);
    double worst = 0.0, level = 0.0, wetPeak = 0.0;
    for (int n = 0; n < static_cast<int>(0.5 * rate); ++n)
    {
        const float input = 0.4f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 330.0 * n / rate));
        float l0 {}, r0 {}, l1 {}, r1 {};
        nominal->process(input, ChorusMode::Two, 0.0f, l0, r0, 1.0f, true, true, true,
                         ChorusTimingProfile::HardwareEvidence);
        drawn->process(input, ChorusMode::Two, 0.0f, l1, r1, 1.0f, true, true, true,
                       ChorusTimingProfile::HardwareEvidence,
                       false, false, false, 0.0f, 1.0f, false, false, false, false, false, true);
        const double g = Chorus::dryMixGain;
        const double wetLeft = l0 / g - input, wetRight = r0 / g - input;
        const double expectLeft = g * (input * scale.dryLeft + wetLeft * scale.wetLeft);
        const double expectRight = g * (input * scale.dryRight + wetRight * scale.wetRight);
        worst = std::max({ worst, std::abs(l1 - expectLeft), std::abs(r1 - expectRight) });
        level = std::max({ level, std::abs(static_cast<double>(l0)), std::abs(static_cast<double>(r0)) });
        wetPeak = std::max({ wetPeak, std::abs(wetLeft), std::abs(wetRight) });
    }
    require(wetPeak > 0.05, "the wet lines never reached the summer");
    require(worst < 2e-6 * level, "an output does not apply its own drawn legs");
}

void settledBypassFollowsTheLegs()
{
    // The settled bypass must equal the full path in mode Off, also after
    // Unit Character moves while the effect sits bypassed.
    constexpr double rate = 48000.0;
    auto full = std::make_unique<Chorus>();
    auto skipped = std::make_unique<Chorus>();
    full->prepare(rate);
    skipped->prepare(rate);
    int bypassed = 0;
    for (int n = 0; n < static_cast<int>(1.0 * rate); ++n)
    {
        const float calibration = n < static_cast<int>(0.6 * rate) ? 1.0f : 2.0f;
        const float input = 0.3f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 110.0 * n / rate));
        const ChorusMode mode = n < static_cast<int>(0.05 * rate) ? ChorusMode::One : ChorusMode::Off;
        float l0 {}, r0 {}, l1 {}, r1 {};
        const auto run = [&](Chorus& chorus, float& left, float& right) {
            chorus.process(input, mode, 0.0f, left, right, calibration, true, false, true,
                           ChorusTimingProfile::HardwareEvidence,
                           false, false, false, 0.0f, 1.0f, false, false, false, false, false, true);
        };
        run(*full, l0, r0);
        if (mode == ChorusMode::Off
            && skipped->processBypassedWhenSettled(input, l1, r1, calibration, true))
        {
            ++bypassed;
            require(l1 == l0 && r1 == r0, "settled bypass left an output's drawn dry leg");
        }
        else
            run(*skipped, l1, r1);
    }
    require(bypassed > static_cast<int>(0.4 * rate), "the bypass never settled across the change");
}
}

int main()
{
    try
    {
        legLaw();
        reachesTheOutputs();
        settledBypassFollowsTheLegs();
        std::cout << "Output summer legs passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
