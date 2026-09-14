#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace youknow
{
// Named component substitute, NOT a measured KMFC1034T1 temperature law.
// Murata P16E-9, printed p.10 / PDF p.11, CSA8.00MTZ frequency-vs-temperature
// plot: https://www.homepages.ed.ac.uk/jwp/radio/projects/p16e9.pdf#page=11
// PDF SHA256: a59c92633575ea96a92496e536b4ca74b61ac32c28315db49a194cb0a570551c
// These are digitized vector vertices, not raw manufacturer measurements.
// The plot-reading allowances (+/-1 C, +/-0.015 percentage points) describe
// drawing resolution, not statistical confidence or hardware tolerances.
// Printed p.9's MTZ test uses CD4069UBE, 12 V and two 30 pF loads; the Juno's
// installed inverter/load are different. Use only the normalized SHAPE, with
// an explicit reference temperature and independently supplied reference Hz.
// The older CSA8.00MT motional RLC table is a different part/model.
struct Csa8MtzTemperatureProxy
{
    static constexpr double minimumCelsius = -20.0;
    static constexpr double maximumCelsius = 80.0;

    [[nodiscard]] static bool supports(double celsius) noexcept
    {
        return std::isfinite(celsius)
            && celsius >= minimumCelsius && celsius <= maximumCelsius;
    }

    [[nodiscard]] static double frequencyFactor(double celsius) noexcept
    {
        // Stay within the catalogue's temperature-stability specification
        // domain. The drawing extends beyond it; no extrapolation is used.
        const double temperature = std::clamp(
            std::isfinite(celsius) ? celsius : 25.0,
            minimumCelsius, maximumCelsius);
        struct Point { double celsius, percent; };
        static constexpr std::array<Point, 7> points {{
            { -20.6, -0.1832 }, { 0.0, -0.0972 }, { 20.0, -0.0152 },
            { 40.0, 0.0629 }, { 51.8, 0.1020 }, { 66.7, 0.1840 },
            { 81.2, 0.2621 }
        }};
        for (std::size_t index = 1; index < points.size(); ++index)
        {
            const auto& high = points[index];
            if (temperature <= high.celsius)
            {
                const auto& low = points[index - 1];
                const double fraction = (temperature - low.celsius)
                                      / (high.celsius - low.celsius);
                const double percent = low.percent
                    + fraction * (high.percent - low.percent);
                return 1.0 + percent / 100.0;
            }
        }
        return 1.0; // Bounded temperature always finds an interval above.
    }

    [[nodiscard]] static double frequencyRatio(
        double celsius, double referenceCelsius) noexcept
    {
        return frequencyFactor(celsius) / frequencyFactor(referenceCelsius);
    }
};
}
