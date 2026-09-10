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

// Placeholder score, replaced by the composed piece. Kept only long enough to
// prove the render path end to end.
Composition composition()
{
    Composition piece;
    piece.title = "Placeholder";
    piece.key = "C minor";
    piece.tempo = 96.0;
    piece.beatsPerBar = 4.0;
    piece.tailBeats = 4.0;
    piece.parts = {
        Part { "A11", "brass", 0.80f,
               { Event { 0.0, 2.0, { 48, 55, 60 } },
                 Event { 4.0, 2.0, { 46, 53, 58 } } },
               {}, {} },
        Part { "A17", "pad", 0.70f,
               { Event { 0.0, 8.0, { 60, 63, 67 } } },
               {}, {} },
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

    std::vector<float> mixLeft (static_cast<std::size_t> (totalFrames), 0.0f);
    std::vector<float> mixRight (static_cast<std::size_t> (totalFrames), 0.0f);
    std::vector<PartLevel> levels;

    for (const auto& part : piece.parts)
    {
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
        const int peakNotes = peakSimultaneousNotes (part);
        if (peakNotes > limit)
        {
            std::fprintf (stderr,
                          "part %s (%s) needs %d simultaneous notes but the "
                          "preset plays %d\n",
                          part.slot, preset->name, peakNotes, limit);
            return 1;
        }

        const auto rendered = renderPart (part, *preset, piece.tempo, totalFrames);
        for (std::size_t index = 0; index < mixLeft.size(); ++index)
        {
            mixLeft[index] += rendered.left[index];
            mixRight[index] += rendered.right[index];
        }

        PartLevel level;
        level.slot = part.slot;
        level.name = preset->name;
        level.role = part.role;
        level.peakDb = 20.0 * std::log10 (std::max (peakOf (rendered.left, rendered.right),
                                                    1.0e-9));
        level.peakNotes = peakNotes;
        levels.push_back (level);

        std::printf ("Rendered %-4s %-26s %-14s peak %6.3f, %d notes\n", part.slot,
                     preset->name, part.role,
                     peakOf (rendered.left, rendered.right), peakNotes);
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
