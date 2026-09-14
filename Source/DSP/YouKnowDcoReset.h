#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace youknow
{
// Roland p.9 identifies a differentiated timer edge and discharge transistor
// across C54, but prints no internal pulse RC or discharge-path values.
// This explicit reduced comparison circuit is not a calibrated MC5534A model.
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=9
struct DcoResetCircuit
{
    struct Calibration
    {
        double gateSeconds { std::numeric_limits<double>::quiet_NaN() };
        double dischargeOhms { std::numeric_limits<double>::quiet_NaN() };
        double clampVolts { std::numeric_limits<double>::quiet_NaN() };

        [[nodiscard]] bool valid() const noexcept
        {
            // Numerical comparison domain, NOT manufacturer limits or a
            // proposed typical population. All three inputs are required.
            return std::isfinite(gateSeconds) && gateSeconds >= 1e-9 && gateSeconds <= 1e-3
                && std::isfinite(dischargeOhms) && dischargeOhms >= 1.0 && dischargeOhms <= 1e7
                && std::isfinite(clampVolts) && clampVolts >= 0.0 && clampVolts <= 15.0;
        }
    };

    // With the reset gate active: C*dV/dt=Ic-(V-Vclamp)/Rd. Charging current
    // continues to flow, so the endpoint is Vclamp+Ic*Rd, not forced zero.
    [[nodiscard]] static double voltage(double start, double asymptote,
                                        double tau, double seconds) noexcept
    {
        return start + (asymptote - start) * -std::expm1(-seconds / tau);
    }

    [[nodiscard]] static double integral(double start, double asymptote,
                                         double tau, double seconds) noexcept
    {
        return asymptote * seconds
             + (start - asymptote) * tau * -std::expm1(-seconds / tau);
    }

    // Centroid of exp(-t/tau) over [0,seconds]. Used to integrate a linear
    // reconstruction-table cell against the exact reset curvature weight.
    [[nodiscard]] static double curvatureCentroid(double tau, double seconds) noexcept
    {
        const double x = seconds / tau;
        if (x < 1e-3)
            return seconds * (0.5 - x / 12.0 + x * x * x / 720.0);
        return x > 50.0 ? tau : tau - seconds / std::expm1(x);
    }
};
}
