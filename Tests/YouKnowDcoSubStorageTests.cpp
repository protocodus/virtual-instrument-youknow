// A transistor storage delay belongs to continuous time, not to the remaining
// fraction of an audio sample. Check its chronology and duty harmonics against
// an independent delayed-square reference, for the 0.2 us part-class estimate
// and for the 5.82 us measured on #439522, which outlasts several intervals at
// high internal rates.
#include "DSP/YouKnowEngine.h"

#include <array>
#include <cmath>
#include <complex>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace youknow
{
struct YouKnowTestAccess
{
    static void configure(YouKnowEngine& engine, double rate,
                          double firstEdgeSeconds, bool skew,
                          bool measured = false)
    {
        engine.prepare(rate, 64, false);
        auto parameters = engine.activeParameters_;
        parameters.range = DcoRange::Four;
        parameters.calibration = 0;
        parameters.enableSubStorageSkew = skew;
        parameters.useMeasuredSubStorageSkew = measured;
        engine.setParameters(parameters);
        auto& voice = engine.voices_[0];
        auto& dco = voice.dco;
        dco.reset();
        dco.divider = 1000; // 4 MHz / 1000: sub period 0.5 ms.
        dco.periodSamples = rate / 4000.0;
        dco.pitState = YouKnowEngine::Dco::PitState::running;
        dco.pitOutHigh = false;
        dco.pitClocksToEvent = firstEdgeSeconds * 4e6;
        dco.subState = -1;
        dco.sub.prime(-1);
        dco.rampSlopePerSecond = 8000;
        voice.rampCurrentScale = 1;
    }

    static float advance(YouKnowEngine& engine, bool corrections = true)
    {
        auto& dco = engine.voices_[0].dco;
        engine.advanceDcoPitAndRamp(engine.voices_[0], DcoRange::Four,
            -1, -1, true, true, corrections);
        engine.advanceRangeClock(DcoRange::Four);
        return dco.sub.advance(dco.subOutputState());
    }

    static float physicalLevel(const YouKnowEngine& engine)
    {
        return engine.voices_[0].dco.subOutputState();
    }

    static float dividerLevel(const YouKnowEngine& engine)
    {
        return engine.voices_[0].dco.subState;
    }

    static double remaining(const YouKnowEngine& engine)
    {
        return engine.voices_[0].dco.subStorageSecondsRemaining;
    }
};
}

namespace
{
using Engine = youknow::YouKnowEngine;
using Probe = youknow::YouKnowTestAccess;
constexpr double pi = 3.14159265358979323846;
constexpr double storageSeconds = 0.2e-6;
constexpr double measuredStorageSeconds = 5.82e-6;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void checkBoundaryChronology()
{
    int cases = 0;
    for (double rate : { 8000., 44100., 48000., 96000., 192000., 768000. })
        for (bool corrections : { false, true })
            for (double timeBeforeEnd : { 0., 0.05e-6, 0.10e-6, 0.30e-6 })
            {
                auto engine = std::make_unique<Engine>();
                Probe::configure(*engine, rate, 1.0 / rate - timeBeforeEnd, true);
                Probe::advance(*engine, corrections);
                require(Probe::dividerLevel(*engine) == 1,
                        "divider did not change on the original PIT edge");
                const bool delayedPastBoundary = timeBeforeEnd < storageSeconds;
                require(Probe::physicalLevel(*engine)
                            == (delayedPastBoundary ? -1 : 1),
                        "collector edge was clipped to the current sample boundary");
                const double expectedRemaining = delayedPastBoundary
                    ? storageSeconds - timeBeforeEnd : 0;
                require(std::abs(Probe::remaining(*engine) - expectedRemaining) < 1e-14,
                        "storage delay changed with sample placement");
                Probe::advance(*engine, corrections);
                require(Probe::physicalLevel(*engine) == 1
                            && Probe::remaining(*engine) == 0,
                        "pending collector release did not complete in the next interval");
                ++cases;
            }
    std::cout << "sub storage boundary chronology: " << cases << " cases\n";
}

void checkMeasuredChronology()
{
    // At 192 and 768 kHz the measured delay outlasts one and four intervals:
    // the collector must stay low through each whole interval it spans and
    // rise in the one where the delay ends, wherever the PIT edge fell.
    require(Engine::subSwitchStorageMeasuredSeconds == measuredStorageSeconds,
            "the measured Tr19 storage time changed without this reference");
    int cases = 0;
    for (double rate : { 48000., 192000., 768000. })
        for (bool corrections : { false, true })
            for (double timeBeforeEnd : { 0., 0.3e-6, 1.0e-6 })
            {
                auto engine = std::make_unique<Engine>();
                Probe::configure(*engine, rate, 1.0 / rate - timeBeforeEnd, true, true);
                Probe::advance(*engine, corrections);
                require(Probe::dividerLevel(*engine) == 1,
                        "divider did not change on the original PIT edge");
                double remaining = measuredStorageSeconds - timeBeforeEnd;
                while (remaining > 0)
                {
                    require(Probe::physicalLevel(*engine) == -1,
                            "measured collector edge arrived early");
                    require(std::abs(Probe::remaining(*engine) - remaining) < 1e-12,
                            "measured storage delay drifted across intervals");
                    Probe::advance(*engine, corrections);
                    remaining -= 1.0 / rate;
                }
                require(Probe::physicalLevel(*engine) == 1
                            && Probe::remaining(*engine) == 0,
                        "measured collector release did not complete in its interval");
                ++cases;
            }
    std::cout << "measured sub storage chronology: " << cases << " cases\n";
}

double secondHarmonicRatio(double rate, double firstEdgeSeconds, bool skew,
                           bool measured = false)
{
    auto engine = std::make_unique<Engine>();
    Probe::configure(*engine, rate, firstEdgeSeconds, skew, measured);
    constexpr double subFrequency = 2000;
    const int periodSamples = static_cast<int>(rate / subFrequency);
    // The chosen sample rates have an integral number of samples per period.
    const int warmup = periodSamples * 8;
    for (int i = 0; i < warmup; ++i) Probe::advance(*engine);
    const int samples = periodSamples * 1024;
    std::array<std::complex<double>, 2> coefficients {};
    for (int i = 0; i < samples; ++i)
    {
        const double sample = Probe::advance(*engine);
        for (int harmonic = 1; harmonic <= 2; ++harmonic)
        {
            const double phase = -2 * pi * harmonic * subFrequency * i / rate;
            coefficients[static_cast<std::size_t>(harmonic - 1)]
                += sample * std::complex<double>(std::cos(phase), std::sin(phase));
        }
    }
    return std::abs(coefficients[1]) / std::abs(coefficients[0]);
}

void checkDutyHarmonics()
{
    // For a two-level waveform of duty D, |H2/H1| = |cos(pi*D)|.
    // Only its positive edge is delayed, giving D=1/2 - f_sub*t_storage.
    const double expected = std::sin(pi * 2000 * storageSeconds);
    double maximumDbError = 0;
    for (double rate : { 48000., 96000., 192000. })
        for (double beforeBoundary : { 0.025e-6, 0.10e-6, 0.30e-6, 1e-6 })
        {
            const double actual = secondHarmonicRatio(
                rate, 1.0 / rate - beforeBoundary, true);
            const double dbError = 20 * std::log10(actual / expected);
            maximumDbError = std::max(maximumDbError, std::abs(dbError));
            // Finite reconstruction-table precision dominates at this tiny
            // duty perturbation. A clipped 0.025us delay misses by 18 dB.
            require(std::abs(dbError) < 0.12,
                    "sub H2 depends on where the storage edge crosses the sample boundary");
        }
    const double measuredExpected = std::sin(pi * 2000 * measuredStorageSeconds);
    for (double rate : { 48000., 192000. })
    {
        const double actual = secondHarmonicRatio(rate, 1.0 / rate - 0.3e-6, true, true);
        require(std::abs(20 * std::log10(actual / measuredExpected)) < 0.05,
                "measured sub skew H2 departs from the delayed-square reference");
    }
    const double noSkew = secondHarmonicRatio(48000, 1.0 / 48000 - 0.1e-6, false);
    require(noSkew < 1e-6, "disabled storage skew generated even harmonics");
    std::cout << "delayed-square analytic H2: "
              << 20 * std::log10(expected) << " dBc, worst error "
              << maximumDbError << " dB across phases/rates\n";
}
}

int main()
{
    try
    {
        checkBoundaryChronology();
        checkMeasuredChronology();
        checkDutyHarmonics();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
