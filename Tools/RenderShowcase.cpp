// Five original musical performances of one YouKnow instance each. Scores and
// visible demo patch choices live in the two score headers; the factory bank
// and DSP model are unchanged. Reproduce with YouKnowRenderShowcase [directory].
//
// Max quality is requested explicitly. At a 96 kHz host the engine caps the
// selected 4x ceiling to 2x, so its actual internal grid is 192 kHz. Exact tanh
// and the reference two-half-step Merson solver remain enabled for every take.
#include "ShowcaseScore.h"
#include "ShowcaseBassLead.h"
#include "ShowcasePolyphonic.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>

namespace
{
using namespace youknow;
using namespace youknow::showcase;
constexpr double rate = 96000.0;
constexpr int blockSize = 256;
constexpr int requestedQuality = 4;
constexpr double peakTarget = 0.7079457843841379; // -3 dBFS
constexpr double prefaceSeconds = 0.15;
constexpr double keyUpGuardSeconds = 0.006;

void require (bool condition, const std::string& message)
{
    if (! condition) throw std::runtime_error (message);
}

int validate (const Demo& d)
{
    const auto fail = [&d] (bool ok, const std::string& why)
    { require (ok, d.category + ": " + why); };
    fail (!d.events.empty() && !d.harmony.empty(), "empty score");
    fail (d.tempo > 0 && d.musicBeats > 0 && d.tailSeconds >= 2, "invalid duration");
    fail (d.parameters.polyphony >= 1 && d.parameters.polyphony <= 6, "voice count");
    fail (d.parameters.masterTuneCents == 0 && d.parameters.keyTranspose % 12 == 0,
          "score tuning is not centered");
    fail (d.parameters.vcfTanhMode == VcfTanhMode::Exact
          && d.parameters.vcfSolverMode == VcfSolverMode::MersonHalfSteps,
          "maximum filter quality is required");
    double nextBeat = 0;
    for (const auto& h : d.harmony)
    {
        fail (std::abs (h.beat - nextBeat) < 1e-8 && h.length > 0, "gap/overlap in harmony");
        fail (h.mask != 0 && (h.mask & ~d.scale) == 0, "chord leaves declared scale");
        nextBeat = h.beat + h.length;
    }
    fail (std::abs (nextBeat - d.musicBeats) < 1e-8, "incomplete harmony timeline");
    struct Edge { double beat; int note; bool on; };
    std::vector<Edge> edges;
    for (const auto& event : d.events)
    {
        const double end = event.beat + event.length;
        fail (std::isfinite (event.beat) && std::isfinite (end)
              && event.beat >= 0 && event.length > 0 && end <= d.musicBeats + 1e-8
              && !event.notes.empty(), "invalid note span");
        fail (!event.passing || (event.length <= 0.5 && event.notes.size() == 1),
              "passing tone must be a short single note");
        for (int note : event.notes)
        {
            fail (note >= 0 && note <= 127, "MIDI note range");
            const auto mask = 1u << (note % 12);
            fail ((d.scale & mask) != 0, "note leaves declared scale");
            for (const auto& h : d.harmony)
                if (!event.passing && event.beat < h.beat + h.length - 1e-8
                    && end > h.beat + 1e-8)
                    fail ((h.mask & mask) != 0,
                          "note " + std::to_string (note) + " at beat "
                          + std::to_string (event.beat) + " conflicts with " + h.name);
            edges.push_back ({event.beat, note, true});
            edges.push_back ({end, note, false});
        }
    }
    std::stable_sort (edges.begin(), edges.end(), [] (const Edge& a, const Edge& b)
    { return a.beat != b.beat ? a.beat < b.beat : a.on < b.on; });
    std::set<int> held;
    int peak = 0;
    const int limit = d.parameters.keyMode == KeyMode::Unison ? 1 : d.parameters.polyphony;
    for (const auto& e : edges)
    {
        if (e.on) fail (held.insert (e.note).second, "same key is pressed twice before release");
        else fail (held.erase (e.note) == 1, "unmatched release");
        peak = std::max (peak, static_cast<int> (held.size()));
        fail (static_cast<int> (held.size()) <= limit, "score would drop a key");
    }
    for (const auto* lane : { &d.cutoff, &d.wheel })
    {
        if (lane->empty()) continue;
        fail (lane->front().beat == 0, "control lane must begin at beat zero");
        double previous = -1;
        for (const auto& p : *lane)
        {
            fail (std::isfinite (p.beat) && p.beat > previous
                  && p.beat <= d.musicBeats && std::isfinite (p.value)
                  && p.value >= 0 && p.value <= 1, "invalid control lane");
            previous = p.beat;
        }
    }
    return peak;
}

float valueAt (const std::vector<CurvePoint>& lane, double beat, float fallback)
{
    if (lane.empty() || beat < lane.front().beat) return fallback;
    for (std::size_t i = 1; i < lane.size(); ++i)
        if (beat < lane[i].beat)
            return lane[i-1].value + static_cast<float> (
                (beat - lane[i-1].beat) / (lane[i].beat - lane[i-1].beat))
                * (lane[i].value - lane[i-1].value);
    return lane.back().value;
}

struct Audio { std::vector<float> left, right; };
struct Result
{
    Demo demo;
    double seconds, rawPeak, rmsDb, normalizationDb, dc;
    int keyPeak, voicePeak, actualFactor;
    double tailSeconds, tailRelativeDb;
};

double peakOf (const Audio& a)
{
    double peak = 0;
    for (std::size_t i = 0; i < a.left.size(); ++i)
    {
        require (std::isfinite (a.left[i]) && std::isfinite (a.right[i]), "Nonfinite render");
        peak = std::max ({peak, std::abs (static_cast<double> (a.left[i])),
                         std::abs (static_cast<double> (a.right[i]))});
    }
    return peak;
}

void writeWav (const std::filesystem::path& path, const Audio& a)
{
    std::ofstream f (path, std::ios::binary);
    require (static_cast<bool> (f), "Cannot open " + path.string());
    const auto le = [&f] (std::uint32_t n, int bytes)
    { for (int i = 0; i < bytes; ++i) f.put (static_cast<char> ((n >> (8*i)) & 255)); };
    const auto size = static_cast<std::uint32_t> (a.left.size() * 6);
    f.write ("RIFF", 4); le (36 + size, 4); f.write ("WAVEfmt ", 8);
    le (16, 4); le (1, 2); le (2, 2); le (static_cast<std::uint32_t> (rate), 4);
    le (static_cast<std::uint32_t> (rate * 6), 4); le (6, 2); le (24, 2);
    f.write ("data", 4); le (size, 4);
    // Deterministic TPDF dither at one 24-bit LSB. It is far below the modeled
    // analog noise, and prevents correlated quantization in the file fades.
    std::uint32_t state = 0x834d719bu;
    const auto uniform = [&state] ()
    {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return static_cast<double> (state) / 4294967296.0;
    };
    for (std::size_t i = 0; i < a.left.size(); ++i)
        for (float value : {a.left[i], a.right[i]})
        {
            const double firstNoise = uniform();
            const double secondNoise = uniform();
            const auto quantized = static_cast<std::int32_t> (
                std::llround (value * 8388607.0 + firstNoise - secondNoise));
            le (static_cast<std::uint32_t> (quantized) & 0xffffffu, 3);
        }
    f.close();
    require (static_cast<bool> (f), "Failed writing " + path.string());
}

Result render (const Demo& d, const std::filesystem::path& directory)
{
    const int keyPeak = validate (d);
    const double secondsPerBeat = 60.0 / d.tempo;
    const auto frameAt = [secondsPerBeat] (double beat)
    { return static_cast<std::int64_t> (std::llround ((prefaceSeconds + beat * secondsPerBeat) * rate)); };
    struct Gate { std::int64_t frame; int note; bool on; };
    std::vector<Gate> gates;
    for (const auto& e : d.events)
    {
        const auto on = frameAt (e.beat);
        const auto off = std::max (on + 1, frameAt (e.beat + e.length)
                                - static_cast<std::int64_t> (keyUpGuardSeconds * rate));
        for (int note : e.notes)
        { gates.push_back ({on, note, true}); gates.push_back ({off, note, false}); }
    }
    std::stable_sort (gates.begin(), gates.end(), [] (const Gate& a, const Gate& b)
    { return a.frame != b.frame ? a.frame < b.frame : a.on < b.on; });
    auto engine = std::make_unique<YouKnowEngine>();
    ProductFidelityProfile::configureBeforePrepare (*engine);
    engine->selectConverterTimingProfile (YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
    engine->prepare (rate, blockSize, requestedQuality);
    auto p = d.parameters;
    engine->setParameters (p);
    engine->setPitchBend (0);
    engine->setModWheel (0);
    require (engine->getRequestedOversamplingFactor() == 4
             && engine->getOversamplingFactor() == 2, "Unexpected Max-quality rate policy");
    // Slow pad releases need longer than a generic four/six-second tail.
    // Bound from a full envelope and allow the chorus/output nodes to settle.
    const double tailSeconds = std::max (d.tailSeconds,
        YouKnowEngine::envelopeReleaseSeconds (p.release) + 0.5);
    const auto frames = frameAt (d.musicBeats) + static_cast<std::int64_t> (std::llround (tailSeconds * rate));
    Audio audio;
    audio.left.resize (static_cast<std::size_t> (frames)); audio.right.resize (audio.left.size());
    std::array<float, blockSize> left {}, right {};
    // Pre-roll the physical states before the recording opens.
    for (int i = 0; i < static_cast<int> (0.37 * rate); i += blockSize)
        engine->process (left.data(), right.data(), blockSize);
    std::size_t next = 0;
    int voicePeak = 0;
    for (std::int64_t frame = 0; frame < frames;)
    {
        while (next < gates.size() && gates[next].frame <= frame)
        {
            const auto& gate = gates[next++];
            if (gate.on) engine->noteOn (gate.note, 1);
            else engine->noteOff (gate.note);
        }
        auto count = std::min<std::int64_t> (blockSize, frames - frame);
        if (next < gates.size()) count = std::min (count, gates[next].frame - frame);
        const double beat = (static_cast<double> (frame) / rate - prefaceSeconds) / secondsPerBeat;
        const float cutoff = valueAt (d.cutoff, beat, d.parameters.cutoff);
        if (cutoff != p.cutoff) { p.cutoff = cutoff; engine->setParameters (p); }
        engine->setModWheel (valueAt (d.wheel, beat, 0));
        engine->process (audio.left.data() + frame, audio.right.data() + frame, static_cast<int> (count));
        voicePeak = std::max (voicePeak, engine->getActiveVoiceCount());
        frame += count;
    }
    require (voicePeak > 0 && voicePeak <= d.parameters.polyphony, "Unexpected active voice count");
    if (d.parameters.keyMode == KeyMode::Unison)
        require (voicePeak == d.parameters.polyphony, "Unison did not use its promised voices");
    require (engine->getActiveVoiceCount() == 0, "A note envelope is still active at the file ending");
    const double rawPeak = peakOf (audio);
    require (rawPeak > 1e-5, "Silent demo");
    // Only recording-boundary fades: 5ms at the head, 150ms after the natural
    // tail. There is no EQ, compression, delay or reverb added to the synth.
    const auto inFrames = static_cast<std::size_t> (0.005 * rate);
    const auto outFrames = static_cast<std::size_t> (0.15 * rate);
    double tailPeak = 0;
    for (std::size_t i = audio.left.size() - outFrames; i < audio.left.size(); ++i)
        tailPeak = std::max ({tailPeak, std::abs (static_cast<double> (audio.left[i])),
                             std::abs (static_cast<double> (audio.right[i]))});
    for (std::size_t i = 0; i < inFrames; ++i)
    { const float g = static_cast<float> (i) / static_cast<float> (inFrames); audio.left[i] *= g; audio.right[i] *= g; }
    for (std::size_t i = 0; i < outFrames; ++i)
    { const auto j = audio.left.size() - 1 - i; const float g = static_cast<float> (i) / static_cast<float> (outFrames); audio.left[j] *= g; audio.right[j] *= g; }
    const double gain = peakTarget / peakOf (audio);
    double energy = 0, dcLeft = 0, dcRight = 0;
    for (std::size_t i = 0; i < audio.left.size(); ++i)
    {
        audio.left[i] = static_cast<float> (audio.left[i] * gain);
        audio.right[i] = static_cast<float> (audio.right[i] * gain);
        energy += static_cast<double> (audio.left[i]) * audio.left[i]
                + static_cast<double> (audio.right[i]) * audio.right[i];
        dcLeft += audio.left[i]; dcRight += audio.right[i];
    }
    const double dc = std::max (std::abs (dcLeft), std::abs (dcRight)) / audio.left.size();
    require (dc < 0.005 && peakOf (audio) < 0.71, "Delivery level/DC check failed");
    writeWav (directory / d.filename, audio);
    return { d, frames / rate, rawPeak,
             10.0 * std::log10 (energy / (2.0 * audio.left.size())),
             20.0 * std::log10 (gain), dc, keyPeak, voicePeak, engine->getOversamplingFactor(),
             tailSeconds, 20.0 * std::log10 (std::max (tailPeak / rawPeak, 1e-18)) };
}

void manifest (const std::filesystem::path& directory, const std::vector<Result>& results)
{
    std::ofstream out (directory / "showcase-manifest.json");
    out << std::setprecision (9)
        << "{\n  \"sample_rate\": 96000, \"bits\": 24, \"channels\": 2,\n"
           "  \"quality_selected\": 4, \"oversampling_applied\": 2, \"internal_rate\": 192000,\n"
           "  \"vcf_tanh\": \"Exact\", \"vcf_solver\": \"MersonHalfSteps\",\n"
           "  \"peak_dbfs\": -3, \"dither\": \"24-bit TPDF\",\n  \"demos\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i)
    {
        const auto& r = results[i]; const auto& d = r.demo;
        out << "    {\"file\": " << std::quoted (d.filename) << ", \"title\": " << std::quoted (d.title)
            << ", \"category\": " << std::quoted (d.category) << ", \"key\": " << std::quoted (d.key)
            << ", \"preset_base\": " << std::quoted (d.preset)
            << ", \"description\": " << std::quoted (d.description)
            << ", \"tempo\": " << d.tempo << ", \"seconds\": " << r.seconds
            << ", \"peak_held_keys\": " << r.keyPeak << ", \"peak_active_voices\": " << r.voicePeak
            << ", \"rms_dbfs\": " << r.rmsDb << ", \"normalization_db\": " << r.normalizationDb
            << ", \"max_dc\": " << r.dc << ", \"chords\": [";
        for (std::size_t h = 0; h < d.harmony.size(); ++h)
        { if (h) out << ", "; out << std::quoted (d.harmony[h].name); }
        out << "], \"tail_seconds\": " << r.tailSeconds
            << ", \"pre_fade_tail_relative_db\": " << r.tailRelativeDb
            << "}" << (i + 1 == results.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
    out.close();
    require (static_cast<bool> (out), "Cannot write showcase manifest");
}
} // namespace

int main (int argc, char** argv)
{
    try
    {
        bool check = false;
        std::filesystem::path directory = "Docs/audio";
        for (int i = 1; i < argc; ++i)
            if (std::string (argv[i]) == "--check") check = true;
            else directory = argv[i];
        const std::vector<Demo> demos { bassDemo(), leadDemo(), stringsDemo(), brassDemo(), padDemo() };
        for (const auto& d : demos)
            std::cout << d.category << ": " << validate (d) << " held keys, "
                      << d.parameters.polyphony << " voices, " << d.key << ", " << d.tempo << " BPM\n";
        // Prove that MIDI membership is not the only score guard.
        auto invalid = demos.front(); invalid.parameters.masterTuneCents = 25;
        bool rejected = false;
        try { validate (invalid); } catch (const std::exception&) { rejected = true; }
        require (rejected, "Hidden detune was not rejected");
        invalid = demos.front(); invalid.events.front().notes.push_back (invalid.events.front().notes.front());
        rejected = false;
        try { validate (invalid); } catch (const std::exception&) { rejected = true; }
        require (rejected, "Repeated simultaneous key was not rejected");
        if (check) { std::cout << "All showcase score/quality checks passed.\n"; return 0; }
        std::filesystem::create_directories (directory);
        std::vector<std::future<Result>> jobs;
        for (const auto& d : demos)
            jobs.push_back (std::async (std::launch::async, [d, directory] { return render (d, directory); }));
        std::vector<Result> results;
        for (auto& job : jobs)
        {
            results.push_back (job.get());
            const auto& r = results.back();
            std::cout << "Rendered " << r.demo.filename << ": " << r.seconds << " s, "
                      << r.voicePeak << " active voices, " << r.rmsDb << " dBFS RMS\n" << std::flush;
        }
        manifest (directory, results);
        return 0;
    }
    catch (const std::exception& e)
    { std::cerr << "Showcase: " << e.what() << '\n'; return 1; }
}
