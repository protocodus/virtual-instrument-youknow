#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace youknow
{
struct YouKnowTestAccess
{
    static float noiseHold(const YouKnowEngine& engine) { return engine.noiseCv_; }
    static auto randomState(const YouKnowEngine& engine)
    {
        return std::array<std::uint32_t, 3> {
            engine.noiseState_, std::bit_cast<std::uint32_t>(engine.noiseGaussianSpare_),
            static_cast<std::uint32_t>(engine.noiseGaussianSpareValid_)};
    }
};
}

namespace
{
using namespace youknow;
using Law = YouKnowEngine::CircuitDerivedNoiseLevelProfile;
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

// Independent reference: first solve the two nominal pre-BBD followers'
// resistor-node DC equations for the documented current coordinate. Then
// bisect Tr22's original emitter-node KCL directly in long double. No engine
// current, Wright-omega solve or table supplies reference values.
constexpr long double beta = 200.0L, alpha = beta / (beta + 1.0L);
constexpr long double vt = 1.380649e-23L * 298.15L / 1.602176634e-19L;
long double referenceCollector()
{
    constexpr long double a = 1.0L + 44000.0L / ((beta + 1.0L) * 22000.0L);
    constexpr long double b = -1.0L / (beta + 1.0L);
    constexpr long double c = 44000.0L / (beta + 1.0L);
    constexpr long double d = 10000.0L + c;
    constexpr long double x = (15.0L - .61L) / 22000.0L, y = 15.0L - 2.0L * .61L;
    return alpha * (x * d - b * y) / (a * d - b * c);
}
long double oracleCurrent(long double holdVolts, long double rs = 10000.0L)
{
    const long double is = referenceCollector() / std::expm1(.61L / vt);
    long double low = -1.0L, high = holdVolts;
    for (int i = 0; i < 120; ++i)
    {
        const long double ve = (low + high) / 2.0L;
        const long double collector = is * std::expm1(ve / vt);
        const long double residual = (holdVolts - ve) / rs
            - (ve + 15.0L) / 2200000.0L - collector / alpha;
        if (residual > 0) low = ve; else high = ve;
    }
    return is * std::expm1((low + high) / (2.0L * vt));
}
long double oracleDrive(long double control)
{
    // These float conversions restate the established converter coordinates;
    // the new junction model does not alter them.
    constexpr long double standoff = static_cast<long double>(.26f);
    constexpr long double span = static_cast<long double>(10.026514f);
    return oracleCurrent(standoff + span * control) / oracleCurrent(standoff + span);
}

void currentAndTableOracle()
{
    require(std::abs(referenceCollector() / Law::junctionReferenceCollectorAmps - 1) < 2e-14L,
            "Tr22 nominal scale-current anchor does not follow the source DC circuit");
    require(std::abs(static_cast<long double>(Law::junctionThermalVolts) / vt - 1) < 2e-16L,
            "Tr22 thermal voltage does not describe the declared 25 C reference");
    double worstCurrentRelative = 0.0, worstTableDb = 0.0;
    float previous = -1.0f;
    for (int i = 0; i <= 32768; ++i)
    {
        const float control = static_cast<float>(i) / 32768.0f;
        const double hold = static_cast<double>(.26f) + static_cast<double>(10.026514f) * control;
        const double expected = static_cast<double>(oracleCurrent(hold));
        const double actual = Law::junctionCollectorCurrent(hold);
        worstCurrentRelative = std::max(worstCurrentRelative, std::abs(actual / expected - 1));
        const float drive = Law::junctionDrive(control);
        require(std::isfinite(drive) && drive >= previous && drive >= 0 && drive <= 1,
                "Tr22 junction table is nonfinite, nonmonotone or outside normalized current bounds");
        previous = drive;
        if (control >= 1.0f / Law::junctionTableSteps)
            worstTableDb = std::max(worstTableDb, std::abs(20 * std::log10(
                drive / static_cast<double>(oracleDrive(control)))));
        else
            require(std::abs(drive - static_cast<double>(oracleDrive(control))) < 6e-8,
                    "exact Off policy introduced an excessive tiny-tail interpolation error");
    }
    require(worstCurrentRelative < 3e-13, "Tr22 current solve disagrees with independent KCL");
    require(worstTableDb < .012, "Tr22 interpolation moves the nominal current by more than .012 dB");
    require(Law::junctionDrive(0) == 0 && Law::junctionDrive(1) == 1,
            "Tr22 junction changed the exact Off/full endpoints");
    for (float invalid : {-1.f, -std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN()})
        require(Law::junctionDrive(invalid) == 0, "invalid Tr22 control did not sanitize to Off");
    require(Law::junctionDrive(2) == 1 && Law::junctionDrive(std::numeric_limits<float>::infinity()) == 0,
            "Tr22 control limits or nonfinite sanitization changed");
    require(Law::drive(4.f / 127) == 0 && Law::junctionDrive(4.f / 127) > 0,
            "Tr22 did not replace the hard low-byte deadband with a soft junction");
    std::cout << "Independent KCL current relative error " << worstCurrentRelative
              << ", maximum table error " << worstTableDb << " dB\n";
    for (int byte : {4, 5, 6, 8, 16, 64})
    {
        const float control = static_cast<float>(byte) / 127.f;
        std::cout << "Stored byte " << byte << ": " << 20 * std::log10(Law::junctionDrive(control))
                  << " dB re full";
        if (Law::drive(control) > 0)
            std::cout << ", change " << 20 * std::log10(Law::junctionDrive(control) / Law::drive(control))
                      << " dB";
        std::cout << '\n';
    }
    // VR32's uncertain setting remains an identification problem. The real
    // current solve, unlike its old ideal corner, stays monotone throughout
    // the circuit's documented 10..110k series range; no extra tail-current
    // ceiling from a sibling chip is imposed.
    for (long double rs : {10000.0L, 60000.0L, 110000.0L})
    {
        long double previousCurrent = -1;
        for (int byte = 0; byte <= 127; ++byte)
        {
            const auto current = oracleCurrent(.26L + 10.026514L * byte / 127, rs);
            const long double reverseLimit = -referenceCollector() / std::expm1(.61L / vt);
            require(current >= reverseLimit && current >= previousCurrent && current < .001L,
                    "drawn Tr22 circuit has an invalid nominal current domain");
            previousCurrent = current;
        }
    }
}

EngineParameters parameters(bool corrected, float level)
{
    EngineParameters p;
    p.enableNoiseLevelSoftJunction = corrected;
    p.sawEnabled = p.pulseEnabled = false;
    p.subLevel = p.chorusNoise = p.calibration = p.aging = 0;
    p.chorus = ChorusMode::Off;
    p.noiseLevel = level;
    p.vcaLevel = p.sustain = p.cutoff = 1;
    p.attack = p.decay = p.release = p.resonance = p.envDepth = p.keyFollow = 0;
    p.enableVoiceVcaSignalSaturation = false;
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    return p;
}
struct Take
{
    std::vector<float> left, holds;
    std::array<std::uint32_t, 3> random {};
};
Take render(bool corrected, float level, int block, bool moves = false, bool circuitShape = true)
{
    auto engine = std::make_unique<YouKnowEngine>();
    engine->prepare(48000, 128, 1);
    auto p = parameters(corrected, level);
    p.useCircuitDerivedNoiseLevelShape = circuitShape;
    engine->setParameters(p);
    engine->noteOn(48, 1);
    Take result;
    result.left.resize(32768);
    std::vector<float> right(result.left.size());
    std::size_t cursor = 0;
    for (int event = 0; event < 4; ++event)
    {
        const std::size_t end = (event + 1) * result.left.size() / 4;
        if (moves)
        {
            p.noiseLevel = std::array<float, 4> {2.f / 127, 6.f / 127, 9.f / 127, 0}[event];
            engine->setParameters(p);
        }
        while (cursor < end)
        {
            const int count = std::min(block, static_cast<int>(end - cursor));
            engine->process(result.left.data() + cursor, right.data() + cursor, count);
            cursor += static_cast<std::size_t>(count);
            if (block == 1) result.holds.push_back(YouKnowTestAccess::noiseHold(*engine));
        }
    }
    result.random = YouKnowTestAccess::randomState(*engine);
    return result;
}
double power(const std::vector<float>& signal)
{
    double sum = 0;
    for (std::size_t i = signal.size() / 2; i < signal.size(); ++i)
    {
        require(std::isfinite(signal[i]), "Tr22 law produced nonfinite audio");
        sum += static_cast<double>(signal[i]) * signal[i];
    }
    return sum;
}

void audioAndControlInvariance()
{
    require(!EngineParameters {}.enableNoiseLevelSoftJunction,
            "raw reference lost its hard-junction approximation");
    EngineParameters product;
    ProductFidelityProfile::applyTo(product);
    require(product.enableNoiseLevelSoftJunction, "product omitted the nominal soft Tr22 junction");
    for (float level : {0.f, 1.f})
    {
        const auto before = render(false, level, 128), after = render(true, level, 128);
        require(before.left == after.left, "Tr22 correction moved an exact Off/full audio endpoint");
        require(before.random == after.random, "Tr22 correction moved noise RNG at an endpoint");
    }
    const auto before = render(false, 6.f / 127, 1, true);
    const auto after = render(true, 6.f / 127, 1, true);
    const auto partitioned = render(true, 6.f / 127, 137, true);
    require(before.random == after.random && after.random == partitioned.random,
            "Tr22 law changed Gaussian draws or cached-spare chronology");
    require(before.holds == after.holds, "Tr22 law changed the scanned/smoothed NOISE hold trajectory");
    require(after.left == partitioned.left, "Tr22 correction depends on host block partition");
    require(before.left != after.left, "Tr22 correction did not affect low-level noise automation");
    require(render(false, .1f, 128, false, false).left
                == render(true, .1f, 128, false, false).left,
            "linear-from-zero diagnostic no longer bypasses the circuit law");
    const auto oldSix = render(false, 6.f / 127, 128);
    const auto newSix = render(true, 6.f / 127, 128);
    const double change = 10 * std::log10(power(newSix.left) / power(oldSix.left));
    require(change > 6 && change < 9, "actual low-byte noise change absent or inconsistent with nominal junction");
    require(power(render(true, 4.f / 127, 128).left) > 0,
            "actual byte4 NOISE remained in the hard deadband");
    std::cout << "Complete engine byte6 noise change " << change << " dB; endpoints bit-exact; "
              << "RNG/control/block invariance passed\n";
}
}

int main()
{
    try { currentAndTableOracle(); audioAndControlInvariance(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
