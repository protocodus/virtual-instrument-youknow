#include "DSP/YouKnowEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <tuple>

namespace youknow
{
struct YouKnowTestAccess
{
    static int keyedRoot(const YouKnowEngine& engine)
    {
        for (const auto& voice : engine.voices_)
            if (voice.active && voice.keyDown)
                return voice.rootMidi;
        return -1;
    }
    static auto envelope(const YouKnowEngine& engine)
    {
        const auto& env = engine.voices_[0].envelope;
        return std::make_tuple(env.stage, env.level, env.value, env.attackPhase,
            env.decayPhase, env.phase, env.gate, env.running);
    }
    static auto pitchState(const YouKnowEngine& engine)
    {
        const auto& voice = engine.voices_[0];
        return std::make_tuple(voice.currentMidi, voice.dco.divider, voice.dcoCv);
    }
    static float tune(const YouKnowEngine& engine)
    { return engine.activeParameters_.masterTuneCents; }
};
}

namespace
{
using namespace youknow;
using Access = YouKnowTestAccess;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "Host adapter contract: %s\n", message);
        std::exit(1);
    }
}

void advance(YouKnowEngine& engine, int samples = 1024)
{
    std::array<float, 64> left {}, right {};
    while (samples > 0)
    {
        const int count = std::min(samples, 64);
        engine.process(left.data(), right.data(), count);
        for (int i = 0; i < count; ++i)
            require(std::isfinite(left[i]) && std::isfinite(right[i]), "finite audio");
        samples -= count;
    }
}

void monoPriority()
{
    for (const auto mode : { KeyMode::Poly1, KeyMode::Poly2, KeyMode::Unison })
    {
        auto engine = std::make_unique<YouKnowEngine>();
        require(engine->configureSingleVoiceLastNotePriority(true), "enable mono policy");
        EngineParameters parameters;
        parameters.polyphony = 1;
        parameters.keyMode = mode;
        engine->setParameters(parameters);
        engine->prepare(48000, 64, 1);
        require(!engine->configureSingleVoiceLastNotePriority(false), "reject live policy edit");
        // A cold prepare/reset retains the explicitly configured host policy.
        engine->reset();
        engine->prepare(48000, 64, 1);
        engine->noteOn(72, 0.8f);
        engine->noteOn(60, 0.6f);
        engine->noteOn(67, 0.7f);
        require(Access::keyedRoot(*engine) == 67, "latest distinct press wins in every key mode");
        engine->noteOff(67);
        require(Access::keyedRoot(*engine) == 60, "release falls back by press order, not pitch");
        engine->noteOn(60, 0.9f);
        engine->noteOff(60);
        require(engine->isNoteHeld(60) && Access::keyedRoot(*engine) == 60,
                "overlapping equal pitches retain the remaining owner");
        engine->noteOff(60);
        require(!engine->isNoteHeld(60) && Access::keyedRoot(*engine) == 72,
                "last owner release restores previous held key");
        engine->noteOn(55, 0.5f);
        const auto beforeOldRelease = Access::envelope(*engine);
        engine->noteOff(72);
        require(Access::keyedRoot(*engine) == 55
                && Access::envelope(*engine) == beforeOldRelease,
                "releasing an older hold does not retrigger the latest voice");
        engine->noteOff(55);
        require(Access::keyedRoot(*engine) == -1 && !engine->isNoteHeld(55),
                "last release clears the keyed assignment");
    }

    auto reference = std::make_unique<YouKnowEngine>();
    EngineParameters parameters;
    parameters.polyphony = 1;
    reference->setParameters(parameters);
    reference->prepare(48000, 64, 1);
    reference->noteOn(72, 1.0f);
    reference->noteOn(60, 1.0f);
    require(Access::keyedRoot(*reference) == 72 && reference->isNoteHeld(60),
            "default reference keeps the full-pool no-steal policy");
}

void legatoCv()
{
    auto engine = std::make_unique<YouKnowEngine>();
    require(engine->configureSingleVoiceLastNotePriority(true), "configure CV mono policy");
    EngineParameters parameters;
    parameters.polyphony = 1;
    parameters.attack = 0.9f;
    parameters.portamento = 0.6f;
    engine->setParameters(parameters);
    engine->prepare(48000, 64, 1);
    engine->noteOn(60, 0.8f);
    advance(*engine);
    const auto envelope = Access::envelope(*engine);
    const auto pitch = Access::pitchState(*engine);
    require(std::get<1>(envelope) > 0, "fixture has an advancing envelope");
    require(engine->retargetHeldNoteLegato(60, 67), "accept unambiguous held-gate retarget");
    require(!engine->isNoteHeld(60) && engine->isNoteHeld(67)
            && Access::keyedRoot(*engine) == 67, "retarget transfers hold ownership and assignment");
    require(Access::envelope(*engine) == envelope && Access::pitchState(*engine) == pitch,
            "CV retarget preserves envelope and current oscillator/glide state");
    advance(*engine);
    require(std::get<1>(Access::envelope(*engine)) >= std::get<1>(envelope),
            "next scan continues attack instead of restarting it");

    engine->noteOn(67, 0.8f);
    const auto repeatedEnvelope = Access::envelope(*engine);
    require(!engine->retargetHeldNoteLegato(67, 69), "repeated source is ambiguous");
    require(engine->isNoteHeld(67) && !engine->isNoteHeld(69)
            && Access::envelope(*engine) == repeatedEnvelope, "rejected retarget is inert");
    engine->noteOff(67);
    engine->noteOn(72, 0.8f);
    require(!engine->retargetHeldNoteLegato(72, 67), "already-owned destination is ambiguous");
    require(engine->retargetHeldNoteLegato(72, 74), "new held-gate pitch keeps priority slot");
    engine->noteOff(74);
    require(Access::keyedRoot(*engine) == 67, "CV release restores the older MIDI hold");
    require(!engine->retargetHeldNoteLegato(-1, 60)
            && !engine->retargetHeldNoteLegato(67, 128), "reject invalid MIDI pitches");
    engine->noteOff(67);
    require(!engine->retargetHeldNoteLegato(67, 70), "released source cannot move a voice");
}

void masterTuneExtension()
{
    for (int eighthCents = -1600; eighthCents <= 1600; ++eighthCents)
    {
        const double cents = eighthCents / 8.0;
        const auto original = std::clamp(std::lround(std::clamp(cents, -50.0, 50.0) * 2.56),
                                         -128L, 127L);
        require(YouKnowEngine::masterTunePitchWordOffset(cents) == original,
                "default hardware signed-byte tuning is unchanged");
        if (cents >= -50.0 && cents <= 50.0)
            require(YouKnowEngine::masterTunePitchWordOffset(cents, true) == original,
                    "extended tuning preserves the entire hardware range");
    }
    require(YouKnowEngine::masterTunePitchWordOffset(150.0, true) == 383
            && YouKnowEngine::masterTunePitchWordOffset(-150.0, true) == -384,
            "extension adds exactly one semitone beyond each original endpoint");
    require(YouKnowEngine::masterTunePitchWordOffset(1e30, true) == 383
            && YouKnowEngine::masterTunePitchWordOffset(-1e30, true) == -384
            && YouKnowEngine::masterTunePitchWordOffset(
                std::numeric_limits<double>::quiet_NaN(), true) == 0,
            "extended tuning remains bounded for hostile controls");

    auto reference = std::make_unique<YouKnowEngine>();
    auto extended = std::make_unique<YouKnowEngine>();
    EngineParameters parameters;
    parameters.masterTuneCents = 120.0f;
    reference->setParameters(parameters);
    require(Access::tune(*reference) == 50.0f, "default snapshot retains fifty-cent limit");
    parameters.allowHostMasterTuneExtension = true;
    extended->setParameters(parameters);
    require(Access::tune(*extended) == 120.0f, "host snapshot explicitly permits extended tuning");
    parameters = {};
    reference->setParameters(parameters);
    parameters.allowHostMasterTuneExtension = true;
    extended->setParameters(parameters);
    for (auto* engine : { reference.get(), extended.get() })
    {
        engine->prepare(48000, 64, 1);
        engine->noteOn(60, 1.0f);
    }
    std::array<float, 64> a {}, b {}, c {}, d {};
    double energy = 0.0;
    for (int block = 0; block < 64; ++block)
    {
        reference->process(a.data(), b.data(), 64);
        extended->process(c.data(), d.data(), 64);
        require(std::memcmp(a.data(), c.data(), sizeof(a)) == 0
                && std::memcmp(b.data(), d.data(), sizeof(b)) == 0,
                "opting into wider tuning at center preserves default audio exactly");
        for (float sample : a) energy += sample * sample;
    }
    require(energy > 1e-6, "audio-preservation fixture is audible");
}
}

int main()
{
    monoPriority();
    legatoCv();
    masterTuneExtension();
    std::puts("PASS: host mono priority, held-gate CV continuity, ownership guards and opt-in tuning");
}
