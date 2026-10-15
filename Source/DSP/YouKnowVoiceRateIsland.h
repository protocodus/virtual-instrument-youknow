#pragma once

#include "YouKnowVoiceVcaAntialias.h"

#include <array>
#include <cstdint>

namespace youknow
{
// Transport for a stateful voice circuit inside the existing VCA FIR pair.
// The interpolation midpoint is 24 input samples old; decimation supplies
// the other 24 samples of the already reported 48-sample roundtrip delay.
// Moving a circuit inside this pair must also move its control timestamps.
// No component law, physical smoothing, noise density or extra delay lives
// here. The caller owns the circuit and its rate-dependent coefficients.
//
// A context describes the physical interval ending at its input frame. The
// phase-zero callback ends the last quarter/half of that delayed interval;
// subsequent callbacks occupy the NEXT interval. Treating all phases as
// belonging to the delayed frame would put converter events one frame late.
// The existing seven-node VCF trajectory can therefore be retained in Context
// and evaluated on [begin,end] without flattening it into a current endpoint.
template <class Context>
struct VoiceRateIsland
{
    static constexpr int lookBack = VoiceVcaAntialias::delaySamples / 2;
    // Only the 24-frame interpolation delay plus adjacent endpoints is
    // required for controls. Reusing the audio ring's64 slots would double
    // every voice's context storage for no chronology benefit.
    static constexpr int ringSize = 32;
    static_assert(ringSize > lookBack + 1);

    struct Interval
    {
        const Context& previous;
        const Context& current;
        double begin {};
        double end {};
        int factor {1};
        std::int64_t frame {};

        [[nodiscard]] double interpolate(double before, double after) const noexcept
        { return end == 1.0 ? after : before + end * (after - before); }
    };

    // Numerical context history is deliberately separate from physical
    // capacitor state. A zero-gain rate rebuild resets this history and the
    // FIR, while the caller retains/re-times its actual circuit state.
    void reset(const Context& prehistory = {}) noexcept
    {
        contexts.fill(prehistory);
        frames = 0;
    }

    template <class Process>
    [[nodiscard]] float process(float input, const Context& context,
        const VoiceVcaAntialias::Kernel& kernel,
        VoiceVcaAntialias& transport, Process&& circuit) noexcept
    {
        // Gain is owned by circuit(), so this transport's otherwise delayed
        // gain must be unity even during FIR priming. Zero gain history would
        // apply a second, unrelated startup envelope to the circuit output.
        if (frames == 0)
            transport.gains.fill(1.0f);
        const int write = static_cast<int>(frames & (ringSize - 1));
        contexts[static_cast<std::size_t>(write)] = context;
        int phase = 0;
        const auto shape = [&](float reconstructed) noexcept {
            const int delay = kernel.factor > 1 ? lookBack : 0;
            const int next = phase == 0 ? 0 : 1;
            const int index = (write - delay + next + ringSize) & (ringSize - 1);
            const int previous = (index - 1 + ringSize) & (ringSize - 1);
            const double reciprocal = 1.0 / kernel.factor;
            const double begin = kernel.factor == 1 ? 0.0
                : (phase == 0 ? kernel.factor - 1 : phase - 1) * reciprocal;
            const double end = phase == 0 ? 1.0 : phase * reciprocal;
            const Interval interval {contexts[static_cast<std::size_t>(previous)],
                contexts[static_cast<std::size_t>(index)], begin, end,
                kernel.factor, frames - delay + next};
            ++phase;
            return circuit(reconstructed, interval);
        };
        // Gain and all output-current sources belong in circuit(), after
        // its stateful VCF/C59 solve and before this shared decimation FIR.
        // Keeping the existing transport also retains its exact tap/order
        // conventions; the disabled engine path need not change at all.
        const float output = transport.process(input, 1.0f, kernel, shape);
        ++frames;
        return output;
    }

    std::array<Context, ringSize> contexts {};
    std::int64_t frames {};
};
} // namespace youknow
