#pragma once

#include <algorithm>
#include <array>

namespace youknow
{
// The VCF's C61 network is a continuous RC control, not a straight ramp
// between audio samples (Roland JUNO-106 Service Notes, module board p.13):
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=13
// The existing converter-event interval keeps its exact exponential mapping.
// Between events, reconstruct the already mapped omega endpoints with a
// causal cubic. This is a numerical approximation of that same control law,
// not another physical smoothing stage or a hardware-value calibration.
// Four points at {-2,-1,0,1} add no lookahead or delay. Reset at target/event
// boundaries, so none of the polynomial's history precedes its RC segment.
// Bounding to the two physical endpoints prevents extrapolation overshoot
// at the frequency cap or when endpoint float precision runs out.
struct CutoffReconstruction
{
    void reset() noexcept { count = 0; ready = false; }

    void advance(double endpoint, float heldTarget, bool boundary) noexcept
    {
        if (boundary || count == 0 || heldTarget != target)
            count = 0;
        target = heldTarget;
        const double oldest = history[2];
        history[2] = history[1];
        history[1] = history[0];
        history[0] = endpoint;
        count = std::min(count + 1, 4);
        const double a=history[0], b=history[1], c=history[2], d=oldest;
        // A cap departure, reversal, or more than one octave between any
        // adjacent endpoints is not a resolved smooth four-point segment.
        // Keep the established bounded linear fallback in those intervals.
        // The octave is a numerical admission limit, not a component value;
        // the exhaustive RC sweep includes the minimum 8 kHz engine grid.
        ready = count == 4 && (a-b)*(b-c)>0 && (b-c)*(c-d)>0
            && std::max(a,b)<=2.0*std::min(a,b)
            && std::max(b,c)<=2.0*std::min(b,c)
            && std::max(c,d)<=2.0*std::min(c,d);
        if (ready)
        {
            const double step=a-b, previous=b-c, older=c-d;
            coefficients[0]=step/3.0+5.0*previous/6.0-older/6.0;
            coefficients[1]=.5*(step-previous);
            coefficients[2]=(step-2.0*previous+older)/6.0;
        }
    }

    [[nodiscard]] double at(double before, double after, double position) const noexcept
    {
        if (position == 0.0) return before;
        if (position == 1.0) return after;
        const double linear = before + position * (after - before);
        if (!ready || before == after) return linear;
        // Difference-form Newton polynomial avoids cancellation between four
        // nearly equal cutoff endpoints; coefficients are built once/frame.
        const double value = before + position*(coefficients[0]
            +position*(coefficients[1]+position*coefficients[2]));
        return std::clamp(value, std::min(before, after), std::max(before, after));
    }

    std::array<double, 3> history {};
    std::array<double, 3> coefficients {};
    float target {};
    int count {};
    bool ready {};
};
} // namespace youknow
