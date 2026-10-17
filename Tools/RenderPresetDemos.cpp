// One demo per factory and product preset (README G3), played from the
// committed sources in Docs/audio/midi through the shipping engine, on the
// deepest 4x oversampling rung with Exact tanh and the half-step Merson solver.
// The shared PresetDemoRenderer.h owns the command line, timing, checks and
// MP3 encoding; this file only describes YouKnow.

#include "PresetDemoRenderer.h"
#include "PresetScores.h"
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowPresets.h"
#include "DSP/YouKnowActiveProductFidelity.h"

#include <array>
#include <memory>
#include <string>

namespace
{
using namespace youknow;

constexpr int requestedQuality = 4;
// Lets the physical nodes (chorus clock, DC couplings) settle before the
// first note, as a freshly loaded instance would by the time a player plays.
constexpr double preRollSeconds = 0.37;

const presets::Preset& presetAt (std::size_t index)
{
    const auto& factory = presets::factoryBank();
    return index < factory.size() ? factory[index] : presets::productBank()[index - factory.size()];
}

class YouKnowVoice final : public protocodus::demos::Voice
{
public:
    YouKnowVoice (const presets::Preset& preset, double sampleRate, int maxBlockFrames)
        : engine (std::make_unique<YouKnowEngine>())
    {
        ActiveProductFidelityProfile::configureBeforePrepare (*engine);
        engine->selectConverterTimingProfile (YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
        engine->prepare (sampleRate, maxBlockFrames, requestedQuality);
        engine->setParameters (preset_demos::loadPresetParameters (preset.number));
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
    protocodus::demos::Instrument instrument;
    instrument.name = "YouKnow";
    const auto count = presets::factoryBank().size() + presets::productBank().size();
    for (std::size_t i = 0; i < count; ++i)
        instrument.presets.push_back (std::string (presetAt (i).number) + " " + presetAt (i).name);
    // The engine is deterministic without a seed: its component spreads are
    // fixed per Unit Character setting.
    instrument.open = [] (std::size_t preset, double sampleRate, int maxBlockFrames, std::uint32_t)
    {
        return std::make_unique<YouKnowVoice> (presetAt (preset), sampleRate, maxBlockFrames);
    };
    return protocodus::demos::Main (argc, argv, instrument);
}
