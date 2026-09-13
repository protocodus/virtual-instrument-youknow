// Check the shipping wet-input support against an independent two-node AC
// admittance solve, not against copies of the production state-space rows.
// Roland JUNO-106 Service Notes (July 31, 1984), jack board p. 15:
// https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=15
// C44/C47=100 nF, R120/R114=100 kOhm, R122/R115=10 kOhm,
// C52/C56=2.2 nF. The latter branch loads the coupling node. As in the
// production circuit boundary, treat the follower and bias source as ideal;
// this test cannot establish their unknown installed impedances or trim.
// At HQ rates compare to the continuous circuit; at lower rates compare to
// the same nodal circuit with the established independently prewarped reactive
// rates. No amplitude normalization, wet-gain fitting or noise is involved.
#include "DSP/YouKnowChorus.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <iomanip>
#include <iostream>
#include <limits>

namespace youknow
{
struct YouKnowTestAccess
{
    static float input(Chorus& chorus, float sample) noexcept
    {
        return chorus.advanceInputSupport(sample);
    }
    static void setAntiAliasEquilibrium(Chorus& chorus, float sample) noexcept
    {
        // At DC the two upstream lowpass outputs equal the source. Leave
        // C44/C52 cold so their full charging transient still has to settle.
        auto& state = chorus.inputSupport_;
        state.antiAliasFirst = { 0.0f, sample };
        state.antiAliasSecond = { 0.0f, sample };
    }
};
}

namespace
{
constexpr double pi = 3.14159265358979323846;
using Complex = std::complex<double>;

double warpedOmega(double frequency, double sampleRate)
{
    return 2.0 * sampleRate * std::tan(pi
        * std::clamp(frequency, 0.1, 0.45 * sampleRate) / sampleRate);
}

Complex expected(double frequency, double rate, bool loaded)
{
    const bool exact = rate >= youknow::Chorus::minimumExactInputSupportRate;
    const Complex s(0.0, exact ? 2.0 * pi * frequency
                               : warpedOmega(frequency, rate));
    const auto omega = [exact, rate](double corner) {
        return exact ? 2.0 * pi * corner : warpedOmega(corner, rate);
    };
    Complex sections(1.0, 0.0);
    for (const auto& section : {
             std::array<double, 3> { 9688.0, 820.0e-12, 680.0e-12 },
             std::array<double, 3> { 10377.0, 1.8e-9, 270.0e-12 } })
    {
        const double w = omega(section[0]);
        const double q = 0.5 * std::sqrt(section[1] / section[2]);
        sections *= w * w / (s * s + s * w / q + w * w);
    }

    constexpr double bias = 100000.0;
    constexpr double series = 10000.0;
    const double cc = exact ? 0.1e-6
        : 1.0 / (bias * omega(1.0 / (2.0 * pi * bias * 0.1e-6)));
    const double cp = exact ? 2.2e-9
        : 1.0 / (series * omega(1.0 / (2.0 * pi * series * 2.2e-9)));
    if (!loaded)
        return sections * s * bias * cc
             / ((1.0 + s * bias * cc) * (1.0 + s * series * cp));

    // KCL at the C44/R120/R122 junction and at the R122/C52 junction.
    const Complex firstAdmittance = s * cc + 1.0 / bias + 1.0 / series;
    const Complex secondAdmittance = s * cp + 1.0 / series;
    return sections * (s * cc / series)
         / (firstAdmittance * secondAdmittance - 1.0 / (series * series));
}

Complex measured(double frequency, double rate)
{
    youknow::Chorus chorus;
    chorus.prepare(rate);
    const auto count = static_cast<int>(rate);
    Complex sum {};
    for (int frame = 0; frame < 2 * count; ++frame)
    {
        const double phase = 2.0 * pi * frequency * frame / rate;
        const float output = youknow::YouKnowTestAccess::input(
            chorus, static_cast<float>(0.25 * std::cos(phase)));
        if (frame >= count)
            sum += static_cast<double>(output)
                 * Complex(std::cos(phase), -std::sin(phase));
    }
    return sum * (8.0 / count);
}
}

int main()
{
    int failures = 0;
    double worstDb = 0.0;
    double worstPhase = 0.0;
    for (double rate : { 32000.0, 44100.0, 48000.0, 96000.0,
                         176400.0, 192000.0, 384000.0, 768000.0 })
    {
        for (double frequency : { 15.0, 80.0, 400.0, 1000.0,
                                  4000.0, 8000.0, 10000.0 })
        {
            const Complex ratio = measured(frequency, rate)
                                / expected(frequency, rate, true);
            const double db = 20.0 * std::log10(std::abs(ratio));
            const double degrees = std::arg(ratio) * 180.0 / pi;
            worstDb = std::max(worstDb, std::abs(db));
            worstPhase = std::max(worstPhase, std::abs(degrees));
            // Causal cubic driving of the exact six-state transition leaves
            // a small finite-grid error at 10 kHz. These limits are still
            // over an order of magnitude below the missing loading term.
            if (!std::isfinite(db) || !std::isfinite(degrees)
                || std::abs(db) > 0.004 || std::abs(degrees) > 0.008)
            {
                std::cerr << "input AC mismatch rate=" << rate
                          << " frequency=" << frequency << " db=" << db
                          << " phase=" << degrees << '\n';
                ++failures;
            }
        }
    }

    // Extreme supported numerical grids must also decay without becoming
    // unstable, including the low-rate Nyquist clamps and corrupt input guard.
    for (double rate : { 8000.0, 768000.0 })
    {
        youknow::Chorus chorus;
        chorus.prepare(rate);
        double tail = 0.0;
        bool finite = true;
        for (int frame = 0; frame < static_cast<int>(rate); ++frame)
        {
            const float input = frame == 0 ? 1.0f : frame == 1
                ? std::numeric_limits<float>::quiet_NaN() : 0.0f;
            const float output = youknow::YouKnowTestAccess::input(chorus, input);
            finite = finite && std::isfinite(output);
            if (frame > static_cast<int>(rate * 0.5))
                tail = std::max(tail, std::abs(static_cast<double>(output)));
        }
        if (!finite || tail > 1.0e-8)
        {
            std::cerr << "input support decay failure rate=" << rate
                      << " tail=" << tail << '\n';
            ++failures;
        }
    }
    // C44 blocks DC in either discretization. Keep DC (the signed mean)
    // separate from zero-mean float-rounding ripple in the upstream lowpasses.
    // With unfused x86 arithmetic at 8 kHz, their cold-start limit cycle gives
    // a 2.92703e-8 peak but only -9.07e-12 mean: 0.98215 of one float ULP at
    // the 0.25 stimulus. A 1e-8 peak gate mistakes that ripple for DC leakage.
    // One representable float step is an explicit low-grid ripple budget;
    // the original 1e-8 DC gate, exact-path peak gate, AC and decay limits stay.
    // Starting just the upstream biquads at their known DC equilibrium
    // independently checks C44/C52's cold-start rejection without exciting
    // the float lowpasses' startup cycle. The coupling carries stay at zero.
    constexpr float dcInput = 0.25f;
    const double floatUlp = std::nextafter(dcInput,
        std::numeric_limits<float>::infinity()) - dcInput;
    double worstDcMean = 0.0;
    double worstColdRipple = 0.0;
    double worstEquilibriumPeak = 0.0;
    for (double rate : { 8000.0, 48000.0, 96000.0, 176400.0, 768000.0 })
    {
        for (bool equilibrium : { false, true })
        {
            if (equilibrium && rate >= youknow::Chorus::minimumExactInputSupportRate)
                continue; // The exact path has no upstream float biquads.
            youknow::Chorus chorus;
            chorus.prepare(rate);
            if (equilibrium)
                youknow::YouKnowTestAccess::setAntiAliasEquilibrium(chorus, dcInput);
            double peak = 0.0;
            double sum = 0.0;
            int count = 0;
            bool finite = true;
            for (int frame = 0; frame < static_cast<int>(rate); ++frame)
            {
                const float output = youknow::YouKnowTestAccess::input(chorus, dcInput);
                finite = finite && std::isfinite(output);
                if (frame >= static_cast<int>(rate * 0.5))
                {
                    peak = std::max(peak, std::abs(static_cast<double>(output)));
                    sum += output;
                    ++count;
                }
            }
            const double mean = sum / count;
            const double peakLimit = !equilibrium
                    && rate < youknow::Chorus::minimumExactInputSupportRate
                ? floatUlp : 1.0e-8;
            worstDcMean = std::max(worstDcMean, std::abs(mean));
            if (equilibrium)
                worstEquilibriumPeak = std::max(worstEquilibriumPeak, peak);
            else
                worstColdRipple = std::max(worstColdRipple, peak);
            if (!finite || !(std::abs(mean) < 1.0e-8) || !(peak <= peakLimit))
            {
                std::cerr << "input support DC failure rate=" << rate
                          << " equilibrium=" << equilibrium << " mean=" << mean
                          << " peak=" << peak << " peakLimit=" << peakLimit << '\n';
                ++failures;
            }
        }
    }
    std::cout << std::setprecision(8)
              << "8 DC cases: worst mean " << worstDcMean
              << ", cold peak " << worstColdRipple << " ("
              << worstColdRipple / floatUlp << " float ULP), equilibrium peak "
              << worstEquilibriumPeak << '\n';
    std::cout << std::setprecision(8)
              << "56 AC cases: worst magnitude error " << worstDb
              << " dB, worst phase error " << worstPhase << " degrees\n";
    for (double frequency : { 80.0, 400.0, 1000.0, 4000.0, 10000.0 })
        std::cout << "loading correction at " << frequency << " Hz: "
                  << 20.0 * std::log10(std::abs(
                         expected(frequency, 192000.0, true)
                         / expected(frequency, 192000.0, false))) << " dB\n";
    return failures == 0 ? 0 : 1;
}
