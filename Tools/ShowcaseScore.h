#pragma once

#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowPresets.h"
#include "DSP/YouKnowProductFidelity.h"

#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

namespace youknow::showcase
{
struct NoteEvent
{
    double beat;
    double length;
    std::vector<int> notes;
    // Short, in-scale connecting tones may pass outside the current chord.
    bool passing = false;
};
struct HarmonySpan
{
    double beat;
    double length;
    std::string name;
    unsigned int mask;
};
struct CurvePoint { double beat; float value; };
struct Demo
{
    std::string filename, title, category, key, description, preset;
    double tempo = 80.0;
    double musicBeats = 32.0;
    double tailSeconds = 5.0;
    unsigned int scale = 0;
    EngineParameters parameters;
    std::vector<NoteEvent> events;
    std::vector<HarmonySpan> harmony;
    std::vector<CurvePoint> cutoff;
    std::vector<CurvePoint> wheel;
};
inline unsigned int pitchClasses (std::initializer_list<int> notes)
{
    unsigned int result = 0;
    for (int note : notes)
        result |= 1u << ((note % 12 + 12) % 12);
    return result;
}
inline void note (Demo& d, double beat, double length,
                  std::initializer_list<int> notes, bool passing = false)
{
    d.events.push_back ({ beat, length, notes, passing });
}
inline void chord (Demo& d, double beat, double length, const char* name,
                   std::initializer_list<int> notes)
{
    d.harmony.push_back ({ beat, length, name, pitchClasses (notes) });
}
inline EngineParameters presetParameters (const char* number)
{
    const auto* preset = presets::findByNumber (number);
    if (preset == nullptr)
        throw std::runtime_error (std::string ("No showcase preset ") + number);
    const auto& patch = preset->patch;
    const auto& controls = preset->controls;
    EngineParameters p;
    ProductFidelityProfile::applyTo (p);
    p.lfoRate = patch.lfoRate; p.lfoDelay = patch.lfoDelay;
    p.dcoLfoDepth = patch.dcoLfo; p.pwmDepth = patch.pwm;
    p.noiseLevel = patch.noise; p.cutoff = patch.cutoff;
    p.resonance = patch.resonance; p.envDepth = patch.vcfEnv;
    p.vcfLfoDepth = patch.vcfLfo; p.keyFollow = patch.keyFollow;
    p.vcaLevel = patch.vcaLevel; p.attack = patch.attack;
    p.decay = patch.decay; p.sustain = patch.sustain;
    p.release = patch.release; p.subLevel = patch.sub;
    p.range = patch.range; p.sawEnabled = patch.saw;
    p.pulseEnabled = patch.pulse; p.pwmSource = patch.pwmSource;
    p.vcaMode = patch.vcaMode; p.envPolarity = patch.envPolarity;
    p.highPass = patch.highPass; p.chorus = patch.chorus;
    p.benderDcoDepth = controls.benderDco;
    p.benderVcfDepth = controls.benderVcf;
    p.benderLfoDepth = controls.benderLfo;
    p.portamento = controls.portamento; p.keyMode = controls.keyMode;
    p.keyTranspose = controls.transpose; p.masterTuneCents = controls.masterTune;
    p.velocityDepth = controls.velocity; p.calibration = controls.calibration;
    p.chorusNoise = controls.chorusNoise; p.polyphony = controls.polyphony;
    p.volume = controls.volume;
    // Explicit maximum quality: these settings cannot change during a take.
    p.vcfTanhMode = VcfTanhMode::Exact;
    p.vcfSolverMode = VcfSolverMode::MersonHalfSteps;
    p.aging = 0.0f;
    return p;
}
} // namespace youknow::showcase
