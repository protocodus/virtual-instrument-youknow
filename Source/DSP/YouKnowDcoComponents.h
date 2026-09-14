#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace youknow
{
// Installed topology: one C54 (.001G) and three selected 399/200/100k MF
// resistors per card. Roland's p.12 legend identifies the resistor class as
// 1%; G identifies +/-2% capacitance. These are component bounds, not a
// measured probability distribution or a temperature coefficient.
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=12
struct DcoComponentTolerance
{
    static constexpr double nominalCapacitance = 1e-9;
    static constexpr double capacitanceTolerance = 0.02;
    static constexpr double resistanceTolerance = 0.01;
    static constexpr std::array<double, 3> nominalResistance {
        399000.0, 200000.0, 100000.0
    };

    // Fixed signed coordinates in [-1,1]. Engine construction chooses a
    // deterministic approximately uniform population as a product prior.
    // The same capacitor participates in every range; resistors are distinct.
    float capacitorDraw { 0.0f };
    std::array<float, 3> resistorDraw {};

    [[nodiscard]] static double bounded(float draw) noexcept
    {
        return std::isfinite(draw) ? std::clamp(static_cast<double>(draw), -1.0, 1.0) : 0.0;
    }

    [[nodiscard]] double capacitance(float character) const noexcept
    {
        return nominalCapacitance * (1.0 + capacitanceTolerance
            * bounded(capacitorDraw) * characterDrive(character));
    }

    [[nodiscard]] static double characterDrive(float character) noexcept
    {
        // The existing Character 1..2 product extension exaggerates component
        // errors beyond their manufacturing classes. It is not a hardware bound.
        return std::isfinite(character)
            ? std::clamp(static_cast<double>(character), 0.0, 2.0) : 0.0;
    }

    [[nodiscard]] double resistance(std::size_t range, float character) const noexcept
    {
        range = std::min(range, nominalResistance.size() - 1);
        return nominalResistance[range] * (1.0 + resistanceTolerance
            * bounded(resistorDraw[range]) * characterDrive(character));
    }

    [[nodiscard]] float chargingScale(std::size_t range, float character) const noexcept
    {
        range = std::min(range, nominalResistance.size() - 1);
        return static_cast<float>((nominalCapacitance / capacitance(character))
            * (nominalResistance[range] / resistance(range, character)));
    }
};
}
