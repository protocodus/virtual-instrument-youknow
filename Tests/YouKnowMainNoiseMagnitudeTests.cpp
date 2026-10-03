// Independent continuous RC magnitude and zero-current charge-decay oracles.
// The full engine checks source chronology, block partitions, control order,
// hard reset and live quality rebuilds; no fitted level/EQ enters the oracle.
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace youknow
{
struct YouKnowTestAccess
{
    static double source(YouKnowEngine& e, float raw, float level)
    {
        (void) e.processMainNoiseSource(raw, level, true);
        return e.noiseSourceMagnitude_.capacitorVoltage;
    }
    static double voltage(const YouKnowEngine& e)
    { return e.noiseSourceMagnitude_.capacitorVoltage; }
    static double previousSource(const YouKnowEngine& e)
    { return e.noiseSourceMagnitude_.previousSource; }
    static auto random(const YouKnowEngine& e)
    { return std::tuple {e.noiseState_, e.noiseGaussianSpare_, e.noiseGaussianSpareValid_}; }
    static bool rebuildAtQualifiedSilence(YouKnowEngine& e, int factor)
    {
        // Isolate the rebuild boundary after the existing silence/fade policy
        // has qualified it; advancing audio to get there would also discharge
        // the deliberately injected C41 state we need to inspect here.
        e.oversamplingIdleSamples_ = e.oversamplingQuietSamples_;
        e.rateTransition_ = YouKnowEngine::RateTransition::FadingOut;
        e.rateTransitionGain_ = 0;
        return e.setOversamplingFactor(factor);
    }
};
}

namespace
{
using namespace youknow;
constexpr double pi = std::numbers::pi;
constexpr double tau = 330000.0 * 100e-12;
constexpr double hpTau = 4700.0 * 1e-6;
void require(bool ok, const char* message)
{ if (!ok) throw std::runtime_error(message); }

std::unique_ptr<YouKnowEngine> prepared(double rate, bool corrected)
{
    auto e = std::make_unique<YouKnowEngine>();
    e->prepare(rate, 128, 1);
    EngineParameters p;
    p.enableMainNoiseMagnitudePole = corrected;
    p.calibration = p.aging = p.chorusNoise = p.subLevel = 0;
    p.enableCommonVcaNoise = false;
    p.sawEnabled = p.pulseEnabled = false;
    p.noiseLevel = 0.25f;
    p.cutoff = 1;
    p.vcaMode = VcaMode::Gate;
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    e->setParameters(p);
    return e;
}

void analogueMagnitude()
{
    double worst = 0;
    for (double rate : {8000., 44100., 48000., 96000., 192000.})
    {
        const auto coefficients = NoiseC41LowPass::coefficients(rate);
        require(coefficients.currentWeight >= 0 && coefficients.previousWeight >= 0,
                "C41 forcing weights stopped being positive");
        require(coefficients.decay > 0 && coefficients.decay < 1,
                "C41 pole no longer represents monotonic RC discharge");
        auto e = prepared(rate, true);
        constexpr int count = 8192;
        // Exact-bin sinusoidal forcing avoids a window/estimator fit. C42's
        // physical high-pass is included in the independent analogue oracle.
        const int last = static_cast<int>(std::min(20000., .45 * rate) * count / rate);
        for (int bin = 32; bin <= last; bin += std::max(1, last / 17))
        {
            const double f = bin * rate / count;
            std::complex<double> projection {};
            for (int i = -count; i < count; ++i)
            {
                const double phase = 2 * pi * bin * i / count;
                const double y = YouKnowTestAccess::source(*e, static_cast<float>(std::cos(phase)), 1);
                if (i >= 0) projection += y * std::polar(1., -phase);
            }
            const double measured = 2 * std::abs(projection) / count;
            const double w = 2 * pi * f;
            const double analogue = w * hpTau / std::sqrt(1 + w * w * hpTau * hpTau)
                / std::sqrt(1 + w * w * tau * tau);
            const double error = 20 * std::log10(measured / analogue);
            worst = std::max(worst, std::abs(error));
            require(std::abs(error) < .51, "actual C42/C41 magnitude missed its analogue RC oracle");
        }
    }
    std::cout << "Actual C42/C41 worst analogue magnitude error: " << worst << " dB\n";
}

void chargeAndControl()
{
    for (double rate : {8000., 44100., 48000., 96000., 192000., 384000., 768000.})
    {
        const auto c = NoiseC41LowPass::coefficients(rate);
        require(c.currentWeight >= 0 && c.previousWeight >= 0
                    && c.decay > 0 && c.decay < 1,
                "C41 lost positive decay/forcing on a supported internal grid");
    }
    for (double rate : {8000., 44100., 48000., 96000., 192000.})
    {
        auto e = prepared(rate, true);
        for (int i = 0; i < 73; ++i)
            YouKnowTestAccess::source(*e, 1, 1);
        const double charged = YouKnowTestAccess::voltage(*e);
        require(charged > .01, "C41 did not acquire stored voltage");
        const double p = std::exp(-1 / (rate * tau));
        double expected = charged;
        // Raw forcing keeps moving ahead of the shut OTA. Stored charge is
        // not post-multiplied, and neither the old sample nor C42 can leak
        // new current into a closed C41 drive.
        for (int i = 0; i < 24; ++i)
        {
            expected *= p;
            const double y = YouKnowTestAccess::source(*e, (i & 1) ? -.7f : .9f, 0);
            require(std::abs(y - expected) <= 1e-13 * std::max(1., std::abs(expected)),
                    "NOISE zero did not preserve exact undriven C41 charge decay");
        }
        // Interrupted restart adds forcing to the retained charge, while
        // hard reset discharges both voltage and source history.
        const double before = YouKnowTestAccess::voltage(*e);
        const double reopened = YouKnowTestAccess::source(*e, .75f, .3f);
        require(std::isfinite(reopened) && reopened != before * p,
                "C41 failed to reacquire current on interrupted restart");
        e->reset();
        require(YouKnowTestAccess::voltage(*e) == 0
                    && YouKnowTestAccess::previousSource(*e) == 0,
                "hard reset retained C41 charge/source history");
    }
    auto e = prepared(48000, true);
    for (int i = 0; i < 73; ++i) YouKnowTestAccess::source(*e, 1, 1);
    const double charge = YouKnowTestAccess::voltage(*e);
    const double source = YouKnowTestAccess::previousSource(*e);
    require(YouKnowTestAccess::rebuildAtQualifiedSilence(*e, 4),
            "qualified idle quality rebuild was rejected");
    require(YouKnowTestAccess::voltage(*e) == charge
                && YouKnowTestAccess::previousSource(*e) == source,
            "quality rebuild changed physical C41 charge/source history");
    const double y = YouKnowTestAccess::source(*e, 1, 0);
    require(std::abs(y - charge * std::exp(-1 / (192000 * tau))) < 1e-13,
            "quality rebuild used the old C41 discharge interval");
}

struct Render
{
    std::vector<float> left;
    std::tuple<std::uint32_t, float, bool> random;
};
Render render(double rate, bool corrected, int block)
{
    auto e = prepared(rate, corrected);
    e->noteOn(72, 1);
    Render r;
    r.left.resize(4096);
    std::vector<float> right(r.left.size());
    for (int from = 0; from < static_cast<int>(r.left.size());)
    {
        const int size = std::min(block, static_cast<int>(r.left.size()) - from);
        e->process(r.left.data() + from, right.data() + from, size);
        from += size;
    }
    for (float x : r.left) require(std::isfinite(x), "actual corrected engine became nonfinite");
    r.random = YouKnowTestAccess::random(*e);
    return r;
}
void actualEngine()
{
    require(!EngineParameters {}.enableMainNoiseMagnitudePole, "raw reference lost TPT convention");
    EngineParameters p;
    ProductFidelityProfile::applyTo(p);
    require(p.enableMainNoiseMagnitudePole, "product omitted C41 magnitude correction");
    for (double rate : {8000., 44100., 48000., 96000., 192000.})
    {
        const auto old = render(rate, false, 128);
        const auto current = render(rate, true, 128);
        require(current.left == render(rate, true, 1).left,
                "corrected actual engine depends on block partition");
        require(current.random == old.random,
                "C41 correction changed Gaussian/RNG chronology");
        require(current.left != old.left, "actual engine omitted the C41 correction");
    }
}
}

int main()
{
    try
    {
        analogueMagnitude();
        chargeAndControl();
        actualEngine();
        std::cout << "C41 magnitude, charge/control and full-engine checks passed\n";
    }
    catch (const std::exception& e)
    { std::cerr << e.what() << '\n'; return 1; }
}
