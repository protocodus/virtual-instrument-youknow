// Export the selected linear support response for offline stereo-capture
// identification. The analyzer solves these prepared transitions in the
// frequency domain; it does not replace them with a guessed low-pass corner.
// This describes the model, not a measured physical wet-path transfer.
// Defaults retain the historical raw reference. To analyze the current
// product, explicitly pass `owner-blend nominal-2sa1015-nonlinear`; recording both
// selections prevents an old ideal-buffer export from silently describing
// a new finite-buffer render. The fixed 192 kHz grid uses exact cubic input.
// `nominal-2sa1015-nonlinear` exports only that model's zero-signal tangent;
// its amplitude-dependent harmonics require a time-domain measurement.
#include "DSP/YouKnowChorus.h"

#include <iomanip>
#include <string_view>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    using youknow::Chorus;
    if (argc > 3)
    {
        std::cerr << "usage: YouKnowMeasureChorusSupport "
                     "[shipping|a11-spectral|a11-click|derived|owner-blend] "
                     "[ideal|nominal-2sa1015|nominal-2sa1015-nonlinear]\n";
        return 2;
    }
    constexpr float rate = 192000.0f;
    // Any of OQ-01's Mode I candidates can be measured, not just the shipping
    // value and the A11 spectral fit the flag used to choose between.
    auto profile = youknow::ChorusTimingProfile::Shipping;
    if (argc >= 2)
    {
        const std::string_view selected { argv[1] };
        if (selected == "a11-spectral" || selected == "a11-effective")
            profile = youknow::ChorusTimingProfile::A11Spectral;
        else if (selected == "a11-click")
            profile = youknow::ChorusTimingProfile::A11ClickTiming;
        else if (selected == "derived")
            profile = youknow::ChorusTimingProfile::DerivedNominal;
        else if (selected == "owner-blend")
            profile = youknow::ChorusTimingProfile::OwnerBlend;
        else if (selected != "shipping")
        {
            std::cerr << "unknown timing profile: " << selected << '\n';
            return 2;
        }
    }
    const std::string_view supportName { argc == 3 ? argv[2] : "ideal" };
    auto supportProfile = youknow::ChorusSupportProfile::IdealFollowers;
    if (supportName == "nominal-2sa1015")
        supportProfile = youknow::ChorusSupportProfile::Nominal2SA1015;
    else if (supportName == "nominal-2sa1015-nonlinear")
        supportProfile = youknow::ChorusSupportProfile::Nominal2SA1015Nonlinear;
    else if (supportName != "ideal")
    {
        std::cerr << "unknown support profile: " << supportName << '\n';
        return 2;
    }
    const auto support = Chorus::supportChainFor(rate, supportProfile);
    const auto mode = Chorus::settingsFor(youknow::ChorusMode::One, profile);
    std::cout << std::setprecision(17)
              << "{\"sample_rate\":" << rate
              << ",\"timing_profile\":\"" << (argc >= 2 ? argv[1] : "shipping")
              << "\",\"support_profile\":\"" << supportName
              << "\",\"input_scheme\":\"exact-cubic\"";
    if (supportProfile == youknow::ChorusSupportProfile::Nominal2SA1015Nonlinear)
        std::cout << ",\"response_scope\":\"zero-signal tangent; excludes nonlinear harmonics\"";
    std::cout << ",\"mode_one\":{\"centre_s\":" << mode.centreDelaySeconds
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
        // The finite circuit stores physical capacitor voltages; its
        // output is no longer necessarily the legacy node difference.
        std::cout << ",\"output_by_state\":[";
        for (std::size_t index = 0; index < 6; ++index)
        {
            if (index != 0)
                std::cout << ',';
            std::cout << (transition == &support.exactInput
                ? (index == 5 ? 1.0 : 0.0)
                : transition->outputByState[index]);
        }
        std::cout << "],\"output_direct\":" << transition->outputDirect;
        std::cout << '}';
    }
    std::cout << "]}\n";
}
