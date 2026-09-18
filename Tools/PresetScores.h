#pragma once

#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowPresets.h"
#include "DSP/YouKnowProductFidelity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

namespace youknow::preset_demos
{

struct NoteEvent
{
    double beat;
    double length;
    std::vector<int> notes;
};

struct PresetDemo
{
    const char* slot;
    const char* filename;
    const char* title;
    const char* category;
    const char* musicalDescription;
    double tempo;
    double musicBeats;
    double tailSeconds;
    std::vector<NoteEvent> events;
};

inline void note (PresetDemo& d, double beat, double length,
                  std::initializer_list<int> notes)
{
    d.events.push_back ({ beat, length, notes });
}

inline EngineParameters loadPresetParameters (const char* slot)
{
    const auto* preset = presets::findByNumber (slot);
    if (preset == nullptr)
        throw std::runtime_error (std::string ("No factory preset ") + slot);

    const auto& patch = preset->patch;
    const auto& controls = preset->controls;
    EngineParameters p;
    ProductFidelityProfile::applyTo (p);

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

    // 4x oversampling ceiling with exact tanh and half-step Merson filter solver
    p.vcfTanhMode = VcfTanhMode::Exact;
    p.vcfSolverMode = VcfSolverMode::MersonHalfSteps;
    p.aging = 0.0f;
    return p;
}

inline PresetDemo brassSet1Demo()
{
    PresetDemo d;
    d.slot = "A11";
    d.filename = "demo-A11-brass-set-1.wav";
    d.title = "Brass Set 1";
    d.category = "Poly Brass";
    d.musicalDescription = "Syncopated 80s brass stabs and punchy fanfare cadence in B-flat";
    d.tempo = 105.0;
    d.musicBeats = 12.0;
    d.tailSeconds = 2.5;

    // Bb major fanfare and stabs (transposed to horn/fanfare octave for 16' DCO)
    note (d, 0.00, 0.40, { 58, 65, 74 });
    note (d, 0.75, 0.20, { 58, 65, 74 });
    note (d, 1.50, 0.40, { 51, 67, 75 }); // Eb
    note (d, 2.50, 0.60, { 53, 69, 77 }); // F
    note (d, 4.00, 0.35, { 58, 65, 74 }); // Bb
    note (d, 5.00, 0.50, { 62, 69, 74, 77 }); // Dm7
    note (d, 6.00, 0.70, { 63, 67, 70, 74 }); // Ebmaj7
    note (d, 7.50, 0.45, { 60, 65, 70, 75 }); // F7sus4
    note (d, 8.00, 0.50, { 60, 65, 69, 75 }); // F7
    note (d, 9.00, 2.80, { 58, 65, 70, 74, 77 }); // Bb final swell
    return d;
}

inline PresetDemo choirDemo()
{
    PresetDemo d;
    d.slot = "A17";
    d.filename = "demo-A17-choir.wav";
    d.title = "Choir";
    d.category = "Pad / Vocal";
    d.musicalDescription = "Ethereal vocal wash with slow-blooming voice leading in D minor";
    d.tempo = 70.0;
    d.musicBeats = 21.0;
    d.tailSeconds = 4.0;

    // Dm9 -> Bbmaj7 -> Gm9 -> Asus4 -> A7 -> Dm(add9)
    note (d, 0.0, 3.8, { 62, 69, 72, 76 });
    note (d, 4.0, 3.8, { 58, 69, 74, 77 });
    note (d, 8.0, 3.8, { 55, 67, 70, 74 });
    note (d, 12.0, 1.9, { 57, 64, 69, 74 });
    note (d, 14.0, 1.9, { 57, 64, 69, 73 });
    note (d, 16.0, 4.8, { 50, 62, 69, 74, 76 });
    return d;
}

inline PresetDemo electPianoDemo()
{
    PresetDemo d;
    d.slot = "A28";
    d.filename = "demo-A28-elect-piano-2.wav";
    d.title = "Elect. Piano II";
    d.category = "Keyboards";
    d.musicalDescription = "Warm chorused neo-soul progression with singing upper harmonics";
    d.tempo = 80.0;
    d.musicBeats = 20.0;
    d.tailSeconds = 3.0;

    // Fmaj9 -> Dm9 -> Gm9 -> C13 -> F6/9 (transposed to keyboard register for 16' DCO)
    note (d, 0.0, 3.5, { 53, 65, 69, 72, 76 });
    note (d, 4.0, 3.5, { 50, 65, 69, 72, 76 });
    note (d, 8.0, 3.5, { 55, 65, 69, 70, 74 });
    note (d, 12.0, 3.5, { 48, 64, 69, 70, 74 });
    note (d, 16.0, 3.8, { 53, 64, 69, 72, 74, 79 });
    return d;
}

inline PresetDemo steelDrumsDemo()
{
    PresetDemo d;
    d.slot = "A32";
    d.filename = "demo-A32-steel-drums.wav";
    d.title = "Steel Drums";
    d.category = "Percussive";
    d.musicalDescription = "Syncopated tropical calypso bounce and bright envelope strike";
    d.tempo = 115.0;
    d.musicBeats = 16.0;
    d.tailSeconds = 2.0;

    // Caribbean melodic motif
    note (d, 0.00, 0.40, { 60 });
    note (d, 0.75, 0.40, { 67 });
    note (d, 1.50, 0.40, { 64 });
    note (d, 2.00, 0.40, { 67 });
    note (d, 2.50, 0.40, { 69 });
    note (d, 3.25, 0.40, { 67 });

    note (d, 4.00, 0.40, { 65 });
    note (d, 4.75, 0.40, { 69 });
    note (d, 5.50, 0.50, { 74 });
    note (d, 6.50, 0.50, { 72 });

    note (d, 8.00, 0.40, { 64, 67, 72 });
    note (d, 8.75, 0.30, { 76 });
    note (d, 9.50, 0.30, { 74 });
    note (d, 10.00, 0.40, { 72 });

    note (d, 11.00, 0.40, { 65, 69, 72, 77 });
    note (d, 12.00, 0.50, { 67, 71, 74, 79 });
    note (d, 13.00, 2.50, { 60, 67, 72, 76 });
    return d;
}

inline PresetDemo synthBassDemo()
{
    PresetDemo d;
    d.slot = "A48";
    d.filename = "demo-A48-synth-bass-unison.wav";
    d.title = "Synth Bass I (unison)";
    d.category = "Bass";
    d.musicalDescription = "Punchy 16th-note analog funk bassline driving with 6-voice unison bite";
    d.tempo = 110.0;
    d.musicBeats = 15.0;
    d.tailSeconds = 2.0;

    // Strictly monophonic notes (transposed +12 for 16' DCO + heavy sub-oscillator punch)
    note (d, 0.00, 0.45, { 40 });
    note (d, 0.75, 0.20, { 40 });
    note (d, 1.25, 0.20, { 52 });
    note (d, 1.75, 0.20, { 40 });
    note (d, 2.25, 0.35, { 43 });
    note (d, 2.75, 0.35, { 45 });
    note (d, 3.25, 0.20, { 47 });
    note (d, 3.75, 0.20, { 50 });

    note (d, 4.00, 0.45, { 52 });
    note (d, 4.75, 0.20, { 50 });
    note (d, 5.25, 0.20, { 47 });
    note (d, 5.75, 0.25, { 45 });
    note (d, 6.25, 0.25, { 43 });
    note (d, 6.75, 0.20, { 40 });
    note (d, 7.25, 0.20, { 42 });
    note (d, 7.60, 0.30, { 43 });

    note (d, 8.00, 0.45, { 48 });
    note (d, 8.75, 0.20, { 48 });
    note (d, 9.25, 0.40, { 50 });
    note (d, 9.75, 0.20, { 50 });
    note (d, 10.25, 0.35, { 52 });
    note (d, 10.75, 0.20, { 47 });
    note (d, 11.25, 0.25, { 43 });
    note (d, 11.60, 0.30, { 38 });

    note (d, 12.00, 2.50, { 40 });
    return d;
}

inline PresetDemo leadDemo()
{
    PresetDemo d;
    d.slot = "A53";
    d.filename = "demo-A53-lead-3.wav";
    d.title = "Lead III";
    d.category = "Lead";
    d.musicalDescription = "Singing solo synth lead with expressive portamento glides";
    d.tempo = 85.0;
    d.musicBeats = 18.0;
    d.tailSeconds = 3.0;

    // Soaring solo melody
    note (d, 0.0, 1.8, { 69 });
    note (d, 2.0, 0.8, { 72 });
    note (d, 3.0, 0.8, { 74 });
    note (d, 4.0, 2.8, { 76 });
    note (d, 7.0, 0.8, { 79 });
    note (d, 8.0, 1.8, { 77 });
    note (d, 10.0, 1.8, { 74 });
    note (d, 12.0, 0.8, { 72 });
    note (d, 13.0, 0.8, { 71 });
    note (d, 14.0, 3.5, { 69 });
    return d;
}

inline PresetDemo funkyDemo()
{
    PresetDemo d;
    d.slot = "A54";
    d.filename = "demo-A54-funky-2.wav";
    d.title = "Funky II";
    d.category = "Pluck / Stab";
    d.musicalDescription = "Resonant funk stab groove with dynamic envelope bounce in E Dorian";
    d.tempo = 105.0;
    d.musicBeats = 10.0;
    d.tailSeconds = 2.0;

    note (d, 0.00, 0.25, { 64, 71, 74 });
    note (d, 0.75, 0.20, { 64, 71, 74 });
    note (d, 1.50, 0.25, { 64, 69, 73 });
    note (d, 2.25, 0.25, { 66, 71, 74 });
    note (d, 3.00, 0.40, { 64, 71, 74 });
    note (d, 4.00, 0.25, { 67, 71, 76 });
    note (d, 4.75, 0.25, { 69, 73, 76 });
    note (d, 5.50, 0.35, { 64, 67, 71, 74 });
    note (d, 6.25, 0.20, { 64, 67, 71, 74 });
    note (d, 7.00, 0.30, { 64, 69, 74 });
    note (d, 7.75, 1.80, { 64, 67, 71, 74 });
    return d;
}

inline PresetDemo clavDemo()
{
    PresetDemo d;
    d.slot = "A62";
    d.filename = "demo-A62-clav.wav";
    d.title = "Clav";
    d.category = "Pluck";
    d.musicalDescription = "Crisp percussive 70s funk clavinet pattern in D Dorian";
    d.tempo = 100.0;
    d.musicBeats = 10.0;
    d.tailSeconds = 2.0;

    // Transposed +12 to match funky clavinet register with 16' DCO
    note (d, 0.00, 0.20, { 62 });
    note (d, 0.50, 0.15, { 62 });
    note (d, 0.75, 0.20, { 65 });
    note (d, 1.25, 0.25, { 67 });
    note (d, 1.75, 0.15, { 68 });
    note (d, 2.00, 0.35, { 69 });
    note (d, 2.75, 0.20, { 72 });
    note (d, 3.25, 0.40, { 74 });
    note (d, 4.00, 0.25, { 72 });
    note (d, 4.50, 0.25, { 69 });
    note (d, 5.00, 0.30, { 67 });
    note (d, 5.50, 0.25, { 65 });
    note (d, 6.00, 0.40, { 62 });
    note (d, 6.75, 0.20, { 60 });
    note (d, 7.25, 2.00, { 62 });
    return d;
}

inline PresetDemo synthPadDemo()
{
    PresetDemo d;
    d.slot = "A68";
    d.filename = "demo-A68-synth-pad.wav";
    d.title = "Synth Pad";
    d.category = "Pad";
    d.musicalDescription = "Deep warm analog pad with slow filter bloom and moving inner voices";
    d.tempo = 65.0;
    d.musicBeats = 21.0;
    d.tailSeconds = 4.5;

    // Cmaj9 -> Am9 -> Fmaj9 -> G9sus4 -> C6/9
    note (d, 0.0, 3.8, { 60, 67, 71, 74, 76 });
    note (d, 4.0, 3.8, { 57, 67, 71, 72, 76 });
    note (d, 8.0, 3.8, { 53, 64, 69, 72, 76 });
    note (d, 12.0, 3.8, { 55, 65, 69, 72, 74 });
    note (d, 16.0, 4.8, { 48, 60, 67, 71, 74, 76 });
    return d;
}

inline PresetDemo handClapsDemo()
{
    PresetDemo d;
    d.slot = "A86";
    d.filename = "demo-A86-hand-claps.wav";
    d.title = "Hand Claps";
    d.category = "Percussive / FX";
    d.musicalDescription = "Rhythmic analog noise burst claps in an 80s drum groove";
    d.tempo = 100.0;
    d.musicBeats = 9.0;
    d.tailSeconds = 2.0;

    note (d, 0.00, 0.20, { 60 });
    note (d, 1.00, 0.20, { 60 });
    note (d, 1.75, 0.15, { 60 });
    note (d, 2.00, 0.20, { 60 });
    note (d, 3.00, 0.20, { 60 });
    note (d, 3.50, 0.20, { 60 });
    note (d, 4.00, 0.20, { 60 });
    note (d, 5.00, 0.20, { 60 });
    note (d, 5.66, 0.15, { 60 });
    note (d, 6.00, 0.20, { 60 });
    note (d, 7.00, 0.20, { 60 });
    note (d, 7.50, 0.20, { 60 });
    note (d, 8.00, 0.80, { 60 });
    return d;
}

inline PresetDemo stringsDemo()
{
    PresetDemo d;
    d.slot = "B11";
    d.filename = "demo-B11-strings.wav";
    d.title = "Strings";
    d.category = "Strings";
    d.musicalDescription = "The iconic Juno PWM chorused strings with lush voice-led counterpoint";
    d.tempo = 75.0;
    d.musicBeats = 21.0;
    d.tailSeconds = 4.0;

    // Dmaj7 -> Bm7 -> Gmaj9 -> A7sus4 -> A7 -> D6/9
    note (d, 0.0, 3.8, { 62, 69, 73, 78 });
    note (d, 4.0, 3.8, { 59, 69, 74, 78 });
    note (d, 8.0, 3.8, { 55, 67, 71, 74, 78 });
    note (d, 12.0, 1.9, { 57, 67, 69, 74 });
    note (d, 14.0, 1.9, { 57, 67, 69, 73 });
    note (d, 16.0, 4.8, { 50, 62, 69, 74, 76, 81 });
    return d;
}

inline PresetDemo chorusVibesDemo()
{
    PresetDemo d;
    d.slot = "B13";
    d.filename = "demo-B13-chorus-vibes.wav";
    d.title = "Chorus Vibes";
    d.category = "Mallet / Keys";
    d.musicalDescription = "Shimmering vibraphone jazz chords and sparkling upward arpeggio";
    d.tempo = 85.0;
    d.musicBeats = 22.0;
    d.tailSeconds = 3.5;

    // Cmaj9 -> Am9 -> Dm9 -> G13 -> arpeggio to C6/9
    note (d, 0.0, 3.5, { 60, 67, 71, 74, 76 });
    note (d, 4.0, 3.5, { 57, 64, 67, 72, 76 });
    note (d, 8.0, 3.5, { 62, 69, 72, 76, 79 });
    note (d, 12.0, 3.5, { 55, 65, 69, 71, 76 });
    note (d, 16.0, 0.4, { 72 });
    note (d, 16.5, 0.4, { 76 });
    note (d, 17.0, 0.4, { 79 });
    note (d, 17.5, 0.4, { 81 });
    note (d, 18.0, 0.4, { 86 });
    note (d, 18.5, 3.0, { 60, 72, 76, 81, 86 });
    return d;
}

inline PresetDemo brassDemo()
{
    PresetDemo d;
    d.slot = "B31";
    d.filename = "demo-B31-brass.wav";
    d.title = "Brass";
    d.category = "Brass";
    d.musicalDescription = "Warm, mellow brass chorale with gentle filter swells in F major";
    d.tempo = 72.0;
    d.musicBeats = 21.0;
    d.tailSeconds = 3.5;

    // F -> Bb/F -> Dm7 -> Csus4 -> C -> F(add9)
    note (d, 0.0, 3.8, { 53, 65, 69, 72 });
    note (d, 4.0, 3.8, { 53, 65, 70, 74 });
    note (d, 8.0, 3.8, { 50, 65, 69, 74, 77 });
    note (d, 12.0, 1.9, { 48, 67, 72, 77 });
    note (d, 14.0, 1.9, { 48, 67, 72, 76 });
    note (d, 16.0, 4.5, { 53, 65, 69, 72, 74, 77 });
    return d;
}

inline PresetDemo luteDemo()
{
    PresetDemo d;
    d.slot = "B33";
    d.filename = "demo-B33-lute.wav";
    d.title = "Lute";
    d.category = "Pluck";
    d.musicalDescription = "Baroque plucked string arpeggiation with natural acoustic damping in D minor";
    d.tempo = 95.0;
    d.musicBeats = 16.0;
    d.tailSeconds = 2.5;

    // Bar 1
    note (d, 0.0, 0.6, { 62 });
    note (d, 0.5, 0.6, { 69 });
    note (d, 1.0, 0.6, { 77 });
    note (d, 1.5, 0.6, { 74 });
    note (d, 2.0, 0.6, { 69 });
    note (d, 2.5, 0.6, { 77 });
    note (d, 3.0, 0.6, { 74 });
    note (d, 3.5, 0.6, { 69 });

    // Bar 2
    note (d, 4.0, 0.6, { 60 });
    note (d, 4.5, 0.6, { 67 });
    note (d, 5.0, 0.6, { 76 });
    note (d, 5.5, 0.6, { 72 });
    note (d, 6.0, 0.6, { 67 });
    note (d, 6.5, 0.6, { 76 });
    note (d, 7.0, 0.6, { 72 });
    note (d, 7.5, 0.6, { 67 });

    // Bar 3
    note (d, 8.0, 0.6, { 58 });
    note (d, 8.5, 0.6, { 65 });
    note (d, 9.0, 0.6, { 74 });
    note (d, 9.5, 0.6, { 70 });
    note (d, 10.0, 0.6, { 57 });
    note (d, 10.5, 0.6, { 64 });
    note (d, 11.0, 0.6, { 73 });
    note (d, 11.5, 0.6, { 69 });

    // Bar 4 resolution
    note (d, 12.0, 3.5, { 62, 69, 77 });
    return d;
}

inline PresetDemo contactWahDemo()
{
    PresetDemo d;
    d.slot = "B44";
    d.filename = "demo-B44-contact-wah.wav";
    d.title = "Contact Wah";
    d.category = "Filter / Mod";
    d.musicalDescription = "Funky envelope-wah comping with dynamic rhythmic filter sweeps";
    d.tempo = 100.0;
    d.musicBeats = 10.0;
    d.tailSeconds = 2.0;

    note (d, 0.00, 0.40, { 57, 67, 72, 76 });
    note (d, 0.75, 0.20, { 57, 67, 72, 76 });
    note (d, 1.50, 0.35, { 57, 66, 69, 74 });
    note (d, 2.25, 0.30, { 57, 67, 72, 76 });
    note (d, 3.00, 0.35, { 52, 67, 71, 74 });
    note (d, 3.75, 0.40, { 57, 67, 72, 76 });
    note (d, 4.75, 0.20, { 57, 67, 72, 76 });
    note (d, 5.50, 0.35, { 57, 66, 69, 74 });
    note (d, 6.25, 0.30, { 57, 64, 67, 72 });
    note (d, 7.00, 2.50, { 57, 67, 72, 76 });
    return d;
}

inline std::vector<PresetDemo> allPresetDemos()
{
    return {
        brassSet1Demo(),
        choirDemo(),
        electPianoDemo(),
        steelDrumsDemo(),
        synthBassDemo(),
        leadDemo(),
        funkyDemo(),
        clavDemo(),
        synthPadDemo(),
        handClapsDemo(),
        stringsDemo(),
        chorusVibesDemo(),
        brassDemo(),
        luteDemo(),
        contactWahDemo()
    };
}

} // namespace youknow::preset_demos
