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
            dataOffset = body;
            dataBytes = std::min (static_cast<std::size_t> (size), bytes.size() - body);
        }
        cursor = body + size + (size & 1u); // chunks are word-aligned
    }

    if (channels == 0 || channels > 2 || rate <= 0.0 || dataBytes == 0)
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
            return std::isfinite (value) ? static_cast<double> (value) : 0.0;
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
constexpr std::int64_t coarseDecimation = 32;
// C0 is 16.35 Hz, so the longest period a 16' patch on the lowest key produces
// is about 61 ms. The fine search stays inside half of that and cannot cross
// into a neighbouring cycle.
constexpr double fineSearchSeconds = 0.03;

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
                                     double sampleRate)
{
    const auto usable = static_cast<std::int64_t> (
        std::min (reference.size(), candidate.size()));
    const auto wanted = static_cast<std::int64_t> (
        std::llround (alignmentSearchSeconds * sampleRate));
    // A quarter of the pair, so at least half of it always remains as the
    // window the correlation is actually computed over.
    const auto search = std::min (wanted, usable / 4);
    if (search <= 0)
        return std::nullopt;

    // Coarse: the envelopes, decimated. Mean-removed so the correlation
    // responds to the shape of the attack rather than to the standing level.
    const auto referenceEnvelope = amplitudeEnvelope (reference, sampleRate);
    const auto candidateEnvelope = amplitudeEnvelope (candidate, sampleRate);
    std::vector<double> coarseReference;
    std::vector<double> coarseCandidate;
    for (std::int64_t index = 0; index < usable; index += coarseDecimation)
    {
        coarseReference.push_back (referenceEnvelope[static_cast<std::size_t> (index)]);
        coarseCandidate.push_back (candidateEnvelope[static_cast<std::size_t> (index)]);
    }
    if (coarseReference.size() < 2)
        return std::nullopt;
    const auto removeMean = [] (std::vector<double>& series)
    {
        double sum = 0.0;
        for (const auto value : series)
            sum += value;
        const auto mean = sum / static_cast<double> (series.size());
        for (auto& value : series)
            value -= mean;
    };
    removeMean (coarseReference);
    removeMean (coarseCandidate);

    const auto coarse = correlateForLag (coarseReference, coarseCandidate,
                                         search / coarseDecimation, 0)
                      * coarseDecimation;

    // Fine: samples, bounded to less than half a period of the lowest note.
    const auto fine = std::max<std::int64_t> (
        coarseDecimation,
        static_cast<std::int64_t> (std::llround (fineSearchSeconds * sampleRate)));
    return correlateForLag (reference, candidate, fine, coarse);
}

// RMS over the windows within `levelGateDb` of the loudest, so silence between
// notes cannot drag the measurement toward the noise floor.
double gatedRms (const std::vector<double>& samples, double sampleRate)
{
    const auto window = static_cast<std::size_t> (std::llround (0.05 * sampleRate));
    if (samples.size() < window || window == 0)
        return 0.0;

    std::vector<double> windows;
    for (std::size_t start = 0; start + window <= samples.size(); start += window)
    {
        double sum = 0.0;
        for (std::size_t index = start; index < start + window; ++index)
            sum += samples[index] * samples[index];
        windows.push_back (std::sqrt (sum / static_cast<double> (window)));
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

struct Spectrum
{
    std::array<double, harmonicCount> harmonicDb {};
    double centroidHz = 0.0;
};

Spectrum measureSpectrum (std::span<const double> samples, double sampleRate,
                          double fundamentalHz)
{
    Spectrum spectrum;
    double weighted = 0.0;
    double total = 0.0;
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
        weighted += frequency * projection.amplitude * projection.amplitude;
        total += projection.amplitude * projection.amplitude;
    }

    const auto first = amplitude[0];
    for (int index = 0; index < harmonicCount; ++index)
        spectrum.harmonicDb[static_cast<std::size_t> (index)] =
            first > 0.0 ? toDecibels (amplitude[static_cast<std::size_t> (index)] / first)
                        : meterFloorDb;
    spectrum.centroidHz = total > 0.0 ? weighted / total : 0.0;
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
        // Anything already sounding when this stroke begins contaminates it.
        if (other.onSeconds <= stroke->onSeconds && other.offSeconds > stroke->onSeconds)
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
    double decayMs = 0.0;    // from the peak down to the sustain plateau
    double sustainDb = 0.0;  // the plateau, relative to the peak
    double releaseMs = 0.0;  // from key-up down to -40 dB of the peak
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
    result.releaseMs = 1000.0 * static_cast<double> (releaseIndex - keyUp) / sampleRate;
    return result;
}

double channelCorrelation (const Capture& capture)
{
    double dot = 0.0;
    double leftEnergy = 0.0;
    double rightEnergy = 0.0;
    for (std::size_t index = 0; index < capture.left.size(); ++index)
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

std::optional<double> noiseFloorDb (const std::vector<double>& samples,
                                    double sampleRate)
{
    const auto window = static_cast<std::size_t> (std::llround (0.1 * sampleRate));
    if (window == 0 || samples.size() < window)
        return std::nullopt;
    const auto silence =
        youknow::oversampling_quality::decibelsToAmplitude (digitalSilenceDb);

    double quietest = std::numeric_limits<double>::max();
    bool found = false;
    for (std::size_t start = 0; start + window <= samples.size(); start += window / 2)
    {
        double sum = 0.0;
        for (std::size_t index = start; index < start + window; ++index)
            sum += samples[index] * samples[index];
        const auto rms = std::sqrt (sum / static_cast<double> (window));
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
std::optional<Comparison> compare (const ReferenceCase& item, const Capture& reference,
                                   const Capture& rendered, double lagFrames,
                                   double trimDb, std::string& why)
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

    {
        const std::span<const double> referenceWindow (referenceMono.data() + from, to - from);
        const std::span<const double> renderedWindow (renderedMono.data() + from, to - from);
        const auto nominal = 440.0 * std::pow (2.0, (item.analysisNote - 69) / 12.0);

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
        result.spectrumMeasured = true;
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
        }
    }

    const auto referenceFloor = noiseFloorDb (referenceMono, reference.sampleRate);
    const auto renderedFloor = noiseFloorDb (renderedMono, rendered.sampleRate);
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
        result.correlationError = channelCorrelation (rendered) - channelCorrelation (reference);
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
    const auto identityResult = compare (item, baseline, baseline, 0.0, 0.0, why);
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
        const auto result = *compare (item, baseline, shifted, 0.0, 0.0, why);
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
        const auto result = *compare (item, baseline, shifted, 0.0, 0.0, why);
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
        const auto result = *compare (item, baseline, shifted, 0.0, 0.0, why);
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
        const auto result = *compare (item, baseline, shifted, 0.0, 0.0, why);
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
        const auto result = *compare (item, baseline, shifted, 0.0, 0.0, why);
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
        const auto result = *compare (item, baseline, shifted, 0.0, 0.0, why);
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
        const auto result = *compare (item, baseline, shifted, 0.0, 0.0, why);
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
        const auto result = *compare (item, baseline, shifted, 0.0, 0.0, why);
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
        const auto result = *compare (item, baseline, shifted, 0.0, 0.0, why);
        if (result.correlationError < 0.05)
        {
            std::fprintf (stderr,
                          "self-test: switching the chorus off moved the channel "
                          "correlation only %.4f\n", result.correlationError);
            return 1;
        }
    }

    // A known lag must be recovered by the alignment search.
    {
        const auto lagFrames = static_cast<std::size_t> (std::llround (0.037 * rate));
        Capture delayed = baseline;
        delayed.left.insert (delayed.left.begin(), lagFrames, 0.0);
        delayed.right.insert (delayed.right.begin(), lagFrames, 0.0);
        const auto found = bestLag (monoOf (baseline), monoOf (delayed), rate);
        if (! found.has_value()
            || std::abs (*found - static_cast<std::int64_t> (lagFrames)) > 2)
        {
            std::fprintf (stderr,
                          "self-test: a %zu-frame lag was found at %s\n",
                          lagFrames,
                          found.has_value() ? std::to_string (*found).c_str() : "nothing");
            return 1;
        }
    }

    // A capture too short to align must say so rather than reporting a zero
    // lag it never measured.
    {
        Capture stub;
        stub.sampleRate = rate;
        stub.left.assign (3, 0.5);
        stub.right.assign (3, 0.5);
        if (bestLag (monoOf (stub), monoOf (stub), rate).has_value())
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
        const auto trim = toDecibels (gatedRms (monoOf (baseline), rate)
                                      / gatedRms (monoOf (quieter), rate));
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
        const auto result = compare (item, mono, baseline, 0.0, 0.0, why);
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
        if (compare (broken, baseline, baseline, 0.0, 0.0, why).has_value())
        {
            std::fprintf (stderr,
                          "self-test: an analysis window past the end was graded\n");
            return 1;
        }
        broken = item;
        broken.analysisToSeconds = broken.analysisFromSeconds;
        if (compare (broken, baseline, baseline, 0.0, 0.0, why).has_value())
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
        const auto floor = noiseFloorDb (monoOf (padded), rate);
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
        const auto result = compare (crowded, baseline, baseline, 0.0, 0.0, why);
        if (! result.has_value() || result->envelopeMeasured)
        {
            std::fprintf (stderr,
                          "self-test: an overlapped note was still timed\n");
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
                                  capture->sampleRate);
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

        const auto referenceLevel = gatedRms (monoOf (*capture), capture->sampleRate);
        const auto renderedLevel = gatedRms (monoOf (rendered), rendered.sampleRate);
        const auto trim = renderedLevel > 0.0 ? referenceLevel / renderedLevel : 1.0;
        for (auto& sample : rendered.left)
            sample *= trim;
        for (auto& sample : rendered.right)
            sample *= trim;

        std::string why;
        auto comparison = compare (item, *capture, rendered,
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
        std::printf ("  pitch       %+.2f cents\n", last.centsError);
        std::printf ("  harmonics   worst H%d at %+.2f dB\n",
                     last.worstHarmonic, last.worstHarmonicDb);
        std::printf ("  brightness  centroid ratio %.4f (%+.2f dB)\n",
                     last.centroidRatio,
                     last.centroidRatio > 0.0 ? 20.0 * std::log10 (last.centroidRatio) : 0.0);
        if (last.envelopeMeasured)
            std::printf ("  envelope    attack %+.1f ms, decay %+.1f ms, "
                         "sustain %+.2f dB, release %+.1f ms\n",
                         last.attackErrorMs, last.decayErrorMs, last.sustainErrorDb,
                         last.releaseErrorMs);
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
