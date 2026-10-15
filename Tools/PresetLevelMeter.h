// Product-level measurement only; this filter never touches engine output.
// At 96 kHz, an analog-model resonance near 45.6 kHz belongs in the native
// peak/full-band diagnostics but cannot be compared with a 48 kHz level target.
// Put every 96 kHz preset on that same measurement grid before applying the
// existing 400 ms / 100 ms, -20 dB relative gate. Lower rates retain their
// original sample arithmetic. This is not LUFS or an equal-loudness weighting.
#pragma once

#include "OversamplingQualitySupport.h"

namespace youknow::preset_level_meter
{
inline double decibels(double amplitude)
{
    return 20.0 * std::log10(std::max(1.0e-12, amplitude));
}

inline double gatedDb(const std::vector<double>& energy, double rate)
{
    const auto window = static_cast<std::size_t>(std::llround(rate * 0.4));
    const auto hop = static_cast<std::size_t>(std::llround(rate * 0.1));
    std::vector<double> windows;
    for (std::size_t start = 0; start + window < energy.size(); start += hop)
        windows.push_back(std::max(0.0, (energy[start + window] - energy[start]) / window));
    if (windows.empty()) throw std::invalid_argument("Preset meter needs at least 400 ms");
    const double loudest = *std::max_element(windows.begin(), windows.end());
    double sum = 0.0;
    int count = 0;
    for (double square : windows)
        if (square >= std::max(1.0e-14, loudest * 0.01)) { sum += square; ++count; }
    return decibels(count ? std::sqrt(sum / count) : 0.0);
}

inline const oversampling_quality::KaiserLowPass& referenceFilter()
{
    // Reuse the independently qualified audit FIR designer. 257 taps, beta
    // 8.6, a 22 kHz sinc cutoff, and a unity DC normalization. Tests measure
    // the actual 0..20 kHz passband and 24..48 kHz stopband, rather than relying
    // on the Kaiser order estimate. The attenuation value selects beta=8.6.
    static const auto filter = oversampling_quality::designKaiserLowPass(
        { 96000.0, 20000.0, 24000.0, 8.6 / 0.1102 + 8.7, 257 });
    return filter;
}

inline std::vector<double> to48k(const std::vector<double>& source)
{
    const auto& filter = referenceFilter();
    const auto delay = filter.groupDelayInputFrames;
    // The audit decimator returns only fully supported centres. Explicit zero
    // extension supplies the finite recording's boundaries and flushes the
    // tail. After removing the padded timeline offset, its first centre is
    // original frame zero and the last is original frame N-2 for even N.
    // Thus the 128-input-frame FIR delay neither shifts nor shortens a score.
    // This matches centred FIR/polyphase resampling with constant-zero padding:
    // https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.resample_poly.html
    std::vector<double> padded(source.size() + 2 * delay, 0.0);
    std::copy(source.begin(), source.end(), padded.begin() + static_cast<std::ptrdiff_t>(delay));
    return oversampling_quality::decimateToHostBoundary(padded, 2, filter).samples;
}

struct Result
{
    double gatedRmsDb {};
    double nativeGatedRmsDb {};
    double nativePeakDb {};
    double measurementSampleRate {};
    std::size_t measurementFrames {};
};

class StereoMeter
{
public:
    explicit StereoMeter(double rate, std::size_t reserveFrames = 0) : rate_(rate)
    {
        if (rate != 44100.0 && rate != 48000.0 && rate != 96000.0)
            throw std::invalid_argument("Unsupported preset-meter sample rate");
        energy_.reserve(reserveFrames + 1);
        if (rate_ == 96000.0)
        {
            left_.reserve(reserveFrames);
            right_.reserve(reserveFrames);
        }
    }

    void add(double left, double right)
    {
        if (!std::isfinite(left) || !std::isfinite(right))
            throw std::invalid_argument("Nonfinite preset-meter sample");
        // Preserve the previous native accumulation and peak arithmetic.
        peak_ = std::max({peak_, std::abs(left), std::abs(right)});
        energy_.push_back(energy_.back() + 0.5 * (left*left + right*right));
        if (rate_ == 96000.0)
        {
            left_.push_back(left);
            right_.push_back(right);
        }
    }

    Result finish() const
    {
        const double native = gatedDb(energy_, rate_);
        if (rate_ != 96000.0)
            return { native, native, decibels(peak_), rate_, energy_.size() - 1 };
        const auto left = to48k(left_);
        const auto right = to48k(right_);
        std::vector<double> energy { 0.0 };
        energy.reserve(left.size() + 1);
        for (std::size_t i = 0; i < left.size(); ++i)
            energy.push_back(energy.back() + 0.5 * (left[i]*left[i] + right[i]*right[i]));
        return { gatedDb(energy, 48000.0), native, decibels(peak_), 48000.0, left.size() };
    }

private:
    double rate_ {};
    double peak_ {};
    std::vector<double> energy_ { 0.0 };
    std::vector<double> left_;
    std::vector<double> right_;
};
} // namespace youknow::preset_level_meter
