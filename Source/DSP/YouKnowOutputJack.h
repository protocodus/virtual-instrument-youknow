#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace youknow
{
// Magnitude-matched discretization of the existing R64/R65--C22/C21 output
// pole. The circuit corner (including VOLUME wiper loading) is supplied by
// the engine, not fitted here. Roland JUNO-106 Service Notes, p. 15:
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf
//
// The former exponential one-pole preserves the sampled decay but aliases
// its impulse response: at 44.1 kHz it loses most of this above-Nyquist
// circuit's audible roll-off. Use the low-pass limit of Vicanek's matched
// one-pole design, eqs. 3--12 (2019):
// https://vicanek.de/articles/ShelvingFits.pdf
// |H|^2=(1+beta*phi)/(1+alpha*phi), phi=1-cos(w).
// Match unity DC, the analogue -1/wc^2 low-frequency curvature, and the
// analogue magnitude at min(20 kHz, 0.9 Nyquist). The paper's 0.9 Nyquist
// matching point is capped at the audited audio-band edge so high-rate hosts
// spend their approximation accuracy in the audible band. This is a numerical
// design boundary, not a component or hearing-threshold calibration.
// Consequently beta=2/wm^2-1/(1-cos(wm)), alpha=beta+2/wc^2.
// This is a minimum-phase magnitude approximation, not an exact capacitor
// trajectory/phase model. No extra physical pole or user-facing EQ is added.
struct OutputJackLowPass
{
    struct Coefficients
    {
        double a1 {};
        double correction {};
    };

    [[nodiscard]] static Coefficients coefficients(double cornerHz,
                                                    double sampleRate) noexcept
    {
        const double wm = std::min(0.9 * std::numbers::pi,
                                   2.0 * std::numbers::pi * 20000.0 / sampleRate);
        const double beta = 2.0 / (wm * wm) - 1.0 / (1.0 - std::cos(wm));
        const double wc = 2.0 * std::numbers::pi * cornerHz / sampleRate;
        const double alpha = beta + 2.0 / (wc * wc);
        const auto root = [](double value) {
            return -value / (1.0 + value + std::sqrt(1.0 + 2.0 * value));
        };
        const double a1 = root(alpha);
        const double b = root(beta);
        return { a1, (a1 - b) / (1.0 + b) };
    }

    void reset() noexcept { previousInput = difference = 0.0; }

    [[nodiscard]] float process(float input, const Coefficients& c) noexcept
    {
        // y=x+d, d=(b0-1)*(x-x_previous)-a1*d_previous. Unlike a
        // transposed direct form, stored values remain actual signal history
        // when VOLUME changes the coefficients. Constant input stays exactly
        // constant, and no small difference of near-unity gains is evaluated
        // per sample. The correction has zero DC and no added sample delay.
        difference = c.correction * (static_cast<double>(input) - previousInput)
                   - c.a1 * difference;
        previousInput = input;
        return static_cast<float>(static_cast<double>(input) + difference);
    }

    double previousInput {};
    double difference {};
};
} // namespace youknow
