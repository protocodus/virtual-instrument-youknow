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
    // Hiss B's captured-level target was chosen on 2026-09-22; the
    // gain was corrected on 2026-09-28 (Docs/decisions.md).
    // Tools/AnalyzeChorusIdleFloors.py compares the A-weighted idle floor
    // with each patch's unweighted C4 note, cancelling recording gain.
    // Its Hann window rejects the sub-20 Hz drift that biased the former
    // boxcar measurement. A-weighting selects audible hiss rather than the
    // additional low-frequency chorus-on energy in these captures; it is
    // also the measure used by Panasonic's noise row and HISS-100 policy.
    //
    // The four hash-pinned April captures of #439522's original chorus board
    // contain seven tail-free Mode I patches. The paired comparison uses
    // Character 1, 96 kHz/2x, shipping kernels and the complete product path
    // before the separate pulse-level correction: x3.98 reads +4.48 dB
    // A-weighted over both channels; x2.37 reads +0.03 dB. That paired render
    // reads +0.51 dB with the pulse correction lowering the note reference,
    // with the same idle floor. Later circuit changes retain this source
    // scalar rather than silently refitting it to absorb their attenuation.
    // This rounded, single-unit level calibration
    // is not a typical MN3009 noise voltage or a population estimate.
    //
    // Level only. A separate Welch/Hann density audit (0.2 s, 50% overlap)
    // reads mean PSD 4.75 dB higher in 2-8 kHz than 0.2-2 kHz across the
    // same seven Mode I patches, versus -1.17 dB for the finite-support
    // product. These are per-hertz densities: integrated band powers differ
    // by another 5.23 dB solely from their unequal bandwidths. The stricter
    // RELEASE<=11 subset reads +4.76 dB, so release tails do not explain it.
    // Transfer-noise correlations are a supported mechanism candidate, but
    // their mixture with storage/output noise remains unidentified (OQ-03).
    // No spectral correction is hidden in this scalar. The two Mode II patches
    // are too few for a separate fit; its existing relative lift is kept.
    return profile == ChorusNoiseCalibrationProfile::IdleFloor439522
        ? 2.37f : 1.0f;
}
} // namespace youknow
