#pragma once

// The panels the technique demos play: the README's More demos
// (RenderPresetDemos.cpp --more, from Docs/audio/midi/more) and the local
// takes (RenderDemos.cpp). Each is a whole front panel, not a preset, because
// it is set up to show one mechanism.

#include "DSP/YouKnowEngine.h"

namespace youknow::demo_panels
{

// The panel every demo starts from: a plain saw voice with the filter well
// open and no effects. VCA LEVEL sets the shared pre-chorus drive while main
// VOLUME follows the IC6 output summers. About 0.84 LEVEL is a useful
// multi-note working point with the exact 0.1 voice summer and IC6 gain; the
// old unity voice sum needed misleading near-minimum settings.
inline EngineParameters plainPanel()
{
    EngineParameters parameters;
    parameters.sawEnabled = true;
    parameters.pulseEnabled = false;
    parameters.subLevel = 0.0f;
    parameters.noiseLevel = 0.0f;
    parameters.highPass = HighPassMode::One;
    parameters.cutoff = 0.62f;
    parameters.resonance = 0.10f;
    parameters.envDepth = 0.35f;
    parameters.keyFollow = 0.5f;
    parameters.attack = 0.04f;
    parameters.decay = 0.45f;
    parameters.sustain = 0.70f;
    parameters.release = 0.30f;
    parameters.vcaLevel = 0.84f;
    parameters.chorus = ChorusMode::Off;
    parameters.volume = 0.45f;
    return parameters;
}

// Brass stabs: the filter envelope working a resonant low-pass, with the
// bender pushed on the final chord.
inline EngineParameters filterBrass()
{
    auto parameters = plainPanel();
    parameters.subLevel = 0.4f;
    parameters.cutoff = 0.30f;
    parameters.resonance = 0.42f;
    parameters.envDepth = 0.55f;
    parameters.attack = 0.02f;
    parameters.decay = 0.5f;
    parameters.sustain = 0.45f;
    parameters.release = 0.25f;
    return parameters;
}

// The filter played as a voice: resonance past the threshold with full key
// follow, no oscillator running. What sounds is the four-pole cascade's own
// limit cycle, landing on the control law's pitch as the service procedure
// trims it to.
inline EngineParameters selfOscillation()
{
    auto parameters = plainPanel();
    parameters.sawEnabled = false;
    parameters.vcaLevel = 1.0f;
    parameters.cutoff = 0.34f;
    parameters.resonance = 1.0f;
    parameters.envDepth = 0.0f;
    parameters.keyFollow = 1.0f;
    parameters.attack = 0.01f;
    parameters.decay = 0.4f;
    parameters.sustain = 1.0f;
    parameters.release = 0.45f;
    // One voice, reused for every note: the limit cycle takes over a second
    // to grow out of the transconductors' own noise, so the phrase
    // establishes it once and then keeps the same filter ringing across the
    // whole melody, exactly as a player leaning on the resonance does.
    parameters.polyphony = 1;
    parameters.volume = 0.7f;
    return parameters;
}

// Unison with portamento: six voices on one key with no pitch spread at all -
// six timers dividing one reference cannot disagree - so what thickens the
// note is the analogue block and the effect, and the glide runs at a constant
// rate in pitch, taking proportionally longer over the wider leaps.
inline EngineParameters unisonGlide()
{
    auto parameters = plainPanel();
    parameters.keyMode = KeyMode::Unison;
    parameters.subLevel = 0.5f;
    parameters.cutoff = 0.45f;
    parameters.envDepth = 0.4f;
    parameters.portamento = 0.42f;
    parameters.attack = 0.01f;
    parameters.decay = 0.55f;
    parameters.sustain = 0.7f;
    parameters.release = 0.3f;
    parameters.chorus = ChorusMode::One;
    parameters.volume = 0.35f;
    return parameters;
}

// The modulator's delayed onset: a held chord whose vibrato fades in on the
// two-stage hold-then-ramp envelope, stepping at the scan rate as the
// firmware's staircase does.
inline EngineParameters delayedVibrato()
{
    auto parameters = plainPanel();
    parameters.subLevel = 0.5f;
    parameters.cutoff = 0.5f;
    parameters.dcoLfoDepth = 0.45f;
    parameters.lfoRate = 0.5f;
    parameters.lfoDelay = 0.72f;
    parameters.attack = 0.15f;
    parameters.sustain = 0.9f;
    parameters.release = 0.4f;
    return parameters;
}

} // namespace youknow::demo_panels
