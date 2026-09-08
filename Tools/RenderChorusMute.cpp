// Build-to-build review of chorus engagement through the complete shipping
// engine. Compile this same source against the parent and candidate DSP,
// retain their raw float WAVs, and compare identical sample positions. The
// long and interrupted Off commands expose capacitor history; neither the
// production circuit nor its previous version is approximated by this tool.
// Usage: YouKnowRenderChorusMute /absolute/path/to/raw.wav
#include "DSP/YouKnowEngine.h"
#include "RealismComparisonSupport.h"

#include <iostream>
#include <stdexcept>
#include <utility>

int main(int argc, char** argv)
{
    using namespace youknow;
    using namespace youknow::tools::realism;
    try
    {
        if (argc != 2)
            throw std::runtime_error("usage: YouKnowRenderChorusMute output.wav");
        YouKnowEngine engine;
        engine.selectConverterTimingProfile(
            YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
        engine.prepare(comparisonSampleRate, comparisonBlockSize, 4);
        EngineParameters p;
        p.vcfTanhMode = VcfTanhMode::PolyZoned;
        p.vcfFastEarlyMode = VcfFastEarlyMode::Cubic;
        p.vcfSolverMode = VcfSolverMode::Rk4Single;
        p.sawEnabled = true;
        p.pulseEnabled = false;
        p.subLevel = 0.25f;
        p.noiseLevel = 0.0f;
        p.cutoff = 0.78f;
        p.resonance = 0.1f;
        p.envDepth = 0.0f;
        p.vcaMode = VcaMode::Gate;
        p.chorus = ChorusMode::Off;
        p.volume = 1.0f;
        p.calibration = 1.0f;
        engine.setParameters(p);
        for (const int note : { 48, 55, 60 })
            engine.noteOn(note, 1.0f);
        StereoBuffer audio;
        audio.left.resize(8u * comparisonSampleRate);
        audio.right.resize(audio.left.size());
        std::size_t cursor = 0;
        const auto renderUntil = [&](double seconds) {
            const auto end = static_cast<std::size_t>(std::llround(seconds * comparisonSampleRate));
            while (cursor < end)
            {
                const auto count = std::min<std::size_t>(comparisonBlockSize, end - cursor);
                engine.process(audio.left.data() + cursor, audio.right.data() + cursor,
                               static_cast<int>(count));
                cursor += count;
            }
        };
        for (const auto [seconds, mode] : std::array<std::pair<double, ChorusMode>, 6> {{
                 { 1.0, ChorusMode::One }, { 2.4, ChorusMode::Off },
                 { 2.46, ChorusMode::One }, { 3.4, ChorusMode::Off },
                 { 4.4, ChorusMode::Two }, { 6.4, ChorusMode::Off }
             }})
        {
            renderUntil(seconds);
            p.chorus = mode;
            engine.setParameters(p);
        }
        renderUntil(7.2);
        engine.releaseAllNotes();
        renderUntil(8.0);
        std::string error;
        if (!writeFloatWav(argv[1], audio, error))
            throw std::runtime_error(error);
        const auto level = measure(audio);
        std::cout << "8 s, 48 kHz stereo float, 4x, blocks <=128; shipping Poly/Cubic/RK4 x1; "
                  << "deterministic cold start; no level trim; RMS "
                  << decibels(level.rms) << " dBFS, peak "
                  << decibels(level.peak) << " dBFS\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
