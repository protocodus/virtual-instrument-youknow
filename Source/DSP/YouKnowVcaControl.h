#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace youknow
{
// Roland JUNO-106 module board, Service Notes p. 13:
// https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=13
// CV -- R106 10k -- C58 node -- R105 22k -- Tr20 emitter; base grounded.
// The previous fixed (10k || 22k)*C58 pole assumes zero transistor incremental
// resistance. At low current the actual resistance Vt/I cannot be neglected.
// This solver retains the existing voice VCA's ideal-junction/knee prior;
// it introduces no newly fitted transistor parameter or BA662 gain claim.
class VcaControlCircuit
{
public:
    static constexpr double inputOhms = 10000.0;
    static constexpr double emitterOhms = 22000.0;
    static constexpr double capacitanceFarads = 0.1e-6;
    static constexpr int tableSteps = 4096;

    VcaControlCircuit(double thermalVolts, double spanVolts, double knee) noexcept
        : kneeRegionEnd_(knee + 8.0 * thermalVolts / spanVolts),
          kneeStep_(0.5 * thermalVolts / spanVolts)
    {
        const double scale = spanVolts / thermalVolts;
        for (int index = 0; index <= tableSteps; ++index)
        {
            const double u = static_cast<double>(index) / tableSteps;
            const double v = (u - knee) * scale;
            double y = v > 1.0 ? v - std::log(v) : std::exp(v);
            for (int iteration = 0; iteration < 12; ++iteration)
            {
                const double delta = (y + std::log(y) - v) * y / (y + 1.0);
                y -= delta;
                if (std::abs(delta) <= 1.0e-15 * y)
                    break;
            }
            const double current = y * thermalVolts / (inputOhms + emitterOhms);
            // q is C58 voltage/span, apart from the constant +0.26V standoff.
            // u is the equivalent settled CV coordinate used by the unchanged
            // VoiceVcaControlLaw. This mapping preserves its exact DC gain.
            charge_[static_cast<std::size_t>(index)] = u - inputOhms * current / spanVolts;
            differential_[static_cast<std::size_t>(index)] =
                1.0 - inputOhms / (inputOhms + emitterOhms) * y / (1.0 + y);
        }
    }

    [[nodiscard]] double capacitorCoordinate(double control) const noexcept
    {
        return interpolate(control).value;
    }

    // q'(u) du/dt = (target-u)/(R106*C58), with q' in [22/32,1].
    // Fixed-cost RK4 uses the tabulated analytic differential, avoiding a
    // transistor solve or exponential in the callback. Engine intervals are
    // <=1/8000s: dt/tau <=0.182, inside its stable monotone operating range.
    // Fractional converter writes split the interval at their actual event.
    [[nodiscard]] double advance(double control, double target, double seconds) const noexcept
    {
        if (!(seconds > 0.0) || control == target)
            return control;
        // A large upward CV write can cross the narrow transistor knee in
        // one sample despite the RC itself being slow. Eight bounded local
        // substeps resolve that crossing; ordinary intervals keep one RK4
        // step. The trigger is expressed in thermal-voltage units, not tuned
        // to an audio reference. No unbounded adaptive work occurs here.
        const int steps = control < kneeRegionEnd_ && target > control
            && seconds * (target - control) / (inputOhms * capacitanceFarads) > kneeStep_
                ? 8 : 1;
        for (int step=0;step<steps;++step)
            control=advanceRk4(control,target,seconds/steps);
        return control;
    }

private:
    [[nodiscard]] double advanceRk4(double control, double target, double seconds) const noexcept
    {
        const double u0 = std::clamp(control, 0.0, 1.0);
        target = std::clamp(target, 0.0, 1.0);
        if (std::abs(target - u0) < 1.0e-14)
            return target;
        const auto derivative = [this,target](double u) {
            const double position=std::clamp(u,0.0,1.0)*tableSteps;
            const auto index=static_cast<std::size_t>(std::min(static_cast<int>(position),tableSteps-1));
            const double slope=differential_[index]+(position-static_cast<double>(index))
                *(differential_[index+1]-differential_[index]);
            return (target-u)/(inputOhms*capacitanceFarads*slope);
        };
        const double a=derivative(u0);
        const double b=derivative(u0+seconds*a*0.5);
        const double c=derivative(u0+seconds*b*0.5);
        const double d=derivative(u0+seconds*c);
        return std::clamp(u0+seconds*(a+2.0*b+2.0*c+d)/6.0,
                          std::min(u0,target),std::max(u0,target));
    }

    struct Sample { double value; double slope; };
    [[nodiscard]] Sample interpolate(double control) const noexcept
    {
        const double position = std::clamp(control, 0.0, 1.0) * tableSteps;
        const auto index = static_cast<std::size_t>(std::min(static_cast<int>(position), tableSteps - 1));
        const double difference = charge_[index + 1] - charge_[index];
        return { charge_[index] + (position - static_cast<double>(index)) * difference,
                 difference * tableSteps };
    }
    std::array<double, tableSteps + 1> charge_ {};
    std::array<double, tableSteps + 1> differential_ {};
    double kneeRegionEnd_;
    double kneeStep_;
};
} // namespace youknow
