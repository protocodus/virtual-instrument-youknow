#pragma once

#include <cstdint>

namespace youknow
{
// Engine-only calibration candidates; these are not host parameter ordinals
// and are not serialised. Nominal keeps existing sessions and service policy.
enum class MainNoiseCalibrationProfile : std::uint8_t
{
    Nominal,
    Serviced439522
};

[[nodiscard]] constexpr float mainNoiseCalibrationScale(
    MainNoiseCalibrationProfile profile) noexcept
{
    // Reference candidate, not a replacement for Roland's 4 Vp-p TP8 trim:
    // Lewis Francis's Juno-106 #439522 has original DCOs and Borish VCF/VCA
    // replacements; VR32, the TP8 crest convention and recorder gain are not
    // established. https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/44
    //
    // AnalyzeHardwareNoise fits one scalar through the complete engine to the
    // May recording's 20 Hz--20 kHz noise/saw and noise/true-selfosc ratios.
    // Baseline 7ed16c4, Character 1, Aging 0, 48 kHz/4x, shipping kernels:
    // first disjoint 0.55 s windows yield 2.8889713 (+9.2149 dB source gain).
    // Held-out late windows reduce ratio RMS error 9.163 -> 1.640 dB, leaving
    // noise/saw -1.526 and noise/selfosc +1.746 dB. The independently recorded
    // 29-point resonance sweep reduces normalized RMS error 4.047 -> 0.461 dB;
    // the untouched noise-control law gives 0.099 -> 0.054 dB on its holdout.
    // These digits reproduce the fit, not a physical precision claim.
    //
    // This output-based fit includes the reference's installed/service state.
    // Use Aging 0 for its documented comparison; adding Aging is a separate
    // extrapolation. Refit after material VCF/VCA changes. No spectrum, control
    // shape, per-patch gain or original-card population parameter was fitted.
    return profile == MainNoiseCalibrationProfile::Serviced439522
        ? 2.8889713f : 1.0f;
}
} // namespace youknow
