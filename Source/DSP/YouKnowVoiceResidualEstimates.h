#pragma once

namespace youknow
{
// Conservative engineering priors for a serviced instrument at Character 1.
// These are not measured original-card populations or confidence intervals.
// Keep acceptance limits, component classes and the chosen residuals distinct.
// Character above 1 remains an intentional product exaggeration.
struct VoiceResidualEstimates
{
    // Roland's two VCF service checkpoints accept +/-10 cents. Use one quarter
    // of that window for each fixed residual; the distribution is a prior.
    static constexpr float vcfServiceAcceptanceCents = 10.0f;
    static constexpr float vcfServiceResidualCents = 2.5f;
    // Existing AR(1) state/cadence are unchanged. Eight converter counts per
    // state unit imply about 0.485 cents RMS at Character 1, not DCO drift.
    static constexpr float cutoffWanderCounts = 8.0f;
    // Additive normalized CV-coordinate residual BEFORE the nonlinear RES
    // law and endpoint clamp. This is not a feedback-gain percentage.
    static constexpr float resonanceControlFraction = 0.002f;
    // Relative VCA input-trimmer gain and aggregate normalized control error.
    // Neither is an untrimmed resistor tolerance or a measured offset spread.
    static constexpr float vcaInputTrimFraction = 0.01f;
    static constexpr float vcaControlFraction = 0.001f;
    // AS3109's typical 0.33%/C is a replacement-part control coefficient,
    // not installed original-card drift. The board has PTC compensation;
    // retain only 10% as an explicit 90%-compensation estimate. Its accuracy
    // is unknown; do not call this an original 80017A temperature bound.
    static constexpr float vcfReferenceTempcoPerCelsius = 0.0033f;
    static constexpr float vcfResidualTempcoPerCelsius = 0.00033f;
    static_assert(vcfServiceResidualCents <= vcfServiceAcceptanceCents,
                  "service residual must fit the acceptance window");
};
} // namespace youknow
