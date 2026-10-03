#pragma once

#include "YouKnowOutputJack.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>

namespace youknow
{
// Nominal post-IC6 output network, Service Notes pp.15-16: C17/C20 10uF,
// R54/R57 1.5k, dual VR1 10KB, IC7 input 1k+100k, the 33k/6.8k/1.5k
// selector ladder, R64/R65 2.2k and C22/C21 1nF. The source is ideal IC6;
// loaded IC6 clipping, pot tracking and receiver noise are not inferred.
// https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=15
// https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=16
//
// Eliminating only resistor nodes leaves a symmetric conductance two-port G
// between C17's right terminal and the jack. Both capacitors remain in the
// analogue transfer; C17 is never approximated as a short. Its source transfer
// factors into a low-frequency high-pass and a high-frequency low-pass. The
// internal resistors' exact Norton-current PSD is 4kT*G; the jack PSD factors
// into a low-frequency shelf followed by that same high-frequency low-pass.
// One independent white draw per channel therefore reproduces the complete
// stationary output-noise PSD. Cable/input capacitance is a shunt at the
// physical jack, added to C21/C22 rather than a separate post-output EQ. For
// example Canare GS-6 specifies 160 pF/m conductor-to-shield, so 3 m/6 m add
// 480 pF/960 pF; input capacitance, if known, adds to that declared load.
// https://www.canare.com/guitarinstrumentcable
// The external load is a noiseless termination:
// this models noise generated inside the instrument, not the receiver.
//
// Digital policy: TPT for the sub-audio pole/shelf, the existing magnitude-
// matched OutputJackLowPass for the upper pole. This is a magnitude model,
// not a guarantee of analogue phase or physical capacitor-charge trajectories
// on selector/load changes. Filter histories survive coefficient changes.
// The caller retains its separate High/open legacy path when compatibility
// is required; constructing this class does not select a product policy.
class OutputNetwork
{
public:
    enum class Selector { High, Medium, Low };

    struct Configuration
    {
        Selector selector { Selector::High };
        // 0 means open circuit. Positive values describe the external input
        // resistance, not a manufacturer-specified JUNO output termination.
        double loadOhms {};
        bool mono {};
        // Per connected cable/input: one on each stereo jack, or ONE on the
        // shared mono jack. 0 retains the original 1nF internal capacitors.
        double externalCapacitanceFarads {};
        bool operator==(const Configuration&) const = default;
    };

    struct Coefficients
    {
        double lowPoleHz {}, highPoleHz {}, passbandGain {};
        // Output PSD at temperature T is T*density^2 times the squared
        // transfer of a shelf (DC=noiseLowGain, HF=1) and the upper low-pass.
        double noiseLowGain {}, noiseDensityPerRootKelvin {};
    };

    // No allocation. Re-preparing preserves histories. The limits are
    // numerical domains, not component tolerances or receiver measurements.
    [[nodiscard]] bool prepare(double sampleRate, Configuration configuration) noexcept
    {
        if (!std::isfinite(sampleRate) || sampleRate < 8000.0 || sampleRate > 768000.0
            || !valid(configuration))
            return false;
        sampleRate_ = sampleRate;
        configuration_ = configuration;
        rebuild();
        return true;
    }

    [[nodiscard]] bool configure(Configuration configuration) noexcept
    {
        if (!valid(configuration)) return false;
        if (!(configuration == configuration_))
        {
            configuration_ = configuration;
            rebuild();
        }
        return true;
    }

    [[nodiscard]] bool setVolume(double position) noexcept
    {
        if (!std::isfinite(position)) return false;
        position = std::clamp(position, 0.0, 1.0);
        if (position != volume_)
        {
            volume_ = position;
            rebuild();
        }
        return true;
    }

    // Unit Character/amplitude policy belongs to the caller. Temperature is
    // Kelvin; zero temperature or amount disables noise without freezing its
    // filter history. Unit noise inputs to process() have variance one.
    [[nodiscard]] bool setNoise(double temperatureKelvin, double amount) noexcept
    {
        if (!std::isfinite(temperatureKelvin) || temperatureKelvin < 0.0
            || temperatureKelvin > 1000.0 || !std::isfinite(amount)
            || amount < 0.0 || amount > 16.0)
            return false;
        if (temperatureKelvin != temperatureKelvin_ || amount != noiseAmount_)
        {
            temperatureKelvin_ = temperatureKelvin;
            noiseAmount_ = amount;
            refreshNoiseScale();
        }
        return true;
    }

    void reset() noexcept { channels_ = {}; }

    [[nodiscard]] std::array<double, 2> process(
        double leftVolts, double rightVolts,
        double leftUnitNoise = 0.0, double rightUnitNoise = 0.0) noexcept
    {
        double left = step(channels_[0], finiteOrZero(leftVolts),
                           finiteOrZero(leftUnitNoise));
        double right = step(channels_[1], finiteOrZero(rightVolts),
                            finiteOrZero(rightUnitNoise));
        if (configuration_.mono)
            left = right = 0.5 * (left + right);
        return { left, right };
    }

    [[nodiscard]] const Configuration& configuration() const noexcept { return configuration_; }
    [[nodiscard]] const Coefficients& coefficients() const noexcept { return coefficients_; }
    [[nodiscard]] double volume() const noexcept { return volume_; }
    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }

    // Common-mode mono voltage response: both sources driven equally. A
    // single driven side contributes half this response at the mono jack.
    [[nodiscard]] std::complex<double> analogResponse(double frequencyHz) const noexcept
    {
        const std::complex<double> s { 0.0, twoPi * frequencyHz };
        const double low = twoPi * coefficients_.lowPoleHz;
        const double high = twoPi * coefficients_.highPoleHz;
        return coefficients_.passbandGain * (s / (s + low)) * (high / (s + high));
    }

    // One-sided V^2/Hz, internal resistors only, before the caller's Character
    // amount or PCM normalization. Mono combines independent branch noises.
    [[nodiscard]] double analogNoisePsd(double frequencyHz,
                                        double temperatureKelvin) const noexcept
    {
        const double w = twoPi * frequencyHz;
        const double low = twoPi * coefficients_.lowPoleHz;
        const double high = twoPi * coefficients_.highPoleHz;
        const double z = low * coefficients_.noiseLowGain;
        const double density = coefficients_.noiseDensityPerRootKelvin;
        return temperatureKelvin * density * density
             * (w * w + z * z) / (w * w + low * low)
             * high * high / (w * w + high * high)
             * (configuration_.mono ? 0.5 : 1.0);
    }

private:
    static constexpr double twoPi = 2.0 * std::numbers::pi;
    static constexpr double couplingFarads = 10e-6;
    static constexpr double jackFarads = 1e-9;
    static constexpr double boltzmann = 1.380649e-23;
    struct Channel
    {
        double signalLow {}, noiseLow {}, jackPreviousInput {}, jackDifference {};
    };
    Configuration configuration_ {};
    double sampleRate_ {48000.0}, volume_ {1.0};
    double temperatureKelvin_ {}, noiseAmount_ {}, noiseScale_ {}, lowG_ {};
    Coefficients coefficients_ {};
    OutputJackLowPass::Coefficients jackCoefficients_ {};
    std::array<Channel, 2> channels_ {};

    static bool valid(const Configuration& c) noexcept
    {
        return (c.selector == Selector::High || c.selector == Selector::Medium
                    || c.selector == Selector::Low)
            && std::isfinite(c.loadOhms)
            && (c.loadOhms == 0.0 || (c.loadOhms >= 1.0 && c.loadOhms <= 1e12))
            && std::isfinite(c.externalCapacitanceFarads)
            && c.externalCapacitanceFarads >= 0.0
            && c.externalCapacitanceFarads <= 100e-9;
    }

    static double finiteOrZero(double value) noexcept
    {
        return std::isfinite(value) ? value : 0.0;
    }

    void refreshNoiseScale() noexcept
    {
        noiseScale_ = coefficients_.noiseDensityPerRootKelvin
                    * std::sqrt(0.5 * sampleRate_ * temperatureKelvin_) * noiseAmount_;
    }

    void rebuild() noexcept
    {
        const double upper = 1500.0 + (1.0 - volume_) * 10000.0;
        const double lower = volume_ * 10000.0;
        const double a = configuration_.selector == Selector::High ? 0.0
                       : configuration_.selector == Selector::Medium ? 33000.0 : 39800.0;
        const double b = 41300.0 - a;
        const double gu = 1.0 / upper, gj = 1.0 / 2200.0;
        double gaa {}, gaj {}, gjj {};
        if (lower == 0.0)
        {
            // Wiper is an exact ground, not an epsilon resistor. The two
            // capacitor ports decouple; the selected ladder tap can still
            // have a finite noise resistance in Medium or Low.
            gaa = gu;
            gjj = 1.0 / (2200.0 + (a == 0.0 ? 0.0 : a * b / (a + b)));
        }
        else if (a == 0.0)
        {
            // Multiply through by lower rather than forming 1/lower: this
            // retains the grounded-wiper limit even for subnormal positions.
            const double inverseTotal = lower
                / (1.0 + lower * (gu + 1.0 / 101000.0 + 1.0 / b + gj));
            gaa = gu * (1.0 - gu * inverseTotal);
            gaj = -gu * gj * inverseTotal;
            gjj = gj * (1.0 - gj * inverseTotal);
        }
        else
        {
            // Schur complement of the wiper/tap resistor-node matrix.
            const double ga = 1.0 / a;
            const double d0WithoutLower = gu + 1.0 / 101000.0 + ga;
            const double d1 = ga + 1.0 / b + gj;
            const double denominator = d1 + lower * (d0WithoutLower * d1 - ga * ga);
            const double inverseDeterminant = lower / denominator;
            gaa = gu * (1.0 - gu * d1 * inverseDeterminant);
            gaj = -gu * gj * ga * inverseDeterminant;
            gjj = gj * (1.0 - gj * (1.0 + lower * d0WithoutLower) / denominator);
        }
        // With the jack nodes normalled together, symmetry separates common
        // and differential modes. Common mode is one branch with twice the
        // external RL and half the external capacitance, plus its own 1nF.
        // Two internal capacitors remain physical; the single external cable
        // is not doubled by connecting the jack nodes. Averaging branch outputs
        // gives the actual mono voltage and independent-noise power. This is
        // exact for equal pots/components; no physical channel mismatch is claimed.
        const double load = configuration_.loadOhms == 0.0 ? 0.0
            : 1.0 / (configuration_.loadOhms * (configuration_.mono ? 2.0 : 1.0));
        const double capacitance = jackFarads + configuration_.externalCapacitanceFarads
            / (configuration_.mono ? 2.0 : 1.0);
        const double determinant = gaa * (gjj + load) - gaj * gaj;
        const double sum = gaa / couplingFarads + (gjj + load) / capacitance;
        const double product = determinant / (couplingFarads * capacitance);
        const double high = 0.5 * (sum + std::sqrt(std::max(0.0, sum * sum - 4.0 * product)));
        const double low = product / high; // avoids subtracting nearly equal roots
        const double noiseZero = std::sqrt(gaa * (gaa * gjj - gaj * gaj) / gjj)
                               / couplingFarads;
        coefficients_ = { low / twoPi, high / twoPi, -gaj / (capacitance * high),
                          noiseZero / low, std::sqrt(4.0 * boltzmann * gjj)
                              / (capacitance * high) };
        lowG_ = std::tan(std::numbers::pi * coefficients_.lowPoleHz / sampleRate_);
        jackCoefficients_ = OutputJackLowPass::coefficients(coefficients_.highPoleHz, sampleRate_);
        refreshNoiseScale();
    }

    double step(Channel& state, double input, double unitNoise) noexcept
    {
        const double signalV = (input - state.signalLow) * lowG_ / (1.0 + lowG_);
        const double signalLow = state.signalLow + signalV;
        state.signalLow = signalLow + signalV;
        const double signal = (input - signalLow) * coefficients_.passbandGain;
        const double white = unitNoise * noiseScale_;
        const double noiseV = (white - state.noiseLow) * lowG_ / (1.0 + lowG_);
        const double noiseLow = state.noiseLow + noiseV;
        state.noiseLow = noiseLow + noiseV;
        const double mixed = signal + white + (coefficients_.noiseLowGain - 1.0) * noiseLow;
        // Double-precision form of OutputJackLowPass's signal-history update.
        // The coefficients and numerical matching policy are shared exactly.
        state.jackDifference = jackCoefficients_.correction * (mixed - state.jackPreviousInput)
                            - jackCoefficients_.a1 * state.jackDifference;
        state.jackPreviousInput = mixed;
        const double output = mixed + state.jackDifference;
        if (!std::isfinite(output)) { state = {}; return 0.0; }
        return output;
    }
};
} // namespace youknow
