#pragma once

#include "ShowcaseScore.h"

#include <algorithm>

namespace youknow::showcase
{
namespace polyphonic_detail
{
struct Voicing
{
    double beat, length;
    std::vector<int> notes;
};

// Tie common pitches through consecutive voicings. A new chord changes only
// its moving voices; it does not retrigger the whole ensemble's envelopes.
// The score therefore needs no extra layered synth to connect its harmony.
inline void heldVoicings (Demo& d, std::initializer_list<Voicing> input)
{
    const std::vector<Voicing> voicings (input);
    const auto contains = [] (const Voicing& v, int pitch) {
        return std::find (v.notes.begin(), v.notes.end(), pitch) != v.notes.end();
    };
    for (std::size_t i = 0; i < voicings.size(); ++i)
    {
        const auto& current = voicings[i];
        for (int pitch : current.notes)
        {
            if (i > 0 && voicings[i - 1].beat + voicings[i - 1].length == current.beat
                && contains (voicings[i - 1], pitch))
                continue;
            double end = current.beat + current.length;
            for (std::size_t next = i + 1; next < voicings.size()
                 && voicings[next].beat == end && contains (voicings[next], pitch); ++next)
                end += voicings[next].length;
            note (d, current.beat, end - current.beat, { pitch });
        }
    }
}
} // namespace polyphonic_detail

inline Demo stringsDemo()
{
    Demo d;
    d.filename = "03-strings-silver-canopy.wav";
    d.title = "Silver Canopy";
    d.category = "Strings";
    d.key = "D major";
    d.description = "Five voice-led inner parts support a singing upper voice; "
                    "factory PWM strings resolve an A7 suspension into D6/9.";
    d.preset = "B11 Strings";
    d.tempo = 76.0;
    d.tailSeconds = 4.0;
    d.scale = pitchClasses ({ 2, 4, 6, 7, 9, 11, 1 });
    d.parameters = presetParameters ("B11");
    // B11 already has saw + pulse, LFO-driven PWM, chorus II and no DCO pitch
    // modulation or SUB. Preserve its factory tone: ensemble motion comes
    // from PWM/chorus, while all six pitched voices remain in D major.
    d.cutoff = { { 0.0, d.parameters.cutoff } };
    d.wheel = { { 0.0, 0.0f } };

    chord (d,  0, 4, "Dmaj9",    { 2, 6, 9, 1, 4 });
    chord (d,  4, 4, "Bm7",      { 11, 2, 6, 9 });
    chord (d,  8, 4, "Gmaj9",    { 7, 11, 2, 6, 9 });
    chord (d, 12, 4, "Em9",      { 4, 7, 11, 2, 6 });
    chord (d, 16, 4, "Dmaj9/F#", { 2, 6, 9, 1, 4 });
    chord (d, 20, 4, "Gmaj9",    { 7, 11, 2, 6, 9 });
    chord (d, 24, 2, "A7sus4",   { 9, 2, 4, 7 });
    chord (d, 26, 2, "A7",       { 9, 1, 4, 7 });
    chord (d, 28, 4, "D6/9",     { 2, 6, 9, 11, 4 });

    polyphonic_detail::heldVoicings (d, {
        {  0, 4, { 50, 57, 61, 64, 66 } },
        {  4, 4, { 47, 54, 57, 62, 66 } },
        {  8, 4, { 43, 55, 59, 62, 66 } },
        { 12, 4, { 52, 55, 59, 62, 66 } },
        { 16, 4, { 54, 57, 61, 64, 66 } },
        { 20, 4, { 55, 59, 62, 66, 69 } },
        { 24, 2, { 45, 57, 62, 64, 67 } },
        { 26, 2, { 45, 57, 61, 64, 67 } },
        { 28, 4, { 50, 57, 59, 64, 66 } }
    });

    // The sixth voice rises toward the second phrase, then resolves C# to D
    // above the dominant-to-tonic bass motion. These are chord tones, with
    // small breaths rather than a pasted scale run or continuous pitch bend.
    note (d,  0, 1.95, { 69 });
    note (d,  2, 0.95, { 73 });
    note (d,  3, 3.90, { 74 }); // D is shared by Dmaj9 and Bm7.
    note (d,  7, 0.95, { 71 });
    note (d,  8, 1.95, { 69 });
    note (d, 10, 3.90, { 71 }); // B is shared by Gmaj9 and Em9.
    note (d, 14, 1.95, { 74 });
    note (d, 16, 1.95, { 76 });
    note (d, 18, 7.95, { 74 }); // D stays through Gmaj9 and A7sus4.
    note (d, 26, 1.95, { 73 });
    note (d, 28, 4.00, { 74 });
    return d;
}

inline Demo brassDemo()
{
    Demo d;
    d.filename = "04-brass-city-lights.wav";
    d.title = "City Lights";
    d.category = "Brass";
    d.key = "B-flat major";
    d.description = "Syncopated four-note brass hits answer a rising melody, "
                    "then a Cm9-F9 turnaround lands on a six-voice B-flat sixth.";
    d.preset = "A11 Brass Set 1, 8-foot range and short release";
    d.tempo = 80.0;
    d.tailSeconds = 4.0;
    d.scale = pitchClasses ({ 10, 0, 2, 3, 5, 7, 9 });
    d.parameters = presetParameters ("A11");
    // Authored demo panel settings: the factory program is 16'. The 8' range
    // puts these written horn voicings in their sounding register; the shorter
    // release leaves space around the offbeat answers. The bank is unchanged.
    d.parameters.range = DcoRange::Eight;
    d.parameters.release = 0.18f;
    d.cutoff = { { 0.0, d.parameters.cutoff } };
    d.wheel = { { 0.0, 0.0f } };

    chord (d,  0, 4, "Bb6/9",  { 10, 2, 5, 7, 0 });
    chord (d,  4, 4, "Gm9",    { 7, 10, 2, 5, 9 });
    chord (d,  8, 4, "Ebmaj9", { 3, 7, 10, 2, 5 });
    chord (d, 12, 4, "F9",     { 5, 9, 0, 3, 7 });
    chord (d, 16, 4, "Dm7",    { 2, 5, 9, 0 });
    chord (d, 20, 4, "Ebmaj9", { 3, 7, 10, 2, 5 });
    chord (d, 24, 2, "Cm9",    { 0, 3, 7, 10, 2 });
    chord (d, 26, 2, "F9",     { 5, 9, 0, 3, 7 });
    chord (d, 28, 4, "Bb6",    { 10, 2, 5, 7 });

    note (d,  0.00, 0.70, { 58, 62, 65, 67 });
    note (d,  1.25, 0.40, { 58, 62, 65, 67 });
    note (d,  2.00, 0.45, { 74 });
    note (d,  2.75, 0.45, { 77 });
    note (d,  3.50, 0.40, { 79 });

    note (d,  4.00, 0.60, { 55, 62, 65, 70 });
    note (d,  5.00, 0.60, { 55, 62, 65, 70 });
    note (d,  6.00, 0.65, { 77 });
    note (d,  6.75, 0.45, { 74 });
    note (d,  7.50, 0.40, { 70 });

    note (d,  8.00, 0.80, { 51, 58, 62, 67 });
    note (d,  9.50, 0.40, { 51, 58, 62, 67 });
    note (d, 10.25, 0.40, { 74 });
    note (d, 10.75, 0.40, { 77 });
    note (d, 11.50, 0.40, { 79 });

    note (d, 12.00, 0.60, { 53, 57, 63, 67 });
    note (d, 13.25, 0.40, { 53, 57, 63, 67 });
    note (d, 14.00, 0.40, { 75 });
    note (d, 14.50, 0.40, { 72 });
    note (d, 15.00, 0.40, { 69 });
    note (d, 15.50, 0.40, { 67 });

    note (d, 16.00, 0.40, { 50, 57, 60, 65 });
    note (d, 16.75, 0.40, { 50, 57, 60, 65 });
    note (d, 17.50, 0.70, { 50, 57, 60, 65 });
    note (d, 18.50, 0.40, { 77 });
    note (d, 19.00, 0.40, { 74 });
    note (d, 19.50, 0.40, { 72 });

    note (d, 20.00, 0.65, { 51, 58, 62, 67 });
    note (d, 21.25, 0.45, { 51, 58, 62, 67 });
    note (d, 22.00, 0.40, { 74 });
    note (d, 22.50, 0.40, { 77 });
    note (d, 23.00, 0.40, { 79 });
    note (d, 23.50, 0.40, { 77 });

    note (d, 24.00, 0.50, { 48, 55, 58, 63 });
    note (d, 24.75, 0.50, { 48, 55, 58, 63 });
    note (d, 25.50, 0.40, { 74 });
    note (d, 26.00, 0.50, { 53, 57, 63, 67 });
    note (d, 26.75, 0.50, { 53, 57, 63, 67 });
    note (d, 27.50, 0.40, { 69, 75 });
    // The exposed A/Eb dominant interval resolves outward to Bb/D. A low
    // tonic and its sixth fill the final six-voice chord, after the short hits.
    note (d, 28.00, 4.00, { 46, 58, 65, 67, 70, 74 });
    return d;
}

inline Demo padDemo()
{
    Demo d;
    d.filename = "05-pad-slow-horizon.wav";
    d.title = "Slow Horizon";
    d.category = "Pad";
    d.key = "C major";
    d.description = "Six spacious voices hold common tones through major and "
                    "minor ninth chords; slow PWM and a gentle filter opening "
                    "lead to a G9 suspension resolving into C6/9.";
    d.preset = "YP2 Slow Horizon, lighter sub oscillator";
    d.tempo = 68.0;
    d.tailSeconds = 6.0;
    d.scale = pitchClasses ({ 0, 2, 4, 5, 7, 9, 11 });
    d.parameters = presetParameters ("YP2");
    // A visible arrangement patch variation: keep the preset's slow envelope,
    // pulse PWM and chorus I, but reduce SUB for six spread chord voices. Pitch
    // modulation remains zero. The cutoff curve is a panel performance, not
    // a different DSP model or an effect added after the synth.
    d.parameters.subLevel = 0.10f;
    d.cutoff = { { 0, 0.28f }, { 8, 0.31f }, { 16, 0.38f },
                 { 20, 0.40f }, { 24, 0.35f }, { 28, 0.32f },
                 { 32, 0.30f } };
    d.wheel = { { 0.0, 0.0f } };

    chord (d,  0, 4, "Cmaj9",  { 0, 4, 7, 11, 2 });
    chord (d,  4, 4, "Am9",    { 9, 0, 4, 7, 11 });
    chord (d,  8, 4, "Fmaj9",  { 5, 9, 0, 4, 7 });
    chord (d, 12, 4, "Dm9",    { 2, 5, 9, 0, 4 });
    chord (d, 16, 4, "Em7",    { 4, 7, 11, 2 });
    chord (d, 20, 4, "Fmaj9",  { 5, 9, 0, 4, 7 });
    chord (d, 24, 2, "G9sus4", { 7, 0, 2, 5, 9 });
    chord (d, 26, 2, "G9",     { 7, 11, 2, 5, 9 });
    chord (d, 28, 4, "C6/9",   { 0, 4, 7, 9, 2 });

    polyphonic_detail::heldVoicings (d, {
        {  0, 4, { 48, 55, 59, 64, 74, 79 } },
        {  4, 4, { 45, 55, 59, 64, 72, 79 } },
        {  8, 4, { 41, 53, 57, 64, 72, 79 } },
        { 12, 4, { 50, 57, 60, 64, 69, 77 } },
        { 16, 4, { 52, 55, 59, 62, 67, 79 } },
        { 20, 4, { 53, 57, 60, 64, 67, 81 } },
        { 24, 2, { 43, 55, 60, 62, 65, 69 } },
        { 26, 2, { 43, 55, 59, 62, 65, 69 } },
        { 28, 4, { 48, 55, 60, 64, 69, 74 } }
    });
    return d;
}
} // namespace youknow::showcase
