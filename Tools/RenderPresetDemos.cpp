// Renders the README's preset demos (G3), one for each committed phrase in
// Docs/audio/midi, through the shipping engine, on the deepest 4x
// oversampling rung with Exact tanh and the half-step Merson solver. With
// --more it renders the More demos (G3d) instead, from Docs/audio/midi/more,
// each played on the panel Tools/DemoPanels.h sets up for it. The shared
// PresetDemoRenderer.h owns the command line, timing, checks and WAV output;
// this file only describes YouKnow.

#include "DemoPanels.h"
#include "PresetDemoRenderer.h"
#include "PresetScores.h"
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowPresets.h"
#include "DSP/YouKnowActiveProductFidelity.h"

#include <array>
#include <cstring>
#include <memory>
#include <string>

namespace
{
using namespace youknow;

constexpr int requestedQuality = 4;
// Lets the physical nodes (chorus clock, DC couplings) settle before the
// first note, as a freshly loaded instance would by the time a player plays.
constexpr double preRollSeconds = 0.37;

struct Showcase
{
    const char* name; // its phrase is Docs/audio/midi/more/<NN>-<slug of name>.mid
    EngineParameters (*panel)();
};

constexpr Showcase showcases[] {
    { "Filter Brass", demo_panels::filterBrass },
    { "Self-Oscillation", demo_panels::selfOscillation },
    { "Unison Glide", demo_panels::unisonGlide },
    { "Delayed Vibrato", demo_panels::delayedVibrato },
};

const presets::Preset& presetAt (std::size_t index)
{
    const auto& factory = presets::factoryBank();
    return index < factory.size() ? factory[index] : presets::productBank()[index - factory.size()];
}

class YouKnowVoice final : public protocodus::demos::Voice
{
public:
    YouKnowVoice (const EngineParameters& parameters, double sampleRate, int maxBlockFrames)
        : engine (std::make_unique<YouKnowEngine>())
    {
        ActiveProductFidelityProfile::configureBeforePrepare (*engine);
        engine->selectConverterTimingProfile (YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
        engine->prepare (sampleRate, maxBlockFrames, requestedQuality);
        engine->setParameters (parameters);
        engine->setPitchBend (0);
        engine->setModWheel (0);

        std::array<float, protocodus::demos::kBlockFrames> left {}, right {};
        for (int done = 0; done < static_cast<int> (preRollSeconds * sampleRate); done += maxBlockFrames)
            engine->process (left.data(), right.data(), maxBlockFrames);
    }

    void Midi (const std::uint8_t* bytes, int size) override
    {
        if (size < 3)
            return;
        const auto kind = bytes[0] & 0xf0;
        if (kind == 0x90 && bytes[2] > 0)
            engine->noteOn (bytes[1], static_cast<float> (bytes[2]) / 127.0f);
        else if (kind == 0x80 || kind == 0x90)
            engine->noteOff (bytes[1]);
        else if (kind == 0xe0) // as the plug-in maps the wheel
            engine->setPitchBend ((static_cast<float> (bytes[2] << 7 | bytes[1]) - 8192.0f) / 8192.0f);
    }

    void Render (float* left, float* right, int frames) override
    {
        engine->process (left, right, frames);
    }

private:
    std::unique_ptr<YouKnowEngine> engine;
};
} // namespace

int main (int argc, char** argv)
{
    const bool more = argc > 1 && std::strcmp (argv[1], "--more") == 0;
    if (more)
    {
        argv[1] = argv[0];
        ++argv;
        --argc;
    }

    protocodus::demos::Instrument instrument;
    instrument.name = "YouKnow";
    if (more)
        for (const auto& showcase : showcases)
            instrument.presets.emplace_back (showcase.name);
    else
    {
        const auto count = presets::factoryBank().size() + presets::productBank().size();
        for (std::size_t i = 0; i < count; ++i)
            instrument.presets.push_back (std::string (presetAt (i).number) + " " + presetAt (i).name);
    }
    // U7 perf budget: about 3x the slowest demo's median realtime factor on
    // the CI runner, 3.07 (061-a85-rocket-men) in ci.yml run 174
    // (ubuntu-24.04, AMD EPYC 7763, three repeats), 2026-10-27. Above 1
    // because the demos render at the 4x rung, not at the plug-in default.
    instrument.cpuBudget = 9.2;
    // The engine is deterministic without a seed: its component spreads are
    // fixed per Unit Character setting.
    instrument.open = [more] (std::size_t demo, double sampleRate, int maxBlockFrames, std::uint32_t)
    {
        auto parameters = more ? showcases[demo].panel()
                               : preset_demos::loadPresetParameters (presetAt (demo).number);
        if (more)
            ActiveProductFidelityProfile::applyTo (parameters);
        return std::make_unique<YouKnowVoice> (parameters, sampleRate, maxBlockFrames);
    };
    return protocodus::demos::Main (argc, argv, instrument);
}
