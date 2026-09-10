// Grades this engine against recordings of a real Roland Juno-106.
//
// ===========================================================================
// THE MEASUREMENT PROTOCOL
// ===========================================================================
//
// Everything else in this repository measures the model against its own
// derivations: service-note component values, firmware disassembly, datasheet
// curves. That is the right way to build it, and it is not the same thing as
// knowing what the instrument sounds like. This tool closes that loop from the
// other end, on whatever hardware captures the project is legally able to hold.
//
// WHAT A CAPTURE MUST BE, to be admitted to the table below:
//
//  1. LICENCE. CC0, public domain, or a licence that explicitly permits
//     redistribution inside this MIT-licensed repository. The licence and its
//     source are recorded per capture in THIRD_PARTY_NOTICES.md, not here.
//     "Royalty free", "free download" and "free for personal use" are none of
//     these. A capture whose licence cannot be cited does not go in.
//  2. A KNOWN PANEL. A factory patch number, an eighteen-byte tone dump, or a
//     photograph of the panel that resolves every control. Without it there is
//     nothing to set the engine to and the comparison means nothing.
//  3. KNOWN NOTES. Which pitches, struck when, held how long.
//  4. A DECLARED SIGNAL PATH. Direct from the instrument's outputs, or else
//     with whatever it went through named. A capture through an unnamed
//     preamp, tape machine or compressor is a measurement of that chain.
//
// HOW A COMPARISON IS MADE. The engine renders the case's patch and notes at
// the capture's own sample rate. The two are then:
//
//  a. ALIGNED by the lag that maximises their normalised cross-correlation,
//     searched over `alignmentSearchSeconds`. A capture has an arbitrary
//     lead-in and no shared clock, so an assumed alignment would turn a timing
//     offset into a spectral error.
//  b. LEVEL-MATCHED on gated RMS - the RMS of every window within
//     `levelGateDb` of the loudest - so silence between notes cannot drag the
//     match, and so a difference in monitoring gain is not read as a
//     difference in the instrument. The applied trim is reported, because a
//     large one is itself a finding.
//  c. Compared on the measures below, each chosen because a specific modelled
//     block moves it.
//
// WHAT EACH MEASURE TESTS:
//
//   Pitch error (cents)        The DCO's integer division of the master clock.
//                              This should be near zero: the divider arithmetic
//                              is exact and temperature has no term in it.
//   Harmonic levels (dB)       The oscillator waveform and the filter's shape
//                              together. Deviation concentrated in the low
//                              harmonics is a waveform or sub-oscillator
//                              mismatch; deviation rising with harmonic number
//                              is a filter cutoff or slope mismatch.
//   Envelope timings (ms)      The firmware's envelope arithmetic and the
//                              quasi-linear VCA law it drives.
//   Spectral centroid (Hz)     Overall brightness - the filter's operating
//                              point, independent of the harmonic detail.
//   Noise floor (dBFS)         The avalanche source and the BBD's own hiss.
//   Channel correlation        The chorus: its depth, rate and the antiphase
//                              relationship between the two delay lines.
//
// WHAT THIS CANNOT TELL YOU. One capture is one unit, on one day, at one
// temperature, after forty years of component drift, through one converter.
// Agreement is evidence that the model is in the right place; disagreement
// names a block to look at but does not by itself prove the model wrong,
// because the reference unit is not a specification either. These numbers are
// reported, never asserted as pass or fail, and no constant in the engine may
// be fitted to them - see the listening-test limit in CLAUDE.md, which applies
// here with more force, not less.
//
// SELF-TEST. `--self-test` renders one case twice, the second time with a
// deliberate, known deviation, and checks that every measure above actually
// moves in the direction and by roughly the amount the deviation implies. A
// metric that cannot detect a difference it is pointed at reports agreement it
// has not earned, so the suite runs this whether or not any capture is
// committed.

#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowPresets.h"
#include "OversamplingQualitySupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace
{
using youknow::EngineParameters;
using youknow::KeyMode;
using youknow::YouKnowEngine;
using youknow::presets::Preset;
using youknow::oversampling_quality::ProjectionWindow;
using youknow::oversampling_quality::projectTone;

constexpr int renderBlockSize = 256;
// The engine renders at the deepest rung, so a difference the comparison finds
// is the model's and not the internal grid's.
constexpr int benchmarkOversampleFactor = 4;
// How far apart the capture and the render may start. A capture's lead-in is
// arbitrary, but a search wide enough to slide a whole note is a search that
// can align the wrong note.
constexpr double alignmentSearchSeconds = 1.0;
// Windows within this of the loudest take part in the gated level match.
constexpr double levelGateDb = -20.0;
constexpr double meterFloorDb = -140.0;
// Harmonics reported. Above the sixteenth a Juno-106 voice through its own
// filter is usually into its noise floor at any useful cutoff.
constexpr int harmonicCount = 16;

double toDecibels (double amplitude) noexcept
{
    return amplitude > 0.0 ? std::max (20.0 * std::log10 (amplitude), meterFloorDb)
                           : meterFloorDb;
}

// ---------------------------------------------------------------------------
// The cases
// ---------------------------------------------------------------------------

// One note or gesture, as the capture played it.
struct Stroke
{
    int note;
    double onSeconds;
    double offSeconds;
};

// A committed hardware capture and everything needed to reproduce it.
//
// `slot` names the factory patch the capture used. A capture of a panel setting
// that is not a factory patch cannot be entered this way and needs its
// eighteen tone bytes instead; there is no such case yet, so there is no code
// for one.
struct ReferenceCase
{
    const char* id;
    const char* fileName;
    const char* slot;
    const char* what;
    // The note whose steady state the harmonic and pitch measures are taken
    // from, and the window inside it that is steady.
    int analysisNote;
    double analysisFromSeconds;
    double analysisToSeconds;
    std::vector<Stroke> strokes;
};

// No hardware capture has cleared the four admission tests above yet. The
// table is deliberately empty rather than populated with material whose licence
// cannot be cited: an unusable capture in a public repository is a worse
// outcome than no capture at all. The protocol and the harness are here so
// that a capture which does clear them can be graded the day it arrives.
std::vector<ReferenceCase> referenceCases()
{
    return {};
}

// ---------------------------------------------------------------------------
// Reading a capture
// ---------------------------------------------------------------------------

struct Capture
{
    std::vector<double> left;
    std::vector<double> right;
    double sampleRate = 0.0;
    bool stereo = false;
};

std::uint32_t readLittleEndian (const std::vector<std::uint8_t>& bytes,
                                std::size_t offset, int byteCount)
{
    std::uint32_t value = 0;
    for (int index = 0; index < byteCount; ++index)
        value |= static_cast<std::uint32_t> (bytes[offset + static_cast<std::size_t> (index)])
              << (8 * index);
    return value;
}

// A capture comes from someone else's converter and someone else's editor, so
// this reads the WAV variants such a file actually arrives in - 16, 24 and 32
// bit integer and 32-bit float, mono or stereo - and walks the chunk list
// rather than assuming a 44-byte header.
std::optional<Capture> readWav (const std::filesystem::path& path)
{
    std::ifstream input (path, std::ios::binary);
    if (! input)
        return std::nullopt;
    const std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (input)),
                                           std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || std::memcmp (bytes.data(), "RIFF", 4) != 0
        || std::memcmp (bytes.data() + 8, "WAVE", 4) != 0)
        return std::nullopt;

    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits = 0;
    double rate = 0.0;
    std::size_t dataOffset = 0;
    std::size_t dataBytes = 0;

    std::size_t cursor = 12;
    while (cursor + 8 <= bytes.size())
    {
        const auto size = readLittleEndian (bytes, cursor + 4, 4);
        const auto body = cursor + 8;
        // A declared size is not a present size. These captures come from
        // someone else's converter and editor, and a truncated file can
        // declare a chunk it does not carry; reading the declared length would
        // walk off the end of a capture this tool is supposed to reject.
        const bool bodyPresent = size <= bytes.size() - std::min (body, bytes.size());
        if (std::memcmp (bytes.data() + cursor, "fmt ", 4) == 0 && size >= 16
            && bodyPresent)
        {
            format = static_cast<std::uint16_t> (readLittleEndian (bytes, body, 2));
            channels = static_cast<std::uint16_t> (readLittleEndian (bytes, body + 2, 2));
            rate = static_cast<double> (readLittleEndian (bytes, body + 4, 4));
            bits = static_cast<std::uint16_t> (readLittleEndian (bytes, body + 14, 2));
        }
        else if (std::memcmp (bytes.data() + cursor, "data", 4) == 0
                 && body <= bytes.size())
        {
            // Clamping a declared length to what survives would accept a
            // truncated capture whose tail is simply missing, and the release
            // and noise-floor measures would then report that truncation as a
            // difference in the instrument.
            if (! bodyPresent)
                return std::nullopt;
            dataOffset = body;
            dataBytes = static_cast<std::size_t> (size);
        }
        cursor = body + size + (size & 1u); // chunks are word-aligned
    }

    // The engine clamps its own rate into a supported range, and renderCase
    // would then size and label the render with the capture's original figure:
    // a 4 kHz capture would be compared against an 8 kHz timeline read as
    // 4 kHz, corrupting alignment and every measure derived from time.
    if (channels == 0 || channels > 2 || rate <= 0.0 || dataBytes == 0
        || rate < YouKnowEngine::minimumSupportedSampleRate
        || rate > YouKnowEngine::maximumSupportedSampleRate)
        return std::nullopt;

    const std::size_t bytesPerSample = bits / 8u;
    if (bytesPerSample == 0)
        return std::nullopt;
    // Format 1 is integer PCM, 3 is IEEE float; 0xfffe is extensible, whose
    // real format lives in the extension this reader does not parse.
    if (! ((format == 1 && (bits == 16 || bits == 24 || bits == 32))
           || (format == 3 && bits == 32)))
        return std::nullopt;

    const std::size_t frames = dataBytes / (bytesPerSample * channels);
    Capture capture;
    capture.sampleRate = rate;
    capture.stereo = channels == 2;
    capture.left.resize (frames);
    capture.right.resize (frames);

    const auto sampleAt = [&] (std::size_t offset) -> double
    {
        if (format == 3)
        {
            const auto raw = readLittleEndian (bytes, offset, 4);
            float value = 0.0f;
            std::memcpy (&value, &raw, 4);
            // Substituting a zero for a NaN would manufacture a sample and let
            // the capture through; it would then move the alignment, the level
            // match, the spectra and the floor while the run presented itself
            // as a valid hardware comparison.
            return static_cast<double> (value);
        }
        const auto raw = readLittleEndian (bytes, offset, static_cast<int> (bytesPerSample));
        const auto signBit = std::uint32_t { 1u } << (bits - 1u);
        const auto full = static_cast<double> (signBit);
        auto value = static_cast<std::int64_t> (raw);
        if ((raw & signBit) != 0)
            value -= static_cast<std::int64_t> (signBit) * 2;
        return static_cast<double> (value) / full;
    };

    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto base = dataOffset + frame * bytesPerSample * channels;
        capture.left[frame] = sampleAt (base);
        capture.right[frame] = channels == 2 ? sampleAt (base + bytesPerSample)
                                             : capture.left[frame];
        if (! std::isfinite (capture.left[frame]) || ! std::isfinite (capture.right[frame]))
            return std::nullopt;
    }
    return capture;
}

// ---------------------------------------------------------------------------
// Rendering the same case
// ---------------------------------------------------------------------------

EngineParameters parametersFor (const Preset& preset)
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

    parameters.volume = controls.volume;
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
    return parameters;
}

// The frequency the case's analysis note actually sounds, which is not the
// frequency of its MIDI number: the patch's octave selector and the preset's
// stored transpose and master tune all move it, and 16' - which many factory
// patches use - puts it a full octave below. Searching +/-100 cents around the
// MIDI note would then be searching an octave away from the tone.
double nominalFundamentalHz (const Preset& preset, int analysisNote)
{
    const auto octaves = preset.patch.range == youknow::DcoRange::Sixteen ? -1
                       : preset.patch.range == youknow::DcoRange::Four    ? 1
                                                                          : 0;
    const auto semitones = static_cast<double> (analysisNote)
                         + static_cast<double> (preset.controls.transpose)
                         + 12.0 * octaves
                         + static_cast<double> (preset.controls.masterTune) / 100.0;
    return 440.0 * std::pow (2.0, (semitones - 69.0) / 12.0);
}

Capture renderCase (const ReferenceCase& item, const Preset& preset,
                    double sampleRate, double seconds)
{
    auto engine = std::make_unique<YouKnowEngine>();
    engine->selectConverterTimingProfile (
        YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
    engine->prepare (sampleRate, renderBlockSize, benchmarkOversampleFactor);
    engine->setParameters (parametersFor (preset));

    struct Gate { std::int64_t frame; int note; bool on; };
    std::vector<Gate> gates;
    for (const auto& stroke : item.strokes)
    {
        gates.push_back (Gate { static_cast<std::int64_t> (
                                    std::llround (stroke.onSeconds * sampleRate)),
                                stroke.note, true });
        gates.push_back (Gate { static_cast<std::int64_t> (
                                    std::llround (stroke.offSeconds * sampleRate)),
                                stroke.note, false });
    }
    std::stable_sort (gates.begin(), gates.end(),
                      [] (const Gate& a, const Gate& b)
                      {
                          if (a.frame != b.frame)
                              return a.frame < b.frame;
                          return static_cast<int> (a.on) < static_cast<int> (b.on);
                      });

    const auto totalFrames = static_cast<std::int64_t> (std::llround (seconds * sampleRate));
    Capture rendered;
    rendered.sampleRate = sampleRate;
    rendered.stereo = true;
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
        auto count = std::min (static_cast<std::int64_t> (renderBlockSize),
                               totalFrames - frame);
        if (nextGate < gates.size())
            count = std::min (count, gates[nextGate].frame - frame);
        engine->process (blockLeft.data(), blockRight.data(), static_cast<int> (count));
        for (std::int64_t index = 0; index < count; ++index)
        {
            rendered.left.push_back (static_cast<double> (blockLeft[static_cast<std::size_t> (index)]));
            rendered.right.push_back (static_cast<double> (blockRight[static_cast<std::size_t> (index)]));
        }
        frame += count;
    }
    return rendered;
}

// ---------------------------------------------------------------------------
// Alignment and level matching
// ---------------------------------------------------------------------------

std::vector<double> monoOf (const Capture& capture)
{
    std::vector<double> mono (capture.left.size());
    for (std::size_t index = 0; index < mono.size(); ++index)
        mono[index] = 0.5 * (capture.left[index] + capture.right[index]);
    return mono;
}

// Root-mean-square over a centred sliding window.
//
// A one-pole follower on the rectified signal is not usable here. The lowest
// note a 16' patch plays has a period around 8 ms, and the two chorus lines
// beat against each other on top of that, so any follower fast enough to
// resolve a 2 ms attack also tracks the waveform and reports the next ripple
// trough as the end of the decay. This window is long enough to ride both and
// short enough that the envelope segments a Juno-106 produces are still
// resolved; it is centred so the timings it yields are not biased late.
constexpr double envelopeWindowSeconds = 0.05;

std::vector<double> amplitudeEnvelope (std::span<const double> samples,
                                       double sampleRate)
{
    const auto half = std::max<std::size_t> (
        1, static_cast<std::size_t> (std::llround (0.5 * envelopeWindowSeconds * sampleRate)));
    std::vector<double> envelope (samples.size(), 0.0);
    if (samples.empty())
        return envelope;

    // Running sum of squares over the window, so the cost does not grow with it.
    double sum = 0.0;
    std::size_t from = 0;
    std::size_t to = 0; // exclusive
    for (std::size_t index = 0; index < samples.size(); ++index)
    {
        const auto wanted = index + half + 1 < samples.size() ? index + half + 1
                                                              : samples.size();
        const auto start = index > half ? index - half : 0u;
        while (to < wanted)
        {
            sum += samples[to] * samples[to];
            ++to;
        }
        while (from < start)
        {
            sum -= samples[from] * samples[from];
            ++from;
        }
        const auto count = to - from;
        envelope[index] = count > 0 ? std::sqrt (std::max (0.0, sum) / static_cast<double> (count))
                                    : 0.0;
    }
    return envelope;
}

// The lag, in frames, at which `candidate` best matches `reference`. Positive
// means the candidate's content happens later than the reference's.
//
// Two stages, because neither alone is right for this material.
//
// A held note is very nearly periodic, so its sample correlation has almost
// equal maxima one period apart across the whole search: matching cycles is
// not the same as matching notes, and the winner can sit a whole number of
// periods away from the truth. The coarse stage therefore correlates the
// AMPLITUDE ENVELOPES, where the attack is a unique feature and there is no
// period to be confused by, on a decimated grid. The fine stage then refines
// that answer with sample correlation over a window narrower than one period
// of the lowest note this instrument produces, so it can sharpen the coarse
// answer without being able to jump a cycle.
//
// The decimation is also what makes this affordable. Correlating every lag
// against every sample is quadratic in the capture length; at 44.1 kHz an
// exhaustive one-second search over a ten-second capture is on the order of
// 10^10 multiply-adds before any measurement begins.
//
// Returns nothing when the pair is too short to align. A short capture is
// exactly the kind that carries converter latency and an arbitrary lead-in, so
// reporting an unmeasured zero would quietly feed a misaligned pair into every
// spectral and envelope measure below. The search span shrinks to fit what the
// capture can support rather than being abandoned at the first short file.
// Where the sound starts: the first sample at which the amplitude envelope
// reaches a tenth of that signal's own peak.
//
// This needs the capture to begin before its first note does, which the
// protocol's arbitrary lead-in already provides: the envelope window is
// centred, so at a buffer edge it has only half its support, and a note
// beginning at the very first sample is measured with a differently shaped
// window than the same note beginning inside the capture.
//
// This is the coarse alignment, in place of correlating anything. An envelope
// correlation over a long decaying note has a broad, nearly flat maximum whose
// argmax drifts; an onset is a single well-defined instant in each signal, and
// their difference is the lag directly. It is also O(n) rather than quadratic
// in the capture length, which is what the exhaustive lag scan it replaces was.
constexpr double onsetFraction = 0.1;

std::optional<std::int64_t> onsetFrame (const std::vector<double>& envelope)
{
    if (envelope.empty())
        return std::nullopt;
    const auto peak = *std::max_element (envelope.begin(), envelope.end());
    if (peak <= 0.0)
        return std::nullopt;
    const auto threshold = onsetFraction * peak;
    for (std::size_t index = 0; index < envelope.size(); ++index)
        if (envelope[index] >= threshold)
            return static_cast<std::int64_t> (index);
    return std::nullopt;
}

// Best lag by plain normalised cross-correlation of two series, searched over
// [-search, +search] and evaluated on the overlap.
std::int64_t correlateForLag (const std::vector<double>& reference,
                              const std::vector<double>& candidate,
                              std::int64_t search, std::int64_t centre)
{
    const auto usable = static_cast<std::int64_t> (
        std::min (reference.size(), candidate.size()));
    std::int64_t best = centre;
    double bestScore = -2.0;
    for (std::int64_t lag = centre - search; lag <= centre + search; ++lag)
    {
        double dot = 0.0;
        double referenceEnergy = 0.0;
        double candidateEnergy = 0.0;
        for (std::int64_t index = 0; index < usable; ++index)
        {
            const auto candidateIndex = index + lag;
            if (candidateIndex < 0 || candidateIndex >= usable)
                continue;
            const auto a = reference[static_cast<std::size_t> (index)];
            const auto b = candidate[static_cast<std::size_t> (candidateIndex)];
            dot += a * b;
            referenceEnergy += a * a;
            candidateEnergy += b * b;
        }
        const auto denominator = std::sqrt (referenceEnergy * candidateEnergy);
        if (denominator <= 0.0)
            continue;
        const auto score = dot / denominator;
        if (score > bestScore)
        {
            bestScore = score;
            best = lag;
        }
    }
    return best;
}

std::optional<std::int64_t> bestLag (const std::vector<double>& reference,
                                     const std::vector<double>& candidate,
                                     double sampleRate, double noteHz)
{
    const auto usable = static_cast<std::int64_t> (
        std::min (reference.size(), candidate.size()));
    const auto wanted = static_cast<std::int64_t> (
        std::llround (alignmentSearchSeconds * sampleRate));
    // A quarter of the pair, so at least half of it always remains as the
    // window the fine correlation is computed over.
    const auto search = std::min (wanted, usable / 4);
    if (search <= 0)
        return std::nullopt;

    const auto referenceOnset = onsetFrame (amplitudeEnvelope (reference, sampleRate));
    const auto candidateOnset = onsetFrame (amplitudeEnvelope (candidate, sampleRate));
    if (! referenceOnset.has_value() || ! candidateOnset.has_value())
        return std::nullopt;

    const auto coarse = *candidateOnset - *referenceOnset;
    if (std::abs (coarse) > search)
        return std::nullopt; // further apart than the protocol admits

    // Fine: samples, inside one cycle of the note actually sounding. A held
    // note is nearly stationary, so a wider sample search would have almost
    // equal maxima one period apart and could settle a whole cycle away from
    // the onset the stage above located honestly.
    const auto halfPeriod = noteHz > 0.0
                                ? static_cast<std::int64_t> (0.5 * sampleRate / noteHz)
                                : static_cast<std::int64_t> (0.001 * sampleRate);
    const auto fine = std::max<std::int64_t> (1, halfPeriod);
    return correlateForLag (reference, candidate, fine, coarse);
}

// RMS over the windows within `levelGateDb` of the loudest, so silence between
// notes cannot drag the measurement toward the noise floor.
//
// Taken over the two channels' combined power rather than over their sum. The
// chorus clocks its two delay lines in antiphase, so folding to mono lets them
// cancel: a difference in chorus phase or delay would show up as a difference
// in monitoring gain, and the trim derived from it would then be applied to
// both rendered channels and contaminate every measure downstream.
double gatedRms (const std::vector<double>& left, const std::vector<double>& right,
                 double sampleRate)
{
    const auto frames = std::min (left.size(), right.size());
    const auto window = static_cast<std::size_t> (std::llround (0.05 * sampleRate));
    if (frames < window || window == 0)
        return 0.0;

    std::vector<double> windows;
    for (std::size_t start = 0; start + window <= frames; start += window)
    {
        double sum = 0.0;
        for (std::size_t index = start; index < start + window; ++index)
            sum += left[index] * left[index] + right[index] * right[index];
        windows.push_back (std::sqrt (sum / static_cast<double> (2 * window)));
    }
    if (windows.empty())
        return 0.0;

    const auto loudest = *std::max_element (windows.begin(), windows.end());
    if (loudest <= 0.0)
        return 0.0;
    const auto gate = loudest * youknow::oversampling_quality::decibelsToAmplitude (levelGateDb);

    double sum = 0.0;
    std::size_t count = 0;
    for (const auto value : windows)
        if (value >= gate)
        {
            sum += value * value;
            ++count;
        }
    return count == 0 ? 0.0 : std::sqrt (sum / static_cast<double> (count));
}

// ---------------------------------------------------------------------------
// The measures
// ---------------------------------------------------------------------------

// Refines the fundamental around the note's nominal frequency by projecting at
// a fine grid of candidates and taking the strongest. Coherent projection
// rather than an FFT peak, so the answer is not quantised to a bin.
double measureFundamental (std::span<const double> samples, double sampleRate,
                           double nominalHz)
{
    double best = nominalHz;
    double bestAmplitude = -1.0;
    // +/- one semitone in one-cent steps: wide enough for a detuned or drifting
    // reference unit, fine enough that the residual is below the reporting
    // resolution.
    for (int cents = -100; cents <= 100; ++cents)
    {
        const auto candidate = nominalHz * std::pow (2.0, cents / 1200.0);
        if (candidate <= 0.0 || candidate >= 0.5 * sampleRate)
            continue;
        const auto projection =
            projectTone (samples, sampleRate, candidate, ProjectionWindow::Hann);
        if (projection.amplitude > bestAmplitude)
        {
            bestAmplitude = projection.amplitude;
            best = candidate;
        }
    }
    return best;
}

// Radix-2 in place, on a power-of-two prefix of the analysis window. The
// harmonic levels below are still taken by coherent projection at exact
// multiples of the measured fundamental, which needs no transform and no bin
// snapping; this exists only for the centroid, which has to see energy the
// harmonic grid does not sit on.
void forwardTransform (std::vector<std::complex<double>>& values)
{
    const auto size = values.size();
    for (std::size_t index = 1, reverse = 0; index < size; ++index)
    {
        std::size_t bit = size >> 1;
        for (; (reverse & bit) != 0; bit >>= 1)
            reverse ^= bit;
        reverse ^= bit;
        if (index < reverse)
            std::swap (values[index], values[reverse]);
    }
    for (std::size_t length = 2; length <= size; length <<= 1)
    {
        const auto step = std::polar (1.0, -2.0 * std::numbers::pi_v<double>
                                               / static_cast<double> (length));
        for (std::size_t start = 0; start < size; start += length)
        {
            std::complex<double> rotation { 1.0, 0.0 };
            for (std::size_t offset = 0; offset < length / 2; ++offset)
            {
                const auto even = values[start + offset];
                const auto odd = values[start + offset + length / 2] * rotation;
                values[start + offset] = even + odd;
                values[start + offset + length / 2] = even - odd;
                rotation *= step;
            }
        }
    }
}

// The power-weighted mean frequency of everything in the band, not of the
// harmonic series alone.
//
// A centroid accumulated only at the first sixteen exact harmonics is blind to
// precisely what moves the brightness of this instrument: the filter's own
// noise contribution, the chorus's sidebands, and every part of a strongly
// filtered spectrum that does not sit on a harmonic. Those presets could
// change audibly and leave such a figure almost still.
double spectralCentroidHz (std::span<const double> samples, double sampleRate)
{
    std::size_t size = 1;
    while (size * 2 <= samples.size())
        size *= 2;
    if (size < 64)
        return 0.0;

    std::vector<std::complex<double>> spectrum (size);
    for (std::size_t index = 0; index < size; ++index)
    {
        // Hann, so the analysis window's own edges do not smear energy across
        // the band and drag the mean upward.
        const auto phase = 2.0 * std::numbers::pi_v<double> * static_cast<double> (index)
                         / static_cast<double> (size - 1);
        spectrum[index] = samples[index] * 0.5 * (1.0 - std::cos (phase));
    }
    forwardTransform (spectrum);

    // Below 20 Hz there is nothing this instrument produces, only any DC the
    // capture chain left behind, and it would pull the mean toward zero.
    constexpr double lowEdgeHz = 20.0;
    double weighted = 0.0;
    double total = 0.0;
    for (std::size_t bin = 1; bin < size / 2; ++bin)
    {
        const auto frequency = static_cast<double> (bin) * sampleRate
                             / static_cast<double> (size);
        if (frequency < lowEdgeHz)
            continue;
        const auto power = std::norm (spectrum[bin]);
        weighted += frequency * power;
        total += power;
    }
    return total > 0.0 ? weighted / total : 0.0;
}

struct Spectrum
{
    std::array<double, harmonicCount> harmonicDb {};
    double centroidHz = 0.0;
};

Spectrum measureSpectrum (std::span<const double> samples, double sampleRate,
                          double fundamentalHz)
{
    Spectrum spectrum;
    std::array<double, harmonicCount> amplitude {};

    for (int index = 0; index < harmonicCount; ++index)
    {
        const auto frequency = fundamentalHz * (index + 1);
        if (frequency >= 0.5 * sampleRate)
        {
            amplitude[static_cast<std::size_t> (index)] = 0.0;
            continue;
        }
        const auto projection =
            projectTone (samples, sampleRate, frequency, ProjectionWindow::Hann);
        amplitude[static_cast<std::size_t> (index)] = projection.amplitude;
    }

    const auto first = amplitude[0];
    for (int index = 0; index < harmonicCount; ++index)
        spectrum.harmonicDb[static_cast<std::size_t> (index)] =
            first > 0.0 ? toDecibels (amplitude[static_cast<std::size_t> (index)] / first)
                        : meterFloorDb;
    spectrum.centroidHz = spectralCentroidHz (samples, sampleRate);
    return spectrum;
}

// The window belonging to the stroke the case designates for envelope timing:
// from its attack to the next attack anywhere in the part, so no other note is
// sounding inside it.
//
// Returns nothing when no such isolated window exists - when the designated
// note is never struck, when another note is already sounding as it begins, or
// when the next note arrives before this one has been released. In those cases
// the envelope is a property of two notes and cannot be attributed to either.
// How long a previous note is assumed to keep sounding after its key-up. The
// longest release this instrument offers runs a little over two seconds at the
// top of the slider, and this is the quiet side of that: a case whose strokes
// are closer together than this is one whose envelopes cannot be attributed to
// a single note anyway.
constexpr double releaseTailSeconds = 2.5;

struct IsolatedWindow
{
    double fromSeconds;
    double keyUpSeconds;
    double toSeconds;
};

std::optional<IsolatedWindow> isolatedWindowFor (const ReferenceCase& item,
                                                 double availableSeconds)
{
    const auto stroke = std::find_if (item.strokes.begin(), item.strokes.end(),
                                      [&item] (const Stroke& candidate)
                                      { return candidate.note == item.analysisNote; });
    if (stroke == item.strokes.end() || stroke->offSeconds <= stroke->onSeconds)
        return std::nullopt;

    double nextOnset = availableSeconds;
    for (const auto& other : item.strokes)
    {
        if (&other == &*stroke)
            continue;
        // A key-up is not the end of a note. The release keeps the previous
        // one audible through this stroke's attack and decay, and the summed
        // envelope would then be timed as if it belonged to this note alone,
        // so an earlier stroke must have been released long enough ago for its
        // tail to have gone.
        if (other.onSeconds <= stroke->onSeconds
            && other.offSeconds + releaseTailSeconds > stroke->onSeconds)
            return std::nullopt;
        if (other.onSeconds > stroke->onSeconds)
            nextOnset = std::min (nextOnset, other.onSeconds);
    }
    if (nextOnset <= stroke->offSeconds)
        return std::nullopt; // the release runs into the next note

    return IsolatedWindow { stroke->onSeconds, stroke->offSeconds,
                            std::min (nextOnset, availableSeconds) };
}

struct Envelope
{
    double attackMs = 0.0;   // to 90 % of the peak
    double decayMs = 0.0;    // from the peak down to 3 dB below it
    double sustainDb = 0.0;  // the plateau, relative to the peak
    double releaseMs = 0.0;  // from key-up down to -40 dB of the peak
    // A segment that ran out of window before it finished is not a measurement
    // of that segment. Both signals would otherwise be capped at the same
    // window length and report a perfect zero difference between two releases
    // neither of which was seen to end.
    bool sustainSettled = false;
    bool releaseCompleted = false;
};

Envelope measureEnvelope (std::span<const double> samples, double sampleRate,
                          double keyUpSeconds)
{
    const auto envelope = amplitudeEnvelope (samples, sampleRate);
    Envelope result;
    if (envelope.empty())
        return result;

    const auto keyUp = std::min (envelope.size() - 1,
                                 static_cast<std::size_t> (std::llround (keyUpSeconds * sampleRate)));
    const auto peakIterator = std::max_element (envelope.begin(),
                                                envelope.begin() + static_cast<std::ptrdiff_t> (keyUp) + 1);
    const auto peak = *peakIterator;
    if (peak <= 0.0)
        return result;
    const auto peakIndex = static_cast<std::size_t> (peakIterator - envelope.begin());

    std::size_t attackIndex = 0;
    while (attackIndex < peakIndex && envelope[attackIndex] < 0.9 * peak)
        ++attackIndex;
    result.attackMs = 1000.0 * static_cast<double> (attackIndex) / sampleRate;

    // The plateau is the last tenth of the held segment, which is past any
    // decay a Juno-106 envelope can still be running.
    const auto plateauFrom = peakIndex + (keyUp - peakIndex) * 9 / 10;
    double plateau = 0.0;
    if (keyUp > plateauFrom)
    {
        for (std::size_t index = plateauFrom; index < keyUp; ++index)
            plateau += envelope[index];
        plateau /= static_cast<double> (keyUp - plateauFrom);
    }
    result.sustainDb = toDecibels (plateau / peak);
    // The plateau is only a sustain level if the envelope had stopped moving by
    // the time it was taken. On a short held note, or a patch whose decay is
    // longer than the note, that stretch is still the decay, and reporting it
    // would let a decay-rate difference masquerade as a sustain-level one.
    if (keyUp > plateauFrom)
    {
        double lowest = envelope[plateauFrom];
        double highest = envelope[plateauFrom];
        for (std::size_t index = plateauFrom; index < keyUp; ++index)
        {
            lowest = std::min (lowest, envelope[index]);
            highest = std::max (highest, envelope[index]);
        }
        // Half a decibel of drift across the stretch, which is below what any
        // of these comparisons resolve.
        result.sustainSettled = highest <= 0.0
            || toDecibels (highest) - toDecibels (std::max (lowest, 1.0e-12)) < 0.5;
    }

    // Time from the peak down to 3 dB below it.
    //
    // The threshold is fixed against the peak and not against the plateau
    // above, which is the whole point: a plateau-relative target moves up when
    // the decay lengthens - because a slower decay has not finished by key-up -
    // so it is crossed EARLIER and a longer decay measures as a shorter one.
    // Against the peak the reading is monotone in the decay setting whether or
    // not the segment completes inside the held note.
    //
    // A patch whose sustain sits within 3 dB of its peak has no decay to
    // measure at all; that reads as the full held length, which is the honest
    // answer rather than a fitted one.
    std::size_t decayIndex = peakIndex;
    const auto decayTarget =
        peak * youknow::oversampling_quality::decibelsToAmplitude (-3.0);
    while (decayIndex < keyUp && envelope[decayIndex] > decayTarget)
        ++decayIndex;
    result.decayMs = 1000.0 * static_cast<double> (decayIndex - peakIndex) / sampleRate;
    const auto releaseTarget = 0.01 * peak; // -40 dB
    std::size_t releaseIndex = keyUp;
    while (releaseIndex < envelope.size() && envelope[releaseIndex] > releaseTarget)
        ++releaseIndex;
    result.releaseCompleted = releaseIndex < envelope.size();
    result.releaseMs = 1000.0 * static_cast<double> (releaseIndex - keyUp) / sampleRate;
    return result;
}

// Over a stated window rather than the whole buffer. The protocol permits an
// arbitrary lead-in and a long tail, and a capture's idle converter noise or a
// render's silence there would otherwise dominate a measure that is supposed to
// describe the chorus. The two buffers can also differ in length once the
// render has been shifted into alignment.
double channelCorrelation (const Capture& capture, std::size_t from, std::size_t to)
{
    to = std::min (to, std::min (capture.left.size(), capture.right.size()));
    double dot = 0.0;
    double leftEnergy = 0.0;
    double rightEnergy = 0.0;
    for (std::size_t index = from; index < to; ++index)
    {
        dot += capture.left[index] * capture.right[index];
        leftEnergy += capture.left[index] * capture.left[index];
        rightEnergy += capture.right[index] * capture.right[index];
    }
    const auto denominator = std::sqrt (leftEnergy * rightEnergy);
    return denominator > 0.0 ? dot / denominator : 1.0;
}

// The quietest tenth of a second of RECORDED audio, which between and after
// notes is the instrument's own floor rather than any note.
//
// Digitally silent windows are excluded. The protocol permits an arbitrary
// lead-in, and an edited capture routinely carries exact zeros there or at its
// tail; those are the editor's silence, not the instrument's noise, and taking
// the minimum over them would report the meter floor for every capture that
// has been topped and tailed. Anything below this threshold cannot be an
// analogue floor through any real converter.
constexpr double digitalSilenceDb = -120.0;

//
// Over both channels' combined power, for the same reason the level match is:
// the bucket-brigade lines are clocked in antiphase, so folding to mono lets
// their noise cancel or add, and a difference in cross-channel noise
// correlation between the hardware and the model would be reported as a
// difference in noise amplitude.
std::optional<double> noiseFloorDb (const std::vector<double>& left,
                                    const std::vector<double>& right,
                                    double sampleRate)
{
    const auto frames = std::min (left.size(), right.size());
    const auto window = static_cast<std::size_t> (std::llround (0.1 * sampleRate));
    if (window == 0 || frames < window)
        return std::nullopt;
    const auto silence =
        youknow::oversampling_quality::decibelsToAmplitude (digitalSilenceDb);

    double quietest = std::numeric_limits<double>::max();
    bool found = false;
    for (std::size_t start = 0; start + window <= frames; start += window / 2)
    {
        double sum = 0.0;
        for (std::size_t index = start; index < start + window; ++index)
            sum += left[index] * left[index] + right[index] * right[index];
        const auto rms = std::sqrt (sum / static_cast<double> (2 * window));
        if (rms < silence)
            continue;
        quietest = std::min (quietest, rms);
        found = true;
    }
    return found ? std::optional<double> (toDecibels (quietest)) : std::nullopt;
}

struct Comparison
{
    std::string id;
    double lagMs = 0.0;
    double levelTrimDb = 0.0;
    double correlation = 0.0;
    double centsError = 0.0;
    double worstHarmonicDb = 0.0;
    int worstHarmonic = 0;
    double centroidRatio = 0.0;
    double attackErrorMs = 0.0;
    double decayErrorMs = 0.0;
    double sustainErrorDb = 0.0;
    double releaseErrorMs = 0.0;
    double noiseFloorErrorDb = 0.0;
    double correlationError = 0.0;
    bool sustainMeasured = false;
    bool releaseMeasured = false;
    // A measure that could not be taken is reported as not taken. A zero here
    // would read as perfect agreement, which is the one answer never earned.
    bool spectrumMeasured = false;
    bool envelopeMeasured = false;
    bool noiseFloorMeasured = false;
    bool widthMeasured = false;
};

// Compares an already-aligned, already-level-matched pair.
//
// Returns nothing when the case itself is unmeasurable - an analysis window
// outside the audio, or a designated note the strokes do not contain. That is
// a mistyped or truncated case rather than a finding about the engine, and
// grading it would print a row of zeros that reads as perfect agreement.
std::optional<Comparison> compare (const ReferenceCase& item, const Preset& preset,
                                   const Capture& reference, const Capture& rendered,
                                   double lagFrames, double trimDb, std::string& why)
{
    Comparison result;
    result.id = item.id;
    result.lagMs = 1000.0 * lagFrames / reference.sampleRate;
    result.levelTrimDb = trimDb;

    const auto referenceMono = monoOf (reference);
    const auto renderedMono = monoOf (rendered);
    const auto usable = std::min (referenceMono.size(), renderedMono.size());
    if (usable == 0)
    {
        why = "the aligned pair has no overlap";
        return std::nullopt;
    }

    double dot = 0.0;
    double a2 = 0.0;
    double b2 = 0.0;
    for (std::size_t index = 0; index < usable; ++index)
    {
        dot += referenceMono[index] * renderedMono[index];
        a2 += referenceMono[index] * referenceMono[index];
        b2 += renderedMono[index] * renderedMono[index];
    }
    const auto denominator = std::sqrt (a2 * b2);
    result.correlation = denominator > 0.0 ? dot / denominator : 0.0;

    // The spectral window has to lie inside both signals, in that order, and
    // hold something. Falling through with zeros would claim a perfect
    // 0-cent pitch error for a case whose window is simply mistyped.
    if (! (item.analysisToSeconds > item.analysisFromSeconds)
        || item.analysisFromSeconds < 0.0)
    {
        why = "the analysis window is empty or starts before the capture";
        return std::nullopt;
    }
    const auto from = static_cast<std::size_t> (
        std::llround (item.analysisFromSeconds * reference.sampleRate));
    const auto to = static_cast<std::size_t> (
        std::llround (item.analysisToSeconds * reference.sampleRate));
    if (to > usable)
    {
        why = "the analysis window ends past the audio the pair has in common";
        return std::nullopt;
    }

    // The analysis window has to hold the designated note ALONE. Its harmonics
    // are what the pitch, harmonic and centroid measures attribute to it, and a
    // chord or an overlapping neighbour puts another note's partials inside the
    // same projections - including, at the fundamental, another note that may
    // simply be louder.
    const auto sounding = [&item] (double when)
    {
        int count = 0;
        for (const auto& stroke : item.strokes)
            if (stroke.onSeconds <= when && stroke.offSeconds > when)
                ++count;
        return count;
    };
    const auto onlyNoteSounds =
        sounding (item.analysisFromSeconds) == 1
        && sounding (0.5 * (item.analysisFromSeconds + item.analysisToSeconds)) == 1
        && std::none_of (item.strokes.begin(), item.strokes.end(),
                         [&item] (const Stroke& stroke)
                         {
                             // Any stroke that starts or stops inside the window
                             // changes what is sounding partway through it.
                             return (stroke.onSeconds > item.analysisFromSeconds
                                     && stroke.onSeconds < item.analysisToSeconds)
                                 || (stroke.offSeconds > item.analysisFromSeconds
                                     && stroke.offSeconds < item.analysisToSeconds);
                         })
        && std::any_of (item.strokes.begin(), item.strokes.end(),
                        [&item] (const Stroke& stroke)
                        {
                            return stroke.note == item.analysisNote
                                && stroke.onSeconds <= item.analysisFromSeconds
                                && stroke.offSeconds >= item.analysisToSeconds;
                        });
    if (onlyNoteSounds)
    {
        const std::span<const double> referenceWindow (referenceMono.data() + from, to - from);
        const std::span<const double> renderedWindow (renderedMono.data() + from, to - from);
        const auto nominal = nominalFundamentalHz (preset, item.analysisNote);

        const auto referenceF0 = measureFundamental (referenceWindow, reference.sampleRate, nominal);
        const auto renderedF0 = measureFundamental (renderedWindow, rendered.sampleRate, nominal);
        result.centsError = 1200.0 * std::log2 (renderedF0 / referenceF0);

        const auto referenceSpectrum = measureSpectrum (referenceWindow, reference.sampleRate, referenceF0);
        const auto renderedSpectrum = measureSpectrum (renderedWindow, rendered.sampleRate, renderedF0);
        for (int index = 1; index < harmonicCount; ++index)
        {
            const auto difference =
                renderedSpectrum.harmonicDb[static_cast<std::size_t> (index)]
                - referenceSpectrum.harmonicDb[static_cast<std::size_t> (index)];
            if (std::abs (difference) > std::abs (result.worstHarmonicDb))
            {
                result.worstHarmonicDb = difference;
                result.worstHarmonic = index + 1;
            }
        }
        result.centroidRatio = referenceSpectrum.centroidHz > 0.0
                                   ? renderedSpectrum.centroidHz / referenceSpectrum.centroidHz
                                   : 0.0;
        // A window with no signal in it projects to nothing: the fundamental
        // search returns whichever candidate it started from and every harmonic
        // sits at the meter floor, so a muted capture or a mistimed note would
        // report a flawless zero. Gate on the window actually carrying sound.
        const auto energyOf = [] (std::span<const double> window)
        {
            double sum = 0.0;
            for (const auto sample : window)
                sum += sample * sample;
            return window.empty() ? 0.0
                                  : std::sqrt (sum / static_cast<double> (window.size()));
        };
        constexpr double silentWindowDb = -80.0;
        result.spectrumMeasured =
            toDecibels (energyOf (referenceWindow)) > silentWindowDb
            && toDecibels (energyOf (renderedWindow)) > silentWindowDb;
    }

    // The envelope is measured on ONE stroke, in a window where nothing else is
    // sounding. Measuring the whole recording against the first stroke's key-up
    // reports a release that runs through every note that follows it, and reads
    // the wrong note entirely whenever the designated note is not struck first.
    if (const auto window = isolatedWindowFor (item, static_cast<double> (usable)
                                                         / reference.sampleRate))
    {
        const auto first = static_cast<std::size_t> (
            std::llround (window->fromSeconds * reference.sampleRate));
        const auto last = std::min (usable, static_cast<std::size_t> (
            std::llround (window->toSeconds * reference.sampleRate)));
        if (last > first)
        {
            const std::span<const double> referenceWindow (referenceMono.data() + first, last - first);
            const std::span<const double> renderedWindow (renderedMono.data() + first, last - first);
            const auto keyUp = window->keyUpSeconds - window->fromSeconds;
            const auto referenceEnvelope = measureEnvelope (referenceWindow, reference.sampleRate, keyUp);
            const auto renderedEnvelope = measureEnvelope (renderedWindow, rendered.sampleRate, keyUp);
            result.attackErrorMs = renderedEnvelope.attackMs - referenceEnvelope.attackMs;
            result.decayErrorMs = renderedEnvelope.decayMs - referenceEnvelope.decayMs;
            result.sustainErrorDb = renderedEnvelope.sustainDb - referenceEnvelope.sustainDb;
            result.releaseErrorMs = renderedEnvelope.releaseMs - referenceEnvelope.releaseMs;
            result.envelopeMeasured = true;
            result.sustainMeasured = referenceEnvelope.sustainSettled
                                  && renderedEnvelope.sustainSettled;
            result.releaseMeasured = referenceEnvelope.releaseCompleted
                                  && renderedEnvelope.releaseCompleted;
        }
    }

    const auto referenceFloor = noiseFloorDb (reference.left, reference.right,
                                              reference.sampleRate);
    const auto renderedFloor = noiseFloorDb (rendered.left, rendered.right,
                                             rendered.sampleRate);
    if (referenceFloor.has_value() && renderedFloor.has_value())
    {
        result.noiseFloorErrorDb = *renderedFloor - *referenceFloor;
        result.noiseFloorMeasured = true;
    }

    // A mono capture carries no width. readWav copies its one channel into
    // both, so its channel correlation is exactly 1.0 by construction, and
    // subtracting that from a stereo render would report a chorus error for
    // every case whose capture simply had one channel.
    if (reference.stereo)
    {
        result.correlationError = channelCorrelation (rendered, from, to)
                                - channelCorrelation (reference, from, to);
        result.widthMeasured = true;
    }
    return result;
}
} // namespace

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------

namespace
{
// Proves each measure moves when it is pointed at a difference it is supposed
// to see. Without this, a table of small errors is indistinguishable from a
// table of measures that cannot see anything at all.
int runSelfTest()
{
    constexpr double rate = 44100.0;
    const auto* preset = youknow::presets::findByNumber ("A11");
    if (preset == nullptr)
    {
        std::fprintf (stderr, "self-test: factory preset A11 is missing\n");
        return 1;
    }

    ReferenceCase item;
    item.id = "self-test";
    item.fileName = "";
    item.slot = "A11";
    item.what = "a single held note";
    item.analysisNote = 60;
    item.analysisFromSeconds = 0.5;
    item.analysisToSeconds = 1.5;
    item.strokes = { Stroke { 60, 0.0, 2.0 } };

    const auto baseline = renderCase (item, *preset, rate, 3.0);

    // Identity: the same render against itself must report no difference. Any
    // measure that cannot do this is measuring its own noise.
    std::string why;
    const auto identityResult = compare (item, *preset, baseline, baseline, 0.0, 0.0, why);
    if (! identityResult.has_value())
    {
        std::fprintf (stderr, "self-test: the baseline case is unmeasurable (%s)\n",
                      why.c_str());
        return 1;
    }
    const auto identity = *identityResult;
    if (std::abs (identity.centsError) > 0.01 || std::abs (identity.worstHarmonicDb) > 0.01
        || std::abs (identity.attackErrorMs) > 0.01
        || std::abs (identity.releaseErrorMs) > 0.01
        || std::abs (identity.noiseFloorErrorDb) > 0.01
        || identity.correlation < 0.999999
        || ! identity.spectrumMeasured || ! identity.envelopeMeasured
        || ! identity.noiseFloorMeasured || ! identity.widthMeasured)
    {
        std::fprintf (stderr,
                      "self-test: a render does not compare equal to itself "
                      "(cents %.4f, harmonic %.4f dB, attack %.4f ms, "
                      "release %.4f ms, floor %.4f dB, correlation %.8f)\n",
                      identity.centsError, identity.worstHarmonicDb,
                      identity.attackErrorMs, identity.releaseErrorMs,
                      identity.noiseFloorErrorDb, identity.correlation);
        return 1;
    }

    // Detuning by a known amount must show up as that many cents, and must not
    // be mistaken for anything else.
    {
        auto detuned = *preset;
        detuned.controls.masterTune = 25.0f;
        const auto shifted = renderCase (item, detuned, rate, 3.0);
        const auto result = *compare (item, *preset, baseline, shifted, 0.0, 0.0, why);
        if (std::abs (result.centsError - 25.0) > 3.0)
        {
            std::fprintf (stderr,
                          "self-test: a 25-cent detune measured %.2f cents\n",
                          result.centsError);
            return 1;
        }
    }

    // Closing the filter must darken the spectrum: the centroid has to fall
    // well below the reference's, whatever the harmonic detail does.
    {
        auto darker = *preset;
        darker.patch.cutoff = std::max (0.05f, preset->patch.cutoff - 0.15f);
        const auto shifted = renderCase (item, darker, rate, 3.0);
        const auto result = *compare (item, *preset, baseline, shifted, 0.0, 0.0, why);
        if (! (result.centroidRatio > 0.0 && result.centroidRatio < 0.95))
        {
            std::fprintf (stderr,
                          "self-test: closing the filter moved the centroid "
                          "ratio only to %.4f\n", result.centroidRatio);
            return 1;
        }
    }

    // A slower attack must be measured as a longer one.
    {
        auto slower = *preset;
        slower.patch.attack = 0.45f;
        const auto shifted = renderCase (item, slower, rate, 3.0);
        const auto result = *compare (item, *preset, baseline, shifted, 0.0, 0.0, why);
        if (result.attackErrorMs < 50.0)
        {
            std::fprintf (stderr,
                          "self-test: a much slower attack measured only "
                          "%.1f ms longer\n", result.attackErrorMs);
            return 1;
        }
    }

    // Changing the waveform must move the harmonic series. An identity
    // comparison proves only that equal inputs compare equal; without this the
    // harmonic measure could be permanently zero and still look like agreement.
    {
        auto pulsed = *preset;
        pulsed.patch.saw = false;
        pulsed.patch.pulse = true;
        const auto shifted = renderCase (item, pulsed, rate, 3.0);
        const auto result = *compare (item, *preset, baseline, shifted, 0.0, 0.0, why);
        if (std::abs (result.worstHarmonicDb) < 3.0)
        {
            std::fprintf (stderr,
                          "self-test: a saw-to-pulse change moved the worst "
                          "harmonic only %.2f dB\n", result.worstHarmonicDb);
            return 1;
        }
    }

    // A longer decay must be measured as a longer one, and a lower sustain as
    // a lower one. They are taken from the same envelope but by different
    // arithmetic, so neither stands in for the other.
    {
        auto slower = *preset;
        slower.patch.decay = std::min (1.0f, preset->patch.decay + 0.35f);
        const auto shifted = renderCase (item, slower, rate, 3.0);
        const auto result = *compare (item, *preset, baseline, shifted, 0.0, 0.0, why);
        if (result.decayErrorMs < 20.0)
        {
            std::fprintf (stderr,
                          "self-test: a much longer decay measured only "
                          "%.1f ms longer\n", result.decayErrorMs);
            return 1;
        }
    }
    {
        auto quieter = *preset;
        quieter.patch.sustain = std::max (0.05f, preset->patch.sustain - 0.30f);
        const auto shifted = renderCase (item, quieter, rate, 3.0);
        const auto result = *compare (item, *preset, baseline, shifted, 0.0, 0.0, why);
        if (result.sustainErrorDb > -1.0)
        {
            std::fprintf (stderr,
                          "self-test: a much lower sustain measured only "
                          "%.2f dB down\n", result.sustainErrorDb);
            return 1;
        }
    }

    // A longer release must be measured as a longer one.
    {
        auto longer = *preset;
        longer.patch.release = std::min (1.0f, preset->patch.release + 0.40f);
        const auto shifted = renderCase (item, longer, rate, 3.0);
        const auto result = *compare (item, *preset, baseline, shifted, 0.0, 0.0, why);
        if (result.releaseErrorMs < 20.0)
        {
            std::fprintf (stderr,
                          "self-test: a much longer release measured only "
                          "%.1f ms longer\n", result.releaseErrorMs);
            return 1;
        }
    }

    // Raising the chorus hiss must raise the measured floor. The bucket-brigade
    // noise is downstream of the amplifier, so it is what remains between and
    // after notes and it is what this measure is pointed at.
    {
        auto hissier = *preset;
        hissier.controls.chorusNoise = 1.0f;
        const auto shifted = renderCase (item, hissier, rate, 3.0);
        const auto result = *compare (item, *preset, baseline, shifted, 0.0, 0.0, why);
        if (result.noiseFloorErrorDb < 3.0)
        {
            std::fprintf (stderr,
                          "self-test: a much louder chorus hiss raised the floor "
                          "only %.2f dB\n", result.noiseFloorErrorDb);
            return 1;
        }
    }

    // Switching the chorus off must collapse the stereo image. A11 stores
    // chorus I, so its two lines are clocked in antiphase and its channels
    // decorrelate; with the effect off they are the same signal.
    {
        auto dry = *preset;
        dry.patch.chorus = youknow::ChorusMode::Off;
        const auto shifted = renderCase (item, dry, rate, 3.0);
        const auto result = *compare (item, *preset, baseline, shifted, 0.0, 0.0, why);
        if (result.correlationError < 0.05)
        {
            std::fprintf (stderr,
                          "self-test: switching the chorus off moved the channel "
                          "correlation only %.4f\n", result.correlationError);
            return 1;
        }
    }

    // A known lag must be recovered by the alignment search, on a case shaped
    // like a real capture: a lead-in before the note, which is what the
    // protocol admits and what the onset stage needs.
    const auto checkLag = [&] (int note, double delaySeconds, const char* what)
    {
        ReferenceCase lagCase = item;
        lagCase.analysisNote = note;
        lagCase.strokes = { Stroke { note, 0.3, 2.3 } };
        const auto rendered = renderCase (lagCase, *preset, rate, 3.0);
        const auto lagFrames = static_cast<std::size_t> (std::llround (delaySeconds * rate));
        Capture delayed = rendered;
        delayed.left.insert (delayed.left.begin(), lagFrames, 0.0);
        delayed.right.insert (delayed.right.begin(), lagFrames, 0.0);
        const auto found = bestLag (monoOf (rendered), monoOf (delayed), rate,
                                    nominalFundamentalHz (*preset, note));
        if (! found.has_value()
            || std::abs (*found - static_cast<std::int64_t> (lagFrames)) > 2)
        {
            std::fprintf (stderr, "self-test: a %zu-frame lag on %s was found at %s\n",
                          lagFrames, what,
                          found.has_value() ? std::to_string (*found).c_str() : "nothing");
            return false;
        }
        return true;
    };
    if (! checkLag (60, 0.037, "a low note"))
        return 1;
    // A high note is where a wider sample search would jump a cycle: its period
    // is a fraction of a millisecond.
    if (! checkLag (84, 0.021, "a high note"))
        return 1;

    // A capture too short to align must say so rather than reporting a zero
    // lag it never measured.
    {
        Capture stub;
        stub.sampleRate = rate;
        stub.left.assign (3, 0.5);
        stub.right.assign (3, 0.5);
        if (bestLag (monoOf (stub), monoOf (stub), rate, 261.6).has_value())
        {
            std::fprintf (stderr,
                          "self-test: a three-frame pair reported an alignment\n");
            return 1;
        }
    }

    // The gated level match must recover a known trim.
    {
        auto quieter = baseline;
        for (auto& sample : quieter.left)
            sample *= 0.5;
        for (auto& sample : quieter.right)
            sample *= 0.5;
        const auto trim = toDecibels (gatedRms (baseline.left, baseline.right, rate)
                                      / gatedRms (quieter.left, quieter.right, rate));
        if (std::abs (trim - 6.0206) > 0.05)
        {
            std::fprintf (stderr, "self-test: a 6.02 dB trim measured %.3f dB\n", trim);
            return 1;
        }
    }

    // A mono capture carries no width, and must say so rather than reporting
    // the difference between a duplicated channel pair and a real one.
    {
        auto mono = baseline;
        mono.stereo = false;
        for (std::size_t index = 0; index < mono.left.size(); ++index)
            mono.left[index] = mono.right[index] = 0.5 * (mono.left[index] + mono.right[index]);
        const auto result = compare (item, *preset, mono, baseline, 0.0, 0.0, why);
        if (! result.has_value() || result->widthMeasured
            || result->correlationError != 0.0)
        {
            std::fprintf (stderr,
                          "self-test: a mono capture was still scored for width\n");
            return 1;
        }
    }

    // A window outside the audio is a mistyped case, not a perfect score.
    {
        auto broken = item;
        broken.analysisFromSeconds = 10.0;
        broken.analysisToSeconds = 11.0;
        if (compare (broken, *preset, baseline, baseline, 0.0, 0.0, why).has_value())
        {
            std::fprintf (stderr,
                          "self-test: an analysis window past the end was graded\n");
            return 1;
        }
        broken = item;
        broken.analysisToSeconds = broken.analysisFromSeconds;
        if (compare (broken, *preset, baseline, baseline, 0.0, 0.0, why).has_value())
        {
            std::fprintf (stderr,
                          "self-test: an empty analysis window was graded\n");
            return 1;
        }
    }

    // Digital silence is the editor's, not the instrument's. A capture topped
    // and tailed with exact zeros must not report the meter floor.
    {
        auto padded = baseline;
        const auto pad = static_cast<std::size_t> (rate);
        padded.left.insert (padded.left.begin(), pad, 0.0);
        padded.right.insert (padded.right.begin(), pad, 0.0);
        const auto floor = noiseFloorDb (padded.left, padded.right, rate);
        if (! floor.has_value() || *floor <= digitalSilenceDb)
        {
            std::fprintf (stderr,
                          "self-test: a second of digital silence was reported as "
                          "the noise floor (%.1f dB)\n",
                          floor.has_value() ? *floor : meterFloorDb);
            return 1;
        }
    }

    // An envelope that cannot be attributed to one note must not be timed.
    {
        auto crowded = item;
        crowded.strokes = { Stroke { 60, 0.0, 2.0 }, Stroke { 67, 1.0, 2.5 } };
        const auto result = compare (crowded, *preset, baseline, baseline, 0.0, 0.0, why);
        if (! result.has_value() || result->envelopeMeasured)
        {
            std::fprintf (stderr,
                          "self-test: an overlapped note was still timed\n");
            return 1;
        }
    }

    // The analysis note's own frequency, not its MIDI number's. A11 is a 16'
    // patch, so its fundamental is an octave below the key.
    {
        const auto nominal = nominalFundamentalHz (*preset, 60);
        const auto midiHz = 440.0 * std::pow (2.0, (60 - 69) / 12.0);
        if (std::abs (nominal - 0.5 * midiHz) > 0.01)
        {
            std::fprintf (stderr,
                          "self-test: a 16' patch's nominal came out at %.3f Hz "
                          "against %.3f Hz for the key\n", nominal, midiHz);
            return 1;
        }
    }

    // A chord in the analysis window means the harmonics are not the note's.
    {
        auto chorded = item;
        chorded.strokes = { Stroke { 60, 0.0, 2.0 }, Stroke { 64, 0.0, 2.0 } };
        const auto result = compare (chorded, *preset, baseline, baseline, 0.0, 0.0, why);
        if (! result.has_value() || result->spectrumMeasured)
        {
            std::fprintf (stderr,
                          "self-test: a chord was still graded for pitch and "
                          "harmonics\n");
            return 1;
        }
    }

    // Level matching must not read a phase difference as a gain difference.
    // Inverting one channel is the extreme case: it leaves the stereo power
    // untouched and annihilates the mono sum.
    {
        auto flipped = baseline;
        for (auto& sample : flipped.right)
            sample = -sample;
        const auto straight = gatedRms (baseline.left, baseline.right, rate);
        const auto inverted = gatedRms (flipped.left, flipped.right, rate);
        if (std::abs (toDecibels (straight / inverted)) > 0.01)
        {
            std::fprintf (stderr,
                          "self-test: inverting a channel moved the matched level "
                          "by %.3f dB\n", toDecibels (straight / inverted));
            return 1;
        }
    }

    // Noise is energy off the harmonic grid, so a centroid accumulated only at
    // the harmonics would barely see it. This is what the transform is for.
    {
        auto noisy = *preset;
        noisy.patch.noise = 1.0f;
        const auto shifted = renderCase (item, noisy, rate, 3.0);
        const auto result = *compare (item, *preset, baseline, shifted, 0.0, 0.0, why);
        // A modest shift, because A11's filter sits at 0.28 and removes most of
        // what the noise generator contributes. The identity comparison above
        // reports exactly 1.0 when nothing changes, so this is unambiguous
        // movement rather than measurement slop - and a centroid taken only at
        // the first sixteen harmonics would not have moved at all.
        if (! (result.centroidRatio > 1.02))
        {
            std::fprintf (stderr,
                          "self-test: full noise moved the centroid ratio only to "
                          "%.4f\n", result.centroidRatio);
            return 1;
        }
    }

    // A silent window is not a measurement, however well-formed the case.
    {
        auto muted = baseline;
        std::fill (muted.left.begin(), muted.left.end(), 0.0);
        std::fill (muted.right.begin(), muted.right.end(), 0.0);
        const auto result = compare (item, *preset, muted, baseline, 0.0, 0.0, why);
        if (! result.has_value() || result->spectrumMeasured)
        {
            std::fprintf (stderr,
                          "self-test: a silent capture was graded for pitch\n");
            return 1;
        }
    }

    // A release that runs into the end of its window has not been seen to end,
    // and two such releases must not report a perfect zero difference.
    {
        auto lingering = *preset;
        lingering.patch.release = 1.0f;
        ReferenceCase brief = item;
        brief.strokes = { Stroke { 60, 0.0, 2.0 } };
        const auto shifted = renderCase (brief, lingering, rate, 2.4);
        const auto result = compare (brief, lingering, shifted, shifted, 0.0, 0.0, why);
        if (! result.has_value() || result->releaseMeasured)
        {
            std::fprintf (stderr,
                          "self-test: a release cut off by the window was still "
                          "reported\n");
            return 1;
        }
    }

    // A note whose predecessor is still releasing into it cannot be timed.
    {
        auto trailing = item;
        trailing.strokes = { Stroke { 55, 0.0, 0.2 }, Stroke { 60, 0.5, 2.5 } };
        trailing.analysisNote = 60;
        const auto result = compare (trailing, *preset, baseline, baseline, 0.0, 0.0, why);
        if (! result.has_value() || result->envelopeMeasured)
        {
            std::fprintf (stderr,
                          "self-test: a note was timed through the previous "
                          "note's release tail\n");
            return 1;
        }
    }

    // A capture at a rate the engine cannot run at is not comparable, because
    // the render would be built on a clamped timeline and read as the
    // capture's own.
    {
        std::error_code error;
        const auto path =
            std::filesystem::temp_directory_path (error) / "youknow-unsupported-rate.wav";
        std::vector<std::uint8_t> wav;
        const auto tag = [&wav] (const char* text)
        { for (int i = 0; i < 4; ++i) wav.push_back (static_cast<std::uint8_t> (text[i])); };
        const auto le = [&wav] (std::uint32_t value, int count)
        { for (int i = 0; i < count; ++i) wav.push_back (static_cast<std::uint8_t> ((value >> (8 * i)) & 0xffu)); };
        constexpr std::uint32_t frames = 400;
        tag ("RIFF"); le (36u + frames * 4u, 4); tag ("WAVE");
        tag ("fmt "); le (16u, 4); le (1u, 2); le (2u, 2);
        le (4000u, 4); le (4000u * 4u, 4); le (4u, 2); le (16u, 2);
        tag ("data"); le (frames * 4u, 4);
        for (std::uint32_t frame = 0; frame < frames * 2u; ++frame)
            le (1000u, 2);
        std::ofstream out (path, std::ios::binary);
        out.write (reinterpret_cast<const char*> (wav.data()),
                   static_cast<std::streamsize> (wav.size()));
        out.close();
        const auto read = readWav (path);
        std::filesystem::remove (path, error);
        if (read.has_value())
        {
            std::fprintf (stderr,
                          "self-test: a 4 kHz capture was accepted\n");
            return 1;
        }
    }

    std::printf ("YouKnow hardware-benchmark self-test passed: every measure "
                 "detects the difference it is pointed at.\n");
    return 0;
}
} // namespace

int main (int argc, char** argv)
{
    std::vector<std::string> arguments (argv + 1, argv + argc);
    bool selfTest = false;
    std::filesystem::path directory = "Docs/audio/reference";

    for (const auto& argument : arguments)
    {
        if (argument == "--self-test")
        {
            selfTest = true;
        }
        else if (argument == "--help" || argument == "-h")
        {
            std::printf ("usage: YouKnowBenchmarkReference [--self-test] "
                         "[capture-directory]\n");
            return 0;
        }
        else
        {
            directory = argument;
        }
    }

    if (selfTest)
        return runSelfTest();

    const auto cases = referenceCases();
    if (cases.empty())
    {
        std::printf (
            "No hardware capture has cleared the admission tests at the head of "
            "this file, so there is nothing to grade against.\n"
            "The protocol and the harness are ready: add a capture to "
            "%s, enter its case in referenceCases(), and record its licence in "
            "THIRD_PARTY_NOTICES.md.\n",
            directory.string().c_str());
        return 0;
    }

    std::vector<Comparison> results;
    for (const auto& item : cases)
    {
        const auto* preset = youknow::presets::findByNumber (item.slot);
        if (preset == nullptr)
        {
            std::fprintf (stderr, "%s: no factory preset %s\n", item.id, item.slot);
            return 1;
        }

        const auto path = directory / item.fileName;
        const auto capture = readWav (path);
        if (! capture.has_value())
        {
            std::fprintf (stderr, "%s: could not read %s\n", item.id,
                          path.string().c_str());
            return 1;
        }

        const auto seconds =
            static_cast<double> (capture->left.size()) / capture->sampleRate;
        auto rendered = renderCase (item, *preset, capture->sampleRate, seconds);

        const auto lag = bestLag (monoOf (*capture), monoOf (rendered),
                                  capture->sampleRate,
                                  nominalFundamentalHz (*preset, item.analysisNote));
        if (! lag.has_value())
        {
            std::fprintf (stderr,
                          "%s: %s is too short to align, so no measurement here "
                          "would be trustworthy\n", item.id, path.string().c_str());
            return 1;
        }
        if (*lag > 0)
        {
            const auto drop = static_cast<std::size_t> (*lag);
            if (drop < rendered.left.size())
            {
                rendered.left.erase (rendered.left.begin(),
                                     rendered.left.begin() + static_cast<std::ptrdiff_t> (drop));
                rendered.right.erase (rendered.right.begin(),
                                      rendered.right.begin() + static_cast<std::ptrdiff_t> (drop));
            }
        }
        else if (*lag < 0)
        {
            const auto pad = static_cast<std::size_t> (-*lag);
            rendered.left.insert (rendered.left.begin(), pad, 0.0);
            rendered.right.insert (rendered.right.begin(), pad, 0.0);
        }

        const auto referenceLevel = gatedRms (capture->left, capture->right,
                                              capture->sampleRate);
        const auto renderedLevel = gatedRms (rendered.left, rendered.right,
                                             rendered.sampleRate);
        const auto trim = renderedLevel > 0.0 ? referenceLevel / renderedLevel : 1.0;
        for (auto& sample : rendered.left)
            sample *= trim;
        for (auto& sample : rendered.right)
            sample *= trim;

        std::string why;
        auto comparison = compare (item, *preset, *capture, rendered,
                                   static_cast<double> (*lag), toDecibels (trim), why);
        if (! comparison.has_value())
        {
            std::fprintf (stderr, "%s: %s, so it cannot be graded\n", item.id, why.c_str());
            return 1;
        }
        results.push_back (*comparison);
        const auto& last = results.back();
        // Every measure the protocol names, because a dimension that is
        // computed and not printed is a dimension this tool does not grade.
        std::printf ("%s - %s\n", last.id.c_str(), item.what);
        std::printf ("  alignment   lag %+.2f ms, level trim %+.2f dB, "
                     "waveform correlation %+.3f\n",
                     last.lagMs, last.levelTrimDb, last.correlation);
        if (last.spectrumMeasured)
        {
            std::printf ("  pitch       %+.2f cents\n", last.centsError);
            std::printf ("  harmonics   worst H%d at %+.2f dB\n",
                         last.worstHarmonic, last.worstHarmonicDb);
            std::printf ("  brightness  centroid ratio %.4f (%+.2f dB)\n",
                         last.centroidRatio,
                         last.centroidRatio > 0.0
                             ? 20.0 * std::log10 (last.centroidRatio) : 0.0);
        }
        else
        {
            std::printf ("  pitch       not measured: note %d does not sound alone "
                         "throughout the analysis window\n", item.analysisNote);
            std::printf ("  harmonics   not measured, for the same reason\n");
            std::printf ("  brightness  not measured, for the same reason\n");
        }
        if (last.envelopeMeasured)
        {
            std::printf ("  envelope    attack %+.1f ms, decay %+.1f ms\n",
                         last.attackErrorMs, last.decayErrorMs);
            if (last.sustainMeasured)
                std::printf ("              sustain %+.2f dB\n", last.sustainErrorDb);
            else
                std::printf ("              sustain not measured: the envelope was "
                             "still moving at key-up\n");
            if (last.releaseMeasured)
                std::printf ("              release %+.1f ms\n", last.releaseErrorMs);
            else
                std::printf ("              release not measured: it had not reached "
                             "-40 dB before the window ended\n");
        }
        else
            std::printf ("  envelope    not measured: no stroke of note %d is "
                         "isolated enough to time\n", item.analysisNote);
        if (last.noiseFloorMeasured)
            std::printf ("  noise floor %+.2f dB\n", last.noiseFloorErrorDb);
        else
            std::printf ("  noise floor not measured: no window is quiet without "
                         "being digitally silent\n");
        if (last.widthMeasured)
            std::printf ("  chorus      channel correlation %+.4f\n",
                         last.correlationError);
        else
            std::printf ("  chorus      not measured: the capture is mono and "
                         "carries no width\n");
    }

    std::printf ("Graded %zu capture(s).\n", results.size());
    return 0;
}
