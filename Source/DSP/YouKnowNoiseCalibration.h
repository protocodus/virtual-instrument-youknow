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
            // With drive B as well, the product reads that unit's May take
            // noise/saw -1.21 dB and noise/true-selfosc -0.25 dB (-1.19 and
            // -0.28 held out), against -10.92 and -7.36 dB before either.
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
    // floor with its own C4 note, which cancels the recording gain.
    //
    // 3.98 was measured wrongly. It read 0.00 dB through a boxcar window,
    // which leaked the hardware idle floors' sub-20 Hz drift into the
    // 20 Hz-20 kHz band (+1.1 dB there, 0.08 dB on the model), and that band
    // also counts chorus-on energy below 200 Hz that is not hiss. Through the
    // fixed analyzer and the complete product -- drive B, noise B,
    // Character 1, 96 kHz/2x, shipping kernels -- 3.98 reads +1.33 dB over
    // the full band and +4.48 dB A-weighted (seven tail-free Mode I patches,
    // both channels; single patches spread over 9-13 dB with the clock
    // sweep). The corrected matches, 3.41 full-band and 2.37 A-weighted,
    // await a listening choice (2026-09-22).
    //
    // Level only, and the captured hiss is shaped differently: roughly flat
    // over 0.2-2 kHz, about 5 dB higher over 2-8 kHz and falling above,
    // where this one is white to the reconstruction roll-off. No mechanism
    // for that shape is modelled (OQ-03). The two tail-free Mode II patches
    // read L +4.7/+7.8 dB and R +10.2/+16.3 dB A-weighted, too few to
    // calibrate that mode.
    return profile == ChorusNoiseCalibrationProfile::IdleFloor439522
        ? 3.98f : 1.0f;
}
} // namespace youknow
