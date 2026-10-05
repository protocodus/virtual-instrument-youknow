#include "../Source/DSP/YouKnowCoupledMixer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace
{
using Mixer = youknow::CoupledSubMixer;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool sameBits(double first, double second)
{
    return std::bit_cast<std::uint64_t>(first) == std::bit_cast<std::uint64_t>(second);
}

// Frozen C56 update composed from the independent on-demand public node
// solve. It never reads PreparedCoefficients or the instance process path.
struct ReferenceC56
{
    double capacitorVolts {};
    double capacitorAmps {};

    void prime(const Mixer::Calibration& c, double source, double rail, double gate)
    {
        capacitorVolts = Mixer::solve(c, source, rail, gate, 0.0, 0.0).waveVolts;
        capacitorAmps = 0.0;
    }

    Mixer::Result process(const Mixer::Calibration& c, double source,
                          double rail, double gate, double seconds)
    {
        const double companionOhms = seconds / (2.0 * Mixer::couplingFarads);
        const double history = capacitorVolts + companionOhms * capacitorAmps;
        const auto result = Mixer::solve(c, source, rail, gate,
            1.0 / (c.loadOhms + companionOhms), history);
        capacitorVolts = result.waveVolts - result.filterVolts;
        capacitorAmps = result.capacitorAmps;
        return result;
    }
};

void testPreparedTrajectory()
{
    const auto nominal = Mixer::evidenceCalibration();
    auto alternate = nominal;
    alternate.sourceOhms = 32000.0;
    alternate.loadOhms = 8200.0;
    alternate.diodeSlopeVolts = 0.041;
    alternate.diodeReferenceAmps = 0.000082;
    alternate.diodeDropVolts = 0.71;
    alternate.collectorOnVolts = 0.13;
    auto constantDrop = nominal;
    constantDrop.sourceOhms = 10000.0;
    constantDrop.loadOhms = 47000.0;
    constantDrop.diodeSlopeVolts = 0.0;
    constantDrop.diodeReferenceAmps = 0.0;
    constantDrop.collectorOnVolts = 0.1;
    const std::array calibrations { nominal, alternate, constantDrop };
    constexpr std::array rates { 8000.0, 44100.0, 48000.0, 96000.0, 192000.0 };
    constexpr std::array gates { -0.2, 0.0, 1.0e-12, 0.3,
        1.0 - 1.0e-12, 1.0, 1.2 };

    Mixer mixer;
    ReferenceC56 reference;
    mixer.prime(nominal, 0.4, 5.0, 0.5);
    reference.prime(nominal, 0.4, 5.0, 0.5);
    int checked = 0;
    for (const auto& calibration : calibrations)
    {
        require(calibration.valid(), "prepared trajectory calibration invalid");
        for (const double rate : rates)
            for (const bool promotedFloatInterval : { false, true })
            {
                // Engine C56 uses its float reciprocal promoted to double;
                // direct scientific fixtures also use an exact double step.
                const double seconds = promotedFloatInterval
                    ? static_cast<double>(static_cast<float>(1.0 / rate))
                    : 1.0 / rate;
                const auto coefficients = Mixer::prepareCoefficients(calibration, seconds);
                const double chargeBefore = mixer.capacitorVolts();
                require(sameBits(chargeBefore, reference.capacitorVolts),
                        "coefficient rebuild replaced retained C56 charge");
                for (int sample = 0; sample < 64; ++sample)
                {
                    const double source = 0.12 + 2.3 * std::sin(checked * 0.17);
                    const double rail = Mixer::railFullScaleVolts
                        * (0.5 + 0.49 * std::sin(checked * 0.013));
                    const double gate = gates[static_cast<std::size_t>(sample) % gates.size()];
                    const auto expected = reference.process(calibration, source, rail, gate, seconds);
                    const auto actual = mixer.process(calibration, coefficients, source, rail, gate);
                    require(sameBits(actual.waveVolts, expected.waveVolts)
                        && sameBits(actual.filterVolts, expected.filterVolts)
                        && sameBits(actual.capacitorAmps, expected.capacitorAmps)
                        && sameBits(actual.subOffAmps, expected.subOffAmps)
                        && sameBits(actual.subOnAmps, expected.subOnAmps),
                        "prepared mixer changed the public node solve");
                    require(sameBits(mixer.capacitorVolts(), reference.capacitorVolts),
                            "prepared mixer changed the retained C56 trajectory");
                    ++checked;
                }
            }
    }
    std::cout << "prepared/on-demand C56 trajectories: " << checked
              << " samples bit-identical across circuits, rates and gate boundaries\n";
}

// Independent log-current bisection, followed by node-voltage bisection.
// It does not reuse the production Newton solve or its initial estimate.
long double diodeCurrent(const Mixer::Calibration& c, long double voltage,
                         long double resistance)
{
    long double low = -120.0L, high = 5.0L;
    for (int iteration = 0; iteration < 100; ++iteration)
    {
        const long double logarithm = (low + high) * 0.5L;
        const long double residual = resistance * std::exp(logarithm)
            + c.diodeDropVolts + c.diodeSlopeVolts
                * (logarithm - std::log(static_cast<long double>(c.diodeReferenceAmps)))
            - voltage;
        if (residual > 0.0L)
            high = logarithm;
        else
            low = logarithm;
    }
    return std::exp((low + high) * 0.5L);
}

long double referenceWave(const Mixer::Calibration& c, double source,
    double rail, double gate, double loadG, double history)
{
    const long double sourceG = 1.0L / c.sourceOhms;
    const long double baseG = sourceG + loadG;
    const long double base = (sourceG * source + loadG * history) / baseG;
    long double low = base;
    long double high = std::max({ base, static_cast<long double>(rail),
        static_cast<long double>(c.collectorOnVolts) }) + 1.0L;
    for (int iteration = 0; iteration < 100; ++iteration)
    {
        const long double wave = (low + high) * 0.5L;
        const long double current = gate * diodeCurrent(c, rail - wave, 60000.0L)
            + (1.0L - gate) * diodeCurrent(c, c.collectorOnVolts - wave, 27000.0L);
        const long double residual = baseG * (wave - base) - current;
        if (residual > 0.0L)
            high = wave;
        else
            low = wave;
    }
    return (low + high) * 0.5L;
}

double settledSubFundamentalEquivalentSwing(const Mixer::Calibration& c, double control)
{
    Mixer mixer;
    const double source = c.sourceBiasVolts + c.sourceScale
        * c.oscillatorDriveScale * 6.0 * c.pulseSourceScale;
    const double rail = Mixer::railFullScaleVolts * control;
    mixer.prime(c, source, rail, 0.5);
    constexpr int rate = 48000;
    constexpr int samplesPerHalf = 240; // 100 Hz isolated SUB
    double real = 0.0, imaginary = 0.0;
    for (int sample = 0; sample < rate; ++sample)
    {
        const double gate = (sample / samplesPerHalf) % 2;
        const auto value = mixer.process(c, source, rail, gate, 1.0 / rate);
        if (sample >= rate - 4800)
        {
            const double phase = 2.0 * std::acos(-1.0) * 100.0 * sample / rate;
            real += value.filterVolts * std::cos(phase);
            imaginary += value.filterVolts * std::sin(phase);
        }
    }
    // The original calibration analyzer uses the isolated fundamental. A
    // square's peak-to-peak equivalent is pi times its complex coefficient;
    // unlike raw extrema this is not raised by C56's within-cycle droop.
    return std::acos(-1.0) * std::hypot(real, imaginary) / 4800.0;
}
}

int main()
{
    try
    {
        auto c = Mixer::evidenceCalibration();
        require(c.valid(), "evidence calibration invalid");
        auto invalid = c;
        invalid.diodeReferenceAmps = 0.0;
        require(!invalid.valid(), "soft diode accepted missing current reference");
        require(std::abs(c.pinToCoreGain - 68000.0 / 4700.0) < 1.0e-14,
                "physical hybrid input coordinate changed");
        const double effective = c.sourceOhms * c.loadOhms / (c.sourceOhms + c.loadOhms);
        require(std::abs(effective * c.diodeReferenceAmps * c.pinToCoreGain
            - 2.0 * 7.57 * 0.738 * 0.4) < 1.0e-12,
            "SUB endpoint prior not preserved");
        require(std::abs(c.sourceScale * c.pinToCoreGain
            * c.loadOhms / (c.sourceOhms + c.loadOhms) - 0.4) < 1.0e-14,
            "independent source-coordinate mapping changed");

        double worstVolts = 0.0, worstAmps = 0.0;
        int cases = 0;
        for (const double source : { -3.0, -0.1, 0.425, 3.0 })
            for (const double rail : { 0.0, 1.0, Mixer::railFullScaleVolts })
                for (const double gate : { 0.0, 0.3, 1.0 })
                    for (const double loadG : { 0.0, 1.0 / c.loadOhms })
                        for (const double history : { -1.0, 2.0 })
                        {
                            const auto result = Mixer::solve(c, source, rail, gate, loadG, history);
                            const auto expected = referenceWave(c, source, rail, gate, loadG, history);
                            worstVolts = std::max(worstVolts,
                                std::abs(result.waveVolts - static_cast<double>(expected)));
                            const double residual = (source - result.waveVolts) / c.sourceOhms
                                + (history - result.waveVolts) * loadG
                                + result.subOffAmps + result.subOnAmps;
                            worstAmps = std::max(worstAmps, std::abs(residual));
                            ++cases;
                        }
        require(worstVolts < 2.0e-12, "soft coupled mixer disagrees with independent KCL oracle");
        require(worstAmps < 1.0e-14, "soft coupled mixer loses junction current");

        // Adding SUB changes the same node's incremental source gain. A sum
        // of independent waveform rails cannot satisfy both operating points.
        const double g = 1.0 / c.loadOhms;
        const auto dryLow = Mixer::solve(c, 0.4, 0.0, 1.0, g, 0.4);
        const auto dryHigh = Mixer::solve(c, 0.6, 0.0, 1.0, g, 0.4);
        const auto subLow = Mixer::solve(c, 0.4, Mixer::railFullScaleVolts, 1.0, g, 0.4);
        const auto subHigh = Mixer::solve(c, 0.6, Mixer::railFullScaleVolts, 1.0, g, 0.4);
        require((subHigh.waveVolts - subLow.waveVolts)
            < 0.98 * (dryHigh.waveVolts - dryLow.waveVolts),
            "conducting SUB diode failed to load moving WAVE source");

        double worstExtremeRelativeKcl = 0.0;
        for (const double sourceR : { 1.0, 1.0e9 })
            for (const double loadR : { 1.0, 1.0e9 })
                for (const double slope : { 1.0e-4, 0.2 })
                    for (const double reference : { 1.0e-30, 0.1 })
                        for (const double source : { -2000.0, 2000.0 })
                            for (const double rail : { -30.0, 30.0 })
                                for (const double gate : { 0.0, 0.5, 1.0 })
                                {
                                    auto extreme = c;
                                    extreme.sourceOhms = sourceR;
                                    extreme.loadOhms = loadR;
                                    extreme.diodeSlopeVolts = slope;
                                    extreme.diodeReferenceAmps = reference;
                                    require(extreme.valid(), "extreme numerical fixture invalid");
                                    const auto value = Mixer::solve(extreme, source, rail, gate,
                                                                  1.0 / loadR, -1000.0);
                                    require(std::isfinite(value.waveVolts)
                                        && std::isfinite(value.filterVolts)
                                        && std::isfinite(value.subOffAmps)
                                        && std::isfinite(value.subOnAmps),
                                        "validated mixer domain produced non-finite state");
                                    const double sourceCurrent = (source - value.waveVolts) / sourceR;
                                    const double loadCurrent = (-1000.0 - value.waveVolts) / loadR;
                                    const double residual = sourceCurrent + loadCurrent
                                        + value.subOffAmps + value.subOnAmps;
                                    const double scale = std::max(1.0e-12, std::abs(sourceCurrent)
                                        + std::abs(loadCurrent) + value.subOffAmps + value.subOnAmps);
                                    worstExtremeRelativeKcl = std::max(worstExtremeRelativeKcl,
                                                                     std::abs(residual) / scale);
                                }
        std::cout << "extreme relative KCL=" << worstExtremeRelativeKcl << '\n';
        require(worstExtremeRelativeKcl < 1.0e-6,
                "bounded mixer Newton solve lost current at validated numerical limits");

        const double full = settledSubFundamentalEquivalentSwing(c, 1.0);
        double worstLawDb = 0.0;
        for (const int byte : { 25, 38, 51, 64, 76, 89, 102, 114 })
        {
            const double control = byte / 127.0;
            const double actual = settledSubFundamentalEquivalentSwing(c, control) / full;
            const double expected = youknow::SubLevelDiodeLaw::exactGain(control);
            worstLawDb = std::max(worstLawDb, std::abs(20.0 * std::log10(actual / expected)));
        }
        // The capture fixes a settled aggregate. Finite C56 ripple creates a
        // small pitch-dependent departure; do not force its endpoint by gain.
        require(worstLawDb < 0.1, "coupled diode discarded calibrated isolated SUB law");
        std::cout << "C56 endpoint error dB=" << 20.0 * std::log10(full * c.pinToCoreGain
            / (2.0 * 7.57 * 0.738 * 0.4)) << " SUB law error dB=" << worstLawDb << '\n';
        require(std::abs(20.0 * std::log10(full * c.pinToCoreGain
            / (2.0 * 7.57 * 0.738 * 0.4))) < 0.15,
            "finite C56 circuit lost nominal SUB balance");

        Mixer mixer;
        mixer.prime(c, 0.4, 5.0, 0.5);
        for (int i = 0; i < 1000; ++i)
            static_cast<void>(mixer.process(c, 0.4 + 0.2 * std::sin(i * 0.12), 5.0,
                          (i / 20) % 2, 1.0 / 48000.0));
        const double chargeBefore = mixer.capacitorVolts();
        require(std::isfinite(chargeBefore), "coupled capacitor state not finite");
        const auto changedRate = mixer.process(c, 0.4, 5.0, 0.0, 1.0 / 192000.0);
        require(std::isfinite(changedRate.filterVolts)
            && std::abs(mixer.capacitorVolts() - chargeBefore) < 0.01,
            "rate change replaced retained C56 voltage");
        testPreparedTrajectory();
        std::cout << "soft coupled mixer: cases=" << cases
                  << " worst_node_error_V=" << worstVolts
                  << " worst_KCL_A=" << worstAmps
                  << " extreme_relative_KCL=" << worstExtremeRelativeKcl
                  << " isolated_SUB_law_error_dB=" << worstLawDb << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
