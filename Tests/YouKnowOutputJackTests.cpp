#include "DSP/YouKnowEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace youknow
{
struct YouKnowTestAccess
{
    static OutputJackLowPass::Coefficients jack(YouKnowEngine& engine)
    {
        return engine.outputJackCoefficients_;
    }
    static double position(const YouKnowEngine& engine) { return engine.glidedVolume_; }
};
}

namespace
{
using namespace youknow;
constexpr double pi = std::numbers::pi;
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// Independent p.15 component reduction, not the production corner helper.
double corner(double volume)
{
    constexpr double load = 41300.0 * 101000.0 / (41300.0 + 101000.0);
    const double upper = 1500.0 + 10000.0 * (1.0 - volume);
    const double lower = 10000.0 * volume;
    const double shunt = lower * load / (lower + load);
    return 1.0 / (2.0 * pi * 1e-9 * (2200.0 + upper * shunt / (upper + shunt)));
}

double measureError(const OutputJackLowPass::Coefficients& coefficients,
                    double rate, double pole, double& oldError)
{
    require(std::isfinite(coefficients.a1) && std::isfinite(coefficients.correction),
            "jack coefficients are nonfinite");
    OutputJackLowPass filter;
    std::array<float, 256> impulse {};
    for (std::size_t i = 0; i < impulse.size(); ++i)
    {
        impulse[i] = filter.process(i == 0 ? 1.0f : 0.0f, coefficients);
        require(std::isfinite(impulse[i]), "jack impulse response is nonfinite");
    }
    double worst = 0.0;
    const double oldPole = std::exp(-2.0 * pi * pole / rate);
    for (int bin = 0; bin <= 160; ++bin)
    {
        const double f = std::min(20000.0, 0.45 * rate) * bin / 160.0;
        const auto z = std::polar(1.0, -2.0 * pi * f / rate);
        std::complex<double> response {}, power { 1.0, 0.0 };
        for (const float value : impulse)
        {
            response += static_cast<double>(value) * power;
            power *= z;
        }
        const double analog = 1.0 / std::sqrt(1.0 + (f / pole) * (f / pole));
        const double error = std::abs(20.0 * std::log10(std::abs(response) / analog));
        require(std::isfinite(response.real()) && std::isfinite(response.imag())
                    && std::isfinite(error), "jack frequency response is nonfinite");
        worst = std::max(worst, error);
        const auto old = (1.0 - oldPole) / (1.0 - oldPole * z);
        oldError = std::max(oldError, std::abs(20.0 * std::log10(std::abs(old) / analog)));
    }
    return worst;
}

std::vector<float> render(int block)
{
    YouKnowEngine engine;
    engine.prepare(44100.0, 128, 1);
    EngineParameters p;
    p.calibration = 0;
    p.chorus = ChorusMode::Off;
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    engine.setParameters(p);
    engine.noteOn(84, 1.0f);
    std::vector<float> result(4096), right(4096);
    for (int segment = 0; segment < 4; ++segment)
    {
        p.volume = std::array { .2f, 1.0f, 0.0f, .5f }[segment];
        engine.setParameters(p);
        for (int i = 1024 * segment; i < 1024 * (segment + 1);)
        {
            const int count = std::min(block, 1024 * (segment + 1) - i);
            engine.process(result.data() + i, right.data() + i, count);
            i += count;
        }
    }
    for (const float sample : result)
        require(std::isfinite(sample), "volume automation generated nonfinite audio");
    return result;
}

struct NoiseTake
{
    std::vector<float> left, right;
};

NoiseTake renderGroundedWiperNoise(double rate, int quality, float character,
                                 int block, int frames)
{
    YouKnowEngine engine;
    require(engine.configureThermalStart(true), "cannot settle jack temperature");
    engine.prepare(rate, 256, quality);
    EngineParameters p;
    p.volume = 0;
    p.calibration = character;
    p.chorus = ChorusMode::Off;
    p.enableVoiceVcaServiceGain = false;
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    engine.setParameters(p);
    NoiseTake take { std::vector<float>(frames), std::vector<float>(frames) };
    for (int i = 0; i < frames;)
    {
        const int count = std::min(block, frames - i);
        engine.process(take.left.data() + i, take.right.data() + i, count);
        i += count;
    }
    return take;
}

void testOutputSeriesResistorNoise()
{
    // Grounding VR1 removes every upstream signal/noise source. Only the
    // independent R64/R65 thermal sources remain at the jacks. Integrate the
    // analogue 4kTR/(1+(f/fc)^2) PSD over the host Nyquist band, independently
    // of production resistance helpers and digital filter coefficients.
    constexpr double k = 1.380649e-23;
    constexpr double temperature = 313.15; // settled Character 1: 25+15 C
    constexpr double resistance = 2200.0;
    constexpr double cutoff = 1.0 / (2.0 * pi * resistance * 1e-9);
    constexpr int settle = 256, frames = 131072;
    const double boundary = YouKnowEngine::outputBoundaryGain()
                          / YouKnowEngine::internalVoltsPerUnit;
    for (const double rate : { 44100.0, 48000.0, 96000.0 })
    {
        const auto take = renderGroundedWiperNoise(rate, 1, 1, 256, settle + frames);
        double leftPower = 0, rightPower = 0, cross = 0;
        for (int i = settle; i < settle + frames; ++i)
        {
            const double l = take.left[i], r = take.right[i];
            require(std::isfinite(l) && std::isfinite(r), "nonfinite jack noise");
            leftPower += l * l;
            rightPower += r * r;
            cross += l * r;
        }
        const double expectedRms = boundary * std::sqrt(4.0 * k * temperature
            * resistance * cutoff * std::atan(rate / (2.0 * cutoff)));
        // Sampling uncertainty plus the separately tested magnitude-fit
        // approximation (<0.065 dB integrated RMS error at these rates).
        require(std::abs(std::sqrt(leftPower / frames) / expectedRms - 1.0) < .02
                    && std::abs(std::sqrt(rightPower / frames) / expectedRms - 1.0) < .02,
                "grounded-wiper jack noise does not match R64/R65 Johnson PSD");
        require(std::abs(cross / std::sqrt(leftPower * rightPower)) < .02,
                "left and right output resistor noises are correlated");
    }
    const auto reference = renderGroundedWiperNoise(48000, 1, 1, 1, 4096);
    const auto blocked = renderGroundedWiperNoise(48000, 1, 1, 128, 4096);
    const auto oversampled = renderGroundedWiperNoise(48000, 4, 1, 128, 4096);
    require(reference.left == blocked.left && reference.right == blocked.right,
            "output resistor noise depends on block boundaries");
    require(reference.left == oversampled.left && reference.right == oversampled.right,
            "host-rate output resistor noise changes with the quality rung");
    const auto nominal = renderGroundedWiperNoise(48000, 1, 0, 128, 4096);
    require(std::all_of(nominal.left.begin(), nominal.left.end(), [](float x) { return x == 0; })
                && std::all_of(nominal.right.begin(), nominal.right.end(), [](float x) { return x == 0; }),
            "Character zero lost its exact-silence contract");
}
}

int main()
{
    try
    {
        double worst = 0.0, oldWorst = 0.0;
        for (const double rate : { 8000., 11025., 22050., 32000., 44100., 48000.,
                                   64000., 88200., 96000., 192000., 384000., 768000. })
            for (const double position : { 0., .25, .5, .75, 1. })
            {
                const double pole = corner(position);
                const auto c = OutputJackLowPass::coefficients(pole, rate);
                require(std::abs(c.a1) < 1.0, "unstable jack pole");
                double old = 0.0;
                const double error = measureError(c, rate, pole, old);
                require(error < 0.10, "jack magnitude exceeds 0.10 dB analog error");
                require(error < old, "jack magnitude regressed from exponential pole");
                worst = std::max(worst, error);
                oldWorst = std::max(oldWorst, old);
            }
        // Actual engine-updated coefficients must use the glided wiper and
        // host rate, for each processing rung. This catches accidental use of
        // the internal rate in a post-decimation filter.
        for (const int factor : { 1, 2, 4 })
        {
            YouKnowEngine engine;
            engine.prepare(48000.0, 64, factor);
            EngineParameters p;
            p.volume = .5f;
            engine.setParameters(p);
            std::array<float, 64> l {}, r {};
            engine.process(l.data(), r.data(), 64);
            double old = 0.0;
            require(measureError(YouKnowTestAccess::jack(engine), 48000.0,
                                 corner(YouKnowTestAccess::position(engine)), old) < .10,
                    "engine does not wire the host-rate wiper-correct jack filter");
        }
        OutputJackLowPass dc;
        dc.previousInput = 0.75;
        for (int frame = 0; frame < 10000; ++frame)
        {
            const auto c = OutputJackLowPass::coefficients(corner((frame % 101) / 100.0), 48000.0);
            require(dc.process(.75f, c) == .75f, "coefficient updates disturb settled DC");
        }
        dc.reset();
        require(dc.process(0.0f, OutputJackLowPass::coefficients(corner(1), 48000)) == 0.0f,
                "jack reset retains history");
        require(render(1) == render(128), "jack/volume processing depends on block boundaries");
        testOutputSeriesResistorNoise();
        std::cout << "PASS: output-jack worst magnitude error " << worst
                  << " dB (former " << oldWorst << " dB); stable across 8--768 kHz; "
                     "host-rate wiring, DC automation, reset, block invariance and R64/R65 Johnson noise\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
