#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace youknow
{
// Roland module-board p.13: the controlled BA662 current drives R79 330k
// parallel C41 100pF, tau=33us. Keep that ONE physical pole and its charge.
// https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=13
//
// The former prewarped bilinear pole has a Nyquist zero: at 48k its extra
// loss against the analogue RC is 3.85dB at16k and 8.59dB at20k. Use the
// magnitude identity from Vicanek eqs.3-5, with p=exp(-dt/tau) FIXED by the
// physical discharge instead of fitting the digital pole:
// https://vicanek.de/articles/ShelvingFits.pdf
//   |H|^2=(1+B*phi)/(1+A*phi), A=2p/(1-p)^2, phi=1-cos(w).
// Unity DC gives b0+b1=1-p; matching the analogue magnitude at
// min(20k,.9 Nyquist) gives B=(M^2*(1+A*phi)-1)/phi and
// b0,b1=(1-p)/2*(1 +/- sqrt(1+2B)). This matching point is a numerical
// audio-band convention, not a component/level fit. Both weights are
// non-negative on the supported 8-768k grids. The stored voltage follows
// the exact undriven RC decay, including when tau is below one sample.
//
// Source interpolation/phase remains approximate. The two source weights
// describe a causal effective forcing over the interval, not an identified
// avalanche-junction spectrum or exact continuous input trajectory. Apply
// the interval's held OTA gain to BOTH source samples ahead of the charge
// update: zero gain removes all forcing, rather than scaling stored charge.
struct NoiseC41LowPass
{
    static constexpr double resistanceOhms = 330000.0;
    static constexpr double capacitanceFarads = 100e-12;

    struct Coefficients
    {
        double decay {};
        double currentWeight {};
        double previousWeight {};
    };

    [[nodiscard]] static Coefficients coefficients(double sampleRate) noexcept
    {
        const double wc = 1.0 / (resistanceOhms * capacitanceFarads * sampleRate);
        const double p = std::exp(-wc);
        const double sum = -std::expm1(-wc);
        const double a = 2.0 * p / (sum * sum);
        const double wm = std::min(0.9 * std::numbers::pi,
                                   2.0 * std::numbers::pi * 20000.0 / sampleRate);
        const double phi = 2.0 * std::pow(std::sin(0.5 * wm), 2.0);
        const double magnitudeSquared = 1.0 / (1.0 + (wm / wc) * (wm / wc));
        const double b = (magnitudeSquared * (1.0 + a * phi) - 1.0) / phi;
        const double root = std::sqrt(std::max(0.0, 1.0 + 2.0 * b));
        const double current = 0.5 * sum * (1.0 + root);
        return {p, current, sum - current};
    }

    void reset() noexcept { capacitorVoltage = previousSource = 0.0; }

    [[nodiscard]] float process(float source, float gain,
                                const Coefficients& c) noexcept
    {
        capacitorVoltage = c.decay * capacitorVoltage
            + static_cast<double>(gain)
                * (c.currentWeight * static_cast<double>(source)
                   + c.previousWeight * previousSource);
        previousSource = source; // C42 continues ahead of the OTA while muted.
        return static_cast<float>(capacitorVoltage);
    }

    double capacitorVoltage {};
    double previousSource {};
};
}
