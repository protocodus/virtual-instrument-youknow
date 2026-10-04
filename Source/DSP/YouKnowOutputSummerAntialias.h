#pragma once

#include "YouKnowVoiceVcaAntialias.h"

namespace youknow
{
// Roland JUNO-106 jack-board IC6, TA75558S, p15. Its existing static
// clipping policy expands bandwidth before the sourced slew/GBW response.
// Direct evaluation on a coarse grid folds ultrasonic harmonics into audio;
// the physical 3MHz/noise-gain pole cannot remove already-folded components.
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=15
// https://datasheet.datasheetarchive.com/originals/scans/Scans-99/DSAIHSC000102822.pdf#page=3
// Numerical reconstruction/antialiasing, not a different analogue knee:
// https://www.dafx.de/paper-archive/2016/dafxpapers/20-DAFx-16_paper_41-PN.pdf
//
// Reuse the separately qualified polyphase kernel (4x/2x/direct, 48 input
// samples roundtrip). L/R retain independent histories. The wet-switch
// conductance belongs to this SAME delayed IC6 timestamp: downstream noise
// gain, bandwidth and resistor-power selection must not see a future gate.
// Context delay is numerical, not another transistor/RC switching delay.
// Product/reference selection is diagnostic, not a host/preset control.
// Changing it resets FIR/context and latency padding, like the VCA selector;
// the disabled path incurs no reconstruction cost. Live quality changes reset
// numerical state at the engine's existing zero-gain rebuild boundary.
struct OutputSummerAntialias
{
    struct Context { double wetRatio {}; bool wetConnected {}; };
    struct Result { float left {}, right {}; Context context {}; };
    VoiceVcaAntialias left {}, right {};
    std::array<Context, VoiceVcaAntialias::inputRingSize> contexts {};
    int write {};

    void reset() noexcept { *this = {}; }

    template<class Shape>
    [[nodiscard]] Result process(float l, float r, Context context,
        const VoiceVcaAntialias::Kernel& kernel, Shape&& shape) noexcept
    {
        contexts[static_cast<std::size_t>(write)] = context;
        const int delay = kernel.factor > 1 ? VoiceVcaAntialias::delaySamples : 0;
        const auto delayed = contexts[static_cast<std::size_t>(
            (write-delay+VoiceVcaAntialias::inputRingSize)
            & (VoiceVcaAntialias::inputRingSize-1))];
        write = (write+1) & (VoiceVcaAntialias::inputRingSize-1);
        return {left.process(l,1,kernel,shape), right.process(r,1,kernel,shape), delayed};
    }
};
}
