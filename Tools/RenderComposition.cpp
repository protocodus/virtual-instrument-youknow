// Renders the committed composition under Docs/audio from the same
// JUCE-free engine the plug-in runs.
//
// Where the numbered demos under Docs/audio each isolate one mechanism, this is
// a piece of music: ten factory presets, each rendered by its own engine and
// balanced and summed, exactly as ten overdubs onto tape. That is the only way a six-voice
// monotimbral instrument plays ten parts, and it is how records were made with
// this machine. Every sound comes out of the circuit model and its own chorus;
// fixed overdub gains and one final normalization set the recorded balance.
//
// Two things separate this render from the numbered demos. It runs the deepest
// rung of the quality ladder rather than the default, because a one-off offline
// render has no realtime budget to respect. And it is written at 24 bits, so
// the summing floor of ten parts stays below the quantiser rather than beside
// it.

#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"
#include "DSP/YouKnowPresets.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
using youknow::EngineParameters;
using youknow::KeyMode;
using youknow::YouKnowEngine;
using youknow::presets::Preset;

// 44.1 kHz is what a listener's browser, phone and DAW all handle without
// conversion. The quality here is bought with the oversampling rung below and
// with the word length, not with a delivery rate nothing plays natively.
constexpr double compositionSampleRate = 44100.0;
constexpr int renderBlockSize = 256;
// The deepest rung of the quality ladder: a 176.4 kHz internal grid at this
// delivery rate. An offline render has no realtime budget to protect.
constexpr int compositionOversampleFactor = 4;
// 24-bit words, so the floor of a ten-part sum sits well below the quantiser.
constexpr int compositionBitsPerSample = 24;
// How early every gate is lifted, so the scanned assigner always gets a pass in
// which a repeated note's key is up. 5 ms at this rate.
constexpr std::int64_t releaseGuardFrames = 221;

// -3 dBFS, the level the numbered demos are normalised to, so a listener moving
// between the demo files and the composition needs no gain change.
constexpr double normalisedPeak = 0.7079457843841379;
// File-delivery guards rather than synthesizer transfer values, carried over
// from the demo renderer: excess DC wastes headroom and thumps downstream
// equipment, and a large endpoint clicks when a player starts or stops a file.
constexpr double maximumAbsoluteDcMean = 0.005;
constexpr double maximumEdgeMagnitude = 0.01;

// ---------------------------------------------------------------------------
// WAV output
// ---------------------------------------------------------------------------

void appendLittleEndian (std::vector<std::uint8_t>& bytes, std::uint32_t value,
                         int byteCount)
{
    for (int index = 0; index < byteCount; ++index)
        bytes.push_back (static_cast<std::uint8_t> ((value >> (8 * index)) & 0xffu));
}

// Stereo 24-bit PCM. YouKnow is a stereo instrument by construction - its two
// channels are the two chorus lines clocked in antiphase - so the file is
// stereo even where a part's chorus is off and its image collapses.
bool writeWav (const std::filesystem::path& path, const std::vector<float>& left,
               const std::vector<float>& right)
{
    constexpr std::uint16_t channels = 2u;
    constexpr std::uint32_t bytesPerSample = compositionBitsPerSample / 8;
    const auto frames = static_cast<std::uint32_t> (left.size());
    const std::uint32_t frameBytes = channels * bytesPerSample;
    const std::uint32_t dataBytes = frames * frameBytes;

    std::vector<std::uint8_t> bytes;
    bytes.reserve (44u + dataBytes);
    const auto tag = [&bytes] (const char* text)
    {
        for (int index = 0; index < 4; ++index)
            bytes.push_back (static_cast<std::uint8_t> (text[index]));
    };
    tag ("RIFF");
    appendLittleEndian (bytes, 36u + dataBytes, 4);
    tag ("WAVE");
    tag ("fmt ");
    appendLittleEndian (bytes, 16u, 4);
    appendLittleEndian (bytes, 1u, 2); // PCM
    appendLittleEndian (bytes, channels, 2);
    appendLittleEndian (bytes, static_cast<std::uint32_t> (compositionSampleRate), 4);
    appendLittleEndian (bytes,
                        static_cast<std::uint32_t> (compositionSampleRate) * frameBytes, 4);
    appendLittleEndian (bytes, static_cast<std::uint16_t> (frameBytes), 2);
    appendLittleEndian (bytes, compositionBitsPerSample, 2);
    tag ("data");
    appendLittleEndian (bytes, dataBytes, 4);

    const auto encode = [] (float value)
    {
        if (! std::isfinite (value))
            value = 0.0f;
        const float clamped = std::clamp (value, -1.0f, 1.0f);
        // Symmetric scaling by 2^23 - 1 so a full-scale negative peak cannot wrap.
        const auto sample = static_cast<std::int32_t> (
            std::lround (static_cast<double> (clamped) * 8388607.0));
        return static_cast<std::uint32_t> (sample) & 0x00ffffffu;
    };

    for (std::size_t frame = 0; frame < left.size(); ++frame)
    {
        appendLittleEndian (bytes, encode (left[frame]), 3);
        appendLittleEndian (bytes, encode (right[frame]), 3);
    }

    std::FILE* file = std::fopen (path.string().c_str(), "wb");
    if (file == nullptr)
        return false;
    const bool written =
        std::fwrite (bytes.data(), 1, bytes.size(), file) == bytes.size();
    std::fclose (file);
    return written;
}

// ---------------------------------------------------------------------------
// The score
// ---------------------------------------------------------------------------

// One attack: the notes struck together at `beat`, released `lengthBeats`
// later. Beats are absolute from the top of the piece, so a part reads as a
// timeline rather than as a chain of rests whose errors accumulate.
struct Event
{
    double beat = 0.0;
    double lengthBeats = 0.0;
    std::vector<int> notes;
};

// A breakpoint on a part's bender or modulation lane. Values between
// breakpoints are interpolated linearly. A lane is neutral before its first
// breakpoint and holds its final value afterwards.
struct Breakpoint
{
    double beat = 0.0;
    float value = 0.0f;
};

// One overdub: a factory preset, the balance it is printed at, and what it
// plays. `volume` replaces the preset's own stored VR1 position, which is the
// instrument's own output control - the balance between parts is set on the
// machine. `mixGainDb` is a fixed overdub balance after the instrument, used
// to bring percussion forward without changing the factory sound or asking
// the already-full shaker VOLUME control for an impossible value above one.
struct Part
{
    const char* slot;
    const char* role;
    float volume;
    float mixGainDb;
    std::vector<Event> events;
    std::vector<Breakpoint> bend;
    std::vector<Breakpoint> wheel;
};

struct Composition
{
    const char* title;
    const char* key;
    double tempo;
    double beatsPerBar;
    // Beats of silence rendered past the last release, so every tail rings out
    // inside the file instead of being cut at the final note-off.
    double tailBeats;
    std::vector<Part> parts;
};

// One shared harmonic chart for every pitched overdub, including the snare's
// sub-oscillator body. These are arrangement choices, not synthesizer laws.
// All chord tones belong to D natural minor: D E F G A Bb C. Octave ranges,
// inversions and centered factory vibrato remain properties of each sound.
struct Harmony
{
    const char* name;
    unsigned int pitchClasses;
};
constexpr unsigned int pc (int note) { return 1u << (note % 12); }
constexpr Harmony dm { "Dm", pc(2) | pc(5) | pc(9) };
constexpr Harmony dm7 { "Dm7", dm.pitchClasses | pc(0) };
constexpr Harmony bb { "Bb", pc(10) | pc(2) | pc(5) };
constexpr Harmony bbMaj7 { "Bbmaj7", bb.pitchClasses | pc(9) };
constexpr Harmony cMajor { "C", pc(0) | pc(4) | pc(7) };
constexpr Harmony gm { "Gm", pc(7) | pc(10) | pc(2) };
constexpr Harmony gm7 { "Gm7", gm.pitchClasses | pc(5) };
constexpr Harmony am7 { "Am7", pc(9) | pc(0) | pc(4) | pc(7) };
constexpr Harmony fMajor { "F", pc(5) | pc(9) | pc(0) };
constexpr Harmony fMaj7 { "Fmaj7", fMajor.pitchClasses | pc(4) };
constexpr std::array<Harmony, 47> harmonyByBar {{
    dm, dm, dm, dm, bb, cMajor,                         // opening: bars 1–6
    dm, dm, gm, cMajor, dm7, dm7, bbMaj7, am7,          // assembly: 7–14
    dm7, dm7, gm7, cMajor, fMaj7, bbMaj7, gm7, am7, dm7, bbMaj7, // theme: 15–24
    gm7, bbMaj7, cMajor, gm7, am7, bb, cMajor,          // bridge: 25–31
    dm, dm, dm, gm,                                    // breakdown: 32–35
    dm, bb, fMajor, gm, bb, cMajor, am7, dm,            // climax: 36–43
    bb, am7, dm, dm                                    // cadence: 44–47
}};
constexpr unsigned int dNaturalMinor = pc(2) | pc(4) | pc(5) | pc(7)
                                     | pc(9) | pc(10) | pc(0);

// "Low Sun": forty-seven bars in D aeolian at 114 bpm, about 99 seconds of
// notes.
//
// Seven panels of deliberately unequal length - 6, 8, 10, 7, 4, 8, 4 bars - so
// nothing lands as a square four- or eight-bar unit except twice, and both of
// those are broken internally. A curtain of choir and strings alone; the riff
// assembling one part per bar so every entry is its own event; a theme with a
// two-bar brass answer that steals the lead's cadence; a half-time bridge; a
// three-part breakdown; a climax on new material rather than the theme moved
// up; and a cadence back to the instrumentation of bar one.
//
// Every pitched event follows harmonyByBar, including notes held across chord
// changes. The bridge stays in the same scale and the cadence uses Am7–Dm;
// there are no borrowed B naturals, C sharps or independent chord choices.
// Long choir holds are revoiced at changes, and the last choir/strings chord
// shares D/F/A. Rhythms, registers and the seven-panel form carry the variety.
//
// Ten parts summed with no panner separate by register and by chorus width or
// they do not separate at all, so the lanes were assigned before any note was
// written and nothing crosses into a lane it does not own. Bar 42 is the only
// bar in which all ten parts sound.
//
// Pitch bend stays centered. The previous lead lane's first breakpoint was
// negative at beat 165.9; holding it backwards detuned the entire earlier
// melody by -518/256 semitones. This score and its regression reject that.
Composition composition()
{
    Composition piece;
    piece.title = "Low Sun";
    piece.key = "D aeolian";
    piece.tempo = 114.0;
    piece.beatsPerBar = 4.0;
    // The vibes' 0.58 release and the choir's and strings' own tails carry the
    // last chord well past its key-up; this is what lets it ring out rather
    // than stop.
    piece.tailBeats = 8.0;
    piece.parts = {
        // Bass.
        // Key mode is unison, so it is monophonic in hardware and can never blur
        // its own line; every note in this part is written with the gate closing
        // before the next one opens, so nothing is ever stolen. Sustain 0.00
        // with decay 0.27 means each note is a decay-only pluck that is
        // physically incapable of droning, which is what keeps a sixteenth-note
        // line articulate at 114 BPM and is also why the half-time bridge reads
        // as space rather than as a held bass.
        Part { "A48", "bass", 0.77f, 0.0f,
               {
                 { 24, 0.7, { 50 } },
                 { 25.5, 0.25, { 50 } },
                 { 26.5, 0.45, { 50 } },
                 { 27.5, 0.45, { 45 } },
                 { 28, 0.7, { 50 } },
                 { 29.5, 0.25, { 50 } },
                 { 30.5, 0.45, { 50 } },
                 { 31, 0.4, { 50 } },
                 { 31.5, 0.45, { 45 } },
                 { 32, 0.45, { 55 } },
                 { 32.5, 0.2, { 55 } },
                 { 33.25, 0.2, { 58 } },
                 { 33.75, 0.2, { 55 } },
                 { 34.5, 0.45, { 55 } },
                 { 35, 0.4, { 55 } },
                 { 35.5, 0.45, { 50 } },
                 { 36, 0.45, { 55 } },
                 { 36.5, 0.2, { 55 } },
                 { 37.25, 0.2, { 60 } },
                 { 37.75, 0.2, { 55 } },
                 { 38.25, 0.6, { 52 } },
                 { 39.25, 0.2, { 55 } },
                 { 39.75, 0.2, { 48 } },
                 { 40, 0.45, { 50 } },
                 { 40.5, 0.2, { 50 } },
                 { 41.25, 0.2, { 53 } },
                 { 41.75, 0.2, { 50 } },
                 { 42.5, 0.45, { 48 } },
                 { 43, 0.4, { 50 } },
                 { 43.5, 0.45, { 45 } },
                 { 44, 0.45, { 50 } },
                 { 44.5, 0.2, { 50 } },
                 { 45.25, 0.2, { 53 } },
                 { 45.75, 0.2, { 53 } },
                 { 46.25, 0.6, { 57 } },
                 { 47.25, 0.2, { 53 } },
                 { 47.75, 0.2, { 53 } },
                 { 48, 0.45, { 46 } },
                 { 48.5, 0.2, { 46 } },
                 { 49.25, 0.2, { 50 } },
                 { 49.75, 0.2, { 46 } },
                 { 50.5, 0.45, { 53 } },
                 { 51, 0.4, { 46 } },
                 { 51.5, 0.45, { 46 } },
                 { 52, 0.45, { 45 } },
                 { 52.5, 0.2, { 45 } },
                 { 53.25, 0.2, { 48 } },
                 { 53.75, 0.2, { 45 } },
                 { 54.5, 0.45, { 52 } },
                 { 55, 0.3, { 55 } },
                 { 55.5, 0.45, { 48 } },
                 { 56, 0.45, { 50 } },
                 { 56.5, 0.2, { 50 } },
                 { 57.25, 0.2, { 53 } },
                 { 57.75, 0.2, { 50 } },
                 { 58.5, 0.45, { 48 } },
                 { 59, 0.4, { 50 } },
                 { 59.5, 0.45, { 45 } },
                 { 60, 0.45, { 50 } },
                 { 60.5, 0.2, { 50 } },
                 { 61.25, 0.2, { 53 } },
                 { 61.75, 0.2, { 53 } },
                 { 62.25, 0.6, { 57 } },
                 { 63.25, 0.2, { 53 } },
                 { 63.75, 0.2, { 53 } },
                 { 64, 0.45, { 55 } },
                 { 64.5, 0.2, { 55 } },
                 { 65.25, 0.2, { 58 } },
                 { 65.75, 0.2, { 55 } },
                 { 66.5, 0.45, { 53 } },
                 { 67, 0.4, { 55 } },
                 { 67.5, 0.45, { 50 } },
                 { 68, 0.45, { 48 } },
                 { 68.5, 0.2, { 48 } },
                 { 69.25, 0.2, { 52 } },
                 { 69.75, 0.2, { 48 } },
                 { 70.5, 0.45, { 55 } },
                 { 71, 0.4, { 48 } },
                 { 71.5, 0.45, { 48 } },
                 { 72, 0.45, { 53 } },
                 { 72.5, 0.2, { 53 } },
                 { 73.25, 0.2, { 57 } },
                 { 73.75, 0.2, { 53 } },
                 { 74.5, 0.45, { 48 } },
                 { 75, 0.4, { 53 } },
                 { 75.5, 0.45, { 52 } },
                 { 76, 0.45, { 46 } },
                 { 76.5, 0.2, { 46 } },
                 { 77.25, 0.2, { 50 } },
                 { 77.75, 0.2, { 46 } },
                 { 78.5, 0.45, { 53 } },
                 { 79, 0.4, { 46 } },
                 { 79.5, 0.45, { 46 } },
                 { 80, 0.45, { 55 } },
                 { 80.5, 0.2, { 55 } },
                 { 81.25, 0.2, { 58 } },
                 { 81.75, 0.2, { 55 } },
                 { 82.25, 0.6, { 53 } },
                 { 83.25, 0.2, { 55 } },
                 { 83.75, 0.2, { 46 } },
                 { 84, 0.45, { 45 } },
                 { 84.5, 0.2, { 45 } },
                 { 85.25, 0.2, { 48 } },
                 { 85.75, 0.2, { 45 } },
                 { 86.5, 0.45, { 52 } },
                 { 87, 0.4, { 45 } },
                 { 87.5, 0.45, { 48 } },
                 { 88, 0.45, { 50 } },
                 { 88.5, 0.2, { 50 } },
                 { 89.25, 0.2, { 53 } },
                 { 89.75, 0.2, { 50 } },
                 { 90.5, 0.45, { 48 } },
                 { 91, 0.4, { 50 } },
                 { 91.5, 0.45, { 45 } },
                 { 92, 0.7, { 50 } },
                 { 93.5, 0.25, { 50 } },
                 { 94.5, 0.45, { 46 } },
                 { 95.5, 0.45, { 46 } },
                 { 96, 0.7, { 55 } },
                 { 97.5, 0.45, { 50 } },
                 { 99, 0.45, { 53 } },
                 { 100, 0.7, { 53 } },
                 { 101.5, 0.45, { 46 } },
                 { 103, 0.45, { 50 } },
                 { 104, 0.7, { 48 } },
                 { 105.5, 0.45, { 55 } },
                 { 107, 0.45, { 52 } },
                 { 108, 0.7, { 46 } },
                 { 109.5, 0.45, { 55 } },
                 { 111, 0.45, { 50 } },
                 { 112, 0.7, { 45 } },
                 { 113.5, 0.45, { 52 } },
                 { 115, 0.45, { 48 } },
                 { 116, 0.7, { 46 } },
                 { 117.5, 0.45, { 53 } },
                 { 119, 0.45, { 50 } },
                 { 120, 0.7, { 48 } },
                 { 121.5, 0.45, { 55 } },
                 { 122.5, 0.3, { 52 } },
                 { 123.25, 0.3, { 48 } },
                 { 123.75, 0.2, { 48 } },
                 { 124, 0.45, { 50 } },
                 { 125.25, 0.2, { 53 } },
                 { 125.75, 0.2, { 50 } },
                 { 127, 0.4, { 50 } },
                 { 127.5, 0.45, { 45 } },
                 { 128, 0.45, { 50 } },
                 { 128.5, 0.2, { 50 } },
                 { 129.75, 0.2, { 50 } },
                 { 130.5, 0.45, { 50 } },
                 { 131.5, 0.45, { 45 } },
                 { 132, 0.45, { 50 } },
                 { 133.25, 0.2, { 53 } },
                 { 133.75, 0.2, { 50 } },
                 { 134.5, 0.45, { 50 } },
                 { 135, 0.4, { 50 } },
                 { 135.5, 0.45, { 45 } },
                 { 136, 0.45, { 55 } },
                 { 136.5, 0.2, { 55 } },
                 { 137.25, 0.2, { 58 } },
                 { 137.75, 0.2, { 55 } },
                 { 138.25, 0.6, { 55 } },
                 { 139.25, 0.2, { 55 } },
                 { 139.75, 0.2, { 46 } },
                 { 140, 0.2, { 50 } },
                 { 140.25, 0.2, { 50 } },
                 { 140.75, 0.2, { 53 } },
                 { 141.25, 0.2, { 50 } },
                 { 141.75, 0.2, { 50 } },
                 { 142.5, 0.35, { 50 } },
                 { 143, 0.35, { 45 } },
                 { 143.5, 0.45, { 50 } },
                 { 144, 0.2, { 46 } },
                 { 144.25, 0.2, { 46 } },
                 { 144.75, 0.2, { 50 } },
                 { 145.25, 0.2, { 46 } },
                 { 145.75, 0.2, { 53 } },
                 { 146.5, 0.35, { 46 } },
                 { 147, 0.35, { 46 } },
                 { 147.5, 0.45, { 53 } },
                 { 148, 0.2, { 53 } },
                 { 148.25, 0.2, { 53 } },
                 { 148.75, 0.2, { 57 } },
                 { 149.25, 0.2, { 53 } },
                 { 149.75, 0.2, { 48 } },
                 { 150.5, 0.35, { 53 } },
                 { 151, 0.35, { 53 } },
                 { 151.5, 0.45, { 48 } },
                 { 152, 0.2, { 55 } },
                 { 152.25, 0.2, { 55 } },
                 { 152.75, 0.2, { 58 } },
                 { 153.25, 0.2, { 55 } },
                 { 153.75, 0.2, { 55 } },
                 { 154.5, 0.35, { 55 } },
                 { 155, 0.35, { 50 } },
                 { 155.5, 0.45, { 55 } },
                 { 156, 0.2, { 46 } },
                 { 156.25, 0.2, { 46 } },
                 { 156.75, 0.2, { 50 } },
                 { 157.25, 0.2, { 46 } },
                 { 157.75, 0.2, { 53 } },
                 { 158.5, 0.35, { 46 } },
                 { 159, 0.35, { 46 } },
                 { 159.5, 0.45, { 53 } },
                 { 160, 0.2, { 48 } },
                 { 160.25, 0.2, { 48 } },
                 { 160.75, 0.2, { 52 } },
                 { 161.25, 0.2, { 48 } },
                 { 161.75, 0.2, { 55 } },
                 { 162.5, 0.35, { 48 } },
                 { 163, 0.35, { 48 } },
                 { 163.5, 0.45, { 55 } },
                 { 164, 0.2, { 45 } },
                 { 164.25, 0.2, { 45 } },
                 { 164.75, 0.2, { 48 } },
                 { 165.25, 0.2, { 45 } },
                 { 165.75, 0.2, { 52 } },
                 { 166.5, 0.35, { 45 } },
                 { 167, 0.35, { 55 } },
                 { 167.5, 0.45, { 52 } },
                 { 168, 0.2, { 50 } },
                 { 168.25, 0.2, { 50 } },
                 { 168.75, 0.2, { 53 } },
                 { 169.25, 0.2, { 50 } },
                 { 169.75, 0.2, { 50 } },
                 { 170.5, 0.35, { 50 } },
                 { 171, 0.35, { 45 } },
                 { 171.5, 0.45, { 50 } },
                 { 172, 0.7, { 46 } },
                 { 173.5, 0.3, { 46 } },
                 { 174.5, 0.45, { 53 } },
                 { 175.5, 0.45, { 50 } },
                 { 176, 0.7, { 45 } },
                 { 177.5, 0.3, { 48 } },
                 { 178.5, 0.45, { 52 } },
                 { 179.5, 0.45, { 55 } },
                 { 180, 1.5, { 50 } },
               },
               {},
               {} },

        // Percussion.
        // Noise source at 1.00 with no oscillator at all, high-pass in position
        // 3 (the most aggressive in the bank) and cutoff 0.70: a band of hiss
        // with no pitch and no low end. Sustain 0.00 and release 0.00 with decay
        // 0.07 make each hit a tick that is gone in a tenth of a second, so six
        // hits a bar for thirty bars never accumulate.
        Part { "A67", "shaker", 1.00f, 9.0f,
               {
                 { 28, 0.15, { 84 } },
                 { 28.75, 0.15, { 74 } },
                 { 29.5, 0.15, { 79 } },
                 { 30, 0.15, { 84 } },
                 { 30.75, 0.15, { 74 } },
                 { 31.5, 0.15, { 79 } },
                 { 32, 0.15, { 84 } },
                 { 32.75, 0.15, { 74 } },
                 { 33.5, 0.15, { 79 } },
                 { 34, 0.15, { 84 } },
                 { 34.75, 0.15, { 74 } },
                 { 35.5, 0.15, { 79 } },
                 { 36, 0.15, { 84 } },
                 { 36.75, 0.15, { 74 } },
                 { 37.5, 0.15, { 79 } },
                 { 38, 0.15, { 84 } },
                 { 38.75, 0.15, { 74 } },
                 { 39.5, 0.15, { 79 } },
                 { 40, 0.15, { 84 } },
                 { 40.75, 0.15, { 74 } },
                 { 41.5, 0.15, { 79 } },
                 { 42, 0.15, { 84 } },
                 { 42.75, 0.15, { 74 } },
                 { 43.5, 0.15, { 79 } },
                 { 44, 0.15, { 84 } },
                 { 44.75, 0.15, { 74 } },
                 { 45.5, 0.15, { 79 } },
                 { 46, 0.15, { 84 } },
                 { 46.75, 0.15, { 74 } },
                 { 47.5, 0.15, { 79 } },
                 { 48, 0.15, { 84 } },
                 { 48.75, 0.15, { 74 } },
                 { 49.5, 0.15, { 79 } },
                 { 50, 0.15, { 84 } },
                 { 50.75, 0.15, { 74 } },
                 { 51.5, 0.15, { 79 } },
                 { 52, 0.15, { 84 } },
                 { 52.75, 0.15, { 74 } },
                 { 53.5, 0.15, { 79 } },
                 { 54, 0.15, { 84 } },
                 { 54.75, 0.15, { 74 } },
                 { 55.25, 0.15, { 74 } },
                 { 55.75, 0.15, { 79 } },
                 { 56, 0.15, { 84 } },
                 { 56.75, 0.15, { 74 } },
                 { 57.5, 0.15, { 79 } },
                 { 58, 0.15, { 84 } },
                 { 58.75, 0.15, { 74 } },
                 { 59.5, 0.15, { 79 } },
                 { 60, 0.15, { 84 } },
                 { 60.75, 0.15, { 74 } },
                 { 61.5, 0.15, { 79 } },
                 { 62, 0.15, { 84 } },
                 { 62.75, 0.15, { 74 } },
                 { 63.25, 0.15, { 74 } },
                 { 63.75, 0.15, { 79 } },
                 { 64, 0.15, { 84 } },
                 { 64.75, 0.15, { 74 } },
                 { 65.5, 0.15, { 79 } },
                 { 66, 0.15, { 84 } },
                 { 66.75, 0.15, { 74 } },
                 { 67.5, 0.15, { 79 } },
                 { 68, 0.15, { 84 } },
                 { 68.75, 0.15, { 74 } },
                 { 69.5, 0.15, { 79 } },
                 { 70, 0.15, { 84 } },
                 { 70.75, 0.15, { 74 } },
                 { 71.25, 0.15, { 74 } },
                 { 71.75, 0.15, { 79 } },
                 { 72, 0.15, { 84 } },
                 { 72.75, 0.15, { 74 } },
                 { 73.5, 0.15, { 79 } },
                 { 74, 0.15, { 84 } },
                 { 74.75, 0.15, { 74 } },
                 { 75.5, 0.15, { 79 } },
                 { 76, 0.15, { 84 } },
                 { 76.75, 0.15, { 74 } },
                 { 77.5, 0.15, { 79 } },
                 { 78, 0.15, { 84 } },
                 { 78.75, 0.15, { 74 } },
                 { 79.25, 0.15, { 74 } },
                 { 79.75, 0.15, { 79 } },
                 { 80, 0.15, { 84 } },
                 { 80.75, 0.15, { 74 } },
                 { 81.5, 0.15, { 79 } },
                 { 82, 0.15, { 84 } },
                 { 82.75, 0.15, { 74 } },
                 { 83.5, 0.15, { 79 } },
                 { 84, 0.15, { 84 } },
                 { 84.75, 0.15, { 74 } },
                 { 85.5, 0.15, { 79 } },
                 { 86, 0.15, { 84 } },
                 { 86.75, 0.15, { 74 } },
                 { 87.25, 0.15, { 74 } },
                 { 87.75, 0.15, { 79 } },
                 { 88, 0.15, { 84 } },
                 { 88.75, 0.15, { 74 } },
                 { 89.5, 0.15, { 79 } },
                 { 90, 0.15, { 84 } },
                 { 90.75, 0.15, { 74 } },
                 { 91.5, 0.15, { 79 } },
                 { 92, 0.15, { 84 } },
                 { 92.75, 0.15, { 74 } },
                 { 93.5, 0.15, { 79 } },
                 { 94, 0.15, { 84 } },
                 { 94.75, 0.15, { 74 } },
                 { 95.25, 0.15, { 74 } },
                 { 95.75, 0.15, { 79 } },
                 { 96, 0.15, { 79 } },
                 { 97, 0.15, { 74 } },
                 { 98, 0.15, { 84 } },
                 { 99, 0.15, { 74 } },
                 { 100, 0.15, { 79 } },
                 { 101, 0.15, { 74 } },
                 { 102, 0.15, { 84 } },
                 { 103, 0.15, { 74 } },
                 { 104, 0.15, { 79 } },
                 { 105, 0.15, { 74 } },
                 { 106, 0.15, { 84 } },
                 { 107, 0.15, { 74 } },
                 { 108, 0.15, { 79 } },
                 { 109, 0.15, { 74 } },
                 { 110, 0.15, { 84 } },
                 { 111, 0.15, { 74 } },
                 { 112, 0.15, { 79 } },
                 { 113, 0.15, { 74 } },
                 { 114, 0.15, { 84 } },
                 { 115, 0.15, { 74 } },
                 { 116, 0.15, { 79 } },
                 { 117, 0.15, { 74 } },
                 { 118, 0.15, { 84 } },
                 { 119, 0.15, { 74 } },
                 { 120, 0.15, { 79 } },
                 { 121, 0.15, { 74 } },
                 { 122, 0.15, { 84 } },
                 { 123, 0.15, { 74 } },
                 { 123.5, 0.15, { 74 } },
                 { 124, 0.15, { 79 } },
                 { 126, 0.15, { 74 } },
                 { 128, 0.15, { 79 } },
                 { 130, 0.15, { 74 } },
                 { 132, 0.15, { 79 } },
                 { 134, 0.15, { 74 } },
                 { 135.5, 0.15, { 74 } },
                 { 136, 0.15, { 84 } },
                 { 136.5, 0.15, { 74 } },
                 { 137, 0.15, { 79 } },
                 { 137.5, 0.15, { 74 } },
                 { 138, 0.15, { 84 } },
                 { 138.5, 0.15, { 74 } },
                 { 139, 0.15, { 79 } },
                 { 139.25, 0.15, { 74 } },
                 { 139.75, 0.15, { 74 } },
                 { 140, 0.15, { 84 } },
                 { 140.5, 0.15, { 74 } },
                 { 141, 0.15, { 79 } },
                 { 141.5, 0.15, { 74 } },
                 { 142, 0.15, { 84 } },
                 { 142.5, 0.15, { 74 } },
                 { 143, 0.15, { 79 } },
                 { 143.5, 0.15, { 74 } },
                 { 144, 0.15, { 84 } },
                 { 144.5, 0.15, { 74 } },
                 { 145, 0.15, { 79 } },
                 { 145.5, 0.15, { 74 } },
                 { 146, 0.15, { 84 } },
                 { 146.5, 0.15, { 74 } },
                 { 147, 0.15, { 79 } },
                 { 147.25, 0.15, { 74 } },
                 { 147.75, 0.15, { 74 } },
                 { 148, 0.15, { 84 } },
                 { 148.5, 0.15, { 74 } },
                 { 149, 0.15, { 79 } },
                 { 149.5, 0.15, { 74 } },
                 { 150, 0.15, { 84 } },
                 { 150.5, 0.15, { 74 } },
                 { 151, 0.15, { 79 } },
                 { 151.5, 0.15, { 74 } },
                 { 152, 0.15, { 84 } },
                 { 152.5, 0.15, { 74 } },
                 { 153, 0.15, { 79 } },
                 { 153.5, 0.15, { 74 } },
                 { 154, 0.15, { 84 } },
                 { 154.5, 0.15, { 74 } },
                 { 155, 0.15, { 79 } },
                 { 155.25, 0.15, { 74 } },
                 { 155.75, 0.15, { 74 } },
                 { 156, 0.15, { 84 } },
                 { 156.5, 0.15, { 74 } },
                 { 157, 0.15, { 79 } },
                 { 157.5, 0.15, { 74 } },
                 { 158, 0.15, { 84 } },
                 { 158.5, 0.15, { 74 } },
                 { 159, 0.15, { 79 } },
                 { 159.5, 0.15, { 74 } },
                 { 160, 0.15, { 84 } },
                 { 160.5, 0.15, { 74 } },
                 { 161, 0.15, { 79 } },
                 { 161.5, 0.15, { 74 } },
                 { 162, 0.15, { 84 } },
                 { 162.5, 0.15, { 74 } },
                 { 163, 0.15, { 79 } },
                 { 163.25, 0.15, { 74 } },
                 { 163.75, 0.15, { 74 } },
                 { 164, 0.15, { 84 } },
                 { 164.5, 0.15, { 74 } },
                 { 165, 0.15, { 79 } },
                 { 165.5, 0.15, { 74 } },
                 { 166, 0.15, { 84 } },
                 { 166.5, 0.15, { 74 } },
                 { 167, 0.15, { 79 } },
                 { 167.5, 0.15, { 74 } },
                 { 168, 0.15, { 84 } },
                 { 168.5, 0.15, { 74 } },
                 { 169, 0.15, { 79 } },
                 { 169.5, 0.15, { 74 } },
                 { 170, 0.15, { 84 } },
                 { 170.5, 0.15, { 74 } },
                 { 171, 0.15, { 79 } },
                 { 171.25, 0.15, { 74 } },
                 { 171.75, 0.15, { 74 } },
                 { 172, 0.15, { 79 } },
                 { 173, 0.15, { 74 } },
                 { 174, 0.15, { 84 } },
                 { 175, 0.15, { 74 } },
                 { 176, 0.15, { 79 } },
               },
               {},
               {} },

        // Backbeat, ghost notes and five different fills that mark the form.
        // Unison key mode makes it monophonic, which is exactly right for a
        // snare: two hits can never ring together, and every note in this part
        // is written with its gate closed before the next opens. Noise 0.72
        // layered with sub 0.50 gives both the crack and the body; cutoff 0.74
        // keeps the crack bright.
        Part { "A64", "snare", 0.67f, 6.0f,
               {
                 { 41, 0.2, { 57 } },
                 { 43, 0.2, { 57 } },
                 { 45, 0.2, { 57 } },
                 { 46.75, 0.14, { 69 } },
                 { 47, 0.2, { 57 } },
                 { 49, 0.2, { 53 } },
                 { 51, 0.2, { 53 } },
                 { 52, 0.18, { 57 } },
                 { 52.5, 0.14, { 69 } },
                 { 53, 0.18, { 57 } },
                 { 53.75, 0.18, { 57 } },
                 { 54.25, 0.18, { 52 } },
                 { 54.75, 0.18, { 52 } },
                 { 55.25, 0.18, { 57 } },
                 { 55.75, 0.14, { 69 } },
                 { 57, 0.2, { 57 } },
                 { 59, 0.2, { 57 } },
                 { 61, 0.2, { 57 } },
                 { 62.75, 0.14, { 69 } },
                 { 63, 0.2, { 57 } },
                 { 65, 0.2, { 55 } },
                 { 67, 0.2, { 55 } },
                 { 69, 0.2, { 55 } },
                 { 71, 0.2, { 55 } },
                 { 71.75, 0.18, { 55 } },
                 { 73, 0.2, { 53 } },
                 { 75, 0.2, { 53 } },
                 { 77, 0.2, { 53 } },
                 { 78.75, 0.14, { 65 } },
                 { 79, 0.2, { 53 } },
                 { 81, 0.2, { 55 } },
                 { 83, 0.2, { 55 } },
                 { 85, 0.2, { 57 } },
                 { 87, 0.2, { 57 } },
                 { 87.75, 0.18, { 57 } },
                 { 89, 0.2, { 57 } },
                 { 91, 0.2, { 57 } },
                 { 92, 0.18, { 53 } },
                 { 92.25, 0.18, { 53 } },
                 { 93, 0.18, { 53 } },
                 { 93.5, 0.18, { 65 } },
                 { 94, 0.18, { 53 } },
                 { 94.5, 0.18, { 53 } },
                 { 95, 0.14, { 65 } },
                 { 95.25, 0.14, { 65 } },
                 { 95.5, 0.18, { 53 } },
                 { 95.75, 0.18, { 53 } },
                 { 98, 0.25, { 55 } },
                 { 102, 0.25, { 53 } },
                 { 106, 0.25, { 55 } },
                 { 110, 0.25, { 55 } },
                 { 114, 0.25, { 57 } },
                 { 118, 0.25, { 53 } },
                 { 122, 0.2, { 55 } },
                 { 123, 0.18, { 48 } },
                 { 123.5, 0.18, { 48 } },
                 { 123.75, 0.18, { 55 } },
                 { 136, 0.2, { 55 } },
                 { 137, 0.2, { 55 } },
                 { 138, 0.18, { 55 } },
                 { 138.25, 0.18, { 55 } },
                 { 138.5, 0.18, { 62 } },
                 { 138.75, 0.18, { 55 } },
                 { 139, 0.18, { 50 } },
                 { 139.25, 0.18, { 50 } },
                 { 139.5, 0.18, { 55 } },
                 { 139.75, 0.18, { 55 } },
                 { 141, 0.2, { 57 } },
                 { 143, 0.2, { 57 } },
                 { 145, 0.2, { 53 } },
                 { 146.75, 0.14, { 65 } },
                 { 147, 0.2, { 53 } },
                 { 149, 0.2, { 53 } },
                 { 151, 0.2, { 53 } },
                 { 153, 0.2, { 55 } },
                 { 155, 0.2, { 55 } },
                 { 155.75, 0.18, { 55 } },
                 { 157, 0.2, { 53 } },
                 { 159, 0.2, { 53 } },
                 { 161, 0.2, { 55 } },
                 { 162.75, 0.14, { 67 } },
                 { 163, 0.2, { 55 } },
                 { 165, 0.2, { 57 } },
                 { 167, 0.2, { 57 } },
                 { 167.75, 0.18, { 57 } },
                 { 168, 0.18, { 57 } },
                 { 168.5, 0.18, { 62 } },
                 { 169, 0.18, { 57 } },
                 { 169.5, 0.18, { 50 } },
                 { 170, 0.18, { 57 } },
                 { 170.25, 0.18, { 57 } },
                 { 170.75, 0.14, { 69 } },
                 { 171, 0.18, { 62 } },
                 { 171.5, 0.18, { 50 } },
                 { 171.75, 0.18, { 57 } },
                 { 173, 0.2, { 53 } },
                 { 175, 0.2, { 53 } },
                 { 176, 0.18, { 57 } },
                 { 176.5, 0.18, { 57 } },
                 { 177, 0.18, { 57 } },
                 { 177.5, 0.18, { 57 } },
                 { 178, 0.18, { 64 } },
                 { 178.5, 0.18, { 64 } },
                 { 179, 0.18, { 52 } },
                 { 179.5, 0.18, { 52 } },
                 { 180, 0.3, { 57 } },
               },
               {},
               {} },

        // Syncopated chord stabs.
        // Cutoff 0.04 is essentially closed and envelope depth 0.64 is among the
        // deepest in the bank; with attack 0.00 that pairing means the filter
        // slams open on every note-on and shuts again over decay 0.24 to a
        // sustain of 0.31. It is not a chord, it is a wah - the sound exists
        // only at the instant of the stab, which is why four of them a bar do
        // not congest the mid-range the way a held chord would.
        Part { "A54", "stabs", 0.80f, 0.0f,
               {
                 { 41, 0.2, { 62, 65, 69, 72 } },
                 { 42, 0.2, { 62, 65, 69, 72 } },
                 { 42.25, 0.2, { 62, 65, 69, 72 } },
                 { 43.75, 0.2, { 62, 65, 69, 72 } },
                 { 45, 0.2, { 62, 65, 69, 72 } },
                 { 46, 0.2, { 62, 65, 69, 72 } },
                 { 46.75, 0.2, { 62, 65, 69, 72 } },
                 { 47.5, 0.2, { 62, 65, 69, 72 } },
                 { 48.25, 0.2, { 62, 65, 69, 74 } },
                 { 48.75, 0.2, { 62, 65, 69, 74 } },
                 { 50.75, 0.2, { 62, 65, 69, 74 } },
                 { 51.25, 0.2, { 62, 65, 69, 74 } },
                 { 52.75, 0.2, { 64, 67, 69, 72 } },
                 { 53.5, 0.2, { 64, 67, 69, 72 } },
                 { 55.25, 0.2, { 64, 67, 69, 72 } },
                 { 57, 0.2, { 62, 65, 69, 72 } },
                 { 58, 0.2, { 62, 65, 69, 72 } },
                 { 58.25, 0.2, { 62, 65, 69, 72 } },
                 { 59.75, 0.2, { 62, 65, 69, 72 } },
                 { 60.25, 0.2, { 62, 65, 69, 72 } },
                 { 60.75, 0.2, { 62, 65, 69, 72 } },
                 { 62.5, 0.2, { 62, 65, 69, 72 } },
                 { 63, 0.2, { 62, 65, 69, 72 } },
                 { 65, 0.2, { 62, 65, 67, 70 } },
                 { 66, 0.2, { 62, 65, 67, 70 } },
                 { 66.75, 0.2, { 62, 65, 67, 70 } },
                 { 67.25, 0.2, { 62, 65, 67, 70 } },
                 { 68.25, 0.2, { 64, 67, 72, 76 } },
                 { 68.75, 0.2, { 64, 67, 72, 76 } },
                 { 70.75, 0.2, { 64, 67, 72, 76 } },
                 { 71.25, 0.2, { 64, 67, 72, 76 } },
                 { 73, 0.2, { 65, 69, 72 } },
                 { 74, 0.2, { 65, 69, 72 } },
                 { 74.25, 0.2, { 65, 69, 72 } },
                 { 75.75, 0.2, { 65, 69, 72 } },
                 { 76.75, 0.2, { 62, 65, 69, 74 } },
                 { 77.5, 0.2, { 62, 65, 69, 74 } },
                 { 79.25, 0.2, { 62, 65, 69, 74 } },
                 { 81, 0.2, { 62, 65, 67, 70 } },
                 { 82, 0.2, { 62, 65, 67, 70 } },
                 { 82.75, 0.2, { 62, 65, 67, 70 } },
                 { 83.5, 0.2, { 62, 65, 67, 70 } },
                 { 85, 0.2, { 64, 67, 69, 72 } },
                 { 86, 0.2, { 64, 67, 69, 72 } },
                 { 86.75, 0.2, { 64, 67, 69, 72 } },
                 { 87.25, 0.2, { 64, 67, 69, 72 } },
                 { 112, 0.9, { 64, 67, 69, 72 } },
                 { 114, 1.4, { 64, 67, 69, 72 } },
                 { 116, 0.9, { 62, 65, 70, 74 } },
                 { 118, 1.4, { 62, 65, 70, 74 } },
                 { 120, 0.9, { 64, 67, 72, 76 } },
                 { 121.5, 0.45, { 64, 67, 72, 76 } },
                 { 122.5, 0.45, { 64, 67, 72, 76 } },
                 { 123.5, 0.45, { 64, 67, 72, 76 } },
                 { 141, 0.2, { 62, 65, 69, 74 } },
                 { 142, 0.2, { 62, 65, 69, 74 } },
                 { 142.75, 0.2, { 62, 65, 69, 74 } },
                 { 143.75, 0.2, { 62, 65, 69, 74 } },
                 { 144.5, 0.2, { 62, 65, 70, 74 } },
                 { 145.5, 0.2, { 62, 65, 70, 74 } },
                 { 146.25, 0.2, { 62, 65, 70, 74 } },
                 { 147.25, 0.2, { 62, 65, 70, 74 } },
                 { 149, 0.2, { 65, 69, 72 } },
                 { 150, 0.2, { 65, 69, 72 } },
                 { 150.75, 0.2, { 65, 69, 72 } },
                 { 151.75, 0.2, { 65, 69, 72 } },
                 { 153.5, 0.2, { 58, 62, 67, 70 } },
                 { 154, 0.2, { 58, 62, 67, 70 } },
                 { 155.25, 0.2, { 58, 62, 67, 70 } },
                 { 156.5, 0.2, { 62, 65, 70, 74 } },
                 { 157.5, 0.2, { 62, 65, 70, 74 } },
                 { 158.25, 0.2, { 62, 65, 70, 74 } },
                 { 159.25, 0.2, { 62, 65, 70, 74 } },
                 { 161, 0.2, { 64, 67, 72, 76 } },
                 { 162, 0.2, { 64, 67, 72, 76 } },
                 { 162.75, 0.2, { 64, 67, 72, 76 } },
                 { 163.75, 0.2, { 64, 67, 72, 76 } },
                 { 165.5, 0.2, { 64, 67, 69, 72 } },
                 { 166, 0.2, { 64, 67, 69, 72 } },
                 { 167.25, 0.2, { 64, 67, 69, 72 } },
                 { 168.5, 0.2, { 62, 65, 69, 74 } },
                 { 169.5, 0.2, { 62, 65, 69, 74 } },
                 { 170.25, 0.2, { 62, 65, 69, 74 } },
                 { 171.25, 0.2, { 62, 65, 69, 74 } },
                 { 172, 1.9, { 62, 65, 70, 74 } },
                 { 174, 1.9, { 62, 65, 70, 74 } },
                 { 176, 1.9, { 67, 69, 72 } },
                 { 178, 1.9, { 67, 69, 72 } },
                 { 180, 3.5, { 62, 65, 69, 77 } },
               },
               {},
               {} },

        // Sixteenth-note counter-riff, the bridge melody, and the four-bar solo .
        // Attack 0.00 with decay 0.54, sustain 0.00 and release 0.17 is a
        // genuine struck-keyboard envelope: every note blooms and dies, so a
        // sixteenth-note line is legible at 114 BPM and the part can never
        // smear. Its filter is the opposite of a wah - envelope depth is only
        // 0.06, so the cutoff sits still at 0.39 with resonance 0.54 giving a
        // fixed nasal formant peak.
        Part { "A28", "electric piano", 0.72f, 0.0f,
               {
                 { 32.25, 0.2, { 79 } },
                 { 32.75, 0.2, { 82 } },
                 { 33.5, 0.2, { 82 } },
                 { 34.75, 0.2, { 79 } },
                 { 35.25, 0.2, { 79 } },
                 { 36.25, 0.2, { 79 } },
                 { 36.75, 0.2, { 84 } },
                 { 37.5, 0.2, { 84 } },
                 { 38.5, 0.2, { 79 } },
                 { 39, 0.2, { 76 } },
                 { 40.25, 0.2, { 81 } },
                 { 40.75, 0.2, { 86 } },
                 { 41.5, 0.2, { 84 } },
                 { 42.75, 0.2, { 81 } },
                 { 43.25, 0.2, { 77 } },
                 { 44.25, 0.2, { 81 } },
                 { 44.75, 0.2, { 86 } },
                 { 45.5, 0.2, { 84 } },
                 { 46.5, 0.2, { 81 } },
                 { 47, 0.2, { 77 } },
                 { 49, 0.2, { 77 } },
                 { 49.5, 0.2, { 82 } },
                 { 50, 0.2, { 81 } },
                 { 50.25, 0.2, { 77 } },
                 { 51.75, 0.2, { 74 } },
                 { 52.25, 0.2, { 76 } },
                 { 53, 0.2, { 81 } },
                 { 54, 0.2, { 79 } },
                 { 54.25, 0.2, { 76 } },
                 { 54.75, 0.2, { 72 } },
                 { 55.75, 0.2, { 81 } },
                 { 56.25, 0.2, { 81 } },
                 { 56.75, 0.2, { 86 } },
                 { 57.5, 0.2, { 84 } },
                 { 58.75, 0.2, { 81 } },
                 { 59.25, 0.2, { 77 } },
                 { 61, 0.2, { 81 } },
                 { 61.5, 0.2, { 86 } },
                 { 62, 0.2, { 84 } },
                 { 62.75, 0.2, { 81 } },
                 { 63.5, 0.2, { 77 } },
                 { 64.25, 0.2, { 79 } },
                 { 64.75, 0.2, { 82 } },
                 { 65.5, 0.2, { 82 } },
                 { 66.25, 0.2, { 79 } },
                 { 67.75, 0.2, { 77 } },
                 { 69, 0.2, { 79 } },
                 { 69.5, 0.2, { 84 } },
                 { 70, 0.2, { 79 } },
                 { 70.25, 0.2, { 79 } },
                 { 71.75, 0.2, { 76 } },
                 { 72.25, 0.2, { 84 } },
                 { 72.75, 0.2, { 77 } },
                 { 73.5, 0.2, { 77 } },
                 { 74.75, 0.2, { 81 } },
                 { 75.25, 0.2, { 84 } },
                 { 76.25, 0.2, { 82 } },
                 { 77, 0.2, { 77 } },
                 { 78, 0.2, { 77 } },
                 { 78.25, 0.2, { 81 } },
                 { 78.75, 0.2, { 82 } },
                 { 79.75, 0.2, { 86 } },
                 { 80.25, 0.2, { 82 } },
                 { 80.75, 0.2, { 77 } },
                 { 81.5, 0.2, { 79 } },
                 { 82.5, 0.2, { 82 } },
                 { 83, 0.2, { 82 } },
                 { 84.25, 0.2, { 84 } },
                 { 84.75, 0.2, { 76 } },
                 { 85.5, 0.2, { 79 } },
                 { 86.25, 0.2, { 81 } },
                 { 87.75, 0.2, { 84 } },
                 { 88.25, 0.2, { 81 } },
                 { 90.75, 0.2, { 77 } },
                 { 92.25, 0.2, { 81 } },
                 { 94.75, 0.2, { 77 } },
                 { 96, 1.4, { 82 } },
                 { 97.5, 0.9, { 82 } },
                 { 98.5, 0.9, { 86 } },
                 { 99.5, 0.45, { 89 } },
                 { 100, 0.9, { 89 } },
                 { 101, 0.4, { 93 } },
                 { 101.5, 1.4, { 89 } },
                 { 103, 0.9, { 89 } },
                 { 104, 1.4, { 88 } },
                 { 105.5, 0.9, { 88 } },
                 { 106.5, 0.9, { 91 } },
                 { 107.5, 0.45, { 96 } },
                 { 108, 0.9, { 94 } },
                 { 109, 1.4, { 94 } },
                 { 110.5, 0.9, { 94 } },
                 { 111.5, 0.45, { 91 } },
                 { 112.5, 0.2, { 81 } },
                 { 113.5, 0.2, { 79 } },
                 { 114.5, 0.2, { 76 } },
                 { 115.5, 0.2, { 79 } },
                 { 116.5, 0.2, { 82 } },
                 { 117.5, 0.2, { 82 } },
                 { 118.5, 0.2, { 77 } },
                 { 119.5, 0.2, { 82 } },
                 { 120.5, 0.2, { 84 } },
                 { 121.5, 0.2, { 79 } },
                 { 122.5, 0.2, { 79 } },
                 { 123, 0.2, { 76 } },
                 { 123.5, 0.2, { 79 } },
                 { 124, 0.5, { 81 } },
                 { 124.75, 0.5, { 86 } },
                 { 125.5, 0.5, { 86 } },
                 { 126.25, 0.5, { 81 } },
                 { 127, 0.7, { 77 } },
                 { 128.5, 0.5, { 86 } },
                 { 129.25, 0.5, { 89 } },
                 { 130, 0.5, { 89 } },
                 { 130.75, 0.5, { 86 } },
                 { 131.5, 0.4, { 86 } },
                 { 132, 0.2, { 86 } },
                 { 132.25, 0.2, { 89 } },
                 { 132.5, 0.2, { 89 } },
                 { 132.75, 0.2, { 86 } },
                 { 133, 0.6, { 81 } },
                 { 134, 0.2, { 89 } },
                 { 134.25, 0.2, { 93 } },
                 { 134.5, 0.2, { 89 } },
                 { 134.75, 0.2, { 89 } },
                 { 135, 0.7, { 86 } },
                 { 136, 0.2, { 91 } },
                 { 136.25, 0.2, { 94 } },
                 { 136.5, 0.2, { 94 } },
                 { 136.75, 0.2, { 91 } },
                 { 137, 0.4, { 91 } },
                 { 137.5, 0.2, { 94 } },
                 { 137.75, 0.2, { 98 } },
                 { 138, 0.2, { 94 } },
                 { 138.25, 0.2, { 94 } },
                 { 138.5, 0.4, { 91 } },
                 { 140.5, 0.2, { 93 } },
                 { 141.5, 0.2, { 98 } },
                 { 142.25, 0.2, { 93 } },
                 { 143.25, 0.2, { 89 } },
                 { 145, 0.2, { 94 } },
                 { 146, 0.2, { 89 } },
                 { 146.75, 0.2, { 94 } },
                 { 147.75, 0.2, { 86 } },
                 { 148.5, 0.2, { 93 } },
                 { 149.5, 0.2, { 96 } },
                 { 150.25, 0.2, { 89 } },
                 { 151.25, 0.2, { 93 } },
                 { 152.5, 0.2, { 91 } },
                 { 153, 0.2, { 94 } },
                 { 154.25, 0.2, { 91 } },
                 { 154.75, 0.2, { 91 } },
                 { 155.75, 0.2, { 86 } },
                 { 157, 0.2, { 94 } },
                 { 158, 0.2, { 89 } },
                 { 158.75, 0.2, { 94 } },
                 { 159.75, 0.2, { 86 } },
                 { 160.5, 0.2, { 91 } },
                 { 161.5, 0.2, { 96 } },
                 { 162.25, 0.2, { 88 } },
                 { 163.25, 0.2, { 91 } },
                 { 164.5, 0.2, { 93 } },
                 { 165, 0.2, { 96 } },
                 { 166.25, 0.2, { 91 } },
                 { 166.75, 0.2, { 88 } },
                 { 167.75, 0.2, { 84 } },
                 { 169, 0.2, { 93 } },
                 { 170, 0.2, { 98 } },
                 { 170.75, 0.2, { 93 } },
                 { 171.75, 0.2, { 89 } },
               },
               {},
               {} },

        // Punches.
        // Attack 0.02 is nearly instant but not zero, which is the difference
        // between a stab and a brass punch: there is a few-millisecond lip on
        // the front of every note. Cutoff 0.28 with envelope depth 0.46 and key
        // follow 0.68 gives the classic Juno brass swell - the filter opens with
        // the amplitude and tracks the register, so the higher punches at bars
        // 39-41 are brighter than the ones at 36, which is how the part gains
        // force at the climax with no velocity and no trim change.
        Part { "A11", "brass", 1.00f, 0.0f,
               {
                 { 88, 0.7, { 74, 81 } },
                 { 89.25, 0.2, { 74, 81 } },
                 { 89.75, 0.2, { 74, 81 } },
                 { 90.5, 1.4, { 77, 81 } },
                 { 92, 0.7, { 74, 81 } },
                 { 93.25, 0.2, { 74, 81 } },
                 { 93.75, 0.2, { 74, 81 } },
                 { 94.5, 1.4, { 77, 82, 86 } },
                 { 139.5, 0.45, { 79, 82, 86 } },
                 { 140, 0.7, { 74, 81 } },
                 { 141.25, 0.2, { 74, 81 } },
                 { 141.75, 0.2, { 74, 81 } },
                 { 142.5, 1.4, { 77, 81 } },
                 { 144, 0.7, { 82, 86 } },
                 { 145.25, 0.2, { 82, 86 } },
                 { 145.75, 0.2, { 82, 86 } },
                 { 146.5, 1.4, { 77, 82 } },
                 { 148, 0.7, { 77, 84 } },
                 { 149.25, 0.2, { 77, 84 } },
                 { 149.75, 0.2, { 77, 84 } },
                 { 150.5, 1.4, { 81, 84 } },
                 { 152, 0.7, { 79, 86 } },
                 { 153.25, 0.2, { 79, 86 } },
                 { 153.75, 0.2, { 79, 86 } },
                 { 154.5, 1.4, { 82, 86 } },
                 { 156, 0.7, { 82, 86 } },
                 { 157.25, 0.2, { 82, 86 } },
                 { 157.75, 0.2, { 82, 86 } },
                 { 158.5, 1.4, { 77, 82, 86 } },
                 { 160, 0.7, { 79, 84 } },
                 { 161.25, 0.2, { 79, 84 } },
                 { 161.75, 0.2, { 79, 84 } },
                 { 162.5, 1.4, { 76, 79, 84 } },
                 { 164, 1.4, { 76, 84 } },
                 { 165.5, 0.4, { 76, 84 } },
                 { 166, 1.9, { 76, 79, 84 } },
                 { 168, 1.8, { 74, 77, 81 } },
                 { 170, 1.9, { 74, 77, 81 } },
                 { 172, 0.9, { 82, 86 } },
                 { 173.5, 1.4, { 77, 82, 86 } },
                 { 175.5, 0.45, { 82, 86 } },
                 { 176, 1.8, { 79, 81, 84 } },
                 { 178, 1.9, { 79, 81, 84 } },
                 { 180, 2.4, { 74, 77, 81 } },
               },
               {},
               { { 0, 0.0000f }, { 139, 0.0000f }, { 140, 0.1200f }, { 179, 0.1200f }, { 180, 0.0000f } } },

        // The wide sustaining bed.
        // This is the sound most people buy a Juno-106 for and it was missing
        // from the earlier draft entirely. Sawtooth AND pulse together with PWM
        // at 0.43 driven by the LFO gives a genuine ensemble shimmer - unlike
        // the brass patch, this one really does have a pulse wave for the PWM to
        // act on.
        Part { "B11", "strings", 0.62f, 0.0f,
               {
                 { 8, 8, { 65, 69, 74 } },
                 { 16, 4, { 62, 65, 70 } },
                 { 20, 4, { 67, 72, 76 } },
                 { 48, 4, { 62, 65, 69 } },
                 { 52, 4, { 64, 67, 72 } },
                 { 56, 8, { 62, 65, 69 } },
                 { 64, 4, { 62, 65, 70 } },
                 { 68, 4, { 64, 67, 72 } },
                 { 72, 4, { 65, 69, 72 } },
                 { 76, 4, { 65, 69, 74 } },
                 { 80, 4, { 62, 65, 70 } },
                 { 84, 4, { 64, 67, 72 } },
                 { 88, 4, { 62, 65, 69 } },
                 { 92, 4, { 62, 65, 70 } },
                 { 96, 4, { 62, 67, 70 } },
                 { 100, 4, { 65, 69, 70 } },
                 { 104, 4, { 64, 67, 72 } },
                 { 108, 4, { 62, 67, 70 } },
                 { 112, 4, { 64, 69, 72 } },
                 { 116, 4, { 62, 65, 70 } },
                 { 120, 4, { 64, 67, 72 } },
                 { 140, 4, { 69, 74, 77 } },
                 { 144, 4, { 70, 74, 77 } },
                 { 148, 4, { 69, 72, 77 } },
                 { 152, 4, { 70, 74, 79 } },
                 { 156, 4, { 70, 74, 77 } },
                 { 160, 4, { 72, 76, 79 } },
                 { 164, 4, { 72, 76, 79 } },
                 { 168, 4, { 69, 74, 77 } },
                 { 172, 4, { 70, 74, 77 } },
                 { 176, 4, { 69, 72, 76 } },
                 { 180, 8, { 62, 69, 77 } },
               },
               {},
               { { 0, 0.0000f }, { 112, 0.0000f }, { 123, 0.3000f }, { 124, 0.0000f } } },

        // The other sustaining colour.
        // Sustain is 1.00, the only part in the arrangement that holds at full
        // level indefinitely, so the opening Dm can hold for sixteen beats
        // before revoicing to Bb. Attack 0.54 makes it
        // swell in rather than start, so bar 1 fades up out of silence with no
        // fade automation.
        Part { "A17", "choir", 0.51f, 0.0f,
               {
                 { 0, 16, { 62, 65, 69 } },
                 { 16, 4, { 62, 65, 70 } },
                 { 20, 4, { 64, 67, 72 } },
                 { 72, 4, { 69, 72, 76 } },
                 { 76, 4, { 69, 74, 77 } },
                 { 80, 4, { 70, 74, 79 } },
                 { 84, 4, { 64, 69, 72 } },
                 { 88, 4, { 65, 69, 72 } },
                 { 92, 4, { 65, 70, 74 } },
                 { 96, 4, { 67, 70, 74 } },
                 { 100, 4, { 65, 70, 77 } },
                 { 104, 4, { 67, 72, 76 } },
                 { 108, 4, { 67, 70, 74 } },
                 { 112, 4, { 67, 72, 76 } },
                 { 116, 4, { 65, 70, 74 } },
                 { 120, 4, { 67, 72, 76 } },
                 { 164, 4, { 69, 72, 76 } },
                 { 172, 4, { 65, 70, 74 } },
                 { 176, 4, { 67, 69, 72 } },
                 { 180, 8, { 65, 69, 74 } },
               },
               {},
               {} },

        // High sparkle.
        // Release 0.58 is among the longest in the bank and sustain is 0.37, so
        // notes hang and bleed into each other. This is the only part in the
        // arrangement allowed to blur, and it blurs on purpose - which is also
        // why it is the part that rings out at the end after everything else has
        // stopped.
        Part { "B13", "chorus vibes", 0.39f, 0.0f,
               {
                 { 16, 1, { 86 } },
                 { 17, 1, { 82 } },
                 { 18, 1, { 77 } },
                 { 19, 1, { 74 } },
                 { 20, 2, { 79 } },
                 { 22, 2, { 76 } },
                 { 48, 0.9, { 82 } },
                 { 49.5, 0.9, { 86 } },
                 { 51, 1, { 89 } },
                 { 52, 0.9, { 88 } },
                 { 53.5, 0.9, { 84 } },
                 { 55, 1, { 81 } },
                 { 72, 1.5, { 84 } },
                 { 74, 1.5, { 89 } },
                 { 76, 1.5, { 86 } },
                 { 78, 1.5, { 82 } },
                 { 96, 1, { 86 } },
                 { 97, 1, { 91 } },
                 { 98.5, 1.5, { 89 } },
                 { 100, 1, { 89 } },
                 { 101.5, 1.5, { 86 } },
                 { 103, 1, { 89 } },
                 { 104, 1, { 91 } },
                 { 105, 1, { 88 } },
                 { 106.5, 1.5, { 91 } },
                 { 108, 2, { 89 } },
                 { 110, 2, { 91 } },
                 { 112, 1.5, { 88 } },
                 { 114, 1.5, { 84 } },
                 { 116, 1.5, { 86 } },
                 { 118, 1.5, { 82 } },
                 { 120, 1, { 88 } },
                 { 121.5, 1.5, { 91 } },
                 { 123, 1, { 91 } },
                 { 144, 1, { 86 } },
                 { 146, 1, { 82 } },
                 { 152, 1, { 91 } },
                 { 154, 1, { 86 } },
                 { 156, 1, { 89 } },
                 { 160, 1, { 91 } },
                 { 162, 1, { 88 } },
                 { 164, 1.5, { 93 } },
                 { 166, 1.5, { 84 } },
                 { 168, 2, { 89 } },
                 { 170, 2, { 86 } },
                 { 172, 2, { 82 } },
                 { 174, 2, { 86 } },
                 { 180, 1, { 86 } },
                 { 184, 4, { 77 } },
                 { 184.5, 3.5, { 81 } },
                 { 185, 3, { 86 } },
                 { 185.5, 2.5, { 89 } },
               },
               {},
               {} },

        // The melody.
        // Chorus is OFF, so this is the only sustaining part that is near-mono
        // and dead centre, which is precisely what a melody needs when five
        // other parts are spread wide around it. Sawtooth only, cutoff 0.47 with
        // resonance 0.24 and a shallow envelope depth of 0.14: a stable singing
        // tone that does not change character note to note, so a melody reads as
        // a melody rather than as a series of effects.
        Part { "A53", "lead", 0.81f, 0.0f,
               {
                 { 55, 0.9, { 69 } },
                 { 56, 1.4, { 74 } },
                 { 57.5, 0.4, { 72 } },
                 { 58, 0.9, { 69 } },
                 { 59, 0.9, { 69 } },
                 { 60, 1.9, { 69 } },
                 { 62, 0.4, { 72 } },
                 { 62.5, 1.4, { 74 } },
                 { 64, 0.9, { 77 } },
                 { 65, 0.4, { 74 } },
                 { 65.5, 1.4, { 70 } },
                 { 67, 0.9, { 70 } },
                 { 68, 0.9, { 72 } },
                 { 69, 1.4, { 76 } },
                 { 70.5, 0.4, { 72 } },
                 { 71, 0.9, { 72 } },
                 { 72, 0.9, { 69 } },
                 { 73, 1.9, { 77 } },
                 { 75, 0.9, { 76 } },
                 { 76, 1.4, { 74 } },
                 { 77.5, 0.4, { 70 } },
                 { 78, 1.9, { 70 } },
                 { 80, 0.9, { 67 } },
                 { 81, 0.4, { 70 } },
                 { 81.5, 1.4, { 74 } },
                 { 83, 0.9, { 70 } },
                 { 84, 1.9, { 69 } },
                 { 86, 0.9, { 67 } },
                 { 87, 0.9, { 64 } },
                 { 140, 1.4, { 86 } },
                 { 141.5, 1.4, { 89 } },
                 { 143, 0.9, { 89 } },
                 { 144, 1.9, { 89 } },
                 { 146, 0.9, { 86 } },
                 { 147, 0.9, { 82 } },
                 { 148, 1.4, { 84 } },
                 { 149.5, 0.4, { 81 } },
                 { 150, 1.9, { 84 } },
                 { 152, 0.9, { 82 } },
                 { 153, 1.9, { 86 } },
                 { 155, 0.9, { 82 } },
                 { 156, 1.4, { 86 } },
                 { 157.5, 1.4, { 89 } },
                 { 159, 0.9, { 89 } },
                 { 160, 1.9, { 88 } },
                 { 162, 0.9, { 91 } },
                 { 163, 0.9, { 88 } },
                 { 164, 1.4, { 84 } },
                 { 165.5, 0.4, { 88 } },
                 { 166, 1.4, { 91 } },
                 { 167.5, 0.45, { 88 } },
                 { 168, 1.9, { 86 } },
                 { 170, 0.9, { 81 } },
                 { 171, 0.9, { 86 } },
                 { 172, 0.9, { 82 } },
                 { 173, 0.4, { 77 } },
                 { 173.5, 1.4, { 77 } },
                 { 175, 0.9, { 74 } },
                 { 176, 0.9, { 76 } },
                 { 177, 0.4, { 72 } },
                 { 177.5, 1.4, { 69 } },
                 { 179, 0.9, { 72 } },
                 { 180, 3.9, { 74 } },
               },
               {},
               { { 0, 0.0000f }, { 140, 0.0000f }, { 160, 0.4500f }, { 171, 0.4500f }, { 176, 0.0000f } } },
    };
    return piece;
}

// ---------------------------------------------------------------------------
// Rendering one part
// ---------------------------------------------------------------------------

// Every stored tone field and every performance control beside it, without gain
// correction or rebalancing, exactly as the factory-preset audit reads them.
// Only the output volume is overridden, and only to balance the overdubs.
EngineParameters parametersFor (const Preset& preset, float volume)
{
    const auto& patch = preset.patch;
    const auto& controls = preset.controls;
    EngineParameters parameters;
    youknow::ProductFidelityProfile::applyTo (parameters);

    parameters.lfoRate = patch.lfoRate;
    parameters.lfoDelay = patch.lfoDelay;
    parameters.dcoLfoDepth = patch.dcoLfo;
    parameters.pwmDepth = patch.pwm;
    parameters.noiseLevel = patch.noise;
    parameters.cutoff = patch.cutoff;
    parameters.resonance = patch.resonance;
    parameters.envDepth = patch.vcfEnv;
    parameters.vcfLfoDepth = patch.vcfLfo;
    parameters.keyFollow = patch.keyFollow;
    parameters.vcaLevel = patch.vcaLevel;
    parameters.attack = patch.attack;
    parameters.decay = patch.decay;
    parameters.sustain = patch.sustain;
    parameters.release = patch.release;
    parameters.subLevel = patch.sub;
    parameters.range = patch.range;
    parameters.sawEnabled = patch.saw;
    parameters.pulseEnabled = patch.pulse;
    parameters.pwmSource = patch.pwmSource;
    parameters.vcaMode = patch.vcaMode;
    parameters.envPolarity = patch.envPolarity;
    parameters.highPass = patch.highPass;
    parameters.chorus = patch.chorus;

    parameters.benderDcoDepth = controls.benderDco;
    parameters.benderVcfDepth = controls.benderVcf;
    parameters.benderLfoDepth = controls.benderLfo;
    parameters.portamento = controls.portamento;
    parameters.keyMode = controls.keyMode;
    parameters.keyTranspose = controls.transpose;
    parameters.masterTuneCents = controls.masterTune;
    parameters.velocityDepth = controls.velocity;
    parameters.calibration = controls.calibration;
    parameters.chorusNoise = controls.chorusNoise;
    parameters.polyphony = controls.polyphony;

    // The one departure from the stored preset: its output level, which is what
    // balances one overdub against the next.
    parameters.volume = volume;
    return parameters;
}

// A note-on or note-off resolved to an absolute sample position.
struct Gate
{
    std::int64_t frame;
    int note;
    bool on;
};

// Linear interpolation across a lane's breakpoints. An event scheduled late
// in a take must not affect the music before its first breakpoint.
float laneValue (const std::vector<Breakpoint>& lane, double beat)
{
    if (lane.empty())
        return 0.0f;
    if (beat < lane.front().beat)
        return 0.0f;
    if (beat == lane.front().beat)
        return lane.front().value;
    if (beat >= lane.back().beat)
        return lane.back().value;

    for (std::size_t index = 1; index < lane.size(); ++index)
    {
        const auto& previous = lane[index - 1];
        const auto& next = lane[index];
        if (beat > next.beat)
            continue;
        const auto span = next.beat - previous.beat;
        if (span <= 0.0)
            return next.value;
        const auto position = (beat - previous.beat) / span;
        return static_cast<float> (previous.value
                                   + position * (next.value - previous.value));
    }
    return lane.back().value;
}

struct Rendered
{
    std::vector<float> left;
    std::vector<float> right;
};

// Renders one part over `totalFrames`, dispatching its gates at their exact
// sample positions and stepping its bender and modulation lanes once per block.
Rendered renderPart (const Part& part, const Preset& preset, double tempo,
                     std::int64_t totalFrames, std::int64_t leadFrames)
{
    const double framesPerBeat = compositionSampleRate * 60.0 / tempo;
    const auto frameOf = [framesPerBeat] (double beat)
    { return static_cast<std::int64_t> (std::llround (beat * framesPerBeat)); };

    std::vector<Gate> gates;
    for (const auto& event : part.events)
    {
        const auto on = frameOf (event.beat);
        // Every gate is lifted a few milliseconds early. A repeated note whose
        // release landed on the same sample as its own next attack would give
        // the scanned assigner no pass in which to see the key up, and it would
        // read the pair as one held key rather than as two notes. The gap is
        // shorter than the shortest note any score here writes and is how a
        // player's hand leaves a key anyway.
        const auto off = std::max (on + 1, frameOf (event.beat + event.lengthBeats)
                                               - releaseGuardFrames);
        for (const int note : event.notes)
        {
            gates.push_back (Gate { on, note, true });
            gates.push_back (Gate { off, note, false });
        }
    }
    // Releases before attacks at the same frame, so a note repeated exactly on
    // the boundary is retriggered rather than left holding.
    std::stable_sort (gates.begin(), gates.end(),
                      [] (const Gate& a, const Gate& b)
                      {
                          if (a.frame != b.frame)
                              return a.frame < b.frame;
                          return static_cast<int> (a.on) < static_cast<int> (b.on);
                      });

    auto engine = std::make_unique<YouKnowEngine>();
    youknow::ProductFidelityProfile::configureBeforePrepare (*engine);
    engine->selectConverterTimingProfile (
        YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
    engine->prepare (compositionSampleRate, renderBlockSize,
                     compositionOversampleFactor);
    engine->setParameters (parametersFor (preset, part.volume));

    Rendered rendered;
    rendered.left.reserve (static_cast<std::size_t> (totalFrames));
    rendered.right.reserve (static_cast<std::size_t> (totalFrames));

    std::array<float, renderBlockSize> blockLeft {};
    std::array<float, renderBlockSize> blockRight {};

    // Run the instrument before the take starts, and throw the result away.
    //
    // Every engine here is constructed and reset identically, so every part's
    // noise generators would otherwise start from the same fixed seeds and
    // produce the SAME hiss. Ten identical hiss streams sum coherently, at
    // twice the amplitude of ten independent ones, which is not what ten
    // overdubs of this instrument sound like. A distinct lead per part
    // decorrelates the avalanche source, the bucket-brigade lines and the
    // chorus sweep phase together, deterministically, without the engine
    // needing a seed control it does not otherwise have - and it is what
    // actually happens when a player records ten takes one after another.
    for (std::int64_t lead = 0; lead < leadFrames;)
    {
        const auto count = std::min (static_cast<std::int64_t> (renderBlockSize),
                                     leadFrames - lead);
        engine->process (blockLeft.data(), blockRight.data(), static_cast<int> (count));
        lead += count;
    }

    std::size_t nextGate = 0;
    std::int64_t frame = 0;

    while (frame < totalFrames)
    {
        while (nextGate < gates.size() && gates[nextGate].frame <= frame)
        {
            const auto& gate = gates[nextGate];
            if (gate.on)
                engine->noteOn (gate.note, 1.0f);
            else
                engine->noteOff (gate.note);
            ++nextGate;
        }

        // The inner loop above has consumed every gate at or before `frame`, so
        // the next one is strictly ahead and this is always at least one frame.
        auto count = std::min (static_cast<std::int64_t> (renderBlockSize),
                               totalFrames - frame);
        if (nextGate < gates.size())
            count = std::min (count, gates[nextGate].frame - frame);

        const double beat = static_cast<double> (frame) / framesPerBeat;
        if (! part.bend.empty())
            engine->setPitchBend (laneValue (part.bend, beat));
        if (! part.wheel.empty())
            engine->setModWheel (laneValue (part.wheel, beat));

        engine->process (blockLeft.data(), blockRight.data(),
                         static_cast<int> (count));
        rendered.left.insert (rendered.left.end(), blockLeft.begin(),
                              blockLeft.begin() + count);
        rendered.right.insert (rendered.right.end(), blockRight.begin(),
                               blockRight.begin() + count);
        frame += count;
    }
    return rendered;
}

// How much longer each successive part runs before its take begins. An
// irrational-looking figure rather than a round one, so the chorus sweeps of
// different parts do not land back in phase with each other.
constexpr double partLeadSeconds = 0.37;

// The highest number of notes a part ever has sounding at once. The assigner
// drops a key it cannot place rather than stealing a voice, so a part written
// past its polyphony loses notes silently; this is what catches that.
int peakSimultaneousNotes (const Part& part)
{
    std::vector<std::pair<double, int>> edges;
    for (const auto& event : part.events)
    {
        edges.emplace_back (event.beat, static_cast<int> (event.notes.size()));
        edges.emplace_back (event.beat + event.lengthBeats,
                            -static_cast<int> (event.notes.size()));
    }
    // Releases before attacks at the same beat, matching the gate ordering.
    std::sort (edges.begin(), edges.end(),
               [] (const auto& a, const auto& b)
               {
                   if (a.first != b.first)
                       return a.first < b.first;
                   return a.second < b.second;
               });

    int sounding = 0;
    int peak = 0;
    for (const auto& [beat, delta] : edges)
    {
        (void) beat;
        sounding += delta;
        peak = std::max (peak, sounding);
    }
    return peak;
}

double lastBeat (const Part& part)
{
    double last = 0.0;
    for (const auto& event : part.events)
        last = std::max (last, event.beat + event.lengthBeats);
    return last;
}

bool validateHarmony (const Composition& piece, bool report = true)
{
    const auto reject = [report] (const char* reason, const char* slot, double beat)
    {
        if (report)
            std::fprintf (stderr, "harmony: %s at beat %.3f: %s\n", slot, beat, reason);
        return false;
    };
    if (piece.beatsPerBar != 4.0 || piece.parts.empty())
        return reject ("missing four-beat score", "score", 0);
    for (const auto& harmony : harmonyByBar)
        if (harmony.pitchClasses == 0
            || (harmony.pitchClasses & ~dNaturalMinor) != 0)
            return reject ("chord leaves D natural minor", harmony.name, 0);

    for (const auto& part : piece.parts)
    {
        const auto* preset = youknow::presets::findByNumber (part.slot);
        if (preset == nullptr || part.events.empty())
            return reject ("missing preset or notes", part.slot, 0);
        if (! std::isfinite (part.mixGainDb) || ! std::isfinite (part.volume)
            || part.volume < 0.0f || part.volume > 1.0f)
            return reject ("invalid part level", part.slot, 0);
        const auto parameters = parametersFor (*preset, part.volume);
        // Noise-only percussion has no DCO fundamental. The snare's SUB is
        // pitched, so it takes the same checks as the melodic instruments.
        const bool pitched = parameters.sawEnabled || parameters.pulseEnabled
                          || parameters.subLevel > 0.0f || parameters.resonance >= 0.95f;
        if (pitched && (parameters.masterTuneCents != 0.0f
                        || parameters.keyTranspose % 12 != 0
                        || parameters.portamento != 0.0f))
            return reject ("performance controls change score tuning", part.slot, 0);
        for (const auto& point : part.bend)
            if (pitched && point.value != 0.0f)
                return reject ("this arrangement requires centered pitch bend",
                               part.slot, point.beat);
        for (const auto& event : part.events)
        {
            const double end = event.beat + event.lengthBeats;
            if (! std::isfinite (event.beat) || ! std::isfinite (end)
                || event.beat < 0 || event.lengthBeats <= 0
                || end > 4.0 * harmonyByBar.size() || event.notes.empty())
                return reject ("invalid event span", part.slot, event.beat);
            for (const int note : event.notes)
            {
                if (note < 0 || note > 127)
                    return reject ("note outside MIDI range", part.slot, event.beat);
                if (! pitched)
                    continue;
                const auto pitchClass = pc (note);
                // Check the entire key-down duration, not only its attack.
                // A held A from the opening Dm must not survive into Bb at
                // beat 16; the choir is explicitly revoiced there instead.
                for (auto bar = static_cast<std::size_t> (event.beat / 4.0);
                     bar < harmonyByBar.size() && 4.0 * bar < end; ++bar)
                    if ((harmonyByBar[bar].pitchClasses & pitchClass) == 0)
                        return reject ("held note conflicts with shared chord",
                                       part.slot, std::max (event.beat, 4.0 * bar));
            }
        }
    }
    return true;
}

bool checkHarmonyRegression (const Composition& piece)
{
    if (! validateHarmony (piece))
        return false;
    // The original two-semitone lead bug: a late negative breakpoint must
    // never set the pitch of the melody's first entrance at beat 55.
    const std::vector<Breakpoint> lateBend { { 165.9, -0.5572f }, { 166.2, 0.0f } };
    if (laneValue (lateBend, 55.0) != 0.0f
        || laneValue (lateBend, 165.9) != -0.5572f
        || laneValue (lateBend, 166.2) != 0.0f)
        return false;

    const auto choir = std::find_if (piece.parts.begin(), piece.parts.end(),
        [] (const Part& part) { return std::strcmp (part.slot, "A17") == 0; });
    if (choir == piece.parts.end())
        return false;
    const auto index = static_cast<std::size_t> (choir - piece.parts.begin());
    auto invalid = piece;
    invalid.parts[index].events.front().notes.front() = 63; // chromatic Eb
    if (validateHarmony (invalid, false))
        return false;
    invalid = piece;
    invalid.parts[index].events.front().notes.front() = 67; // G is in scale, not Dm
    if (validateHarmony (invalid, false))
        return false;
    invalid = piece;
    invalid.parts[index].events.front().lengthBeats = 20; // A conflicts at beat 16
    if (validateHarmony (invalid, false))
        return false;
    invalid = piece;
    invalid.parts.back().bend = lateBend;
    return ! validateHarmony (invalid, false);
}

// ---------------------------------------------------------------------------
// The mix
// ---------------------------------------------------------------------------

struct PartLevel
{
    std::string slot;
    std::string name;
    std::string role;
    double peakDb = 0.0;
    int peakNotes = 0;
    double mixGainDb = 0.0;
};

double mixGain (const Part& part)
{
    return std::pow (10.0, static_cast<double> (part.mixGainDb) / 20.0);
}

void addPartToMix (const Rendered& part, double gain,
                   std::vector<float>& left, std::vector<float>& right)
{
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        left[index] += static_cast<float> (part.left[index] * gain);
        right[index] += static_cast<float> (part.right[index] * gain);
    }
}

double peakOf (const std::vector<float>& left, const std::vector<float>& right)
{
    double result = 0.0;
    for (std::size_t index = 0; index < left.size(); ++index)
        result = std::max ({ result, std::abs (static_cast<double> (left[index])),
                             std::abs (static_cast<double> (right[index])) });
    return result;
}

double absoluteDcMean (const std::vector<float>& left,
                       const std::vector<float>& right)
{
    if (left.empty())
        return 0.0;
    double leftSum = 0.0;
    double rightSum = 0.0;
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        leftSum += static_cast<double> (left[index]);
        rightSum += static_cast<double> (right[index]);
    }
    const auto frames = static_cast<double> (left.size());
    return std::max (std::abs (leftSum / frames), std::abs (rightSum / frames));
}

// ---------------------------------------------------------------------------
// The documented table
// ---------------------------------------------------------------------------

// The part table in the instrument's README is regenerated on every render, so
// what the document says the piece is made of cannot drift away from what was
// rendered. The markers bound exactly what this tool owns; the prose around
// them stays hand-written. They are also this tool's proof that the output
// directory is its own before it overwrites anything in it.
constexpr const char* partsTableBegin =
    "<!-- composition-table-begin: regenerated by YouKnowRenderComposition;"
    " edits between the markers are overwritten -->";
constexpr const char* partsTableEnd = "<!-- composition-table-end -->";

constexpr const char* compositionFileName = "youknow-composition.wav";

// Only the instrument's own Docs/audio directory has a README one level
// up carrying the markers; an ad-hoc output directory has none, and resolving
// one is how this tool tells the difference.
std::filesystem::path instrumentReadme (const std::filesystem::path& directory)
{
    auto normalised = directory.lexically_normal();
    if (normalised.filename().empty())
        normalised = normalised.parent_path();

    if (normalised.filename() != "audio"
        || normalised.parent_path().filename() != "Docs")
        return {};

    return normalised.parent_path().parent_path() / "README.md";
}

std::string formatSignedDb (double value)
{
    char digits[32];
    std::snprintf (digits, sizeof digits, "%.1f", std::fabs (value));
    const bool negative = value < 0.0 && std::strcmp (digits, "0.0") != 0;
    // A real minus sign rather than a hyphen, so the table reads as typeset prose.
    return std::string (negative ? "\xE2\x88\x92" : "+") + digits;
}

bool updatePartsTable (const std::filesystem::path& directory,
                       const Composition& piece, const std::vector<PartLevel>& levels,
                       double seconds, double mixPeakDb, double normalisationDb)
{
    const auto readmePath = instrumentReadme (directory);
    if (readmePath.empty() || ! std::filesystem::exists (readmePath))
        return true; // An ad-hoc output directory carries no documentation.

    std::ifstream input (readmePath, std::ios::binary);
    std::string readme ((std::istreambuf_iterator<char> (input)),
                        std::istreambuf_iterator<char>());
    input.close();

    const auto beginPos = readme.find (partsTableBegin);
    const auto endPos = readme.find (partsTableEnd);
    if (beginPos == std::string::npos || endPos == std::string::npos
        || endPos < beginPos)
    {
        std::fprintf (stderr,
                      "%s has no composition-table markers, so the part list can "
                      "no longer be kept in sync with the rendered piece.\n",
                      readmePath.string().c_str());
        return false;
    }

    char header[512];
    std::snprintf (header, sizeof header,
                   "\n*%s* — %s, %.0f bpm, %.1f s, %d-bit/%.1f kHz, %dx oversampled. "
                   "Rendered mix peak %s dBFS, normalised %s dB.\n\n",
                   piece.title, piece.key, piece.tempo, seconds,
                   compositionBitsPerSample, compositionSampleRate / 1000.0,
                   compositionOversampleFactor, formatSignedDb (mixPeakDb).c_str(),
                   formatSignedDb (normalisationDb).c_str());

    std::string table (header);
    table += "| Part | Factory preset | Role | Max notes | Overdub gain | Rendered peak |\n"
             "| ---: | --- | --- | ---: | ---: | ---: |\n";
    int index = 1;
    for (const auto& level : levels)
    {
        char row[512];
        std::snprintf (row, sizeof row, "| %d | `%s` %s | %s | %d | %s dB | %s dBFS |\n",
                       index++, level.slot.c_str(), level.name.c_str(),
                       level.role.c_str(), level.peakNotes,
                       formatSignedDb (level.mixGainDb).c_str(),
                       formatSignedDb (level.peakDb).c_str());
        table += row;
    }

    const auto contentStart = beginPos + std::strlen (partsTableBegin);
    const auto updated =
        readme.substr (0, contentStart) + table + readme.substr (endPos);
    if (updated == readme)
        return true;

    std::ofstream output (readmePath, std::ios::binary | std::ios::trunc);
    output << updated;
    output.close();
    std::printf ("Updated the composition table in %s\n", readmePath.string().c_str());
    return ! output.fail();
}

// A short render used by the regression suite: it proves the tool, the score
// and the engine still produce finite, audible audio and a readable WAV without
// committing anything.
int runSmokeTest (const std::filesystem::path& directory)
{
    const auto piece = composition();
    if (! checkHarmonyRegression (piece))
    {
        std::fprintf (stderr, "smoke test: harmony or late-bend regression failed\n");
        return 1;
    }
    if (piece.parts.empty())
    {
        std::fprintf (stderr, "smoke test: the composition has no parts\n");
        return 1;
    }

    // Every part must name a real factory slot and stay inside its polyphony,
    // which is a property of the score rather than of the render, so the whole
    // table is checked even though only one part is rendered.
    for (const auto& part : piece.parts)
    {
        const auto* preset = youknow::presets::findByNumber (part.slot);
        if (preset == nullptr)
        {
            std::fprintf (stderr, "smoke test: no factory preset %s\n", part.slot);
            return 1;
        }
        const int limit = preset->controls.keyMode == KeyMode::Unison
                              ? 1
                              : preset->controls.polyphony;
        const int peak = peakSimultaneousNotes (part);
        if (peak > limit)
        {
            std::fprintf (stderr,
                          "smoke test: part %s (%s) needs %d simultaneous notes "
                          "but the preset plays %d\n",
                          part.slot, preset->name, peak, limit);
            return 1;
        }
    }

    // The part that enters first, not the one written first. A score whose
    // opening part is a bass that waits seven bars would otherwise render half
    // a second of correct silence and be reported as broken.
    const auto& part = *std::min_element (
        piece.parts.begin(), piece.parts.end(),
        [] (const Part& a, const Part& b)
        {
            const auto firstBeat = [] (const Part& candidate)
            {
                double earliest = std::numeric_limits<double>::max();
                for (const auto& event : candidate.events)
                    earliest = std::min (earliest, event.beat);
                return earliest;
            };
            return firstBeat (a) < firstBeat (b);
        });
    const auto* preset = youknow::presets::findByNumber (part.slot);
    double firstBeat = std::numeric_limits<double>::max();
    for (const auto& event : part.events)
        firstBeat = std::min (firstBeat, event.beat);
    const auto frames = static_cast<std::int64_t> (std::llround (
        (firstBeat * 60.0 / piece.tempo + 0.5) * compositionSampleRate));
    const auto rendered = renderPart (part, *preset, piece.tempo, frames, 0);

    for (std::size_t index = 0; index < rendered.left.size(); ++index)
        if (! std::isfinite (rendered.left[index])
            || ! std::isfinite (rendered.right[index]))
        {
            std::fprintf (stderr, "smoke test: rendered a non-finite sample\n");
            return 1;
        }

    const auto peak = peakOf (rendered.left, rendered.right);
    if (peak < 1.0e-4)
    {
        std::fprintf (stderr, "smoke test: rendered silence (peak %.6f)\n", peak);
        return 1;
    }

    // Check the actual mix path with synthesized drum hits and a synthesized
    // musical bed. Subtract the identical bed to measure each drum's stereo
    // contribution; a common final normalization preserves this ratio.
    for (const auto& source : piece.parts)
    {
        if (std::strcmp (source.slot, "A67") != 0
            && std::strcmp (source.slot, "A64") != 0)
            continue;
        auto drum = source;
        drum.events = { Event { 0, 0.25, source.events.front().notes } };
        const auto* drumPreset = youknow::presets::findByNumber (drum.slot);
        const auto hit = renderPart (drum, *drumPreset, piece.tempo, frames, 0);
        auto unityLeft = rendered.left, unityRight = rendered.right;
        auto raisedLeft = rendered.left, raisedRight = rendered.right;
        addPartToMix (hit, 1.0, unityLeft, unityRight);
        addPartToMix (hit, mixGain (drum), raisedLeft, raisedRight);
        double unityEnergy = 0.0, raisedEnergy = 0.0;
        for (std::size_t i = 0; i < rendered.left.size(); ++i)
        {
            const double ul = static_cast<double> (unityLeft[i]) - rendered.left[i];
            const double ur = static_cast<double> (unityRight[i]) - rendered.right[i];
            const double rl = static_cast<double> (raisedLeft[i]) - rendered.left[i];
            const double rr = static_cast<double> (raisedRight[i]) - rendered.right[i];
            unityEnergy += ul * ul + ur * ur;
            raisedEnergy += rl * rl + rr * rr;
        }
        const double changeDb = 10.0 * std::log10 (raisedEnergy / unityEnergy);
        if (unityEnergy < 1.0e-9 || ! std::isfinite (changeDb)
            || std::abs (changeDb - source.mixGainDb) > 0.001)
        {
            std::fprintf (stderr, "smoke test: %s drum balance did not reach %+.1f dB\n",
                          source.slot, source.mixGainDb);
            return 1;
        }
        std::printf ("Measured %s drum contribution %+.3f dB against the same musical bed\n",
                     source.slot, changeDb);
    }

    std::error_code error;
    std::filesystem::create_directories (directory, error);
    const auto path = directory / "composition-smoke.wav";
    if (! writeWav (path, rendered.left, rendered.right))
    {
        std::fprintf (stderr, "smoke test: could not write %s\n",
                      path.string().c_str());
        return 1;
    }
    const auto size = std::filesystem::file_size (path, error);
    std::filesystem::remove (path, error);
    if (size < 44u + rendered.left.size() * 6u)
    {
        std::fprintf (stderr, "smoke test: short WAV (%llu bytes)\n",
                      static_cast<unsigned long long> (size));
        return 1;
    }

    std::printf ("YouKnow composition renderer smoke test passed "
                 "(%zu parts, peak %.3f).\n", piece.parts.size(), peak);
    return 0;
}
} // namespace

int main (int argc, char** argv)
{
    std::vector<std::string> arguments (argv + 1, argv + argc);
    bool smoke = false;
    std::filesystem::path directory = "Docs/audio";

    for (const auto& argument : arguments)
    {
        if (argument == "--smoke")
        {
            smoke = true;
        }
        else if (argument == "--help" || argument == "-h")
        {
            std::printf ("usage: YouKnowRenderComposition [--smoke] "
                         "[output-directory]\n");
            return 0;
        }
        else
        {
            directory = argument;
        }
    }

    if (smoke)
        return runSmokeTest (directory);

    std::error_code error;
    std::filesystem::create_directories (directory, error);
    if (! std::filesystem::is_directory (directory))
    {
        std::fprintf (stderr, "not a directory: %s\n", directory.string().c_str());
        return 1;
    }

    const auto piece = composition();
    if (! validateHarmony (piece))
        return 1;
    if (piece.parts.empty())
    {
        std::fprintf (stderr, "the composition has no parts\n");
        return 1;
    }

    double lastRelease = 0.0;
    for (const auto& part : piece.parts)
        lastRelease = std::max (lastRelease, lastBeat (part));
    const double totalBeats = lastRelease + piece.tailBeats;
    const auto totalFrames = static_cast<std::int64_t> (
        std::llround (totalBeats * compositionSampleRate * 60.0 / piece.tempo));

    // Everything that can refuse the piece is checked before anything is
    // rendered, so a mistyped score fails in a second rather than after the
    // parts that happened to precede it have been computed.
    std::vector<const Preset*> presets (piece.parts.size(), nullptr);
    std::vector<int> peakNotes (piece.parts.size(), 0);
    for (std::size_t index = 0; index < piece.parts.size(); ++index)
    {
        const auto& part = piece.parts[index];
        const auto* preset = youknow::presets::findByNumber (part.slot);
        if (preset == nullptr)
        {
            std::fprintf (stderr, "no factory preset %s\n", part.slot);
            return 1;
        }

        // The assigner drops a key it cannot place rather than stealing a
        // voice, so a part written past its polyphony would lose notes without
        // any other symptom. Refusing to render is how that stays visible.
        const int limit = preset->controls.keyMode == KeyMode::Unison
                              ? 1
                              : preset->controls.polyphony;
        const int peak = peakSimultaneousNotes (part);
        if (peak > limit)
        {
            std::fprintf (stderr,
                          "part %s (%s) needs %d simultaneous notes but the "
                          "preset plays %d\n",
                          part.slot, preset->name, peak, limit);
            return 1;
        }
        presets[index] = preset;
        peakNotes[index] = peak;
    }

    std::vector<float> mixLeft (static_cast<std::size_t> (totalFrames), 0.0f);
    std::vector<float> mixRight (static_cast<std::size_t> (totalFrames), 0.0f);
    std::vector<PartLevel> levels;

    // The parts are rendered concurrently. Each is its own engine instance
    // holding no state in common with any other - the two tables the engine
    // builds lazily are function-local statics, whose initialisation the
    // language already serialises - so this changes only how long the render
    // takes, not what it produces. Rendering the whole piece serially at the
    // deepest oversampling rung takes about half an hour, which is a long time
    // to hold a build for a file that is the same either way.
    //
    // Parts are rendered in batches rather than all at once because each one
    // holds a full-length stereo buffer; the cap bounds that at a few hundred
    // megabytes however many cores the machine turns out to have. They are
    // summed in score order after each batch, so the mix does not depend on
    // which part finished first.
    const auto lanes = std::max<std::size_t> (
        1, std::min<std::size_t> ({ piece.parts.size(),
                                    std::thread::hardware_concurrency() != 0u
                                        ? std::thread::hardware_concurrency()
                                        : 1u,
                                    8u }));

    for (std::size_t base = 0; base < piece.parts.size(); base += lanes)
    {
        const auto count = std::min (lanes, piece.parts.size() - base);
        std::vector<Rendered> batch (count);
        std::vector<std::thread> workers;
        workers.reserve (count);
        for (std::size_t lane = 0; lane < count; ++lane)
            workers.emplace_back (
                [&piece, &presets, &batch, base, lane, totalFrames]
                {
                    batch[lane] = renderPart (
                        piece.parts[base + lane], *presets[base + lane],
                        piece.tempo, totalFrames,
                        static_cast<std::int64_t> (
                            std::llround (static_cast<double> (base + lane)
                                          * partLeadSeconds * compositionSampleRate)));
                });
        for (auto& worker : workers)
            worker.join();

        for (std::size_t lane = 0; lane < count; ++lane)
        {
            const auto& part = piece.parts[base + lane];
            const auto& rendered = batch[lane];
            const auto partGain = mixGain (part);
            addPartToMix (rendered, partGain, mixLeft, mixRight);

            const auto partPeak = peakOf (rendered.left, rendered.right) * partGain;
            PartLevel level;
            level.slot = part.slot;
            level.name = presets[base + lane]->name;
            level.role = part.role;
            level.peakDb = 20.0 * std::log10 (std::max (partPeak, 1.0e-9));
            level.peakNotes = peakNotes[base + lane];
            level.mixGainDb = part.mixGainDb;
            levels.push_back (level);

            std::printf ("Rendered %-4s %-26s %-20s peak %6.3f, %d notes, mix %+.1f dB\n",
                         part.slot, presets[base + lane]->name, part.role,
                         partPeak, peakNotes[base + lane], part.mixGainDb);
            std::fflush (stdout);
        }
    }

    for (std::size_t index = 0; index < mixLeft.size(); ++index)
        if (! std::isfinite (mixLeft[index]) || ! std::isfinite (mixRight[index]))
        {
            std::fprintf (stderr, "the mix has a non-finite sample\n");
            return 1;
        }

    const auto mixPeak = peakOf (mixLeft, mixRight);
    if (mixPeak < 1.0e-4)
    {
        std::fprintf (stderr, "the mix rendered silence (peak %.6f)\n", mixPeak);
        return 1;
    }

    const auto gain = normalisedPeak / mixPeak;
    for (std::size_t index = 0; index < mixLeft.size(); ++index)
    {
        mixLeft[index] = static_cast<float> (mixLeft[index] * gain);
        mixRight[index] = static_cast<float> (mixRight[index] * gain);
    }

    const auto dc = absoluteDcMean (mixLeft, mixRight);
    if (dc > maximumAbsoluteDcMean)
    {
        std::fprintf (stderr,
                      "the mix is rejected: normalised absolute DC mean %.6f FS "
                      "exceeds %.6f FS\n", dc, maximumAbsoluteDcMean);
        return 1;
    }
    const auto firstEdge = std::max (std::abs (static_cast<double> (mixLeft.front())),
                                     std::abs (static_cast<double> (mixRight.front())));
    const auto lastEdge = std::max (std::abs (static_cast<double> (mixLeft.back())),
                                    std::abs (static_cast<double> (mixRight.back())));
    if (firstEdge > maximumEdgeMagnitude || lastEdge > maximumEdgeMagnitude)
    {
        std::fprintf (stderr,
                      "the mix is rejected: normalised first/last edge %.6f/%.6f FS "
                      "exceeds %.6f FS\n", firstEdge, lastEdge, maximumEdgeMagnitude);
        return 1;
    }

    const auto path = directory / compositionFileName;
    if (! writeWav (path, mixLeft, mixRight))
    {
        std::fprintf (stderr, "could not write %s\n", path.string().c_str());
        return 1;
    }

    const auto seconds = static_cast<double> (totalFrames) / compositionSampleRate;
    if (! updatePartsTable (directory, piece, levels, seconds,
                            20.0 * std::log10 (mixPeak), 20.0 * std::log10 (gain)))
        return 1;

    std::printf ("Rendered \"%s\": %zu parts, %.1f s, mix peak %.3f, into %s\n",
                 piece.title, piece.parts.size(), seconds, mixPeak,
                 path.string().c_str());
    return 0;
}
