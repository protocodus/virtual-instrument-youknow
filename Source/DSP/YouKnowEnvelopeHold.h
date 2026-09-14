#pragma once

#include <algorithm>
#include <cmath>

namespace youknow
{
// IC26 C80..C87, '.01 x 8', followed by TL064 unity buffers. The six
// ENV/GATE channels precede (and are distinct from) each card's C58/Tr20.
// Roland JUNO-106 Service Notes, July 31 1984, module schematic p. 13:
// https://www.vintagesynthparts.com/wp-content/uploads/2017/03/JUNO-106_SERVICE_NOTES.pdf
// Constant R_ON + ideal source is a conditional comparison circuit. Installed
// R_ON, driver recovery and parasitics have not been identified. The Hitachi
// 15 V/25 C 80/280 ohm rows do not qualify the actual loaded circuit:
// https://akizukidenshi.com/goodsaffix/hd14051b_e.pdf#page=2
// The capacitor retains signed over/under-range voltage. The existing
// nonlinear C58/VCA model accepts only normalized control 0..1 and clamps
// its input to that domain; this comparison does not identify analog
// overload behavior beyond that calibration span. Its optional linear
// comparison is an ideal linear network, not an overrange transistor model.
class EnvelopeHoldCircuit
{
public:
    static constexpr double capacitanceFarads = 10.0e-9;

    struct Configuration
    {
        double onResistanceOhms { 0.0 }; // required: zero is not a profile
        double inputBiasAmps { 0.0 }; // signed current leaving the hold, always
        double offLeakageAmps { 0.0 }; // additional current while inhibited
        double turnOffChargeCoulombs { 0.0 }; // signed charge entering on inhibit

        [[nodiscard]] bool valid() const noexcept
        {
            // Engineering limits keep explicit pathological configurations
            // finite; these are NOT component tolerances or suggested values.
            return std::isfinite(onResistanceOhms)
                && onResistanceOhms >= 1.0 && onResistanceOhms <= 1.0e9
                && std::isfinite(inputBiasAmps) && std::abs(inputBiasAmps) <= 1.0e-6
                && std::isfinite(offLeakageAmps) && std::abs(offLeakageAmps) <= 1.0e-6
                && std::isfinite(turnOffChargeCoulombs)
                && std::abs(turnOffChargeCoulombs) <= 1.0e-9;
        }
    };

    // v(t) = constant + exponential*exp(-t/tau) + slope*t. Keeping this
    // trajectory lets the following C58 circuit consume the changing voltage
    // instead of sampling only its endpoint (which would apply charge early).
    struct Trajectory
    {
        double constant {}, exponential {}, slope {}, tau { 1.0 };
        [[nodiscard]] double at(double seconds) const noexcept
        {
            return constant + exponential * std::exp(-seconds / tau)
                 + slope * seconds;
        }

        // Exact response of an independent unit-gain one-pole to this input.
        // Used only by the engine's existing linear C58 comparison option.
        [[nodiscard]] double throughOnePole(double initial, double seconds,
                                             double poleSeconds) const noexcept
        {
            const double decay = std::exp(-seconds / poleSeconds);
            double transfer;
            if (std::abs(tau - poleSeconds) <= 1.0e-8 * poleSeconds)
                transfer = seconds / poleSeconds * decay;
            else if (tau < poleSeconds)
                transfer = decay * std::expm1(seconds * (1.0 / poleSeconds - 1.0 / tau))
                         / (1.0 - poleSeconds / tau);
            else
                transfer = -std::exp(-seconds / tau)
                         * std::expm1(seconds * (1.0 / tau - 1.0 / poleSeconds))
                         / (1.0 - poleSeconds / tau);
            return constant + (initial - constant) * decay
                + exponential * transfer
                + slope * (seconds + poleSeconds * std::expm1(-seconds / poleSeconds));
        }
    };

    void reset(double volts) noexcept { volts_ = volts; selected_ = false; bus_ = volts; }
    void select(double busVolts) noexcept { bus_ = busVolts; selected_ = true; }
    void inhibit(const Configuration& configuration) noexcept
    {
        if (selected_)
            volts_ += configuration.turnOffChargeCoulombs / capacitanceFarads;
        selected_ = false;
    }
    [[nodiscard]] Trajectory trajectory(const Configuration& configuration) const noexcept
    {
        if (!selected_)
            return { volts_, 0.0,
                     -(configuration.inputBiasAmps + configuration.offLeakageAmps)
                         / capacitanceFarads, 1.0 };
        const double asymptote = bus_ - configuration.inputBiasAmps
                                       * configuration.onResistanceOhms;
        return { asymptote, volts_ - asymptote, 0.0,
                 configuration.onResistanceOhms * capacitanceFarads };
    }
    void advance(const Trajectory& trajectory, double seconds) noexcept
    {
        volts_ = trajectory.at(seconds);
    }
    [[nodiscard]] double volts() const noexcept { return volts_; }
    [[nodiscard]] bool selected() const noexcept { return selected_; }

private:
    double volts_ {}, bus_ {};
    bool selected_ { false };
};
} // namespace youknow
