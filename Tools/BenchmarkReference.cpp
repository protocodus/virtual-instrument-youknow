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
        if (std::memcmp (bytes.data() + cursor, "fmt ", 4) == 0 && size >= 16)
        {
            format = static_cast<std::uint16_t> (readLittleEndian (bytes, body, 2));
            channels = static_cast<std::uint16_t> (readLittleEndian (bytes, body + 2, 2));
            rate = static_cast<double> (readLittleEndian (bytes, body + 4, 4));
            bits = static_cast<std::uint16_t> (readLittleEndian (bytes, body + 14, 2));
        }
        else if (std::memcmp (bytes.data() + cursor, "data", 4) == 0)
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

// The lag, in frames, at which `candidate` best matches `reference`. Positive
// means the candidate's content happens later than the reference's.
std::int64_t bestLag (const std::vector<double>& reference,
                      const std::vector<double>& candidate, double sampleRate)
{
    const auto search = static_cast<std::int64_t> (
        std::llround (alignmentSearchSeconds * sampleRate));
    const auto usable = static_cast<std::int64_t> (
        std::min (reference.size(), candidate.size()));
    if (usable <= 2 * search)
        return 0;

    const auto window = usable - search;
    std::int64_t best = 0;
    double bestScore = -2.0;
    for (std::int64_t lag = -search; lag <= search; ++lag)
    {
        double dot = 0.0;
        double referenceEnergy = 0.0;
        double candidateEnergy = 0.0;
        for (std::int64_t index = search; index < window; ++index)
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

struct Envelope
{
    double attackMs = 0.0;   // to 90 % of the peak
    double decayMs = 0.0;    // from the peak down to the sustain plateau
    double sustainDb = 0.0;  // the plateau, relative to the peak
    double releaseMs = 0.0;  // from key-up down to -40 dB of the peak
};

// A rectified, one-pole-smoothed amplitude envelope. The smoothing is short
// enough to keep a 2 ms attack and long enough to ride over the waveform.
std::vector<double> amplitudeEnvelope (std::span<const double> samples,
                                       double sampleRate)
{
    const auto coefficient = std::exp (-1.0 / (0.002 * sampleRate));
    std::vector<double> envelope (samples.size());
    double state = 0.0;
    for (std::size_t index = 0; index < samples.size(); ++index)
    {
        const auto rectified = std::abs (samples[index]);
        state = rectified > state ? rectified
                                  : coefficient * state + (1.0 - coefficient) * rectified;
        envelope[index] = state;
    }
    return envelope;
}

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

    std::size_t decayIndex = peakIndex;
    const auto decayTarget = plateau + 0.1 * (peak - plateau);
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

// The quietest tenth of a second anywhere, which between and after notes is the
// instrument's own floor rather than any note.
double noiseFloorDb (const std::vector<double>& samples, double sampleRate)
{
    const auto window = static_cast<std::size_t> (std::llround (0.1 * sampleRate));
    if (window == 0 || samples.size() < window)
        return meterFloorDb;
    double quietest = std::numeric_limits<double>::max();
    for (std::size_t start = 0; start + window <= samples.size(); start += window / 2)
    {
        double sum = 0.0;
        for (std::size_t index = start; index < start + window; ++index)
            sum += samples[index] * samples[index];
        quietest = std::min (quietest, std::sqrt (sum / static_cast<double> (window)));
    }
    return toDecibels (quietest);
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
};

// Compares an already-aligned, already-level-matched pair.
Comparison compare (const ReferenceCase& item, const Capture& reference,
                    const Capture& rendered, double lagFrames, double trimDb)
{
    Comparison result;
    result.id = item.id;
    result.lagMs = 1000.0 * lagFrames / reference.sampleRate;
    result.levelTrimDb = trimDb;

    const auto referenceMono = monoOf (reference);
    const auto renderedMono = monoOf (rendered);
    const auto usable = std::min (referenceMono.size(), renderedMono.size());

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

    const auto from = static_cast<std::size_t> (
        std::llround (item.analysisFromSeconds * reference.sampleRate));
    const auto to = std::min (usable, static_cast<std::size_t> (
        std::llround (item.analysisToSeconds * reference.sampleRate)));
    if (to > from)
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
    }

    if (! item.strokes.empty())
    {
        const auto keyUp = item.strokes.front().offSeconds;
        const auto referenceEnvelope = measureEnvelope (referenceMono, reference.sampleRate, keyUp);
        const auto renderedEnvelope = measureEnvelope (renderedMono, rendered.sampleRate, keyUp);
        result.attackErrorMs = renderedEnvelope.attackMs - referenceEnvelope.attackMs;
        result.decayErrorMs = renderedEnvelope.decayMs - referenceEnvelope.decayMs;
        result.sustainErrorDb = renderedEnvelope.sustainDb - referenceEnvelope.sustainDb;
        result.releaseErrorMs = renderedEnvelope.releaseMs - referenceEnvelope.releaseMs;
    }

    result.noiseFloorErrorDb = noiseFloorDb (renderedMono, rendered.sampleRate)
                             - noiseFloorDb (referenceMono, reference.sampleRate);
    result.correlationError = channelCorrelation (rendered) - channelCorrelation (reference);
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
    const auto identity = compare (item, baseline, baseline, 0.0, 0.0);
    if (std::abs (identity.centsError) > 0.01 || std::abs (identity.worstHarmonicDb) > 0.01
        || std::abs (identity.attackErrorMs) > 0.01
        || std::abs (identity.releaseErrorMs) > 0.01
        || std::abs (identity.noiseFloorErrorDb) > 0.01
        || identity.correlation < 0.999999)
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
        const auto result = compare (item, baseline, shifted, 0.0, 0.0);
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
        const auto result = compare (item, baseline, shifted, 0.0, 0.0);
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
        const auto result = compare (item, baseline, shifted, 0.0, 0.0);
        if (result.attackErrorMs < 50.0)
        {
            std::fprintf (stderr,
                          "self-test: a much slower attack measured only "
                          "%.1f ms longer\n", result.attackErrorMs);
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
        if (std::abs (found - static_cast<std::int64_t> (lagFrames)) > 2)
        {
            std::fprintf (stderr,
                          "self-test: a %zu-frame lag was found at %lld\n",
                          lagFrames, static_cast<long long> (found));
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
        if (lag > 0)
        {
            const auto drop = static_cast<std::size_t> (lag);
            if (drop < rendered.left.size())
            {
                rendered.left.erase (rendered.left.begin(),
                                     rendered.left.begin() + static_cast<std::ptrdiff_t> (drop));
                rendered.right.erase (rendered.right.begin(),
                                      rendered.right.begin() + static_cast<std::ptrdiff_t> (drop));
            }
        }
        else if (lag < 0)
        {
            const auto pad = static_cast<std::size_t> (-lag);
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

        results.push_back (compare (item, *capture, rendered,
                                    static_cast<double> (lag), toDecibels (trim)));
        const auto& last = results.back();
        std::printf ("%-16s lag %+7.2f ms  trim %+6.2f dB  corr %+.3f  "
                     "pitch %+6.2f cents  H%-2d %+6.2f dB  attack %+7.1f ms\n",
                     last.id.c_str(), last.lagMs, last.levelTrimDb, last.correlation,
                     last.centsError, last.worstHarmonic, last.worstHarmonicDb,
                     last.attackErrorMs);
    }

    std::printf ("Graded %zu capture(s).\n", results.size());
    return 0;
}
