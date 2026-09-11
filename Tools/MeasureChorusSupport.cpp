// Export the shipping linear support response for offline stereo-capture
// identification. The analyzer solves these prepared transitions in the
// frequency domain; it does not replace them with a guessed low-pass corner.
// This describes the model, not a measured physical wet-path transfer.
#include "DSP/YouKnowChorus.h"

#include <iomanip>
#include <string_view>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    using youknow::Chorus;
    if (argc > 2)
    {
        std::cerr << "usage: YouKnowMeasureChorusSupport "
                     "[shipping|a11-spectral|a11-click|derived]\n";
        return 2;
    }
    constexpr float rate = 192000.0f;
    const auto support = Chorus::supportChainFor(rate);
    // Any of OQ-01's Mode I candidates can be measured, not just the shipping
    // value and the A11 spectral fit the flag used to choose between.
    auto profile = youknow::ChorusTimingProfile::Shipping;
    if (argc == 2)
    {
        const std::string_view selected { argv[1] };
        if (selected == "a11-spectral" || selected == "a11-effective")
            profile = youknow::ChorusTimingProfile::A11Spectral;
        else if (selected == "a11-click")
            profile = youknow::ChorusTimingProfile::A11ClickTiming;
        else if (selected == "derived")
            profile = youknow::ChorusTimingProfile::DerivedNominal;
        else if (selected != "shipping")
        {
            std::cerr << "unknown timing profile: " << selected << '\n';
            return 2;
        }
    }
    const auto mode = Chorus::settingsFor(youknow::ChorusMode::One, profile);
    std::cout << std::setprecision(17)
              << "{\"sample_rate\":" << rate
              << ",\"mode_one\":{\"centre_s\":" << mode.centreDelaySeconds
              << ",\"depth_s\":" << mode.sweepSeconds
              << ",\"rate_hz\":" << mode.rateHz << "},\"networks\":[";
    bool firstNetwork = true;
    for (const auto* transition : { &support.exactInput, &support.exactOutputConnected })
    {
        if (!firstNetwork)
            std::cout << ',';
        firstNetwork = false;
        const auto writeRows = [](const auto& rows) {
            std::cout << '[';
            for (std::size_t row = 0; row < rows.size(); ++row)
            {
                if (row != 0)
                    std::cout << ',';
                std::cout << '[';
                for (std::size_t column = 0; column < rows[row].size(); ++column)
                {
                    if (column != 0)
                        std::cout << ',';
                    std::cout << rows[row][column];
                }
                std::cout << ']';
            }
            std::cout << ']';
        };
        std::cout << "{\"state_by_column\":";
        writeRows(transition->stateByColumn);
        std::cout << ",\"drive_by_sample\":";
        writeRows(transition->driveBySample);
        std::cout << '}';
    }
    std::cout << "]}\n";
}
