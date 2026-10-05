#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "YouKnowSubLevel.h"

namespace youknow
{

// Explicitly configured model of the module-board WAVE/Sub/C56 network.
//
// Roland JUNO-106 Service Notes, printed pp. 9 and 13:
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf
// Visually re-read 2026-09-08. R102 = 33k is Tr19's collector pull-up;
// R101 = 27k then D6 reach WAVE; C56 = 10 uF NP reaches VCF IN.
// Tr19 off therefore gives a 60k source from SUB LEVEL. With Tr19 on,
// the source is its collector voltage through 27k, NOT an open circuit:
// D6 can still conduct if WAVE falls below that voltage minus its drop.
// The p.12 module-board legend identifies D6 as 1SS133 (printed 1SS-133):
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=12
// Its installed forward drop, the MC5534A's internal resistors, collector
// swing and loaded module input impedance remain unmeasured.
//
// Those quantities are deliberately REQUIRED calibration inputs. Zero-filled
// Calibration is invalid, and the raw engine keeps this model disabled.
// evidenceCalibration supplies a named, endpoint-constrained source prior;
// that candidate does not turn these unknowns into installed-part readings.
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
        // Appended fields preserve the original constant-drop fixtures exactly.
        // A zero slope selects that legacy diode. A positive slope requires a
        // declared forward-current operating point at diodeDropVolts.
        double diodeSlopeVolts {};
        double diodeReferenceAmps {};
        // Physical pin-1 voltage and the cascade's feedback-node coordinate are
        // different: the reconstructed hybrid has 4.7k input and 68k feedback.
        double pinToCoreGain { 1.0 };
        // Explicit product source conventions, applied before the physical
        // solve. The oscillator drive's existing digital normalization uses
        // this same coordinate; no compensating gain is hidden in the circuit.
        double oscillatorDriveScale { 1.0 };
        double pulseSourceScale { 1.0 };
        bool unipolarSawSource { false };

        [[nodiscard]] bool valid() const noexcept
        {
            const std::array values { sourceOhms, loadOhms, sourceScale,
                sourceBiasVolts, diodeDropVolts, collectorOnVolts,
                diodeSlopeVolts, diodeReferenceAmps, pinToCoreGain,
                oscillatorDriveScale, pulseSourceScale };
            for (const auto value : values)
                if (!std::isfinite(value))
                    return false;
            // Numerical-domain guards, not claimed installed-part tolerances.
            return sourceOhms >= 1.0 && sourceOhms <= 1.0e9
                && loadOhms >= 1.0 && loadOhms <= 1.0e9
                && sourceScale > 0.0 && sourceScale <= 100.0
                && std::abs(sourceBiasVolts) <= 30.0
                && diodeDropVolts >= 0.0 && diodeDropVolts <= 2.0
                && collectorOnVolts >= 0.0 && collectorOnVolts <= 2.0
                && diodeSlopeVolts >= 0.0 && diodeSlopeVolts <= 0.2
                && (diodeSlopeVolts == 0.0 || (diodeSlopeVolts >= 1.0e-4
                    && diodeReferenceAmps >= 1.0e-30
                    && diodeReferenceAmps <= 0.1))
                && pinToCoreGain > 0.0 && pinToCoreGain <= 100.0
                && oscillatorDriveScale >= 0.25 && oscillatorDriveScale <= 2.0
                && pulseSourceScale >= 0.25 && pulseSourceScale <= 2.0;
        }
    };

    // Evidence-constrained nominal candidate, NOT an installed MC5534A fit.
    // Drawn external resistors and the reconstructed hybrid fix the branch,
    // load and pin-to-core gain. The SUB-only capture fixes the aggregate
    // S=8.896V resistor/junction law (YouKnowSubLevel.h). Its full-level
    // balance remains the product's existing voiced 7.57V source coordinate.
    // Infer a SOURCE PRIOR that retains that endpoint rather than inventing
    // an internal resistor value. For a settled 50% sub with negligible
    // capacitor ripple, Re=Rs||Rl, Iref=S/[60k+(Rs+Re)/2] and Vpin_pp=Re*Iref.
    // There are two positive solutions: ~4.978k and ~95.658k. Choosing the
    // lower-resistance solution is explicit compatibility policy, not a
    // measurement or proof of the custom IC's internal resistor inventory.
    // The nominal 0.6V forward reference and ideal grounded collector remain
    // stated junction/switch priors. Mixed-source and transient captures are
    // required to distinguish these coordinates and validate the candidate.
    [[nodiscard]] static Calibration evidenceCalibration() noexcept
    {
        constexpr double load = 4700.0 * 25500.0 / (4700.0 + 25500.0);
        constexpr double gain = 68000.0 / 4700.0;
        constexpr double oscillatorDrive = 0.738;
        constexpr double pulseScale = 0.857;
        constexpr double coreSubPeakToPeak = 2.0 * 7.57 * oscillatorDrive * 0.4;
        constexpr double pinSubPeakToPeak = coreSubPeakToPeak / gain;
        constexpr double seriesSpan = SubLevelDiodeLaw::referenceSeriesSpanVolts;
        constexpr double branch = pullupOhms + seriesOhms;
        constexpr double a = 0.5 * pinSubPeakToPeak;
        constexpr double b = pinSubPeakToPeak * (branch + load) - seriesSpan * load;
        constexpr double constant = branch * pinSubPeakToPeak * load;
        // Stable lower root of a*Rs^2+b*Rs+constant=0.
        const double source = 2.0 * constant / (-b + std::sqrt(b * b - 4.0 * a * constant));
        const double effective = source * load / (source + load);
        const double current = seriesSpan / (branch + 0.5 * (source + effective));
        const double sourceScale = 0.4 / (gain * load / (source + load));
        constexpr double drop = 0.6;
        const double isolatedBias = railFullScaleVolts - seriesSpan - drop;
        // SUB-only reference has SAW off and Pulse Off's comparator high.
        // Keep its physical Thevenin bias at the aggregate calibration point.
        const double sourceBias = isolatedBias - sourceScale * oscillatorDrive
            * 6.0 * pulseScale;
        return { source, load, sourceScale, sourceBias, drop, 0.0,
            SubLevelDiodeLaw::junctionSlopeVolts, current, gain,
            oscillatorDrive, pulseScale, true };
    }

    struct Result
    {
        double waveVolts {};
        double filterVolts {};
        double capacitorAmps {};
        double subOffAmps {};
        double subOnAmps {};
    };

    // Rate/calibration invariants only. Preparation does not read or replace
    // capacitor charge. The engine rebuilds this bundle only when its
    // configured circuit or processing interval changes.
    struct PreparedCoefficients
    {
        double companionOhms {};
        double loadConductance {};
        double sourceConductance {};
        double baseConductance {};
        double baseResistance {};
        SubLevelDiodeLaw::ForwardCurrentCoefficients offBranch {};
        SubLevelDiodeLaw::ForwardCurrentCoefficients onBranch {};
        SubLevelDiodeLaw::ForwardCurrentCoefficients effectiveOffBranch {};
        SubLevelDiodeLaw::ForwardCurrentCoefficients effectiveOnBranch {};
    };

    [[nodiscard]] static PreparedCoefficients prepareCoefficients(
        const Calibration& c, double seconds) noexcept
    {
        PreparedCoefficients result;
        result.companionOhms = seconds / (2.0 * couplingFarads);
        result.loadConductance = 1.0 / (c.loadOhms + result.companionOhms);
        result.sourceConductance = 1.0 / c.sourceOhms;
        result.baseConductance = result.sourceConductance + result.loadConductance;
        result.baseResistance = 1.0 / result.baseConductance;
        if (c.diodeSlopeVolts > 0.0)
        {
            const auto currentCoefficients = [&](double resistance) {
                return SubLevelDiodeLaw::prepareForwardCurrent(resistance,
                    c.diodeSlopeVolts, c.diodeReferenceAmps, c.diodeDropVolts);
            };
            result.offBranch = currentCoefficients(pullupOhms + seriesOhms);
            result.onBranch = currentCoefficients(seriesOhms);
            result.effectiveOffBranch = currentCoefficients(
                pullupOhms + seriesOhms + result.baseResistance);
            result.effectiveOnBranch = currentCoefficients(
                seriesOhms + result.baseResistance);
        }
        return result;
    }

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
        if (c.diodeSlopeVolts > 0.0)
            return solveExponential<false>(c, sourceVolts, railVolts, q,
                                    loadConductance, historyVolts);
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

    // The bundle belongs to this calibration and interval. Keep the original
    // on-demand API above for independent circuit fixtures and variable-step
    // callers; neither path changes the capacitor's physical state at retime.
    [[nodiscard]] Result process(const Calibration& c,
        const PreparedCoefficients& coefficients, double sourceVolts,
        double railVolts, double gate) noexcept
    {
        const double history = capacitorVolts_
            + coefficients.companionOhms * capacitorAmps_;
        const auto result = c.diodeSlopeVolts > 0.0
            ? solveExponential<true>(c, sourceVolts, railVolts,
                std::clamp(gate, 0.0, 1.0), coefficients.loadConductance,
                history, &coefficients)
            : solve(c, sourceVolts, railVolts, gate,
                coefficients.loadConductance, history);
        capacitorVolts_ = result.waveVolts - result.filterVolts;
        capacitorAmps_ = result.capacitorAmps;
        return result;
    }

    [[nodiscard]] double capacitorVolts() const noexcept { return capacitorVolts_; }
    [[nodiscard]] double capacitorAmps() const noexcept { return capacitorAmps_; }

private:
    template <bool usePrepared>
    [[nodiscard]] static Result solveExponential(const Calibration& c,
        double sourceVolts, double railVolts, double gate,
        double loadConductance, double historyVolts,
        const PreparedCoefficients* coefficients = nullptr) noexcept
    {
        const double sourceG = usePrepared
            ? coefficients->sourceConductance : 1.0 / c.sourceOhms;
        const double baseG = usePrepared
            ? coefficients->baseConductance : sourceG + loadConductance;
        const double baseWave = (sourceVolts * sourceG
            + historyVolts * loadConductance) / baseG;
        if (gate == 0.0 || gate == 1.0)
        {
            // Only one diode branch carries current at an integer gate.
            // The source/load companion is its Thevenin source: KCL gives
            // WAVE = baseWave + I/baseG. Substituting into the diode equation
            // adds 1/baseG to its series resistance, so one log-current solve
            // replaces the nested node/current Newton iterations exactly.
            // Fractional antialias edges still require the two-branch solve.
            const bool offBranch = gate == 1.0;
            const double branchOhms = offBranch
                ? pullupOhms + seriesOhms : seriesOhms;
            const double drive = offBranch ? railVolts : c.collectorOnVolts;
            const double diodeAmps = [&] {
                if constexpr (usePrepared)
                    return SubLevelDiodeLaw::forwardCurrent(drive - baseWave,
                        offBranch ? coefficients->effectiveOffBranch
                                  : coefficients->effectiveOnBranch);
                else
                    return SubLevelDiodeLaw::forwardCurrent(
                        drive - baseWave, branchOhms + 1.0 / baseG,
                        c.diodeSlopeVolts, c.diodeReferenceAmps, c.diodeDropVolts);
            }();
            const double wave = baseWave + diodeAmps / baseG;
            const double current = (wave - historyVolts) * loadConductance;
            return { wave, current * c.loadOhms, current,
                offBranch ? diodeAmps : 0.0, offBranch ? 0.0 : diodeAmps };
        }
        double wave = baseWave;
        double off = 0.0, on = 0.0;
        const auto branchCurrent = [&](double voltage, bool offBranch) {
            if constexpr (usePrepared)
                return SubLevelDiodeLaw::forwardCurrent(voltage,
                    offBranch ? coefficients->offBranch : coefficients->onBranch);
            else
                return SubLevelDiodeLaw::forwardCurrent(voltage,
                    offBranch ? pullupOhms + seriesOhms : seriesOhms,
                    c.diodeSlopeVolts, c.diodeReferenceAmps, c.diodeDropVolts);
        };
        // Current is monotonic in junction voltage; F'(wave) >= baseG.
        // Starting at the unloaded node converges from below. The branch
        // series resistance limits its derivative, even at extreme drives.
        for (int iteration = 0; iteration < 12; ++iteration)
        {
            off = gate > 0.0 ? branchCurrent(railVolts - wave, true) : 0.0;
            on = gate < 1.0 ? branchCurrent(c.collectorOnVolts - wave, false) : 0.0;
            const double residual = (wave - baseWave) * baseG
                - gate * off - (1.0 - gate) * on;
            const double derivative = baseG
                + gate * off / (c.diodeSlopeVolts + (pullupOhms + seriesOhms) * off)
                + (1.0 - gate) * on / (c.diodeSlopeVolts + seriesOhms * on);
            const double step = residual / derivative;
            wave -= step;
            if (std::abs(step) <= 1.0e-13 * std::max(1.0, std::abs(wave)))
                break;
        }
        const double current = (wave - historyVolts) * loadConductance;
        return { wave, current * c.loadOhms, current, gate * off, (1.0 - gate) * on };
    }

    double capacitorVolts_ {};
    double capacitorAmps_ {};
};

} // namespace youknow
