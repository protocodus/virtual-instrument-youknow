// Independent linear-network checks of the four resistor-noise injection paths.
#include "DSP/YouKnowEngine.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numbers>

namespace youknow
{
struct YouKnowTestAccess
{
    using Cascade = YouKnowEngine::OtaCascade;
    static Cascade::Tableau noiseTableau(float omega)
    {
        // Fixture controls: unit stage scales, Early enabled, Character=1,
        // RES=3 and the Normal solver. Assert explicit expected rungs below.
        return Cascade::planTableau(youknow::VcfSolverMode::Rk4Single,
            static_cast<double>(omega) * (1.0 + YouKnowEngine::otaEarlyEffectCoefficient), 3.0);
    }
    static bool sameNoiseState(const YouKnowEngine& a, const YouKnowEngine& b)
    {
        for (std::size_t card = 0; card < 6; ++card)
        {
            const auto& first = a.voices_[card];
            const auto& second = b.voices_[card];
            if (first.noiseState != second.noiseState
                || first.filter.stageNoiseHistory != second.filter.stageNoiseHistory)
                return false;
        }
        return true;
    }
    static std::array<double, 4> draw(YouKnowEngine& engine, int card)
    {
        auto& voice = engine.voices_[static_cast<std::size_t>(card)];
        engine.freewheelVoiceCard(voice);
        std::array<double, 4> sample{};
        for (std::size_t i = 0; i < 4; ++i)
            sample[i] = voice.filter.stageNoiseHistory[i][0];
        return sample;
    }
};
} // namespace youknow
using Cascade = youknow::YouKnowTestAccess::Cascade;
namespace
{
int failures = 0;
void require(bool ok, const char* message)
{
    if (!ok)
    {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
float advance(Cascade& c, double fs, double fc, float feedback, youknow::VcfTanhMode mode)
{
    return c.process(0.0f, static_cast<float>(2.0 * std::numbers::pi * fc / fs), feedback, 6.366f,
                     false, 0.0f, nullptr, mode, youknow::VcfSolverMode::Rk4Single);
}

void transfer()
{
    // For an ideal linearized stage H=1/(1+s/w), independent source j
    // reaches the output through H^(4-j)/(1+k*H^4). This circuit equation
    // is separate from the production RK tableau and its input interpolation.
    // Sum squared transfer magnitudes, never amplitudes, for independent PSD.
    double worstDb = 0.0, worstPhase = 0.0, worstPsdDb = 0.0;
    for (const double fs : { 48000.0, 192000.0 })
        for (const float k : { 0.0f, 3.0f })
            for (const double f : { 125.0, 1000.0, 8000.0 })
                for (const auto mode :
                     { youknow::VcfTanhMode::Exact, youknow::VcfTanhMode::PolyZoned })
                {
                    double measuredPsd = 0.0, circuitPsd = 0.0;
                    for (int stage = 0; stage < 4; ++stage)
                    {
                        Cascade c;
                        c.inputCompensationCoefficient = 0.2751f;
                        constexpr double amplitude = 0.0002, fc = 1000.0;
                        const int settle = static_cast<int>(fs * 0.064);
                        const int count = static_cast<int>(fs * 0.128);
                        std::complex<double> phasor{};
                        for (int n = 0; n < settle + count; ++n)
                        {
                            const double angle = 2.0 * std::numbers::pi * f * (n + 1) / fs;
                            std::array<double, 4> noise{};
                            noise[static_cast<std::size_t>(stage)] = amplitude * std::cos(angle);
                            c.setStageNoise(noise);
                            const double output = advance(c, fs, fc, k, mode);
                            if (n >= settle)
                                phasor += output * std::exp(std::complex<double>(0.0, -angle));
                        }
                        const std::complex<double> actual = phasor * (2.0 / (count * amplitude));
                        const std::complex<double> h = 1.0 / std::complex<double>(1.0, f / fc);
                        const auto expected = std::pow(h, 4 - stage)
                                              / (1.0 + static_cast<double>(k) * std::pow(h, 4));
                        worstDb = std::max(
                            worstDb, std::abs(20.0 * std::log10(std::abs(actual / expected))));
                        worstPhase = std::max(worstPhase, std::abs(std::arg(actual / expected))
                                                              * 180.0 / std::numbers::pi);
                        measuredPsd += std::norm(actual);
                        circuitPsd += std::norm(expected);
                    }
                    worstPsdDb = std::max(worstPsdDb,
                                          std::abs(10.0 * std::log10(measuredPsd / circuitPsd)));
                }
    std::cout << "Four-stage noise transfer: max " << worstDb << " dB, " << worstPhase
              << " deg; summed PSD " << worstPsdDb << " dB\n";
    require(worstDb < 0.18 && worstPhase < 1.5 && worstPsdDb < 0.18,
            "stage noise paths disagree with independent continuous circuit transfer");
    // At DC all four independent paths are identical: total PSD is 4x the
    // one-source PSD. At cutoff k=0 the expected sum is 15 times the old
    // stage-1-only spectrum, +11.761 dB; at high frequencies stage 4 dominates.
}

void streams()
{
    // Test the actual engine's card PRNG, amplitude scaling and freewheel path.
    constexpr int count = 65536;
    for (const double fs : { 48000.0, 192000.0 })
    {
        youknow::YouKnowEngine engine;
        engine.prepare(fs, 1, 1);
        std::array<double, 4> sum{}, power{};
        std::array<std::array<double, 4>, 4> cross{};
        double across = 0.0, otherPower = 0.0;
        for (int n = 0; n < count; ++n)
        {
            const auto a = youknow::YouKnowTestAccess::draw(engine, 0);
            const auto b = youknow::YouKnowTestAccess::draw(engine, 1);
            for (std::size_t i = 0; i < 4; ++i)
            {
                sum[i] += a[i];
                power[i] += a[i] * a[i];
                for (std::size_t j = 0; j < 4; ++j)
                    cross[i][j] += a[i] * a[j];
            }
            across += a[0] * b[0];
            otherPower += b[0] * b[0];
        }
        // Primary read: 68k || 560, node-referred through 560/68560;
        // one-sided sampled density e_n^2 gives variance e_n^2*fs/2.
        constexpr double kb = 1.380649e-23, temp = 298.15;
        constexpr double resistance = 68000.0 * 560.0 / 68560.0;
        constexpr double divider = 560.0 / 68560.0;
        const double expected = 4.0 * kb * temp * resistance / (divider * divider) * fs / 2.0;
        for (std::size_t i = 0; i < 4; ++i)
        {
            require(std::abs(power[i] / count / expected - 1.0) < 0.025,
                    "card resistor-noise density depends incorrectly on rate or stage");
            require(std::abs(sum[i] / count) / std::sqrt(expected) < 0.02,
                    "card resistor noise has a DC component");
            for (std::size_t j = 0; j < i; ++j)
                require(std::abs(cross[i][j] / std::sqrt(power[i] * power[j])) < 0.02,
                        "stage noise streams are correlated");
        }
        require(std::abs(across / std::sqrt(power[0] * otherPower)) < 0.02,
                "different cards share the same noise stream");
    }
}

void integratedPsd()
{
    // An independent continuous integral gives integral_0^infinity sum
    // |H^n|^2 df = (1 + 1/2 + 3/8 + 5/16)*pi*fc/2 = 35*pi*fc/32.
    // At a 1 kHz cutoff the numerical Nyquist tail is under 0.4% of this
    // total; the finite capture uses a stated 0.5 dB statistical bound.
    constexpr double fs = 192000.0, fc = 1000.0;
    constexpr int count = 262144, settle = 4096;
    youknow::YouKnowEngine source;
    source.prepare(fs, 1, 1);
    Cascade filter;
    double sum = 0.0, power = 0.0;
    for (int n = 0; n < count + settle; ++n)
    {
        filter.setStageNoise(youknow::YouKnowTestAccess::draw(source, 0));
        const double y = advance(filter, fs, fc, 0.0f, youknow::VcfTanhMode::PolyZoned);
        if (n >= settle)
        {
            sum += y;
            power += y * y;
        }
    }
    constexpr double densitySquared = 4.0 * 1.380649e-23 * 298.15 * (68000.0 * 560.0 / 68560.0)
                                      / ((560.0 / 68560.0) * (560.0 / 68560.0));
    const double expected = densitySquared * 35.0 * std::numbers::pi * fc / 32.0;
    const double variance = power / count - (sum / count) * (sum / count);
    const double error = 10.0 * std::log10(variance / expected);
    std::cout << "Integrated four-stage resistor PSD error: " << error << " dB\n";
    require(std::abs(error) < 0.5, "rendered noise power disagrees with continuous PSD integral");
}

void engineHistories()
{
    youknow::YouKnowEngine continuous, fast;
    continuous.prepare(48000, 128, 4);
    fast.prepare(48000, 128, 4);
    youknow::EngineParameters p;
    p.chorus = youknow::ChorusMode::Off;
    p.vcfTanhMode = youknow::VcfTanhMode::Exact;
    continuous.setParameters(p);
    p.vcfTanhMode = youknow::VcfTanhMode::PolyZoned;
    p.vcfFastEarlyMode = youknow::VcfFastEarlyMode::Cubic;
    p.vcfSolverMode = youknow::VcfSolverMode::Rk4Single;
    fast.setParameters(p);
    std::array<float, 128> left{}, right{};
    for (int pass = 0; pass < 3; ++pass)
    {
        if (pass == 1)
        {
            continuous.noteOn(60, 1.f);
            fast.noteOn(60, 1.f);
        }
        if (pass == 2)
        {
            continuous.noteOff(60);
            fast.noteOff(60);
        }
        for (int block = 0; block < 32; ++block)
        {
            continuous.process(left.data(), right.data(), 128);
            // Same duration with uneven host boundaries exercises every
            // source through both active and freewheel card paths.
            fast.process(left.data(), right.data(), 37);
            fast.process(left.data(), right.data(), 91);
        }
        require(youknow::YouKnowTestAccess::sameNoiseState(continuous, fast),
                "Exact/freewheel/SIMD or host block boundaries change resistor-noise history");
    }
}

void vectorAndState()
{
#if defined(YOUKNOW_HAS_VCF_PAIR_SIMD)
    double worstPair = 0.0, worstQuad = 0.0;
    int rk4FullPairs = 0, rk4HalfPairs = 0, mersonPairs = 0, mersonQuads = 0;
    struct Case { double cutoff; Cascade::Tableau tableau; };
    const std::array<Case, 3> cases {{
        { 1000.0, Cascade::Tableau::Rk4Full },
        { 12000.0, Cascade::Tableau::Rk4Half },
        { 30000.0, Cascade::Tableau::MersonHalf }
    }};
    for (const auto& fixture : cases)
    {
        constexpr double fs = 96000.0;
        const float omega = static_cast<float>(2.0 * std::numbers::pi * fixture.cutoff / fs);
        require(youknow::YouKnowTestAccess::noiseTableau(omega) == fixture.tableau,
                "noise vector fixture no longer selects its intended solver tableau");
        std::array<Cascade, 4> scalar, vector;
        std::uint32_t rng = 0x12345678;
        for (int n = 0; n < 4096; ++n)
        {
            std::array<float, 4> expected{}, actual{};
            for (std::size_t lane = 0; lane < 4; ++lane)
            {
                std::array<double, 4> noise{};
                for (auto& value : noise)
                {
                    rng ^= rng << 13;
                    rng ^= rng >> 17;
                    rng ^= rng << 5;
                    value = (static_cast<double>(rng) / 4294967296.0 - 0.5) * 0.002;
                }
                scalar[lane].setStageNoise(noise);
                vector[lane].setStageNoise(noise);
                expected[lane] = scalar[lane].process<true>(
                    0.0f, omega, 3.0f, 6.366f, true, 1.0f, nullptr,
                    youknow::VcfTanhMode::PolyZoned, youknow::VcfSolverMode::Rk4Single);
            }
            bool quad = false;
            // Alternate Merson's pair and quad implementations so neither can
            // hide behind the other's fallback while claiming zero error.
            if (n > 3 && fixture.tableau == Cascade::Tableau::MersonHalf && (n & 1) == 0)
            {
                quad = Cascade::tryProcessSettledMersonQuad(
                    { &vector[0], &vector[1], &vector[2], &vector[3] }, {},
                    { omega, omega, omega, omega }, { 3.0f, 3.0f, 3.0f, 3.0f },
                    { 6.366f, 6.366f, 6.366f, 6.366f }, true, 1.0f, actual);
                if (quad) ++mersonQuads;
            }
            for (std::size_t lane = 0; lane < 4; lane += 2)
            {
                bool paired = quad;
                if (!paired && n > 3)
                {
                    paired = Cascade::tryProcessSettledRk4Pair(
                        vector[lane], 0.0f, omega, 3.0f, 6.366f, vector[lane + 1], 0.0f, omega,
                        3.0f, 6.366f, true, 1.0f, actual[lane], actual[lane + 1]);
                    if (paired)
                    {
                        require(fixture.tableau != Cascade::Tableau::MersonHalf,
                                "RK4 pair accepted the Merson-only noise fixture");
                        if (fixture.tableau == Cascade::Tableau::Rk4Full) ++rk4FullPairs;
                        else if (fixture.tableau == Cascade::Tableau::Rk4Half) ++rk4HalfPairs;
                    }
                    else
                    {
                        paired = Cascade::tryProcessSettledMersonPair(
                            vector[lane], 0.0f, omega, 3.0f, 6.366f, vector[lane + 1], 0.0f, omega,
                            3.0f, 6.366f, true, 1.0f, actual[lane], actual[lane + 1]);
                        if (paired)
                        {
                            ++mersonPairs;
                            require(fixture.tableau == Cascade::Tableau::MersonHalf,
                                    "Merson pair replaced an intended RK4 noise fixture");
                        }
                    }
                    require(paired, "settled vector fixture never reached a vector kernel");
                }
                if (!paired)
                    for (std::size_t j = lane; j < lane + 2; ++j)
                        actual[j] = vector[j].process<true>(
                            0.0f, omega, 3.0f, 6.366f, true, 1.0f, nullptr,
                            youknow::VcfTanhMode::PolyZoned, youknow::VcfSolverMode::Rk4Single);
            }
            for (std::size_t lane = 0; lane < 4; ++lane)
            {
                double& worst = quad ? worstQuad : worstPair;
                worst = std::max(worst, std::abs(static_cast<double>(actual[lane]) - expected[lane]));
            }
        }
    }
    require(rk4FullPairs > 0 && rk4HalfPairs > 0 && mersonPairs > 0 && mersonQuads > 0,
            "noise comparison did not execute every intended pair/quad solver path");
    std::cout << "Noise SIMD accepted calls: RK4-full pair " << rk4FullPairs
              << ", RK4-half pair " << rk4HalfPairs << ", Merson pair " << mersonPairs
              << ", Merson quad " << mersonQuads << '\n';
    std::cout << "Noise SIMD max errors: pair " << worstPair << ", quad " << worstQuad << " V\n";
    require(worstPair < 1e-8 && worstQuad < 2e-7,
            "vector kernel lost stage noise or its node timing");
#else
    std::cout << "Noise SIMD checks skipped: vector kernels unavailable on this platform\n";
#endif
    Cascade c;
    for (int n = 0; n < 5; ++n)
        c.setStageNoise({ 0.01, 0.02, 0.03, 0.04 });
    const auto history = c.stageNoiseHistory;
    c.retime(0.1f, 0.2f);
    require(c.stageNoiseHistoryCount == 0, "noise reconstruction retains an obsolete rate grid");
    for (std::size_t i = 0; i < 4; ++i)
        require(c.stageNoiseHistory[i][0] == history[i][0],
                "retime lost the shared noise endpoint");
    c.reset();
    require(c.stageNoiseHistoryCount == 0 && c.stageNoiseAt == decltype(c.stageNoiseAt){},
            "reset leaves stale resistor noise");
}
} // namespace
int main()
{
    transfer();
    streams();
    integratedPsd();
    engineHistories();
    vectorAndState();
    if (failures)
        return EXIT_FAILURE;
    std::cout << "All VCF resistor-noise tests passed\n";
}
