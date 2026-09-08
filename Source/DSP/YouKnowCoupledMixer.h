#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace youknow
{

// Calibration-only model of the module-board WAVE/Sub/C56 network.
//
// Roland JUNO-106 Service Notes, printed pp. 9 and 13:
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf
// Visually re-read 2026-09-08. R102 = 33k is Tr19's collector pull-up;
// R101 = 27k then D6 reach WAVE; C56 = 10 uF NP reaches VCF IN.
// Tr19 off therefore gives a 60k source from SUB LEVEL. With Tr19 on,
// the source is its collector voltage through 27k, NOT an open circuit:
// D6 can still conduct if WAVE falls below that voltage minus its drop.
// The MC5534A's internal resistors, D6 part/drop, installed collector swing
// and loaded module input impedance are not specified by that drawing.
//
// Those quantities are deliberately REQUIRED calibration inputs. Zero-filled
// Calibration is invalid, and the engine never enables this model by default.
// sourceScale/sourceBias map the existing no-sub source coordinate to the
// measured WAVE Thevenin voltage; loadOhms is the measured small-signal VCF
// input resistance. A real unit with frequency-dependent impedance needs a
// richer network, not a fit of these constants to a convenient audio clip.
class CoupledSubMixer
{
public:
    static constexpr double pullupOhms = 33000.0;
    static constexpr double seriesOhms = 27000.0;
    static constexpr double couplingFarads = 10.0e-6;
    static constexpr double railFullScaleVolts = 10.0 * 4064.0 / 4096.0;

    struct Calibration
    {
        double sourceOhms {};
        double loadOhms {};
        double sourceScale {};
        double sourceBiasVolts {};
        double diodeDropVolts {};
        double collectorOnVolts {};

        [[nodiscard]] bool valid() const noexcept
        {
            const std::array values { sourceOhms, loadOhms, sourceScale,
                sourceBiasVolts, diodeDropVolts, collectorOnVolts };
            for (const auto value : values)
                if (!std::isfinite(value))
                    return false;
            // Numerical-domain guards, not claimed installed-part tolerances.
            return sourceOhms >= 1.0 && sourceOhms <= 1.0e9
                && loadOhms >= 1.0 && loadOhms <= 1.0e9
                && sourceScale > 0.0 && sourceScale <= 100.0
                && std::abs(sourceBiasVolts) <= 30.0
                && diodeDropVolts >= 0.0 && diodeDropVolts <= 2.0
                && collectorOnVolts >= 0.0 && collectorOnVolts <= 2.0;
        }
    };

    struct Result
    {
        double waveVolts {};
        double filterVolts {};
        double capacitorAmps {};
        double subOffAmps {};
        double subOnAmps {};
    };

    // Sub gate is an antialias reconstruction weight: 1 means Tr19 off,
    // 0 means on. At fractional edge samples the two branch currents are
    // interpolated. The clamp discards BLEP overshoot to keep conductances
    // non-negative; it is numerical policy, not transistor crossover physics.
    // Piecewise constant-drop diodes are likewise an explicit idealization;
    // there is no invented Shockley Is or fitted waveform asymmetry.
    [[nodiscard]] static Result solve(const Calibration& c, double sourceVolts,
        double railVolts, double gate, double loadConductance,
        double historyVolts) noexcept
    {
        const double q = std::clamp(gate, 0.0, 1.0);
        const std::array drive { railVolts - c.diodeDropVolts,
            c.collectorOnVolts - c.diodeDropVolts };
        const std::array conductance { q / (pullupOhms + seriesOhms),
            (1.0 - q) / seriesOhms };
        const double sourceG = 1.0 / c.sourceOhms;
        const double baseG = sourceG + loadConductance;
        const double baseI = sourceVolts * sourceG
            + historyVolts * loadConductance;

        // Start with both diodes conducting, then remove impossible branches.
        // Removing a branch whose drive is below WAVE can only raise WAVE,
        // so a removed branch cannot need to return. At most three solves.
        unsigned mask = 3;
        double wave = 0.0;
        for (int pass = 0; pass < 3; ++pass)
        {
            double g = baseG, i = baseI;
            for (unsigned branch = 0; branch < 2; ++branch)
                if ((mask & (1u << branch)) != 0)
                {
                    g += conductance[branch];
                    i += conductance[branch] * drive[branch];
                }
            wave = i / g;
            const unsigned next = mask
                & (wave <= drive[0] ? 3u : 2u)
                & (wave <= drive[1] ? 3u : 1u);
            if (next == mask)
                break;
            mask = next;
        }
        const double current = (wave - historyVolts) * loadConductance;
        return { wave, current * c.loadOhms, current,
            conductance[0] * std::max(0.0, drive[0] - wave),
            conductance[1] * std::max(0.0, drive[1] - wave) };
    }

    void reset() noexcept { capacitorVolts_ = capacitorAmps_ = 0.0; }

    void prime(const Calibration& c, double sourceVolts,
               double railVolts, double gate) noexcept
    {
        capacitorVolts_ = solve(c, sourceVolts, railVolts, gate, 0.0, 0.0).waveVolts;
        capacitorAmps_ = 0.0;
    }

    [[nodiscard]] Result process(const Calibration& c, double sourceVolts,
        double railVolts, double gate, double seconds) noexcept
    {
        // One trapezoidal capacitor companion, solved together with the
        // source, both diode states and VCF load. No second C56 high-pass is
        // applied by the engine. Store physical V and I so rate changes retain
        // the same capacitor rather than reusing a rate-dependent history.
        const double companionOhms = seconds / (2.0 * couplingFarads);
        const double history = capacitorVolts_ + companionOhms * capacitorAmps_;
        const auto result = solve(c, sourceVolts, railVolts, gate,
            1.0 / (c.loadOhms + companionOhms), history);
        capacitorVolts_ = result.waveVolts - result.filterVolts;
        capacitorAmps_ = result.capacitorAmps;
        return result;
    }

    [[nodiscard]] double capacitorVolts() const noexcept { return capacitorVolts_; }
    [[nodiscard]] double capacitorAmps() const noexcept { return capacitorAmps_; }

private:
    double capacitorVolts_ {};
    double capacitorAmps_ {};
};

} // namespace youknow
