#pragma once

#include "ShowcaseScore.h"

namespace youknow::showcase
{
inline Demo bassDemo()
{
    Demo d;
    d.filename = "showcase-01-bass-midnight-drive.wav";
    d.title = "Midnight Drive";
    d.category = "Bass";
    d.key = "E natural minor";
    d.description = "A twelve-bar bass groove with six-voice unison, syncopated "
                    "pluck figures, octave answers and a gradually opening resonant filter.";
    d.preset = "YB4 Rubber Bass — six-voice showcase variation";
    d.tempo = 100.0;
    d.musicBeats = 48.0;
    d.tailSeconds = 3.2;
    d.scale = pitchClasses ({ 4, 6, 7, 9, 11, 0, 2 });
    d.parameters = presetParameters ("YB4");
    auto& p = d.parameters;
    // YB4 stores a one-voice product setup. This performance deliberately uses
    // the hardware's whole six-card Solo Unison stack, without added detuning.
    p.keyMode = KeyMode::Unison;
    p.polyphony = 6;
    p.portamento = 0.0f;
    p.keyTranspose = 0;
    p.masterTuneCents = 0.0f;
    p.volume = 0.50f;
    d.cutoff = { { 0, p.cutoff }, { 8, 0.22f }, { 16, 0.16f },
                 { 24, 0.27f }, { 32, 0.20f }, { 40, 0.30f }, { 48, 0.14f } };

    chord (d, 0, 8, "Em7", { 4, 7, 11, 2 });
    chord (d, 8, 4, "C", { 0, 4, 7 });
    chord (d, 12, 4, "D7", { 2, 6, 9, 0 });
    chord (d, 16, 4, "Em7", { 4, 7, 11, 2 });
    chord (d, 20, 4, "G", { 7, 11, 2 });
    chord (d, 24, 4, "Am7", { 9, 0, 4, 7 });
    chord (d, 28, 4, "Bm7", { 11, 2, 6, 9 });
    chord (d, 32, 4, "C", { 0, 4, 7 });
    chord (d, 36, 4, "D", { 2, 6, 9 });
    chord (d, 40, 4, "Bm7", { 11, 2, 6, 9 });
    chord (d, 44, 4, "Em", { 4, 7, 11 });

    // The first phrase establishes the low E and answers it one octave up.
    note (d, 0.00, 0.55, { 40 });
    note (d, 0.75, 0.20, { 40 });
    note (d, 1.50, 0.30, { 47 });
    note (d, 2.00, 0.30, { 50 });
    note (d, 2.50, 0.40, { 52 });
    note (d, 3.25, 0.20, { 47 });
    note (d, 3.50, 0.25, { 43 });

    note (d, 4.00, 0.70, { 40 });
    note (d, 5.00, 0.35, { 43 });
    note (d, 5.50, 0.30, { 45 }, true);
    note (d, 6.00, 0.70, { 47 });
    note (d, 7.00, 0.25, { 50 });
    note (d, 7.50, 0.25, { 47 });

    note (d, 8.00, 0.70, { 36 });
    note (d, 9.00, 0.32, { 43 });
    note (d, 9.50, 0.42, { 48 });
    note (d, 10.25, 0.35, { 52 });
    note (d, 11.00, 0.20, { 50 }, true);
    note (d, 11.50, 0.25, { 48 });

    note (d, 12.00, 0.50, { 38 });
    note (d, 12.75, 0.25, { 45 });
    note (d, 13.50, 0.30, { 48 });
    note (d, 14.00, 0.50, { 50 });
    note (d, 14.75, 0.25, { 45 });
    note (d, 15.50, 0.30, { 38 });

    // A displaced version of the opening riff leads into the higher answer.
    note (d, 16.00, 0.40, { 40 });
    note (d, 16.75, 0.20, { 52 });
    note (d, 17.25, 0.30, { 47 });
    note (d, 18.00, 0.50, { 43 });
    note (d, 18.75, 0.25, { 47 });
    note (d, 19.25, 0.20, { 50 });
    note (d, 19.50, 0.25, { 52 });

    note (d, 20.00, 0.60, { 43 });
    note (d, 21.00, 0.30, { 50 });
    note (d, 21.50, 0.40, { 47 });
    note (d, 22.25, 0.35, { 55 });
    note (d, 23.00, 0.30, { 50 });
    note (d, 23.50, 0.30, { 47 });

    note (d, 24.00, 0.60, { 45 });
    note (d, 25.00, 0.30, { 52 });
    note (d, 25.50, 0.40, { 55 });
    note (d, 26.25, 0.35, { 52 });
    note (d, 27.00, 0.30, { 48 });
    note (d, 27.50, 0.30, { 47 }, true);

    note (d, 28.00, 0.55, { 47 });
    note (d, 28.75, 0.20, { 54 });
    note (d, 29.25, 0.25, { 50 });
    note (d, 30.00, 0.50, { 47 });
    note (d, 30.75, 0.25, { 45 });
    note (d, 31.50, 0.25, { 42 });

    // The final four bars climb through C–D–Bm before the low-E release.
    note (d, 32.00, 0.45, { 48 });
    note (d, 32.75, 0.20, { 43 });
    note (d, 33.25, 0.30, { 48 });
    note (d, 34.00, 0.45, { 52 });
    note (d, 34.75, 0.25, { 55 });
    note (d, 35.50, 0.25, { 52 });

    note (d, 36.00, 0.55, { 50 });
    note (d, 36.75, 0.20, { 45 });
    note (d, 37.25, 0.30, { 50 });
    note (d, 38.00, 0.50, { 54 });
    note (d, 38.75, 0.20, { 52 }, true);
    note (d, 39.25, 0.20, { 50 });
    note (d, 39.50, 0.25, { 47 }, true);

    note (d, 40.00, 0.65, { 47 });
    note (d, 41.00, 0.30, { 50 });
    note (d, 41.50, 0.30, { 54 });
    note (d, 42.00, 0.30, { 57 });
    note (d, 42.50, 0.25, { 54 });
    note (d, 43.00, 0.30, { 50 });
    note (d, 43.50, 0.25, { 47 });

    note (d, 44.00, 0.70, { 40 });
    note (d, 45.00, 0.40, { 47 });
    note (d, 45.75, 0.45, { 52 });
    note (d, 46.50, 1.45, { 40 });
    return d;
}

inline Demo leadDemo()
{
    Demo d;
    d.filename = "showcase-02-lead-signal-fire.wav";
    d.title = "Signal Fire";
    d.category = "Lead";
    d.key = "D natural minor";
    d.description = "An eight-bar singing melody, with two answering phrases, "
                    "gentle centered vibrato and brief glides into sustained notes.";
    d.preset = "A53 Lead III — singing mono showcase variation";
    d.tempo = 72.0;
    d.musicBeats = 32.0;
    d.tailSeconds = 4.0;
    d.scale = pitchClasses ({ 2, 4, 5, 7, 9, 10, 0 });
    d.parameters = presetParameters ("A53");
    auto& p = d.parameters;
    // A53's stored saw voice uses a gate VCA and 21/127 DCO modulation.
    // A shaped VCA and shallower native vibrato make this an authored singing
    // variation; the archival factory tone itself is left untouched.
    p.keyMode = KeyMode::Unison;
    p.polyphony = 1;
    p.keyTranspose = 0;
    p.masterTuneCents = 0.0f;
    p.vcaMode = VcaMode::Envelope;
    p.attack = 0.03f;
    p.sustain = 0.72f;
    p.release = 0.20f;
    p.dcoLfoDepth = 9.0f / 127.0f;
    p.volume = 0.70f;
    // Measured through the shipping loaded-pot adapter: travel 0.20 gives
    // 87/256 semitone per 4.2 ms scan, or 151.2 ms for one octave. Adjacent
    // notes below differ by at most five semitones, settling within 63 ms.
    // Every pitch-bend input remains neutral; the glide is the only slide.
    p.portamento = 0.20f;

    chord (d, 0, 4, "Dm", { 2, 5, 9 });
    chord (d, 4, 4, "C", { 0, 4, 7 });
    chord (d, 8, 4, "Bbmaj7", { 10, 2, 5, 9 });
    chord (d, 12, 4, "Dm", { 2, 5, 9 });
    chord (d, 16, 4, "F", { 5, 9, 0 });
    chord (d, 20, 4, "C", { 0, 4, 7 });
    chord (d, 24, 4, "Am7", { 9, 0, 4, 7 });
    chord (d, 28, 4, "Dm", { 2, 5, 9 });

    // A rising D-minor call; the short G connects A back to F.
    note (d, 0.00, 1.40, { 62 });
    note (d, 1.50, 0.45, { 65 });
    note (d, 2.00, 0.90, { 69 });
    note (d, 3.00, 0.42, { 67 }, true);
    note (d, 3.50, 0.42, { 65 });

    note (d, 4.00, 0.90, { 64 });
    note (d, 5.00, 1.35, { 60 });
    note (d, 6.50, 0.40, { 62 }, true);
    note (d, 7.00, 0.85, { 64 });

    note (d, 8.00, 0.90, { 65 });
    note (d, 9.00, 0.90, { 69 });
    note (d, 10.00, 1.35, { 70 });
    note (d, 11.50, 0.40, { 69 });

    note (d, 12.00, 1.35, { 69 });
    note (d, 13.50, 0.45, { 67 }, true);
    note (d, 14.00, 0.45, { 65 });
    note (d, 14.50, 0.45, { 64 }, true);
    note (d, 15.00, 0.70, { 62 });

    // A lower pickup answers the first phrase and climbs to its high C.
    note (d, 16.00, 0.90, { 57 });
    note (d, 17.00, 0.45, { 60 });
    note (d, 17.50, 0.45, { 65 });
    note (d, 18.00, 1.40, { 69 });
    note (d, 19.50, 0.45, { 67 }, true);

    note (d, 20.00, 0.90, { 67 });
    note (d, 21.00, 0.45, { 64 });
    note (d, 21.50, 0.45, { 62 }, true);
    note (d, 22.00, 0.90, { 60 });
    note (d, 23.00, 0.90, { 64 });

    note (d, 24.00, 0.90, { 67 });
    note (d, 25.00, 0.45, { 69 });
    note (d, 25.50, 0.45, { 72 });
    note (d, 26.00, 0.90, { 69 });
    note (d, 27.00, 0.45, { 65 }, true);
    note (d, 27.50, 0.45, { 64 });

    note (d, 28.00, 0.90, { 62 });
    note (d, 29.00, 0.45, { 57 });
    note (d, 29.50, 0.45, { 60 }, true);
    note (d, 30.00, 1.90, { 62 });
    return d;
}
} // namespace youknow::showcase
