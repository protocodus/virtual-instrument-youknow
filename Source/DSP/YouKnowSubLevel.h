#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace youknow
{

// SUB LEVEL -> diode-gated current, with a reference-unit aggregate calibration.
// Roland's module-board p.13 gives R102 33k + R101 27k + D6 feeding WAVE.
// With a settled WAVE bias and a forward silicon junction, subtracting the
// full-current operating point eliminates the unknown bias and saturation
// current: V - Vfull = S*(j - 1) + Vt*log(j), j = I/Ifull, S = R*Ifull.
// This is a resistor plus diode law, not a fitted generic transfer curve.
//
// Original MC5534A DCOs in Juno-106 #439522 (Borish replacement VCF/VCA cards):
// https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/44#issuecomment-4207426790
// https://lewisfrancis.com/nwio/osc_test_bip.aif
// https://kayrock.org/kr106/osc_test.mid
// The exact MIDI steps SUB 0,13,25,38,51,64,76,89,102,114,127 at 8..28s,
// with saw/pulse/noise off, fixed gate/level, cutoff open and chorus off.
// Tools/AnalyzeSubMixerCalibration.py hashes the artifacts, estimates each
// fundamental independently, normalizes by byte127, and fits S using ONLY
// alternating interior bytes25,51,76,102. Vt=26mV stays the existing nominal
// silicon prior; it is not fitted. Other interior bytes are held out.
//
// S is the ONLY calibrated aggregate. It does not identify WAVE impedance,
// bias, D6 Is, collector voltage or a population distribution. In particular
// the fully coupled model still requires those readings. Full-level source
// balance stays at the separately calibrated/voiced subMixVolts coordinate.
class SubLevelDiodeLaw
{
public:
    static constexpr double fullScaleVolts = 10.0 * 4064.0 / 4096.0;
    static constexpr double junctionSlopeVolts = 0.026;
    static constexpr double referenceSeriesSpanVolts = 8.896019223386196;
    static constexpr int tableSteps = 4096;

    [[nodiscard]] static double exactGain(double control) noexcept
    {
        if (!(control > 0.0))
            return 0.0;
        if (control >= 1.0)
            return 1.0;
        const double scale = referenceSeriesSpanVolts / junctionSlopeVolts;
        const double z = std::log(scale)
            + (fullScaleVolts * (control - 1.0) + referenceSeriesSpanVolts)
                / junctionSlopeVolts;
        double y = z > 1.0 ? z - std::log(z) : std::exp(z);
        for (int iteration = 0; iteration < 12; ++iteration)
        {
            const double delta = (y + std::log(y) - z) * y / (y + 1.0);
            y -= delta;
            if (std::abs(delta) <= 1.0e-15 * y)
                break;
        }
        return y / scale;
    }

    [[nodiscard]] static const std::array<float, tableSteps + 1>& table() noexcept
    {
        static const auto values = [] {
            std::array<float, tableSteps + 1> result {};
            for (int i = 0; i <= tableSteps; ++i)
                result[static_cast<std::size_t>(i)] = static_cast<float>(
                    exactGain(static_cast<double>(i) / tableSteps));
            return result;
        }();
        return values;
    }

    [[nodiscard]] static float gain(double control) noexcept
    {
        if (!(control > 0.0))
            return 0.0f;
        if (control >= 1.0)
            return 1.0f;
        const double position = control * tableSteps;
        const auto index = static_cast<std::size_t>(position);
        const auto& values = table();
        return values[index] + static_cast<float>(position - index)
            * (values[index + 1] - values[index]);
    }
};

} // namespace youknow
