// Each chorus line's own clock parts, Roland jack board p. 15: Tr19 with
// R123 1.8k / R124 8.2k / R125 10k for IC9 and Tr24 with R136 / R137 / R138
// for IC11. The current law is recomputed here from the divider's Thevenin
// form, and the line clocks are counted from actual BBD input captures.
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=15
#include "DSP/YouKnowChorus.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace youknow
{
struct YouKnowTestAccess
{
    static std::array<int, 2> writeIndices(const Chorus& chorus)
    { return { chorus.lineA_.writeIndex, chorus.lineB_.writeIndex }; }
};
}

namespace
{
using namespace youknow;
using Probe = YouKnowTestAccess;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// Thevenin form of the base divider; base current ignored as in the engine.
double oracleAmps(double top, double emitter, double ground)
{
    const double thevenin = 15.0 * top / (top + ground);
    return (thevenin - 0.65) / emitter;
}

void nominalCurrent()
{
    const double nominal = Chorus::clockSourceAmps(1800.0, 8200.0, 10000.0);
    // DerivedNominal's recorded 199.77 uA at the same 0.65 V junction.
    require(std::abs(nominal - 199.77e-6) < 0.01e-6, "nominal clock current moved");
    require(std::abs(nominal - oracleAmps(1800.0, 8200.0, 10000.0)) < 1e-15,
            "clock current law disagrees with the divider oracle");
}

void scaleLaw()
{
    for (float calibration : { 0.0f, 1.0f, 2.0f })
    {
        const auto off = Chorus::lineTimingScale(calibration, false);
        require(off.delayA == 1.0 && off.delayB == 1.0, "disabled spread moved a line");
    }
    const auto zero = Chorus::lineTimingScale(0.0f, true);
    require(zero.delayA == 1.0 && zero.delayB == 1.0,
            "Unit Character zero did not restore the nominal clocks");

    double previousSpread = 0.0;
    for (float calibration : { 0.5f, 1.0f, 2.0f })
    {
        const auto scale = Chorus::lineTimingScale(calibration, true);
        require(std::abs(scale.delayA * scale.delayB - 1.0) < 1e-12,
                "line spread moved the pair's mean timing");
        const auto lineAmps = [&](std::uint32_t first) {
            return oracleAmps(Chorus::drawnResistance(1800.0, first, calibration),
                              Chorus::drawnResistance(8200.0, first + 1u, calibration),
                              Chorus::drawnResistance(10000.0, first + 2u, calibration));
        };
        const double expected = std::sqrt(lineAmps(136u) / lineAmps(123u));
        require(std::abs(scale.delayA - expected) < 1e-12,
                "line A delay is not the inverse of its drawn clock current");

        // No draw may leave the class box: the worst corners bound the ratio.
        const double t = Chorus::carbonResistorToleranceClass * calibration;
        double lowest = 1.0, highest = 0.0;
        for (int corner = 0; corner < 8; ++corner)
        {
            const double amps = oracleAmps(1800.0 * (1.0 + ((corner & 1) ? t : -t)),
                                           8200.0 * (1.0 + ((corner & 2) ? t : -t)),
                                           10000.0 * (1.0 + ((corner & 4) ? t : -t)));
            lowest = std::min(lowest, amps);
            highest = std::max(highest, amps);
        }
        const double spread = std::abs(std::log(scale.delayA / scale.delayB));
        require(spread > 0.0 && spread <= std::log(highest / lowest),
                "line timing left the drawn resistor class");
        require(spread > previousSpread, "spread does not grow with Unit Character");
        previousSpread = spread;
    }
    const auto unit = Chorus::lineTimingScale(1.0f, true);
    std::cout << "Clock-source line delays at Character 1: A x" << unit.delayA
              << ", B x" << unit.delayB << '\n';
}

std::array<double, 2> countedCycles(bool spread, float calibration)
{
    auto chorus = std::make_unique<Chorus>();
    chorus->prepare(192000.0);
    std::array<double, 2> cycles {};
    auto last = Probe::writeIndices(*chorus);
    float left = 0.0f, right = 0.0f;
    // Two full Mode-I periods, so both flanks of the triangle are counted.
    const int samples = static_cast<int>(2.0 * 192000.0 / 0.5159334275);
    for (int n = 0; n < samples; ++n)
    {
        chorus->process(0.0f, ChorusMode::One, 0.0f, left, right, calibration,
                        true, false, false, ChorusTimingProfile::HardwareEvidence,
                        false, false, false, 0.0f, 1.0f, false, false, spread);
        const auto now = Probe::writeIndices(*chorus);
        for (std::size_t line = 0; line < 2; ++line)
            cycles[line] += (now[line] - last[line] + Chorus::cellPairs) % Chorus::cellPairs;
        last = now;
    }
    return cycles;
}

void reachesTheLineClocks()
{
    // Scaling a line's delay trajectory by s divides its instantaneous clock
    // by s, so the captures it makes over the same LFO cycles fall by s.
    const auto nominal = countedCycles(false, 1.0f);
    const auto drawn = countedCycles(true, 1.0f);
    const auto scale = Chorus::lineTimingScale(1.0f, true);
    require(std::abs(nominal[0] / drawn[0] - scale.delayA) < 2e-4
            && std::abs(nominal[1] / drawn[1] - scale.delayB) < 2e-4,
            "a line clock does not follow its own drawn source");
    const auto zero = countedCycles(true, 0.0f);
    require(zero == countedCycles(false, 0.0f),
            "Unit Character zero changed a line clock");
}
}

int main()
{
    try
    {
        nominalCurrent();
        scaleLaw();
        reachesTheLineClocks();
        std::cout << "Chorus line timing passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
