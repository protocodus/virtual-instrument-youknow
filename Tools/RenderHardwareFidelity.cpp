// Build-to-build Juno-106 fidelity auditions. Compile this identical score
// against the archived parent DSP with YOUKNOW_FIDELITY_BASELINE, and against
// the candidate DSP normally. No circuit is approximated by the renderer.
// Protocol: fixed seeds, 48 kHz, 128-frame event-split blocks, same quality,
// product HPF/filter/clock/thermal configuration, settled temperature, 250 ms
// discarded preroll. Raw float files retain unmodified engine output levels. The packaging
// tool RMS-matches each pair and records its trims; never normalize here.
// vca/hold/chorus each expose one change. They are diagnostic performances,
// not hardware recordings or an overall measure of similarity to a Juno.
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"
#include "RealismComparisonSupport.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

int main(int argc, char** argv)
{
    using namespace youknow;
    using namespace youknow::tools::realism;
    try
    {
        if (argc != 4)
            throw std::runtime_error("usage: YouKnowRenderHardwareFidelity vca|hold|chorus|idle output.wav 1|4");
        const std::string_view scenario(argv[1]);
        if (scenario != "vca" && scenario != "hold" && scenario != "chorus" && scenario != "idle")
            throw std::runtime_error("unknown scenario");
        const std::string_view qualityArgument(argv[3]);
        if (qualityArgument != "1" && qualityArgument != "4")
            throw std::runtime_error("quality must be 1 or 4");
        const int quality = qualityArgument == "1" ? 1 : 4;
        EngineParameters p;
        ProductFidelityProfile::applyTo(p);
        p.vcfTanhMode = VcfTanhMode::PolyZoned;
        p.vcfFastEarlyMode = VcfFastEarlyMode::Cubic;
        p.vcfSolverMode = VcfSolverMode::Rk4Single;
        p.calibration = 1; p.aging = .5f; p.volume = .6f;
        p.velocityDepth = 0; p.polyphony = 6;
        p.sawEnabled = true; p.pulseEnabled = true;
        p.pwmSource = PwmSource::Manual; p.pwmDepth = .35f;
        p.subLevel = .65f; p.noiseLevel = 0; p.chorusNoise = 0;
        p.cutoff = .88f; p.resonance = .18f; p.envDepth = 0;
        p.keyFollow = 0; p.vcfLfoDepth = 0; p.dcoLfoDepth = 0;
        p.highPass = HighPassMode::One;
        p.vcaMode = VcaMode::Gate; p.vcaLevel = 1;
        p.attack = 0; p.decay = .2f; p.sustain = .7f; p.release = .1f;
        p.chorus = scenario == "vca" ? ChorusMode::Two : ChorusMode::Off;
#if !defined(YOUKNOW_FIDELITY_BASELINE)
        p.enableVoiceVcaServiceGain = scenario == "vca";
        p.enableChorusClockMuteCircuit = scenario == "chorus" || scenario == "idle";
#endif
        if (scenario == "hold")
        {
            p.keyMode = KeyMode::Unison;
            p.pulseEnabled = false; p.subLevel = .2f;
            p.cutoff = .74f; p.resonance = .32f;
            p.lfoRate = .58f; p.lfoDelay = .42f; p.dcoLfoDepth = .35f;
        }
        auto engine = std::make_unique<YouKnowEngine>();
        ProductFidelityProfile::configureBeforePrepare(*engine);
        if (!engine->configureThermalStart(true))
            throw std::runtime_error("failed to configure thermal start");
        engine->selectConverterTimingProfile(YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
        engine->prepare(comparisonSampleRate, comparisonBlockSize, quality);
        engine->setParameters(p);
        StereoBuffer audio;
        std::array<float, comparisonBlockSize> left {}, right {};
        const auto run = [&](double seconds, bool retain = true) {
            auto remaining = static_cast<int>(std::llround(seconds * comparisonSampleRate));
            while (remaining > 0)
            {
                const int count = std::min(remaining, comparisonBlockSize);
                engine->process(left.data(), right.data(), count);
                if (retain)
                {
                    audio.left.insert(audio.left.end(), left.begin(), left.begin() + count);
                    audio.right.insert(audio.right.end(), right.begin(), right.begin() + count);
                }
                remaining -= count;
            }
        };
        const auto start = std::chrono::steady_clock::now();
        run(.25, false);
        if (scenario == "vca")
        {
            run(.1);
            for (const auto& chord : std::array<std::array<int, 6>, 3> {{
                     { 45, 52, 57, 60, 64, 69 }, { 41, 48, 53, 57, 60, 65 },
                     { 43, 50, 55, 59, 62, 67 } }})
            {
                for (const int note : chord) { engine->noteOn(note, 1); run(.035); }
                run(1.1);
                for (const int note : chord) engine->noteOff(note);
                run(.35);
            }
            run(.65);
        }
        else if (scenario == "hold")
        {
            run(.1);
            for (const int note : { 48, 55, 60, 52 })
            {
                engine->noteOn(note, 1); run(.75);
                engine->setSustainPedal(true);
                engine->noteOff(note); run(.24);
                // Both events are delivered at one host sample. The B-2 run
                // snapshot cannot have observed HOLD release between them.
                engine->setSustainPedal(false);
                engine->noteOn(note + 7, 1); run(.65);
                engine->noteOff(note + 7); run(.25);
            }
            run(.5);
        }
        else if (scenario == "chorus")
        {
            for (const int note : { 48, 55, 60 }) engine->noteOn(note, 1);
            run(.65);
            for (const double offSeconds : { .06, .22, .65, 1.1 })
            {
                p.chorus = ChorusMode::One; engine->setParameters(p); run(.65);
                p.chorus = ChorusMode::Off; engine->setParameters(p); run(offSeconds);
            }
            p.chorus = ChorusMode::Two; engine->setParameters(p); run(1.0);
            engine->releaseAllNotes(); run(.5);
        }
        else run(8);
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        std::string error;
        if (!validate(audio, error) || !writeFloatWav(argv[2], audio, error))
            throw std::runtime_error(error);
        const auto level = measure(audio);
        std::cout << std::setprecision(12) << "{\"scenario\":\"" << scenario
                  << "\",\"quality\":" << quality << ",\"frames\":" << audio.left.size()
                  << ",\"sample_rate\":" << comparisonSampleRate
                  << ",\"render_seconds\":" << elapsed
                  << ",\"rms_dbfs\":" << decibels(level.rms)
                  << ",\"peak_dbfs\":" << decibels(level.peak) << "}\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
