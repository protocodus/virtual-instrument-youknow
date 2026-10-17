#include "../Source/DSP/YouKnowChorus.h"

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

namespace youknow
{
struct YouKnowTestAccess
{
    static void forceNoiseRefresh(Chorus& c) { c.bucketNoiseCoefficientsValid_ = false; }

    template<class T> static bool same(const T& a, const T& b)
    { return std::memcmp(&a, &b, sizeof(T)) == 0; }
    template<class T, std::size_t N>
    static bool same(const std::array<T, N>& a, const std::array<T, N>& b)
    {
        for (std::size_t i = 0; i < N; ++i)
            if (!same(a[i], b[i])) return false;
        return true;
    }
    static bool sameNonlinear(const Chorus::SupportChain::NonlinearState& a,
                              const Chorus::SupportChain::NonlinearState& b)
    {
        return same(a.previousCurrent, b.previousCurrent)
            && same(a.previous2Current, b.previous2Current)
            && same(a.previous3Current, b.previous3Current)
            && same(a.junctionThermalVolts, b.junctionThermalVolts)
            && a.currentHistoryDepth == b.currentHistoryDepth
            && a.fallbackCount == b.fallbackCount
            && a.maximumIterations == b.maximumIterations
            && a.topology == b.topology && a.valid == b.valid;
    }
    static bool sameLine(const Chorus::Line& a, const Chorus::Line& b)
    {
        if (!(same(a.cells, b.cells) && a.writeIndex == b.writeIndex
            && same(a.clockPhase, b.clockPhase) && same(a.held, b.held)
            && same(a.previousInput, b.previousInput)
            && same(a.previousInput2, b.previousInput2)
            && same(a.previousInput3, b.previousInput3)
            && same(a.exactOutputState, b.exactOutputState)
            && sameNonlinear(a.nonlinearOutput, b.nonlinearOutput)
            && same(a.exactOutputPrevious, b.exactOutputPrevious)
            && same(a.exactOutputPrevious2, b.exactOutputPrevious2)
            && same(a.exactOutputPrevious3, b.exactOutputPrevious3)
            && same(a.transferState, b.transferState)
            && a.noiseState == b.noiseState
            && a.transferNoiseState == b.transferNoiseState
            && same(a.previousTransferNoise, b.previousTransferNoise)
            && a.outputEventCount == b.outputEventCount
            && a.pastBlepEventCount == b.pastBlepEventCount)) return false;
        // Compare members, never struct padding. These include the normalized
        // noise coefficients, so a stale cached mix fails immediately.
        const auto& x = a.bucketNoise; const auto& y = b.bucketNoise;
        if (!(same(x.storage, y.storage) && same(x.transfer, y.transfer)
            && same(x.correlation, y.correlation) && same(x.eta, y.eta)
            && same(x.aWeightedScale, y.aWeightedScale))) return false;
        for (std::size_t i = 0; i < a.outputEvents.size(); ++i)
            if (!same(a.outputEvents[i].jump, b.outputEvents[i].jump)
                || !same(a.outputEvents[i].ageInSamples, b.outputEvents[i].ageInSamples)) return false;
        for (std::size_t i = 0; i < a.pastBlepEvents.size(); ++i)
            if (!same(a.pastBlepEvents[i].jump, b.pastBlepEvents[i].jump)
                || !same(a.pastBlepEvents[i].ageInSamples, b.pastBlepEvents[i].ageInSamples)) return false;
        return true;
    }
    static bool sameState(const Chorus& a, const Chorus& b)
    {
        const auto& x = a.inputSupport_; const auto& y = b.inputSupport_;
        return sameLine(a.lineA_, b.lineA_) && sameLine(a.lineB_, b.lineB_)
            && same(x.couplingState, y.couplingState)
            && same(x.passiveState, y.passiveState)
            && same(x.antiAliasFirst.s1, y.antiAliasFirst.s1)
            && same(x.antiAliasFirst.s2, y.antiAliasFirst.s2)
            && same(x.antiAliasSecond.s1, y.antiAliasSecond.s1)
            && same(x.antiAliasSecond.s2, y.antiAliasSecond.s2)
            && same(x.exactState, y.exactState) && sameNonlinear(x.nonlinear, y.nonlinear)
            && same(x.exactPrevious, y.exactPrevious)
            && same(x.exactPrevious2, y.exactPrevious2)
            && same(x.exactPrevious3, y.exactPrevious3)
            && same(x.captureInterval.outputByPower, y.captureInterval.outputByPower)
            && same(x.captureInterval.initialOutput, y.captureInterval.initialOutput)
            && same(x.captureInterval.finalOutput, y.captureInterval.finalOutput)
            && x.captureInterval.enabled == y.captureInterval.enabled
            && same(a.lfoPhase_, b.lfoPhase_) && same(a.wetGain_, b.wetGain_)
            && same(a.muteDriveNodeVolts_, b.muteDriveNodeVolts_)
            && same(a.muteDriveHoldVolts_, b.muteDriveHoldVolts_)
            && same(a.clockMuteVolts_, b.clockMuteVolts_)
            && same(a.muteGateVolts_, b.muteGateVolts_)
            && same(a.wetInputConductanceRatio_, b.wetInputConductanceRatio_)
            && a.muteDriveMuted_ == b.muteDriveMuted_
            && a.clocksStopped_ == b.clocksStopped_
            && a.wetPathFlushPending_ == b.wetPathFlushPending_;
    }
};
}

namespace
{
using namespace youknow;
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

struct Controls { bool enabled; float fraction, correlation; };

void liveCacheLifecycle()
{
    auto cached = std::make_unique<Chorus>();
    auto reference = std::make_unique<Chorus>();
    require(cached->configureSupportProfile(ChorusSupportProfile::Nominal2SA1015Nonlinear)
        && reference->configureSupportProfile(ChorusSupportProfile::Nominal2SA1015Nonlinear),
        "could not configure nonlinear product chorus");
    cached->prepare(48000); reference->prepare(48000);
    std::size_t checked = 0;
    const auto step = [&](Controls controls, ChorusMode mode = ChorusMode::Two,
                          bool tryBypass = false) {
        const float input = float(.13 * std::sin(double(checked) * .071));
        float a {}, b {}, x {}, y {};
        // The reference repeats the original unconditionally prepared mix
        // every sample while sharing all physical processing with the cache.
        YouKnowTestAccess::forceNoiseRefresh(*reference);
        bool skippedA = false, skippedB = false;
        if (tryBypass)
        {
            skippedA = cached->processBypassedWhenSettled(input, a, b);
            skippedB = reference->processBypassedWhenSettled(input, x, y);
            require(skippedA == skippedB, "noise cache changed bypass eligibility");
        }
        const auto process = [&](Chorus& c, float& l, float& r) {
            c.process(input, mode, Chorus::defaultNoiseScale, l, r,
                1.0f, true, false, false, ChorusTimingProfile::OwnerBlend,
                false, false, controls.enabled, controls.fraction, controls.correlation, false);
        };
        if (!skippedA) process(*cached, a, b);
        if (!skippedB) process(*reference, x, y);
        require(YouKnowTestAccess::same(a, x) && YouKnowTestAccess::same(b, y),
                "noise cache changed a stereo output sample");
        require(YouKnowTestAccess::sameState(*cached, *reference),
                "noise cache changed physical/filter/noise state");
        ++checked;
        return skippedA;
    };
    const auto run = [&](Controls c, int frames) {
        for (int i = 0; i < frames; ++i) step(c);
    };

    constexpr Controls initial {true, .9171323f, 1};
    run(initial, 1024); // Warm, then repeat within a live BBD history.
    constexpr Controls changed {true, .7f, .3f};
    run(changed, 512);
    run({false, changed.fraction, changed.correlation}, 128);
    run(changed, 512); // Same keys as before disable must re-prime the mix.
    run({true, 1, 0}, 256);
    run({true, 0, 1}, 256);
    run({true, -0.0f, 1}, 128);
    run({true, +0.0f, 1}, 128);
    run({true, .7f, +0.0f}, 128);
    run({true, .7f, -0.0f}, 128);
    run({true, .7f, +0.0f}, 128);
    run({true, -1, 2}, 128);
    run({true, 2, -1}, 128);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    run({true, nan, nan}, 128);
    run({true, inf, -inf}, 128);
    run(initial, 512);

    cached->reset(); reference->reset();
    run(initial, 512); // Reset retains caller controls, but clears both lines.
    cached->prepare(192000, true); reference->prepare(192000, true);
    run(initial, 1024); // Preserve physical packet state on a numerical-rate change.

    // Exercise the real settled-bypass path. Clock muting is disabled above
    // because that separately modeled circuit deliberately forbids this skip.
    cached->prepare(48000, true); reference->prepare(48000, true);
    bool skipped = false;
    for (int i = 0; i < 48000 && !skipped; ++i)
        skipped = step(initial, ChorusMode::Off, true);
    require(skipped, "regression did not reach settled bypass");
    for (int i = 0; i < 32; ++i) step(initial, ChorusMode::Off, true);
    run(initial, 1024); // Reopening resets stale lines and must rebuild the mix.
    std::cout << checked << " exact stereo/state comparisons: live noise cache lifecycle passed\n";
}
}

int main()
{
    try { liveCacheLifecycle(); }
    catch (const std::exception& error)
    { std::cerr << error.what() << '\n'; return 1; }
}
