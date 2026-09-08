// Qualification and reproducible audition of the calibration-only WAVE mixer.
//
// No arguments / --self-test: compare the production active-set solve with an
// independent long-double KCL bisection, then with a continuous RC transient.
// Test resistances are synthetic numerical fixtures, not installed Juno data.
//
// --sub-level DIR renders the measured SUB law against its linear baseline.
//
// --render DIR Rs Rload scale bias diode collector
// accepts ALL unpublished calibration coordinates explicitly. It renders the
// unchanged shipping path as A and that calibration's circuit as B through the
// shipping Poly/Cubic/RK4 engine, 48 kHz/4x, identical notes/controls/seed. Raw
// float takes remain in raw/, audible A/B share whole-file stereo RMS, and the
// key records exact inputs and gains. Arbitrary fixture values demonstrate
// sensitivity only and must never be presented as measured hardware or selected
// as a faithful default by listening. Real calibration requires loaded/unloaded
// WAVE voltage/impedance, VCF input impedance, D6 drop and Tr19 on-state voltage.
// Identify original/replacement voice electronics, retain recording gain and
// patch bytes, and validate separate all-source/timing takes without refitting.

#include "DSP/YouKnowEngine.h"
#include "RealismComparisonSupport.h"

#include <iostream>
#include <memory>
#include <stdexcept>

namespace
{
using youknow::CoupledSubMixer;
using Calibration = CoupledSubMixer::Calibration;
using namespace youknow::tools::realism;

void require(bool result, const std::string& detail)
{
    if (!result)
        throw std::runtime_error(detail);
}

// KCL independently spells out the printed 33k/27k paths and uses a monotonic
// residual root, with no active-set selection or production solve constants.
long double referenceWave(const Calibration& c, long double source,
    long double rail, long double gate, long double loadG, long double history)
{
    const auto residual = [&](long double wave) {
        return (source - wave) / c.sourceOhms
            + gate * std::max(0.0L, rail - c.diodeDropVolts - wave) / 60000.0L
            + (1.0L - gate)
                * std::max(0.0L, c.collectorOnVolts - c.diodeDropVolts - wave)
                / 27000.0L
            - (wave - history) * loadG;
    };
    long double low = -100.0L, high = 100.0L;
    require(residual(low) >= 0.0L && residual(high) <= 0.0L,
        "independent KCL bracket failed");
    for (int i = 0; i < 100; ++i)
    {
        const auto middle = (low + high) * 0.5L;
        if (residual(middle) > 0.0L)
            low = middle;
        else
            high = middle;
    }
    return (low + high) * 0.5L;
}

void audit()
{
    double worstLawResidual = 0.0, worstTableDb = 0.0;
    for (int n = 1; n <= 100000; ++n)
    {
        const double control = static_cast<double>(n) / 100000;
        const double gain = youknow::SubLevelDiodeLaw::exactGain(control);
        const double residual = youknow::SubLevelDiodeLaw::referenceSeriesSpanVolts * (gain - 1)
            + 0.026 * std::log(gain) - 9.921875 * (control - 1);
        worstLawResidual = std::max(worstLawResidual, std::abs(residual));
        if (gain > 1e-8)
            worstTableDb = std::max(worstTableDb,
                std::abs(decibels(youknow::SubLevelDiodeLaw::gain(control) / gain)));
    }
    require(worstLawResidual < 1e-12 && worstTableDb < .01,
        "reference SUB diode solve/table exceeded its circuit error budget");
    require(youknow::SubLevelDiodeLaw::gain(0) == 0
        && youknow::SubLevelDiodeLaw::gain(1) == 1, "SUB diode endpoints changed");
    require(!Calibration {}.valid(), "empty calibration was accepted");
    Calibration c { 10000, 47000, 0.5, 0, 0.6, 0.1 };
    auto bad = c;
    bad.sourceOhms = std::numeric_limits<double>::quiet_NaN();
    require(!bad.valid(), "NaN calibration was accepted");
    bad = c; bad.diodeDropVolts = -0.1;
    require(!bad.valid(), "negative diode drop was accepted");

    double worstVolts = 0.0, worstAmps = 0.0;
    std::size_t cases = 0;
    for (const double sourceR : { 1000.0, 10000.0, 68000.0, 200000.0 })
    for (const double loadR : { 2200.0, 10000.0, 68000.0, 100000.0 })
    for (const double source : { -12.0, -0.6, 0.0, 5.0, 12.0 })
    for (const double rail : { 0.0, 0.3, 0.6, 3.0, 9.921875 })
    for (const double q : { 0.0, 0.125, 0.5, 0.875, 1.0 })
    for (const double history : { -10.0, 0.0, 10.0 })
    {
        c.sourceOhms = sourceR; c.loadOhms = loadR;
        const double loadG = 1.0 / (loadR + 1.0 / (2.0 * 10e-6 * 192000.0));
        const auto actual = CoupledSubMixer::solve(c, source, rail, q, loadG, history);
        const double oracle = static_cast<double>(referenceWave(c, source, rail, q, loadG, history));
        worstVolts = std::max(worstVolts, std::abs(actual.waveVolts - oracle));
        const double kcl = (source - actual.waveVolts) / sourceR
            + actual.subOffAmps + actual.subOnAmps - actual.capacitorAmps;
        worstAmps = std::max(worstAmps, std::abs(kcl));
        require(actual.subOffAmps >= 0.0 && actual.subOnAmps >= 0.0,
            "a diode carried negative current");
        ++cases;
    }
    require(worstVolts < 1.0e-11 && worstAmps < 1.0e-14,
        "coupled mixer disagreed with independent KCL");

    c = { 10000, 47000, 0.5, 0, 0.6, 0.1 };
    const auto offClamp = CoupledSubMixer::solve(c, -5.0, 0.0, 0.0, 0.0, 0.0);
    require(offClamp.subOnAmps > 0.0 && offClamp.waveVolts > -5.0,
        "Tr19 on-state was incorrectly treated as disconnected");
    const auto belowOnset = CoupledSubMixer::solve(c, 0.0, 0.3, 1.0, 0.0, 0.0);
    require(belowOnset.subOffAmps == 0.0, "sub diode conducted below onset");

    // Positive 5 V step, both diode sources below the node. Its exact physical
    // response is Vfilter = 5 Rload/(Rs+Rload) exp(-t/[C(Rs+Rload)]).
    // Trapezoidal sampling locates the discontinuity halfway before sample 0.
    double worstRelative = 0.0;
    for (const double rate : { 8000.0, 44100.0, 192000.0 })
    {
        CoupledSubMixer state;
        const double tau = 10.0e-6 * (10000.0 + 47000.0);
        for (int n = 0; n < static_cast<int>(rate * 2.0); ++n)
        {
            const auto actual = state.process(c, 5.0, 0.0, 1.0, 1.0 / rate);
            const double expected = 5.0 * 47000.0 / 57000.0
                * std::exp(-(n + 0.5) / rate / tau);
            worstRelative = std::max(worstRelative,
                std::abs(actual.filterVolts - expected) / (5.0 * 47000.0 / 57000.0));
        }
    }
    require(worstRelative < 1.0e-7, "coupled C56 failed continuous RC reference");

    youknow::YouKnowEngine engine;
    require(!engine.configureCoupledMixer(Calibration {}), "invalid engine calibration accepted");
    require(engine.configureCoupledMixer(c), "valid engine calibration rejected");
    engine.prepare(48000, 128, 4);
    require(!engine.configureCoupledMixer(c), "live engine calibration changed capacitor topology");
    std::cout << std::setprecision(12) << "KCL cases=" << cases
        << " worst_wave_error_V=" << worstVolts << " worst_KCL_residual_A=" << worstAmps
        << " worst_continuous_RC_relative_error=" << worstRelative << '\n';
    std::cout << "SUB diode worst_equation_residual_V=" << worstLawResidual
        << " worst_table_error_db=" << worstTableDb << '\n';
}

StereoBuffer render(const Calibration* calibration, bool subDiode)
{
    auto engine = std::make_unique<youknow::YouKnowEngine>();
    if (calibration)
        require(engine->configureCoupledMixer(*calibration), "render calibration rejected");
    engine->selectConverterTimingProfile(
        youknow::YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
    engine->prepare(comparisonSampleRate, comparisonBlockSize, 4);
    youknow::EngineParameters p;
    p.vcfTanhMode = youknow::VcfTanhMode::PolyZoned;
    p.vcfFastEarlyMode = youknow::VcfFastEarlyMode::Cubic;
    p.vcfSolverMode = youknow::VcfSolverMode::Rk4Single;
    p.enableSubDiodeControl = subDiode;
    p.sawEnabled = false; p.pulseEnabled = false; p.subLevel = 1.0f;
    p.noiseLevel = 0.0f; p.chorus = youknow::ChorusMode::Off;
    p.cutoff = 0.9f; p.resonance = 0.0f; p.envDepth = 0.0f;
    p.keyFollow = 0.0f; p.attack = 0.0f; p.sustain = 1.0f; p.release = 0.0f;
    p.vcaMode = youknow::VcaMode::Gate; p.vcaLevel = 0.7f; p.volume = 0.5f;
    engine->setParameters(p);
    StereoBuffer result;
    const auto block = [&](int frames, bool keep) {
        std::array<float, comparisonBlockSize> l {}, r {};
        while (frames > 0)
        {
            const int count = std::min(frames, comparisonBlockSize);
            engine->process(l.data(), r.data(), count);
            if (keep)
            {
                result.left.insert(result.left.end(), l.begin(), l.begin() + count);
                result.right.insert(result.right.end(), r.begin(), r.begin() + count);
            }
            frames -= count;
        }
    };
    block(96000, false);
    engine->noteOn(48, 1.0f);
    for (const float sub : { 1.0f, 0.5f, 0.1f, 0.0f })
    {
        p.subLevel = sub; engine->setParameters(p); block(36000, true);
    }
    engine->noteOff(48); block(12000, true);
    p.sawEnabled = true; p.subLevel = 1.0f; p.cutoff = 0.45f; p.resonance = 0.85f;
    engine->setParameters(p); engine->noteOn(36, 1.0f); block(48000, true);
    p.subLevel = 0.0f; engine->setParameters(p); block(48000, true);
    engine->noteOff(36); block(12000, true);
    p.subLevel = 0.5f; p.pulseEnabled = true; p.cutoff = 0.9f; p.resonance = 0.1f;
    engine->setParameters(p); engine->noteOn(60, 1.0f); block(48000, true);
    engine->noteOff(60); block(24000, true);
    return result;
}

void audition(const std::filesystem::path& directory, const Calibration* calibration)
{
    require(!calibration || calibration->valid(), "invalid explicit circuit calibration");
    require(!std::filesystem::exists(directory / "A.wav")
        && !std::filesystem::exists(directory / "B.wav"), "refusing to replace an existing audition");
    std::filesystem::create_directories(directory / "raw");
    const auto a = render(nullptr, false), b = render(calibration, !calibration);
    std::string error;
    require(validate(a, error) && validate(b, error), error);
    const auto levelA = measure(a), levelB = measure(b);
    require(levelA.rms > 0.0 && levelB.rms > 0.0, "silent audition");
    const double match = levelA.rms / levelB.rms;
    const double common = std::min(1.0, 0.5 / std::max(levelA.peak, levelB.peak * match));
    require(writeFloatWav(directory / "raw/A.wav", a, error), error);
    require(writeFloatWav(directory / "raw/B.wav", b, error), error);
    require(writeFloatWav(directory / "A.wav", applyGain(a, common), error), error);
    require(writeFloatWav(directory / "B.wav", applyGain(b, common * match), error), error);
    StereoBuffer residual;
    require(difference(a, b, residual, error), error);
    const double residualDb = decibels(measure(residual).rms / levelA.rms);
    std::ofstream key(directory / "key.md");
    key << std::setprecision(15)
        << "# Coupled mixer comparison\n\nA is the unchanged shipping mixer. "
        << (calibration ? "B is the coupled, constant-drop D6 / Tr19 / C56 circuit.\n\n"
            : "B is the SUB diode control law calibrated on Juno-106 #439522. "
              "Only the held-rail-to-sub-current law changes; full-scale gain stays fixed.\n\n");
    if (calibration)
    {
        const auto& c = *calibration;
        key
        << "The caller supplied these values; this renderer does not establish "
        << "their measurement provenance. Without unit readings they are "
        << "sensitivity fixtures, not a hardware calibration or a default candidate.\n\n"
        << "Rs=" << c.sourceOhms << " ohm, Rload=" << c.loadOhms
        << " ohm, source scale=" << c.sourceScale << ", source bias=" << c.sourceBiasVolts
        << " V, D6 drop=" << c.diodeDropVolts << " V, collector-on=" << c.collectorOnVolts << " V.\n\n";
    }
    key
        << "48 kHz, 4x, shipping Poly/Cubic/RK4, Unit Character 1, chorus off. "
        << "Identical seed, score and controls; two seconds of discarded settling. "
        << "Raw float takes retain levels. Listening takes match whole-file "
        << "stereo RMS: A gain=" << common << ", B gain=" << common * match << ".\n\n"
        << "Raw B/A level=" << decibels(levelB.rms / levelA.rms)
        << " dB; raw difference/A=" << residualDb << " dBc.\n\n"
        << "Score: SUB 127, 64, 13, 0; resonant saw with SUB on then off; "
        << "bright saw+pulse+sub. The key exists separately to avoid priming audition.\n";
    require(static_cast<bool>(key), "failed to write audition key");
    std::cout << "Raw level B/A=" << decibels(levelB.rms / levelA.rms)
        << " dB; raw residual=" << residualDb << " dBc; output=" << directory << '\n';
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--self-test"))
            audit();
        else if (argc == 9 && std::string(argv[1]) == "--render")
        {
            Calibration c { std::stod(argv[3]), std::stod(argv[4]), std::stod(argv[5]),
                std::stod(argv[6]), std::stod(argv[7]), std::stod(argv[8]) };
            audition(argv[2], &c);
        }
        else if (argc == 3 && std::string(argv[1]) == "--sub-level")
            audition(argv[2], nullptr);
        else
            throw std::runtime_error("usage: YouKnowCoupledMixerAudit [--self-test | --sub-level DIR | --render DIR Rs Rload scale bias diode collector]");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
