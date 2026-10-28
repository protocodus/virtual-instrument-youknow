// Tr11/Tr12, the series 2SK30A wet switches of Roland jack board p. 15, at
// full signal. The oracle bisects the series KCL with its own symmetric
// Shichman-Hodges channel and the D4/D5 gate rule, sharing no algebra with
// the closed forms in ChorusMuteDrive.
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=15
#include "DSP/YouKnowChorus.h"
#include "DSP/YouKnowChorusMuteDrive.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace { thread_local bool countAllocations=false; thread_local std::size_t allocationCount=0; }
void* operator new(std::size_t n) { if(countAllocations)++allocationCount; if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}

namespace
{
using namespace youknow;
using Drive = ChorusMuteDrive;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// Drain-to-source current of an N channel between terminals a and b.
double channelAmps(double a, double b, double gate)
{
    const double source = std::min(a, b), drain = std::max(a, b);
    const double overdrive = gate - source - (-1.8);
    if (overdrive <= 0.0) return 0.0;
    const double vds = drain - source, beta = 2.8e-3 / (1.8 * 1.8);
    const double amps = vds < overdrive ? beta * (2.0 * overdrive * vds - vds * vds)
                                        : beta * overdrive * overdrive;
    return a >= b ? amps : -amps;
}

double oracleNode(double source, double drive)
{
    // Node x between the channel and R72/R74's 39k to the virtual earth:
    // channel current a->x equals x/39k. The floating gate cannot rise above
    // the channel's lower terminal; D4/D5 hold it at `drive` below that.
    const auto residual = [&](double x) {
        const double gate = std::min(drive, std::min(source, x));
        return channelAmps(source, x, gate) - x / 39000.0;
    };
    double low = std::min(source, 0.0), high = std::max(source, 0.0);
    for (int i = 0; i < 200; ++i)
    {
        const double middle = 0.5 * (low + high);
        (residual(middle) > 0.0 ? low : high) = middle;
    }
    return 0.5 * (low + high);
}

void closedFormsAgainstOracle()
{
    double worst = 0.0;
    for (double drive = -16.0; drive <= 16.0; drive += 0.0625)
        for (double source = -6.0; source <= 6.0; source += 0.015625)
            worst = std::max(worst, std::abs(Drive::channelOutputVolts(source, drive)
                                             - oracleNode(source, drive)));
    require(worst < 1e-9, "closed-form switch disagrees with the bisected circuit");
    std::cout << "Wet switch closed form vs bisection: " << worst << " V\n";
}

void smallSignalIsTheExistingLaw()
{
    for (double drive : { -3.0, -1.7, -1.2, -0.6, -0.2, 0.0, 0.4, 15.6 })
        for (double source : { 1e-7, -1e-7 })
        {
            const double ratio = Drive::channelOutputVolts(source, drive) / source;
            const double expected = Drive::conductanceRatio(std::min(drive, 0.0));
            require(std::abs(ratio - expected) <= 1e-5 * std::max(expected, 1e-3),
                    "small-signal switch departed from the incremental channel law");
        }
}

double harmonicDb(int harmonic, double amplitude, double drive)
{
    constexpr int points = 8192;
    double first = 0.0, firstQ = 0.0, upper = 0.0, upperQ = 0.0;
    for (int n = 0; n < points; ++n)
    {
        const double phase = 2.0 * std::numbers::pi * n / points;
        const double y = Drive::channelOutputVolts(amplitude * std::sin(phase), drive);
        first += y * std::sin(phase); firstQ += y * std::cos(phase);
        upper += y * std::sin(harmonic * phase); upperQ += y * std::cos(harmonic * phase);
    }
    return 10.0 * std::log10(std::max(upper * upper + upperQ * upperQ, 1e-300)
                             / (first * first + firstQ * firstQ));
}

void settledStates()
{
    // Settled on: Tr4 off, collector at +15 V. The floating gate follows the
    // source on both halves, so the channel is odd-symmetric: no even
    // harmonic, and a third harmonic from the Vds^2 term alone.
    const double conducting = Drive::gateDriveVolts(-14.04);
    require(conducting > 15.0, "settled-on gate drive is not near the pull-up");
    const double onH2 = harmonicDb(2, 1.0, conducting);
    const double onH3 = harmonicDb(3, 1.0, conducting);
    require(onH2 < -200.0 && onH3 < -105.0 && onH3 > -115.0,
            "conducting switch distortion left the derived Vds^2 size");
    // Settled off: the gates sit a junction above -15 V; nothing passes.
    for (double source : { -10.0, -3.0, 0.0, 3.0, 10.0 })
        require(Drive::channelOutputVolts(source, -14.4) == 0.0, "muted switch leaks");
    // Mid-transition the gate is pinned: the negative half conducts while the
    // positive half is pinched near drive + |Vp|.
    const double drive = -0.8;
    require(-Drive::channelOutputVolts(-1.5, drive) > Drive::channelOutputVolts(1.5, drive),
            "transition does not favour the source-end (negative) half");
    const double limit = drive + 1.8;
    require(Drive::channelOutputVolts(3.0, drive) < limit
            && Drive::channelOutputVolts(5.0, drive) == Drive::channelOutputVolts(3.0, drive),
            "pinched half is not limited at the drain end");
    const double midH2 = harmonicDb(2, 1.0, -0.6);
    require(midH2 > -45.0 && midH2 < -30.0, "a half-open switch left its derived distortion");
    std::cout << "Wet switch at 1 V: on H3 " << onH3 << " dB, half-open H2 " << midH2 << " dB\n";
}

void chorusToggle()
{
    // Same chorus run with the small-signal and large-signal switch through
    // a settled on, an off command, a settled off and an on command.
    constexpr double rate = 96000.0;
    std::vector<std::array<float, 2>> outputs[2];
    for (int large = 0; large < 2; ++large)
    {
        auto chorus = std::make_unique<Chorus>();
        chorus->prepare(rate);
        float left = 0.0f, right = 0.0f;
        const int length = static_cast<int>(1.2 * rate);
        outputs[large].resize(static_cast<std::size_t>(length));
        countAllocations = true; allocationCount = 0;
        for (int n = 0; n < length; ++n)
        {
            const bool on = n < static_cast<int>(0.3 * rate) || n >= static_cast<int>(0.8 * rate);
            const float input = 0.5f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 220.0 * n / rate));
            chorus->process(input, on ? ChorusMode::One : ChorusMode::Off, 0.0f, left, right,
                            1.0f, true, true, false, ChorusTimingProfile::HardwareEvidence,
                            true, true, false, 0.0f, 1.0f, true, true, false, false,
                            large == 1);
            outputs[large][static_cast<std::size_t>(n)] = { left, right };
        }
        countAllocations = false;
        require(allocationCount == 0, "large-signal switch allocates");
    }
    const auto difference = [&](double from, double to) {
        double worst = 0.0, level = 0.0;
        for (auto n = static_cast<std::size_t>(from * rate); n < static_cast<std::size_t>(to * rate); ++n)
            for (int c = 0; c < 2; ++c)
            {
                worst = std::max(worst, static_cast<double>(std::abs(outputs[1][n][c] - outputs[0][n][c])));
                level = std::max(level, static_cast<double>(std::abs(outputs[0][n][c])));
            }
        return 20.0 * std::log10(std::max(worst, 1e-30) / level);
    };
    const double settledOn = difference(0.1, 0.3);
    const double muting = difference(0.3, 0.6);
    const double settledOff = difference(0.65, 0.8);
    const double opening = difference(0.8, 1.2);
    require(settledOn < -80.0, "settled switch is audibly different from the incremental law");
    require(settledOff < -200.0, "a settled mute differs");
    require(muting > -40.0 && opening > -40.0,
            "the switch transitions do not reach the signal");
    std::cout << "Chorus toggle large-signal minus small-signal: on " << settledOn
              << " dB, muting " << muting << " dB, off " << settledOff
              << " dB, opening " << opening << " dB\n";
}
}

int main()
{
    try
    {
        closedFormsAgainstOracle();
        smallSignalIsTheExistingLaw();
        settledStates();
        chorusToggle();
        std::cout << "Chorus wet-switch law passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        countAllocations = false;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
