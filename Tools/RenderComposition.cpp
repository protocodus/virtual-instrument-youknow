// Renders the committed composition under Docs/audio/composition from the same
// JUCE-free engine the plug-in runs.
//
// Where the numbered demos under Docs/audio each isolate one mechanism, this is
// a piece of music: ten factory presets, each rendered by its own engine and
// summed, exactly as ten overdubs onto tape. That is the only way a six-voice
// monotimbral instrument plays ten parts, and it is how records were made with
// this machine. No samples, no external processing and no effects beyond the
// instrument's own chorus: every sound here comes out of the circuit model.
//
// Two things separate this render from the numbered demos. It runs the deepest
// rung of the quality ladder rather than the default, because a one-off offline
// render has no realtime budget to respect. And it is written at 24 bits, so
// the summing floor of ten parts stays below the quantiser rather than beside
// it.

#include "DSP/YouKnowEngine.h"
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
// breakpoints are interpolated linearly, which is what a hand on a lever
// produces; before the first and after the last the lane holds.
struct Breakpoint
{
    double beat = 0.0;
    float value = 0.0f;
};

// One overdub: a factory preset, the balance it is printed at, and what it
// plays. `volume` replaces the preset's own stored VR1 position, which is the
// instrument's own output control - the balance between parts is set on the
// machine, not on a mixer this renderer would otherwise have to model.
struct Part
{
    const char* slot;
    const char* role;
    float volume;
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

// "Long Shadow": thirty-two bars in D aeolian at 76 bpm, about 101 seconds
// before the tails.
//
// The form is five sections. A prologue of two chords on one pad alone; the
// floor arriving at bar 5 with the low strings, the bass and the first chime;
// a dry middle from bar 13 where both chorus-II pads drop out and the plucked
// parts carry it; a climb from bar 21 into the climax at bar 26; and a
// resolution from bar 29. Every entrance is staggered so each preset is heard
// on its own before it becomes texture, and the melody does not play a note
// until bar 20 of 32.
//
// The parts do not share register. Six lanes were assigned before any note was
// written - bass alone below 60, low strings and cello in the tenor, the inner
// voice in a close triad above them, the wide pads and brass in the crowded
// middle, the shimmer pad and chimes at the ceiling - because ten parts summed
// with no panner separate by pitch and by chorus width or they do not separate
// at all. The two 16' parts are written in the middle of the keyboard and
// sound an octave below what is written here.
//
// Dynamics come from the arrangement rather than from key velocity, because
// the hardware ignores velocity and every factory patch stores zero velocity
// depth. What changes is which presets are sounding, in which register, at
// what density.
//
// Bender positions below are lever positions, not semitones. Every factory
// patch stores bender sensitivity 0.30 and full deflection at full sensitivity
// is 11.96484375 semitones, so full lever here is 3.589 semitones and a
// one-semitone scoop is a lever of 0.279.
Composition composition()
{
    Composition piece;
    piece.title = "Long Shadow";
    piece.key = "D minor";
    piece.tempo = 76.0;
    piece.beatsPerBar = 4.0;
    // Six beats past the last release at 76 bpm: the pad release is 0.39 and
    // the chime's 0.47, so the final chord needs about five seconds to leave.
    piece.tailBeats = 6.0;
    piece.parts = {
        // Principal wide pad - the harmonic bed and the first sound in the piece.
        // Attack 0.66 with decay 0.35, sustain 0.45 and release 0.39 is a pad
        // envelope and nothing else: it needs two to three seconds to speak, so
        // every note here is at least a full bar and most are two. Cutoff 0.62
        // with key follow 1.00 and zero resonance gives an even, unpeaky tone
        // across the register lane.
        Part { "B47", "principal wide pad", 0.55f,
               {
                 { 0, 8, { 62, 65, 69, 74 } },
                 { 8, 8, { 58, 62, 65, 69 } },
                 { 16, 8, { 62, 65, 69, 72 } },
                 { 24, 8, { 62, 67, 70, 74 } },
                 { 32, 8, { 65, 69, 74, 77 } },
                 { 40, 4, { 65, 69, 72, 77 } },
                 { 44, 4, { 64, 69, 71, 76 } },
                 { 64, 4, { 65, 69, 74, 77 } },
                 { 68, 4, { 65, 69, 72, 77 } },
                 { 72, 4, { 65, 67, 70, 74 } },
                 { 76, 4, { 64, 69, 71, 76 } },
                 { 80, 8, { 65, 69, 72, 76 } },
                 { 88, 8, { 69, 74, 77, 81 } },
                 { 96, 4, { 69, 72, 77, 81 } },
                 { 100, 4, { 67, 72, 76, 79 } },
                 { 104, 4, { 70, 74, 77 } },
                 { 108, 4, { 69, 71, 76, 79 } },
                 { 112, 4, { 62, 65, 69, 74 } },
                 { 116, 4, { 62, 65, 67, 70 } },
                 { 120, 8, { 62, 69, 74 } },
               },
               {},
               { { 0, 0.0000f }, { 72, 0.0000f }, { 80, 0.3000f }, { 111, 0.3000f }, { 112, 0.3000f }, { 119, 0.0000f } } },

        // High shimmer pad - the ceiling; two notes at a time, never more than three.
        // Resonance 0.94 against a cutoff of 0.37 with key follow 1.00 is the
        // whole reason it is here: the resonant peak tracks the keyboard, so
        // held high notes get a singing formant on top that no other preset in
        // the bank produces at this register. Attack 0.72 is the slowest in the
        // piece, sustain 1.00 and release 0.43, so it fades in and hangs - I
        // never give it a note shorter than four beats.
        Part { "B56", "high shimmer pad", 0.30f,
               {
                 { 8, 8, { 77, 81 } },
                 { 16, 8, { 76, 81 } },
                 { 24, 8, { 77, 82 } },
                 { 32, 8, { 77, 81, 86 } },
                 { 40, 4, { 77, 81 } },
                 { 44, 4, { 76, 79 } },
                 { 64, 12, { 77, 84 } },
                 { 76, 4, { 76, 83 } },
                 { 80, 8, { 81, 86 } },
                 { 88, 8, { 81, 89 } },
                 { 96, 8, { 81, 84 } },
                 { 104, 4, { 79, 86 } },
                 { 108, 4, { 81, 85 } },
                 { 112, 8, { 86, 89 } },
                 { 120, 8, { 86 } },
               },
               {},
               { { 0, 0.0000f }, { 96, 0.0000f }, { 111, 0.2500f }, { 112, 0.0000f } } },

        // Low string bed - the floor under the harmony, root/fifth/octave only.
        // 16' range, so everything written here sounds an octave lower and the
        // part occupies G2-D4 while being written comfortably in the middle of
        // the keyboard. Attack 0.65 and release 0.33 with sustain 0.86 make it
        // swell rather than start, which is exactly what a floor arriving at bar
        // 5 should do.
        Part { "B81", "low string bed", 0.45f,
               {
                 { 16, 8, { 62, 69, 74 } },
                 { 24, 8, { 55, 62, 70 } },
                 { 32, 8, { 58, 65, 74 } },
                 { 40, 4, { 57, 65, 72 } },
                 { 44, 4, { 57, 64, 67 } },
                 { 48, 4, { 62, 69 } },
                 { 52, 4, { 60, 65, 69 } },
                 { 56, 4, { 58, 65, 74 } },
                 { 60, 4, { 55, 62, 70 } },
                 { 64, 4, { 58, 65, 74 } },
                 { 68, 4, { 57, 65, 72 } },
                 { 72, 4, { 55, 62, 70 } },
                 { 76, 4, { 57, 64, 67 } },
                 { 80, 8, { 62, 69, 74 } },
                 { 88, 8, { 58, 65, 74 } },
                 { 96, 4, { 53, 60, 65 } },
                 { 100, 4, { 60, 67, 72 } },
                 { 104, 4, { 55, 62, 70 } },
                 { 108, 4, { 57, 64, 67 } },
                 { 112, 4, { 62, 69, 74 } },
                 { 116, 4, { 62, 67, 70 } },
                 { 120, 8, { 62, 69 } },
               },
               {},
               {} },

        // Bass - one plucked note at a time, dead centre.
        // Key mode is unison, so it is monophonic by construction, and I have
        // written it monophonically: never two notes, never an overlap. That is
        // a feature here, not a limit - a cinematic low end should be one line.
        Part { "A48", "bass", 0.55f,
               {
                 { 16, 2, { 50 } },
                 { 20, 2, { 50 } },
                 { 24, 2, { 55 } },
                 { 28, 2, { 55 } },
                 { 32, 2, { 58 } },
                 { 36, 2, { 58 } },
                 { 40, 2, { 57 } },
                 { 44, 2, { 57 } },
                 { 46, 2, { 57 } },
                 { 48, 2, { 50 } },
                 { 50, 1, { 50 } },
                 { 52, 2, { 48 } },
                 { 54, 1, { 48 } },
                 { 56, 2, { 46 } },
                 { 58, 1, { 46 } },
                 { 60, 2, { 55 } },
                 { 62, 1, { 55 } },
                 { 64, 2, { 58 } },
                 { 66, 1, { 58 } },
                 { 68, 2, { 57 } },
                 { 70, 1, { 57 } },
                 { 72, 2, { 55 } },
                 { 74, 1, { 55 } },
                 { 76, 2, { 57 } },
                 { 78, 1, { 57 } },
                 { 80, 2, { 50 } },
                 { 82, 1, { 57 } },
                 { 84, 2, { 50 } },
                 { 86, 1, { 57 } },
                 { 88, 2, { 46 } },
                 { 90, 1, { 53 } },
                 { 92, 2, { 46 } },
                 { 94, 1, { 53 } },
                 { 96, 2, { 53 } },
                 { 98, 1, { 60 } },
                 { 100, 2, { 48 } },
                 { 102, 1, { 55 } },
                 { 104, 2, { 55 } },
                 { 106, 1, { 50 } },
                 { 108, 2, { 57 } },
                 { 110, 2, { 57 } },
                 { 112, 2, { 50 } },
                 { 116, 2, { 50 } },
                 { 120, 4, { 50 } },
               },
               {},
               {} },

        // Warm inner harmony - the three-note middle voice that does the actual voice leading.
        // Attack 0.17 is the fastest of the four sustaining parts, which is
        // precisely what an inner voice needs: it can change chord on beat 3
        // (bars 12, 20, 28) and be heard doing it, where the 0.66 and 0.72
        // attack pads cannot. Sustain 1.00 with release 0.28 means it holds flat
        // and stops cleanly.
        Part { "B43", "warm inner harmony", 0.40f,
               {
                 { 32, 8, { 58, 62, 65 } },
                 { 40, 4, { 57, 60, 65 } },
                 { 44, 2, { 57, 62, 67 } },
                 { 46, 2, { 57, 61, 67 } },
                 { 48, 4, { 57, 62, 65 } },
                 { 52, 4, { 57, 60, 65 } },
                 { 56, 4, { 58, 62, 65 } },
                 { 60, 4, { 58, 62, 67 } },
                 { 64, 4, { 58, 62, 65 } },
                 { 68, 4, { 57, 60, 65 } },
                 { 72, 4, { 58, 62, 67 } },
                 { 76, 2, { 57, 62, 67 } },
                 { 78, 2, { 55, 61, 64 } },
                 { 80, 8, { 57, 62, 65 } },
                 { 88, 8, { 58, 62, 65 } },
                 { 96, 4, { 57, 60, 65 } },
                 { 100, 4, { 55, 60, 64 } },
                 { 104, 4, { 55, 58, 62 } },
                 { 108, 2, { 57, 62, 67 } },
                 { 110, 2, { 55, 61, 64 } },
                 { 112, 4, { 57, 62, 65 } },
                 { 116, 4, { 55, 58, 62 } },
                 { 120, 8, { 57, 62, 65 } },
               },
               {},
               {} },

        // Counter-melody, single line, dead centre - states the theme before the lead does.
        // 16' range puts a comfortably-played written line down into C3-E4, the
        // real cello register, without asking the part to live at the bottom of
        // the keyboard. Attack 0.38 is a bow, not a hit; decay 0.51 to sustain
        // 0.71 with release 0.27 is a sustained bowed note that settles
        // slightly.
        Part { "B83", "counter-melody", 0.62f,
               {
                 { 32, 2, { 65 } },
                 { 34, 2, { 69 } },
                 { 36, 3, { 74 } },
                 { 39, 1, { 72 } },
                 { 40, 2, { 69 } },
                 { 42, 2, { 72 } },
                 { 44, 2, { 74 } },
                 { 46, 2, { 73 } },
                 { 48, 4, { 74 } },
                 { 64, 4, { 70 } },
                 { 68, 4, { 69 } },
                 { 72, 3, { 67 } },
                 { 75, 1, { 65 } },
                 { 76, 2, { 64 } },
                 { 78, 2, { 67 } },
                 { 96, 4, { 72 } },
                 { 100, 4, { 76 } },
                 { 104, 2, { 74 } },
                 { 106, 2, { 70 } },
                 { 108, 2, { 69 } },
                 { 110, 2, { 73 } },
                 { 112, 4, { 74 } },
                 { 116, 2, { 70 } },
                 { 118, 2, { 69 } },
                 { 120, 6, { 62 } },
               },
               { { 32, -0.2786f }, { 32.3167, 0.0000f }, { 112, -0.2786f }, { 112.317, 0.0000f } },
               { { 36, 0.0000f }, { 39, 0.4000f }, { 40, 0.0000f }, { 112, 0.0000f }, { 115, 0.4500f }, { 118, 0.0000f } } },

        // The only rhythm in the piece - three short notes a bar, arpeggiating the chord upward.
        // Attack 0.00, decay 0.09, sustain 0.00, release 0.09 is the shortest
        // envelope of anything I chose - about a tenth of a second of sound per
        // note, and a measured crest factor of 27.7 dB confirms it is nearly all
        // transient. That is why it can play in the same octaves as the pads
        // without adding any sustained energy to them.
        Part { "A37", "pizzicato rhythm", 1.00f,
               {
                 { 48, 0.5, { 62 } },
                 { 49.5, 0.5, { 65 } },
                 { 51, 0.5, { 69 } },
                 { 52, 0.5, { 60 } },
                 { 53.5, 0.5, { 65 } },
                 { 55, 0.5, { 69 } },
                 { 56, 0.5, { 65 } },
                 { 57.5, 0.5, { 69 } },
                 { 59, 0.5, { 70 } },
                 { 60, 0.5, { 67 } },
                 { 61.5, 0.5, { 70 } },
                 { 63, 0.5, { 74 } },
                 { 64, 0.5, { 70 } },
                 { 65.5, 0.5, { 74 } },
                 { 67, 0.5, { 77 } },
                 { 68, 0.5, { 69 } },
                 { 69.5, 0.5, { 72 } },
                 { 71, 0.5, { 77 } },
                 { 72, 0.5, { 67 } },
                 { 73.5, 0.5, { 70 } },
                 { 75, 0.5, { 74 } },
                 { 76, 0.5, { 69 } },
                 { 77.5, 0.5, { 71 } },
                 { 79, 0.5, { 76 } },
                 { 88, 0.5, { 70 } },
                 { 89.5, 0.5, { 74 } },
                 { 91, 0.5, { 77 } },
                 { 92, 0.5, { 74 } },
                 { 93.5, 0.5, { 77 } },
                 { 95, 0.5, { 81 } },
                 { 96, 0.5, { 72 } },
                 { 97.5, 0.5, { 77 } },
                 { 99, 0.5, { 81 } },
                 { 100, 0.5, { 72 } },
                 { 101.5, 0.5, { 76 } },
                 { 103, 0.5, { 79 } },
                 { 104, 0.5, { 70 } },
                 { 105.5, 0.5, { 74 } },
                 { 107, 0.5, { 77 } },
                 { 108, 0.5, { 69 } },
                 { 109.5, 0.5, { 71 } },
                 { 111, 0.5, { 76 } },
                 { 112, 0.5, { 74 } },
               },
               {},
               {} },

        // Single high pings - punctuation, seventeen notes in the whole piece.
        // Resonance 1.00 with the oscillator contributing almost nothing (pulse
        // only, no sub, no PWM) means the sound is essentially the filter
        // ringing: a near-sine ping. Attack 0.00, decay 0.20, sustain 0.00,
        // release 0.47 gives a strike with a long ring-out, which is why one
        // note can occupy a whole bar of silence.
        Part { "B21", "high pings", 0.95f,
               {
                 { 16, 1, { 69 } },
                 { 22, 1, { 74 } },
                 { 24, 1, { 67 } },
                 { 30, 1, { 70 } },
                 { 40, 1, { 72 } },
                 { 46, 1, { 73 } },
                 { 64, 1, { 77 } },
                 { 72, 1, { 74 } },
                 { 80, 1, { 74 } },
                 { 88, 1, { 77 } },
                 { 96, 1, { 77 } },
                 { 100, 1, { 76 } },
                 { 104, 1, { 74 } },
                 { 108, 1, { 73 } },
                 { 112, 1, { 74 } },
                 { 120, 1, { 69 } },
                 { 126, 1, { 62 } },
               },
               {},
               {} },

        // Slow brass swells - one chord per bar, alternating bars at the climax so the texture breathes.
        // Attack 0.46 with decay 0.79 and sustain 0.74 is a swell, not a stab:
        // about a second to reach full level and then it holds. That makes it
        // useless for anything fast and perfect for a chord-per-bar sequence,
        // which is exactly how I have used it.
        Part { "A34", "slow brass swells", 0.48f,
               {
                 { 64, 4, { 65, 70, 74 } },
                 { 68, 4, { 65, 69, 72 } },
                 { 72, 4, { 67, 70, 74 } },
                 { 76, 4, { 64, 69, 73 } },
                 { 80, 4, { 62, 69, 74 } },
                 { 88, 4, { 65, 70, 74 } },
                 { 96, 4, { 65, 69, 72 } },
                 { 100, 4, { 64, 67, 72 } },
                 { 104, 4, { 62, 67, 70, 74 } },
                 { 108, 4, { 64, 69, 73, 79 } },
                 { 112, 4, { 62, 69, 74 } },
               },
               {},
               { { 0, 0.0000f }, { 100, 0.0000f }, { 111, 0.3000f }, { 112, 0.0000f } } },

        // The melody - single line, centre, enters at bar 20 beat 4 and not one note before.
        // VCA level 1.00 with patch volume 0.80 makes it the loudest-configured
        // patch of the ten before trims, which is what a melody arriving over
        // nine other parts needs. Attack 0.00 with decay 0.52 to sustain 0.38
        // and a short 0.09 release gives a note that speaks instantly, softens,
        // and stops cleanly - so a line of two- and three-beat notes articulates
        // rather than blurs.
        Part { "A53", "melody", 0.75f,
               {
                 { 79, 1, { 69 } },
                 { 80, 3, { 74 } },
                 { 83, 1, { 77 } },
                 { 84, 2, { 81 } },
                 { 86, 2, { 79 } },
                 { 88, 3, { 77 } },
                 { 91, 1, { 79 } },
                 { 92, 2, { 81 } },
                 { 94, 2, { 77 } },
                 { 96, 2, { 77 } },
                 { 98, 2, { 81 } },
                 { 100, 4, { 84 } },
                 { 104, 2, { 81 } },
                 { 106, 2, { 79 } },
                 { 108, 2, { 77 } },
                 { 110, 2, { 73 } },
                 { 112, 6, { 74 } },
                 { 120, 4, { 69 } },
               },
               { { 79, -0.5572f }, { 79.228, 0.0000f }, { 112, -0.5572f }, { 112.19, 0.0000f } },
               { { 84, 0.0000f }, { 87, 0.5000f }, { 88, 0.0000f }, { 100, 0.0000f }, { 103, 0.6000f }, { 104, 0.0000f }, { 112, 0.0000f }, { 115, 0.3500f }, { 118, 0.0000f } } },
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

// Linear interpolation across a lane's breakpoints, holding at both ends.
float laneValue (const std::vector<Breakpoint>& lane, double beat)
{
    if (lane.empty())
        return 0.0f;
    if (beat <= lane.front().beat)
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
                     std::int64_t totalFrames)
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
};

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

// Only the instrument's own Docs/audio/composition directory has a README two
// levels up carrying the markers; an ad-hoc output directory has none, and
// resolving one is how this tool tells the difference.
std::filesystem::path instrumentReadme (const std::filesystem::path& directory)
{
    auto normalised = directory.lexically_normal();
    if (normalised.filename().empty())
        normalised = normalised.parent_path();

    if (normalised.filename() != "composition"
        || normalised.parent_path().filename() != "audio"
        || normalised.parent_path().parent_path().filename() != "Docs")
        return {};

    return normalised.parent_path().parent_path().parent_path() / "README.md";
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
    table += "| Part | Factory preset | Role | Max notes | Rendered peak |\n"
             "| ---: | --- | --- | ---: | ---: |\n";
    int index = 1;
    for (const auto& level : levels)
    {
        char row[512];
        std::snprintf (row, sizeof row, "| %d | `%s` %s | %s | %d | %s dBFS |\n",
                       index++, level.slot.c_str(), level.name.c_str(),
                       level.role.c_str(), level.peakNotes,
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

    const auto& part = piece.parts.front();
    const auto* preset = youknow::presets::findByNumber (part.slot);
    const auto frames = static_cast<std::int64_t> (compositionSampleRate * 0.5);
    const auto rendered = renderPart (part, *preset, piece.tempo, frames);

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
    std::filesystem::path directory = "Docs/audio/composition";

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
                    batch[lane] = renderPart (piece.parts[base + lane],
                                              *presets[base + lane], piece.tempo,
                                              totalFrames);
                });
        for (auto& worker : workers)
            worker.join();

        for (std::size_t lane = 0; lane < count; ++lane)
        {
            const auto& part = piece.parts[base + lane];
            const auto& rendered = batch[lane];
            for (std::size_t index = 0; index < mixLeft.size(); ++index)
            {
                mixLeft[index] += rendered.left[index];
                mixRight[index] += rendered.right[index];
            }

            const auto partPeak = peakOf (rendered.left, rendered.right);
            PartLevel level;
            level.slot = part.slot;
            level.name = presets[base + lane]->name;
            level.role = part.role;
            level.peakDb = 20.0 * std::log10 (std::max (partPeak, 1.0e-9));
            level.peakNotes = peakNotes[base + lane];
            levels.push_back (level);

            std::printf ("Rendered %-4s %-26s %-20s peak %6.3f, %d notes\n",
                         part.slot, presets[base + lane]->name, part.role,
                         partPeak, peakNotes[base + lane]);
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
