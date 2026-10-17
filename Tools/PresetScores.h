#pragma once

#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowPresets.h"
#include "DSP/YouKnowActiveProductFidelity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace youknow::preset_demos
{

inline EngineParameters loadPresetParameters (const char* slot)
{
    const auto* preset = presets::findByNumber (slot);
    if (preset == nullptr)
        throw std::runtime_error (std::string ("No factory preset ") + slot);

    const auto& patch = preset->patch;
    const auto& controls = preset->controls;
    EngineParameters p;
    ActiveProductFidelityProfile::applyTo (p);

    p.lfoRate = patch.lfoRate;
    p.lfoDelay = patch.lfoDelay;
    p.dcoLfoDepth = patch.dcoLfo;
    p.pwmDepth = patch.pwm;
    p.noiseLevel = patch.noise;
    p.cutoff = patch.cutoff;
    p.resonance = patch.resonance;
    p.envDepth = patch.vcfEnv;
    p.vcfLfoDepth = patch.vcfLfo;
    p.keyFollow = patch.keyFollow;
    p.vcaLevel = patch.vcaLevel;
    p.attack = patch.attack;
    p.decay = patch.decay;
    p.sustain = patch.sustain;
    p.release = patch.release;
    p.subLevel = patch.sub;
    p.range = patch.range;
    p.sawEnabled = patch.saw;
    p.pulseEnabled = patch.pulse;
    p.pwmSource = patch.pwmSource;
    p.vcaMode = patch.vcaMode;
    p.envPolarity = patch.envPolarity;
    p.highPass = patch.highPass;
    p.chorus = patch.chorus;

    p.benderDcoDepth = controls.benderDco;
    p.benderVcfDepth = controls.benderVcf;
    p.benderLfoDepth = controls.benderLfo;
    p.portamento = controls.portamento;
    p.keyMode = controls.keyMode;
    p.keyTranspose = controls.transpose;
    p.masterTuneCents = controls.masterTune;
    p.velocityDepth = controls.velocity;
    p.calibration = controls.calibration;
    p.chorusNoise = controls.chorusNoise;
    p.polyphony = controls.polyphony;
    p.volume = controls.volume;
    p.outputTrimDb = controls.outputTrimDb;

    // 4x oversampling ceiling with exact tanh and half-step Merson filter solver
    p.vcfTanhMode = VcfTanhMode::Exact;
    p.vcfSolverMode = VcfSolverMode::MersonHalfSteps;
    p.aging = 0.0f;
    return p;
}

} // namespace youknow::preset_demos
