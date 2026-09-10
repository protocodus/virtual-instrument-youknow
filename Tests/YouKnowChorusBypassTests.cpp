#include "../Source/DSP/YouKnowChorus.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace youknow
{
struct YouKnowTestAccess
{
    static float wetGain(const Chorus& chorus) noexcept
    {
        return chorus.wetGain_;
    }
};
}

namespace
{
using youknow::Chorus;
using youknow::ChorusMode;

struct Interval
{
    ChorusMode mode;
    double seconds;
};

void checkControlContinuity(float rate, bool enableMuteDrive)
{
    Chorus exact;
    Chorus skipped;
    exact.prepare(rate);
    skipped.prepare(rate);

    // Long Off reaches the bypass optimization. The short intervals also
    // interrupt capacitor charging and discharging before either has settled.
    constexpr std::array intervals {
        Interval { ChorusMode::One, 0.02 },
        Interval { ChorusMode::Off, 1.0 },
        Interval { ChorusMode::One, 0.2 },
        Interval { ChorusMode::Off, 0.06 },
        Interval { ChorusMode::Two, 0.025 },
        Interval { ChorusMode::Off, 0.25 },
        Interval { ChorusMode::One, 0.035 },
        Interval { ChorusMode::Off, 0.15 },
        Interval { ChorusMode::Two, 0.2 }
    };
    int skippedSamples = 0;
    int reopened = 0;
    bool previouslyMuted = false;
    for (const auto interval : intervals)
    {
        bool mismatch = false;
        int exactOpening = -1;
        int skippedOpening = -1;
        const int frames = static_cast<int>(interval.seconds * rate);
        for (int frame = 0; frame < frames; ++frame)
        {
            float exactLeft {}, exactRight {}, skippedLeft {}, skippedRight {};
            const auto fullStep = [&](Chorus& chorus, float& left, float& right) {
                chorus.process(0.0f, interval.mode, 0.0f, left, right,
                               false, false, 1.0f, false, true,
                               enableMuteDrive, false);
            };
            fullStep(exact, exactLeft, exactRight);
            if (interval.mode == ChorusMode::Off
                && skipped.processBypassedWhenSettled(
                    0.0f, skippedLeft, skippedRight))
                ++skippedSamples;
            else
                fullStep(skipped, skippedLeft, skippedRight);

            // The two wet audio histories are intentionally different under
            // the fast-mode policy. Tr4 must still switch on the same sample.
            mismatch |= exact.muteDriveMuted() != skipped.muteDriveMuted();
            if (!exact.muteDriveMuted() && exactOpening < 0)
                exactOpening = frame;
            if (!skipped.muteDriveMuted() && skippedOpening < 0)
                skippedOpening = frame;
            if (previouslyMuted && !exact.muteDriveMuted())
                ++reopened;
            previouslyMuted = exact.muteDriveMuted();
        }
        if (mismatch)
        {
            std::cerr << "Mute timing changed after settled bypass at "
                      << rate << " Hz: exact opened at "
                      << 1000.0 * exactOpening / rate << " ms; skipped at "
                      << 1000.0 * skippedOpening / rate << " ms\n";
            std::exit(1);
        }
    }
    if (skippedSamples == 0 || reopened < 2)
    {
        std::cerr << "Regression did not exercise bypass and re-engagement\n";
        std::exit(1);
    }
    std::cout << rate << " Hz, mute drive " << enableMuteDrive << ": "
              << skippedSamples << " skipped samples; " << reopened
              << " matching openings\n";
}

// The settled skip asks for an exact zero, and the 5 ms glide only decays
// geometrically towards it. Without the flush at FLT_MIN the gain parks on a
// denormal a little below 1.4e-42 and the skip never engages in any build
// that lacks the plug-in's ScopedNoDenormals -- every JUCE-free tool and this
// test included. Nothing here reproduces that flush: the engine has to settle
// on its own.
void checkSettledSkipEngagesWithoutFlushToZero(float rate, bool enableMuteDrive)
{
    Chorus chorus;
    chorus.prepare(rate);
    const auto run = [&](ChorusMode mode, double seconds) {
        const int frames = static_cast<int>(seconds * rate);
        for (int frame = 0; frame < frames; ++frame)
        {
            float left {}, right {};
            chorus.process(0.0f, mode, 0.0f, left, right,
                           false, false, 1.0f, false, true,
                           enableMuteDrive, false);
        }
    };
    run(ChorusMode::One, 0.02);
    float left {}, right {};
    if (chorus.processBypassedWhenSettled(0.0f, left, right))
    {
        std::cerr << "Settled skip engaged while the wet path was open at "
                  << rate << " Hz\n";
        std::exit(1);
    }

    // From unity the glide passes FLT_MIN after ln(1 / FLT_MIN) = 87.3 time
    // constants, 437 ms; the denormal park would follow at 482 ms. One
    // second of Off is well past both, for either mute-drive policy.
    run(ChorusMode::Off, 1.0);
    const float gain = youknow::YouKnowTestAccess::wetGain(chorus);
    if (gain != 0.0f
        || !chorus.processBypassedWhenSettled(0.0f, left, right))
    {
        std::cerr << "Settled skip did not engage after 1 s of Off at "
                  << rate << " Hz, mute drive " << enableMuteDrive
                  << ": wet gain " << gain << "\n";
        std::exit(1);
    }
    std::cout << rate << " Hz, mute drive " << enableMuteDrive
              << ": settled skip engaged with wet gain exactly zero\n";
}
} // namespace

int main()
{
    for (const float rate : { 48000.0f, 192000.0f })
        for (const bool enabled : { false, true })
        {
            checkSettledSkipEngagesWithoutFlushToZero(rate, enabled);
            checkControlContinuity(rate, enabled);
        }
}
