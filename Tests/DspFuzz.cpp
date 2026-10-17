// Stateful input fuzzing of the JUCE-free engine boundary the plug-in and the
// Rack adapter both drive (standard U4). Every input starts from a fresh
// engine, so a saved crash replays alone; work and allocation are bounded
// whatever the input length. Panel floats arrive raw (NaN, infinities, any bit
// pattern), integers out of range, and choices only as the values a front end
// can send. Lifecycle calls, quality and circuit-rate changes, Original timing
// and raw DIN bytes are mixed in. Run with -max_len=4096 -timeout=60.
#include "FuzzHarness.h"
#include "DSP/YouKnowEngine.h"

#include <array>
#include <memory>

namespace
{
using youknow::EngineParameters;

constexpr std::array panelControls {
    &EngineParameters::lfoRate, &EngineParameters::lfoDelay,
    &EngineParameters::lfoSyncRateHz, &EngineParameters::lfoSyncDelaySeconds,
    &EngineParameters::dcoLfoDepth, &EngineParameters::pwmDepth,
    &EngineParameters::subLevel, &EngineParameters::noiseLevel,
    &EngineParameters::cutoff, &EngineParameters::resonance,
    &EngineParameters::envDepth, &EngineParameters::vcfLfoDepth,
    &EngineParameters::keyFollow, &EngineParameters::vcaLevel,
    &EngineParameters::attack, &EngineParameters::decay,
    &EngineParameters::sustain, &EngineParameters::release,
    &EngineParameters::portamento, &EngineParameters::benderDcoDepth,
    &EngineParameters::benderVcfDepth, &EngineParameters::benderLfoDepth,
    &EngineParameters::masterTuneCents, &EngineParameters::volume,
    &EngineParameters::outputTrimDb, &EngineParameters::velocityDepth,
    &EngineParameters::calibration, &EngineParameters::chorusNoise
};

template <typename Enum>
Enum choice(protocodus::fuzz::Input& input, std::size_t count)
{
    return static_cast<Enum>(input.Pick(count));
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    using namespace protocodus::fuzz;
    Input input { data, size };
    auto engine = std::make_unique<youknow::YouKnowEngine>();
    EngineParameters parameters;
    engine->prepare(input.Rate(), 256, 1 << input.Pick(3));
    engine->setParameters(parameters);
    Sentinels<256> block;
    const auto render = [&](std::size_t frames)
    {
        block.Arm();
        engine->process(block.Left(), block.Right(), static_cast<int>(frames));
        block.Check(frames);
    };

    for (int command = 0; command < 48 && !input.Empty(); ++command)
    {
        switch (input.Pick(16))
        {
            case 0:
            case 1:
                parameters.*panelControls[input.Pick(panelControls.size())] = input.Scalar();
                engine->setParameters(parameters);
                break;
            case 2:
                parameters.range = choice<youknow::DcoRange>(input, 3);
                parameters.pwmSource = choice<youknow::PwmSource>(input, 2);
                parameters.highPass = choice<youknow::HighPassMode>(input, 4);
                parameters.envPolarity = choice<youknow::EnvPolarity>(input, 2);
                parameters.vcaMode = choice<youknow::VcaMode>(input, 2);
                parameters.chorus = choice<youknow::ChorusMode>(input, 4);
                parameters.keyMode = choice<youknow::KeyMode>(input, 3);
                parameters.sawEnabled = input.Byte() % 2 != 0;
                parameters.pulseEnabled = input.Byte() % 2 != 0;
                engine->setParameters(parameters);
                break;
            case 3:
                parameters.polyphony = static_cast<int>(input.Byte()) - 64;
                parameters.keyTranspose = static_cast<int>(static_cast<std::int8_t>(input.Byte()));
                engine->setParameters(parameters);
                break;
            case 4:
            case 5:
                engine->noteOn(static_cast<int>(input.Byte()) - 32, input.Scalar());
                break;
            case 6:
                engine->noteOff(static_cast<int>(input.Byte()) - 32);
                break;
            case 7: engine->setPitchBend(input.Scalar()); break;
            case 8: engine->setModWheel(input.Scalar()); break;
            case 9: engine->setSustainPedal(input.Byte() % 2 != 0); break;
            case 10:
                switch (input.Pick(5))
                {
                    case 0: engine->allNotesOff(); break;
                    case 1: engine->allSoundOff(); break;
                    case 2: engine->releaseAllNotes(); break;
                    case 3: engine->resetForHostStop(); break;
                    default: engine->reset(); break;
                }
                break;
            case 11: (void) engine->setOversamplingFactor(static_cast<int>(input.Byte()) - 64); break;
            case 12: (void) engine->setCircuitRate(choice<youknow::CircuitRate>(input, 2)); break;
            case 13: engine->setOriginalPerformanceMode(input.Byte() % 2 != 0); break;
            case 14:
            {
                std::array<std::uint8_t, 16> bytes {};
                const auto count = input.Pick(bytes.size() + 1);
                for (std::size_t i = 0; i < count; ++i)
                    bytes[i] = input.Byte();
                (void) engine->receiveOriginalPerformanceMidi({ bytes.data(), count });
                break;
            }
            default:
                engine->prepare(input.Rate(), 256, 1 << input.Pick(3));
                engine->setParameters(parameters);
                break;
        }
        render(1 + input.Pick(256));
    }
    return 0;
}
