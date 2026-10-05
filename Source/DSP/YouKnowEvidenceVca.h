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
    // Conservative horizontal shift of Toshiba's -25/25/100C VBE curves
    // (p.2), applied to the already named 25C specimen. This is not an
    // installed Tr20 temperature measurement. Beta remains the named200
    // prior; changing only Vt while freezing Is would give the wrong sign.
    static constexpr double referenceVbeVoltsPerCelsius = -.0016;
    static constexpr double referenceCollectorAmps = .0006509655627366167;
    static constexpr double standoffVolts = .26;
    static constexpr double inputOhms = 10000;
    static constexpr double emitterOhms = 22000;
    static constexpr double outputLoadOhms = 47000;
    static constexpr int tableSteps = 4096;

    explicit EvidenceVcaCalibration(double spanVolts, double celsius = 25.0) noexcept
        : span_(spanVolts), thermalVolts_(thermalVolts * ((celsius + 273.15) / 298.15)),
          referenceVbe_(referenceVbe + referenceVbeVoltsPerCelsius * (celsius - 25.0))
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
                           - referenceVbe_ / thermalVolts_;
        // Solve voltage-domain KCL in logarithmic current. Bounded work runs
        // only at construction/calibration; the callback uses gain_'s lerp.
        double logarithm = std::log(std::max(supply / (inputOhms + emitterOhms), 1e-30));
        for (int iteration = 0; iteration < 24; ++iteration)
        {
            const double current = std::exp(logarithm);
            const double residual = (inputOhms + emitterOhms) * current
                + thermalVolts_ * (logarithm - logIs) - supply;
            const double step = residual / ((inputOhms + emitterOhms) * current + thermalVolts_);
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
        return referenceVbe_ - thermalVolts_ * std::log(
            emitterReference * (inputOhms + emitterOhms) / thermalVolts_) - standoffVolts;
    }
    [[nodiscard]] double headroomVolts() const noexcept { return headroom_; }
    [[nodiscard]] double serviceGain() const noexcept { return serviceGain_; }
    [[nodiscard]] double referenceTailAmps() const noexcept { return peakEmitterAmps_ * beta / (beta + 1); }
    [[nodiscard]] double spanVolts() const noexcept { return span_; }

private:
    double span_;
    double thermalVolts_;
    double referenceVbe_;
    double peakEmitterAmps_ {};
    double headroom_ {};
    double serviceGain_ {};
    std::array<float, tableSteps + 1> gain_ {};
};

// Prepared, thermal-only physical C58 circuit. The original control solver
// stores equivalent settled CV u. Here q=(V_C58-.26V)/span is retained instead:
// temperature can change the emitter current without changing capacitor
// charge. R105*Ie + Vbe(Ie,T) = .26 + span*q, followed by
// C58*span*dq/dt = span*(target-q)/R106 - Ie.
// No BA662 gm temperature factor lives here; the signal pair's existing
// T_service/T factor is applied once downstream of C59.
// Shared tables are prepared before processing. 1C Hermite interpolation spans the
// engine's entire 25..63C Character/gradient domain, without callback solves.
class VcaJunctionTemperatureCircuit
{
public:
    static constexpr int minimumCelsius = 25;
    static constexpr int maximumCelsius = 64;
    static constexpr int tableSteps = 8192;
    static constexpr double minimumCharge = -.01;
    static constexpr double maximumCharge = 1.01;
    static constexpr double capacitanceFarads = .1e-6;

    explicit VcaJunctionTemperatureCircuit(double span) noexcept : span_(span)
    {
        const double emitterReference = EvidenceVcaCalibration::referenceCollectorAmps
            * (1 + 1 / EvidenceVcaCalibration::beta);
        for (int temperature = minimumCelsius; temperature <= maximumCelsius; ++temperature)
        {
            auto& row = rows_[static_cast<std::size_t>(temperature - minimumCelsius)];
            const double thermal = EvidenceVcaCalibration::thermalVolts
                * ((temperature + 273.15) / 298.15);
            const double vbe = EvidenceVcaCalibration::referenceVbe
                + EvidenceVcaCalibration::referenceVbeVoltsPerCelsius * (temperature - 25);
            const double logIs = std::log(emitterReference) - vbe / thermal;
            for (int index = 0; index <= tableSteps; ++index)
            {
                const double control = static_cast<double>(index) / tableSteps;
                const double charge = minimumCharge + control * (maximumCharge - minimumCharge);
                row.capacitor[static_cast<std::size_t>(index)] = solveEmitter(
                    EvidenceVcaCalibration::standoffVolts + span * charge,
                    EvidenceVcaCalibration::emitterOhms, thermal, logIs);
                row.control[static_cast<std::size_t>(index)] = solveEmitter(
                    EvidenceVcaCalibration::standoffVolts + span * control,
                    EvidenceVcaCalibration::inputOhms + EvidenceVcaCalibration::emitterOhms,
                    thermal, logIs);
                // Implicit differentiation at fixed applied voltage:
                // dIe/dT = -(dVbe_ref/dT + dVt/dT*log(Ie/Ie_ref))
                //          /(R + Vt/Ie). Float storage affects this small
                // interpolation correction, not the retained current table.
                const auto derivative = [&](double current, double resistance) noexcept {
                    return static_cast<float>(-(EvidenceVcaCalibration::referenceVbeVoltsPerCelsius
                        + EvidenceVcaCalibration::thermalVolts / 298.15
                            * std::log(current / emitterReference))
                        / (resistance + thermal / current));
                };
                row.capacitorTemperatureSlope[static_cast<std::size_t>(index)] = derivative(
                    row.capacitor[static_cast<std::size_t>(index)], EvidenceVcaCalibration::emitterOhms);
                row.controlTemperatureSlope[static_cast<std::size_t>(index)] = derivative(
                    row.control[static_cast<std::size_t>(index)],
                    EvidenceVcaCalibration::inputOhms + EvidenceVcaCalibration::emitterOhms);
            }
        }
    }
    [[nodiscard]] double emitterAmpsAtCharge(double charge, double celsius) const noexcept
    {
        return sample(charge, celsius, true);
    }
    [[nodiscard]] double emitterAmpsAtControl(double control, double celsius) const noexcept
    {
        return sample(control, celsius, false);
    }
    [[nodiscard]] double chargeAtControl(double control, double celsius) const noexcept
    {
        return control - EvidenceVcaCalibration::inputOhms
            * emitterAmpsAtControl(control, celsius) / span_;
    }
    [[nodiscard]] double controlAtCharge(double charge, double celsius) const noexcept
    {
        return charge + EvidenceVcaCalibration::inputOhms
            * emitterAmpsAtCharge(charge, celsius) / span_;
    }
    template<class Drive>
    [[nodiscard]] double advanceDriven(double charge, double seconds, double celsius,
                                        const Drive& drive, int steps) const noexcept
    {
        if (!(seconds > 0.0)) return charge;
        const double dt = seconds / std::max(steps, 1);
        const auto derivative = [&](double q, double target) noexcept {
            return (std::clamp(target, 0.0, 1.0) - q)
                       / (EvidenceVcaCalibration::inputOhms * capacitanceFarads)
                - emitterAmpsAtCharge(q, celsius) / (span_ * capacitanceFarads);
        };
        for (int step = 0; step < std::max(steps, 1); ++step)
        {
            const double t = step * dt;
            const double a = derivative(charge, drive(t));
            const double b = derivative(charge + .5 * dt * a, drive(t + .5 * dt));
            const double c = derivative(charge + .5 * dt * b, drive(t + .5 * dt));
            const double d = derivative(charge + dt * c, drive(t + dt));
            charge = std::clamp(charge + dt * (a + 2*b + 2*c + d) / 6,
                                minimumCharge, maximumCharge);
        }
        return charge;
    }
    [[nodiscard]] double advance(double charge, double target, double seconds,
                                 double celsius) const noexcept
    {
        const double thermal = EvidenceVcaCalibration::thermalVolts
            * ((std::clamp(celsius, 25.0, 64.0) + 273.15) / 298.15);
        const int steps = charge < .08 && target > charge
            && seconds * (target - charge)
                / (EvidenceVcaCalibration::inputOhms * capacitanceFarads)
                    > .5 * thermal / span_ ? 8 : 1;
        return advanceDriven(charge, seconds, celsius,
            [target](double) noexcept { return target; }, steps);
    }
private:
    static double solveEmitter(double supply, double series, double thermal, double logIs) noexcept
    {
        double logarithm = std::log(std::max(supply / series, 1e-30));
        for (int iteration = 0; iteration < 24; ++iteration)
        {
            const double current = std::exp(logarithm);
            const double step = (series * current + thermal * (logarithm - logIs) - supply)
                / (series * current + thermal);
            logarithm -= step;
            if (std::abs(step) < 1e-14) break;
        }
        return std::exp(logarithm);
    }
    struct Row
    {
        std::array<double, tableSteps + 1> capacitor {};
        std::array<double, tableSteps + 1> control {};
        std::array<float, tableSteps + 1> capacitorTemperatureSlope {};
        std::array<float, tableSteps + 1> controlTemperatureSlope {};
    };
    [[nodiscard]] double sample(double coordinate, double celsius, bool capacitor) const noexcept
    {
        const double temperature = std::clamp(celsius,
            static_cast<double>(minimumCelsius), static_cast<double>(maximumCelsius)) - minimumCelsius;
        const auto low = static_cast<std::size_t>(std::min(static_cast<int>(temperature),
            maximumCelsius - minimumCelsius - 1));
        const double fraction = temperature - low;
        const double normalized = capacitor
            ? (coordinate - minimumCharge) / (maximumCharge - minimumCharge) : coordinate;
        const double position = std::clamp(normalized, 0.0, 1.0) * tableSteps;
        const auto index = static_cast<std::size_t>(std::min(static_cast<int>(position), tableSteps - 1));
        const auto at = [&](const Row& row) noexcept {
            const auto& values = capacitor ? row.capacitor : row.control;
            return values[index] + (position - index) * (values[index + 1] - values[index]);
        };
        const auto slopeAt = [&](const Row& row) noexcept {
            const auto& values = capacitor ? row.capacitorTemperatureSlope : row.controlTemperatureSlope;
            return static_cast<double>(values[index]) + (position - index)
                * (static_cast<double>(values[index + 1]) - values[index]);
        };
        const double first = at(rows_[low]);
        const double difference = at(rows_[low + 1]) - first;
        return first + fraction * difference + fraction * (1-fraction)
            * ((1-fraction) * slopeAt(rows_[low]) - fraction * slopeAt(rows_[low+1])
                + (2*fraction-1) * difference);
    }
    double span_;
    std::array<Row, maximumCelsius - minimumCelsius + 1> rows_ {};
};
} // namespace youknow
