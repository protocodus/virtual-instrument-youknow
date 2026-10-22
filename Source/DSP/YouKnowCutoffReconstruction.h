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
    void reset() noexcept { count = 0; }

    void push(double endpoint, float heldTarget, bool boundary) noexcept
    {
        if (boundary || count == 0 || heldTarget != target)
            count = 0;
        target = heldTarget;
        history[2] = history[1];
        history[1] = history[0];
        history[0] = endpoint;
        count = std::min(count + 1, 4);
    }

    [[nodiscard]] double at(double before, double after, double position) const noexcept
    {
        if (position == 0.0) return before;
        if (position == 1.0) return after;
        const double linear = before + position * (after - before);
        if (count < 4 || before == after) return linear;
        // push() has retained current, previous and previous2; previous3 is
        // carried separately to avoid moving a larger island-context ring.
        const double a = after, b = before, c = history[2], d = oldest;
        // A cap departure, reversal, or more than one octave between any
        // adjacent endpoints is not a resolved smooth four-point segment.
        // Keep the established bounded linear fallback in those intervals.
        // The octave is a numerical admission limit, not a component value;
        // the exhaustive RC sweep includes the minimum 8 kHz engine grid.
        if ((a-b)*(b-c) <= 0.0 || (b-c)*(c-d) <= 0.0
            || std::max(a,b) > 2.0*std::min(a,b)
            || std::max(b,c) > 2.0*std::min(b,c)
            || std::max(c,d) > 2.0*std::min(c,d))
            return linear;
        const double value = a * (position * (position + 1.0) * (position + 2.0) / 6.0)
            - b * ((position - 1.0) * (position + 1.0) * (position + 2.0) / 2.0)
            + c * ((position - 1.0) * position * (position + 2.0) / 2.0)
            - d * ((position - 1.0) * position * (position + 1.0) / 6.0);
        return std::clamp(value, std::min(before, after), std::max(before, after));
    }

    void advance(double endpoint, float heldTarget, bool boundary) noexcept
    {
        oldest = history[2];
        push(endpoint, heldTarget, boundary);
    }

    std::array<double, 3> history {};
    double oldest {};
    float target {};
    int count {};
};
} // namespace youknow
