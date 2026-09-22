#pragma once

#include <cstdint>

namespace youknow
{
// Engine-only calibration candidates; these are not host parameter ordinals
// and are not serialised. Nominal keeps existing sessions and service policy.
enum class MainNoiseCalibrationProfile : std::uint8_t
{
    Nominal,
    Serviced439522,
    CoreBandTp8
};

[[nodiscard]] constexpr float mainNoiseCalibrationScale(
    MainNoiseCalibrationProfile profile) noexcept
{
    switch (profile)
    {
        case MainNoiseCalibrationProfile::Serviced439522:
            // Reference candidate, not a replacement for Roland's 4 Vp-p TP8
            // trim: Lewis Francis's Juno-106 #439522 has original DCOs and
            // Borish VCF/VCA replacements; VR32, the TP8 crest convention and
            // recorder gain are not established.
            // https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/44
            //
            // AnalyzeHardwareNoise fits one scalar through the complete engine
            // to the May recording's 20 Hz--20 kHz noise/saw and
            // noise/true-selfosc ratios. Baseline 7ed16c4, Character 1,
            // Aging 0, 48 kHz/4x, shipping kernels: first disjoint 0.55 s
            // windows yield 2.8889713 (+9.2149 dB source gain). Held-out late
            // windows reduce ratio RMS error 9.163 -> 1.640 dB, leaving
            // noise/saw -1.526 and noise/selfosc +1.746 dB. The independently
            // recorded 29-point resonance sweep reduces normalized RMS error
            // 4.047 -> 0.461 dB; the untouched noise-control law gives
            // 0.099 -> 0.054 dB on its holdout. These digits reproduce the
            // fit, not a physical precision claim.
            //
            // This output-based fit includes the reference's installed/service
            // state. Use Aging 0 for its documented comparison; adding Aging is
            // a separate extrapolation. Refit after material VCF/VCA changes.
            // No spectrum, control shape, per-patch gain or original-card
            // population parameter was fitted.
            return 2.8889713f;
        case MainNoiseCalibrationProfile::CoreBandTp8:
            // The product's reading of Roland's own trim, chosen by ear on
            // 2026-09-22 (Docs/decisions.md) over a 3.45-sigma reading. The
            // p. 19 figure brackets its "4Vp-p" on the trace's dense core,
            // about 2.9 sigma; the nominal noiseMixVolts reads the same 4 Vpp
            // as the whole trace, 6.615 sigma (TP8 sigma 0.6047 V against the
            // 6 Vpp self-oscillation, bank 6 at FREQ 10). 6.615 / 2.9 = 2.281,
            // +7.16 dB on the shared source only; the C41/R79 spectrum and
            // the NOISE control law are unchanged. The crest reading is the
            // choice; #439522's 2.88 sigma corroborates it and is not fitted.
            return 2.281f;
        case MainNoiseCalibrationProfile::Nominal:
        default:
            return 1.0f;
    }
}

// The chorus's line hiss, on top of the Chorus Noise control. As above, not a
// host parameter and not serialised.
enum class ChorusNoiseCalibrationProfile : std::uint8_t
{
    Nominal,
    IdleFloor439522
};

[[nodiscard]] constexpr float chorusNoiseCalibrationScale(
    ChorusNoiseCalibrationProfile profile) noexcept
{
    // Hiss B, chosen by ear on 2026-09-22 (Docs/decisions.md): the hiss
    // matched to #439522's four hash-pinned April 2026 bank captures, whose
    // chorus board is original. The choice was the target; the number is
    // measured. Tools/AnalyzeChorusIdleFloors.py compares each patch's idle
    // floor with its own C4 note, which cancels the recording gain. Rendered
    // through the complete product -- drive B, noise B, Character 1,
    // 96 kHz/2x, shipping kernels -- 3.98 brings the Mode I mean over seven
    // tail-free patches to 0.00 dB (L -0.43, R +0.43; patches scatter by
    // about 3 dB with the clock sweep). The audition's +14.0 dB predates
    // drive B, which already lowers the notes into the chorus by 2.6 dB.
    // Level only, and the captured hiss is brighter: matched, this one runs
    // 9 dB hot across 0.5-2 kHz, 5.5 dB at 2-4 kHz, 2.5 dB at 4-8 kHz and
    // within 1 dB above 8 kHz. No mechanism for that tilt is modelled
    // (OQ-03). The two Mode II patches read +7.1/-2.1 dB, too few to
    // calibrate that mode.
    return profile == ChorusNoiseCalibrationProfile::IdleFloor439522
        ? 3.98f : 1.0f;
}
} // namespace youknow
