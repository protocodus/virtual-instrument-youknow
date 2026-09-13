// Build-to-build control-DAC listening/measurement protocol. Build this exact
// source against each revision's DSP library and headers; do not approximate
// the output offline. Identical product profile, fixed seed, MIDI, 96 kHz,
// 256-frame blocks, maximum oversampling, Exact tanh and Merson half steps.
// Raw float files preserve level; a separate analysis must report whole-file
// stereo RMS trims for listening pairs and retain unmatched level changes.
// Quiet sustain crosses Tr20's knee; full sustain/GATE distinguish physical
// codes 4064/4095; noise/resonance exercise the shared positive DAC buffer.
// The score is a diagnostic, not a transcription of a hardware capture.
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"
#include "RealismComparisonSupport.h"

#include <iostream>
#include <memory>
#include <stdexcept>

namespace
{
using namespace youknow;
using namespace youknow::tools::realism;
constexpr std::uint32_t rate = 96000;
constexpr int block = 256;

StereoBuffer render(int fixture)
{
    EngineParameters p;
    ProductFidelityProfile::applyTo(p);
    p.calibration = 1; p.aging = 0; p.polyphony = 6;
    p.sawEnabled = true; p.pulseEnabled = false;
    p.subLevel = 0; p.noiseLevel = 0; p.chorus = ChorusMode::Off;
    p.cutoff = .55f; p.resonance = .1f; p.envDepth = 0;
    p.vcfLfoDepth = 0; p.dcoLfoDepth = 0; p.velocityDepth = 0;
    p.highPass = HighPassMode::One;
    p.attack = 0; p.decay = .08f; p.sustain = 1; p.release = .08f;
    p.volume = 1; p.vcaLevel = .5f;
    p.vcfTanhMode = VcfTanhMode::Exact;
    p.vcfSolverMode = VcfSolverMode::MersonHalfSteps;
    if (fixture == 2)
    {
        p.sawEnabled = false; p.noiseLevel = 13.0f / 127.0f;
        p.resonance = .65f; p.cutoff = .42f;
    }
    auto engine = std::make_unique<YouKnowEngine>();
    ProductFidelityProfile::configureBeforePrepare(*engine);
    engine->selectConverterTimingProfile(
        YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
    engine->prepare(rate, block, 4);
    engine->setParameters(p);
    if (engine->getOversamplingFactor() * rate != 192000)
        throw std::runtime_error("probe did not select the maximum internal rate");
    StereoBuffer audio;
    std::array<float, block> left {}, right {};
    const auto run = [&](double seconds, bool retain = true)
    {
        auto remaining = static_cast<int>(std::llround(seconds * rate));
        while (remaining > 0)
        {
            const int count = std::min(block, remaining);
            engine->process(left.data(), right.data(), count);
            if (retain)
            {
                audio.left.insert(audio.left.end(), left.begin(), left.begin() + count);
                audio.right.insert(audio.right.end(), right.begin(), right.begin() + count);
            }
            remaining -= count;
        }
    };
    run(.37, false);
    run(.05);
    constexpr std::array<int, 4> notes {50, 53, 57, 60}; // D minor seventh
    constexpr std::array<int, 4> quiet {2, 3, 5, 13};
    constexpr std::array<int, 4> noise {6, 13, 32, 96};
    for (int step = 0; step < 4; ++step)
    {
        if (fixture == 0) p.sustain = quiet[static_cast<std::size_t>(step)] / 127.0f;
        if (fixture == 1)
        {
            p.sustain = 1;
            p.vcaMode = step < 2 ? VcaMode::Envelope : VcaMode::Gate;
        }
        if (fixture == 2) p.noiseLevel = noise[static_cast<std::size_t>(step)] / 127.0f;
        engine->setParameters(p);
        engine->noteOn(notes[static_cast<std::size_t>(step)], 1);
        run(.65);
        engine->noteOff(notes[static_cast<std::size_t>(step)]);
        run(.25);
    }
    run(.4);
    if (engine->getActiveVoiceCount() != 0)
        throw std::runtime_error("probe has an unfinished release");
    std::string error;
    if (!validate(audio, error) || measure(audio).peak < 1e-6)
        throw std::runtime_error("invalid probe audio: " + error);
    return audio;
}
}

int main(int argc, char** argv)
{
    try
    {
        if (argc != 2)
            throw std::runtime_error("usage: YouKnowRenderControlDacProbe OUTPUT_DIRECTORY");
        const std::filesystem::path directory(argv[1]);
        std::filesystem::create_directories(directory);
        std::ofstream manifest(directory / "probe.json");
        manifest << "{\"sample_rate\":96000,\"internal_rate\":192000,"
                    "\"quality_selected\":4,\"tanh\":\"Exact\","
                    "\"solver\":\"MersonHalfSteps\",\"block_size\":256,"
                    "\"unit_character\":1,\"aging\":0,\"preroll_seconds\":0.37,"
                    "\"fixtures\":[";
        constexpr std::array<const char*, 3> names {
            "quiet-sustain", "sustain-and-gate", "noise-resonance"};
        for (int fixture = 0; fixture < 3; ++fixture)
        {
            const auto audio = render(fixture);
            const auto level = measure(audio);
            std::string error;
            const std::string name = names[static_cast<std::size_t>(fixture)];
            if (!writeFloatWav(directory / (name + ".wav"), audio, error, rate))
                throw std::runtime_error(error);
            if (fixture) manifest << ',';
            manifest << std::setprecision(12) << "{\"name\":" << std::quoted(name)
                     << ",\"seconds\":" << audio.left.size() / double(rate)
                     << ",\"raw_rms_dbfs\":" << decibels(level.rms)
                     << ",\"raw_peak_dbfs\":" << decibels(level.peak) << '}';
            std::cout << name << " peak " << decibels(level.peak) << " dBFS\n";
        }
        manifest << "]}\n";
        manifest.close();
        if (!manifest) throw std::runtime_error("cannot write probe manifest");
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
