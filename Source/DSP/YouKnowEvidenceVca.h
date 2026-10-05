#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace youknow
{
// Conditional nominal Tr20/BA662 calibration, not an original-unit fit.
// Roland p.13: R106 10k, R105 22k and grounded-base Tr20; p.18 sets
// TP7 to +.25...+.27V. Tr20 is one of the listed 2SA1015-class PNPs.
// Reuse the named 25C Toshiba specimen already used by Tr18/Tr22:
// beta200, Vbe=.61V at Ic=.6509655627mA. Its extrapolation to quiet currents
// replaces the compatibility knee, not the ROM envelope or a measured
// original BA662 low-current-gm law. Current-mirror ratio remains 1:1.
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=13
// https://media.digikey.com/PDF/Data%20Sheets/Toshiba%20PDFs/2SA1015.pdf#page=2
class EvidenceVcaCalibration
{
public:
    static constexpr double thermalVolts = 1.380649e-23 * 298.15 / 1.602176634e-19;
    static constexpr double beta = 200.0;
    static constexpr double referenceVbe = .61;
    static constexpr double referenceCollectorAmps = .0006509655627366167;
    static constexpr double standoffVolts = .26;
    static constexpr double inputOhms = 10000;
    static constexpr double emitterOhms = 22000;
    static constexpr double outputLoadOhms = 47000;
    static constexpr int tableSteps = 4096;

    explicit EvidenceVcaCalibration(double spanVolts) noexcept : span_(spanVolts)
    {
        peakEmitterAmps_ = emitterAmps(1);
        for (int index = 0; index <= tableSteps; ++index)
            gain_[static_cast<std::size_t>(index)] = static_cast<float>(
                emitterAmps(static_cast<double>(index) / tableSteps) / peakEmitterAmps_);
        // Bank3 stored SUSTAIN reaches physical4064, ENV peaks at4095.
        // Set the fixed input divider to the actual service current and
        // TP19=2.4Vpeak -> TP8=3Vpeak, preserving the bare-pair tanh law.
        const double sustainCurrent = collectorAmps(4064.0 / 4095.0);
        headroom_ = 2.4 / std::atanh(3.0 / (outputLoadOhms * sustainCurrent));
        serviceGain_ = 3.0 / (headroom_ * std::tanh(2.4 / headroom_)
                            * gain(4064.0f / 4095.0f));
    }

    [[nodiscard]] double emitterAmps(double control) const noexcept
    {
        const double supply = standoffVolts + std::clamp(control, 0.0, 1.0) * span_;
        const double logIs = std::log(referenceCollectorAmps * (1 + 1 / beta))
                           - referenceVbe / thermalVolts;
        // Solve voltage-domain KCL in logarithmic current. Bounded work runs
        // only at construction/calibration; the callback uses gain_'s lerp.
        double logarithm = std::log(std::max(supply / (inputOhms + emitterOhms), 1e-30));
        for (int iteration = 0; iteration < 24; ++iteration)
        {
            const double current = std::exp(logarithm);
            const double residual = (inputOhms + emitterOhms) * current
                + thermalVolts * (logarithm - logIs) - supply;
            const double step = residual / ((inputOhms + emitterOhms) * current + thermalVolts);
            logarithm -= step;
            if (std::abs(step) < 1e-14) break;
        }
        return std::exp(logarithm);
    }
    [[nodiscard]] double collectorAmps(double control) const noexcept
    {
        return emitterAmps(control) * beta / (beta + 1);
    }
    [[nodiscard]] float gain(float control) const noexcept
    {
        // Retain the existing declared off-current/voice-retirement policy.
        if (!(control > .001f)) return 0;
        const double position = std::clamp(static_cast<double>(control), 0.0, 1.0) * tableSteps;
        const auto index = static_cast<std::size_t>(std::min(static_cast<int>(position), tableSteps - 1));
        return gain_[index] + static_cast<float>(position - index) * (gain_[index + 1] - gain_[index]);
    }
    [[nodiscard]] double turnOnVolts() const noexcept
    {
        const double emitterReference = referenceCollectorAmps * (1 + 1 / beta);
        return referenceVbe - thermalVolts * std::log(
            emitterReference * (inputOhms + emitterOhms) / thermalVolts) - standoffVolts;
    }
    [[nodiscard]] double headroomVolts() const noexcept { return headroom_; }
    [[nodiscard]] double serviceGain() const noexcept { return serviceGain_; }
    [[nodiscard]] double referenceTailAmps() const noexcept { return peakEmitterAmps_ * beta / (beta + 1); }
    [[nodiscard]] double spanVolts() const noexcept { return span_; }

private:
    double span_;
    double peakEmitterAmps_ {};
    double headroom_ {};
    double serviceGain_ {};
    std::array<float, tableSteps + 1> gain_ {};
};
} // namespace youknow
