#include "DSP/YouKnowEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace
{
using youknow::YouKnowEngine;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// Solve the drawn nodes independently of the production voltage-divider
// reduction. Unknowns are IC28a output, C7's held node and IC5 GC1. IC28a's
// inverting input is a virtual ground; C7 is open at DC. The component values
// come from Roland's module-board p.13 and jack-board p.15, not DSP constants:
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf
std::array<double, 3> nodalVoltages(double position)
{
    const double dac = 5.0 * 4064.0 * std::clamp(position, 0.0, 1.0) / 4096.0;
    std::array<std::array<double, 4>, 3> equations {{
        { 1.0 / 10000.0, 0.0, 0.0, 15.0 / 39000.0 - dac / 4990.0 },
        { -1.0 / 2200.0, 1.0 / 2200.0 + 1.0 / 1500.0,
          -1.0 / 1500.0, 0.0 },
        { 0.0, -1.0 / 1500.0,
          1.0 / 1500.0 + 1.0 / 47.0 + 1.0 / 15000.0, 15.0 / 15000.0 }
    }};
    for (std::size_t column = 0; column < 3; ++column)
    {
        std::size_t pivot = column;
        for (std::size_t row = column + 1; row < 3; ++row)
            if (std::abs(equations[row][column])
                > std::abs(equations[pivot][column]))
                pivot = row;
        std::swap(equations[column], equations[pivot]);
        const double divisor = equations[column][column];
        for (std::size_t entry = column; entry < 4; ++entry)
            equations[column][entry] /= divisor;
        for (std::size_t row = 0; row < 3; ++row)
        {
            if (row == column)
                continue;
            const double factor = equations[row][column];
            for (std::size_t entry = column; entry < 4; ++entry)
                equations[row][entry] -= factor * equations[column][entry];
        }
    }
    return { equations[0][3], equations[1][3], equations[2][3] };
}

double oldGcVolts(double position)
{
    const double hold = 4.0 - 10.0 * 4064.0 * position / 4096.0;
    return (hold / 3700.0 + 15.0 / 15000.0)
        / (1.0 / 3700.0 + 1.0 / 47.0 + 1.0 / 15000.0);
}
}

int main()
{
    try
    {
        double worstVoltageError = 0.0;
        double worstGainDbError = 0.0;
        double previousGain = 0.0;
        // All 128 stored values, plus fractional positions seen while C7
        // settles. Test the composed circuit voltage and its gain, rather
        // than merely pinning the two new constants.
        for (int index = 0; index <= 508; ++index)
        {
            const float position = static_cast<float>(index) / 508.0f;
            const auto nodes = nodalVoltages(position);
            const double actual = YouKnowEngine::commonVcaControlVolts(position);
            worstVoltageError = std::max(worstVoltageError,
                                         std::abs(actual - nodes[2]));
            for (const float temperature : { -25.0f, 25.0f, 75.0f })
            {
                const double expectedDb = -nodes[2] / 0.0059
                    * 298.15 / (static_cast<double>(temperature) + 273.15);
                const double gain = YouKnowEngine::patchLevelGain(position,
                                                                  temperature);
                require(std::isfinite(gain) && gain > 0.0,
                        "common VCA gain must be positive and finite");
                worstGainDbError = std::max(worstGainDbError,
                    std::abs(20.0 * std::log10(gain) - expectedDb));
            }
            const double gain = YouKnowEngine::patchLevelGain(position);
            require(gain > previousGain, "common VCA level must be monotonic");
            previousGain = gain;
        }
        require(worstVoltageError < 3.0e-8,
                "common VCA voltage differs from the independent nodal circuit");
        require(worstGainDbError < 1.0e-5,
                "common VCA gain differs from NEC's temperature-scaled law");
        for (const auto bounds : { std::array { -1.0f, 0.0f },
                                   std::array { 2.0f, 1.0f } })
            require(YouKnowEngine::commonVcaControlVolts(bounds[0])
                        == YouKnowEngine::commonVcaControlVolts(bounds[1]),
                    "common VCA out-of-range controls must clamp");

        const auto low = nodalVoltages(0.0);
        const auto high = nodalVoltages(1.0);
        // The detailed circuit is distinguishable from p.8's rounded labels.
        require(low[0] > 3.84 && low[0] < 3.85,
                "IC28a zero-code output is not the drawn bias ratio");
        require(high[0] < -6.09 && high[0] > -6.10,
                "IC28a stored full-scale output is not the drawn gain ratio");
        const double lowShift = -(low[2] - oldGcVolts(0.0)) / 0.0059;
        const double highShift = -(high[2] - oldGcVolts(1.0)) / 0.0059;
        std::cout << std::setprecision(12)
                  << "Common VCA buffer: " << low[0] << " .. " << high[0]
                  << " V; gain " << -low[2] / 0.0059 << " .. "
                  << -high[2] / 0.0059 << " dB at 25 C; correction +"
                  << lowShift << " .. +" << highShift << " dB\n"
                  << "Independent nodal worst voltage error " << worstVoltageError
                  << " V; gain error " << worstGainDbError << " dB\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
