#pragma once

#include <algorithm>
#include <cmath>

namespace youknow
{
// Numerical reconstruction of the existing C58/Tr20 current, not a new
// envelope law. Roland module-board p.13: R106=10k, R105=22k, C58=.1uF.
// https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=13
// With q=(V_C58-.26)/span and u=q+R106*Ie/span, KCL gives
// dq/dt=(target-u)/(R106*C58). Implicit differentiation of
// R105*Ie+Vt*log(Ie/Is)=.26+span*q gives
// dG/dt=G*span*(target-u)/[(R106*C58)*(Vt+R105*Ie)], G=Ie/Iservice.
// Existing calibrated current endpoints and the physical converter timestamp
// supply a monotone Hermite interval. No transistor parameter is fitted here.
struct VcaControlReconstruction
{
    float startGain {}, startSlope {}, endSlope {};
    float eventGain {}, eventBeforeSlope {}, eventAfterSlope {};
    // -1: unavailable, 2: no converter event, otherwise its interval position.
    float eventPosition { -1.0f };

    [[nodiscard]] bool valid() const noexcept { return eventPosition >= 0.0f; }

    // Slopes are expressed per whole internal sample, not per second.
    [[nodiscard]] static double gainSlope(double gain, double control,
        double target, double intervalSeconds, double spanVolts,
        double thermalVolts, double serviceEmitterAmps) noexcept
    {
        if (!(gain > 0.0)) return 0.0;
        constexpr double inputTime = 10000.0 * .1e-6;
        return gain * spanVolts * (target - control) * intervalSeconds
            / (inputTime * (thermalVolts + 22000.0 * gain * serviceEmitterAmps));
    }

    [[nodiscard]] static double segment(double before, double after,
        double beforeSlope, double afterSlope, double begin, double end,
        double position) noexcept
    {
        const double width = end - begin;
        if (!(width > 0.0) || position >= end) return after;
        if (position <= begin) return before;
        const double delta = after - before;
        if (delta == 0.0) return after;
        // Keep the existing physical gain's monotone bounds even at the
        // declared off-current boundary or under coarse-grid knee crossings.
        // Each derivative has the correct sign and cannot exceed 3*delta;
        // this sufficient Hermite condition prevents new gain overshoot.
        const auto bounded = [delta, width](double slope) noexcept {
            const double normalized = slope * width / delta;
            return std::clamp(normalized, 0.0, 3.0) * delta;
        };
        const double a = bounded(beforeSlope);
        const double b = bounded(afterSlope);
        const double x = (position - begin) / width;
        const double curved = before + x * (a + x * (3.0 * delta - 2.0 * a - b
            + x * (-2.0 * delta + a + b)));
        return std::clamp(curved, std::min(before, after), std::max(before, after));
    }

    [[nodiscard]] double sample(double endpointGain, double position) const noexcept
    {
        if (eventPosition >= 0.0f && eventPosition <= 1.0f)
        {
            const double event = eventPosition;
            if (position < event)
                return segment(startGain, eventGain, startSlope, eventBeforeSlope,
                    0.0, event, position);
            return segment(eventGain, endpointGain, eventAfterSlope, endSlope,
                event, 1.0, position);
        }
        return segment(startGain, endpointGain, startSlope, endSlope,
            0.0, 1.0, position);
    }
};
} // namespace youknow
