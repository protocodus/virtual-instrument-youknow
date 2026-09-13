// Renders one factory preset through the shipping engine on a fixed score,
// for panel-matched comparison against another instrument playing the same
// tone bytes and the same notes.
//
// The score is deliberately mixed: sustained single notes, a melody, two
// chords and a cutoff slider swept across a held chord, so one take exposes
// level, envelope timing, polyphony and the filter's own trajectory rather
// than only one of them.
//
// The project's per-preset VR1 volume trim is not applied. The physical volume
// knob exists, but its position in a public recording is usually unknown.
// Use one fixed full-volume comparison setup and match playback gain later.
//
// Optional --score <file> replaces the mixed score with a reference passage.
// Lines are `note <on seconds> <off seconds> <MIDI key>` or `end <seconds>`;
// # comments may record the source and transcription uncertainty. No slider
// sweep is added in this mode. --sample-rate 96000 requests a 96 kHz export
// at the maximum quality ceiling (the engine then runs internally at 192 kHz).
// These are raw float comparison renders, with a sidecar recording tone bytes
// and actual processing settings. Match only playback gain during analysis;
// a public MP3 with undocumented controls is not a calibrated circuit target.

#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowPresets.h"
#include "DSP/YouKnowProductFidelity.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace youknow;

double renderRate = 48000.0;

void writeFloatWav(const std::string& path, const std::vector<float>& left,
                   const std::vector<float>& right)
{
    const std::uint32_t frames = static_cast<std::uint32_t>(left.size());
    const std::uint32_t dataBytes = frames * 2u * 4u;
    std::vector<std::uint8_t> f;
    auto tag = [&f](const char* t) { for (int i = 0; i < 4; ++i) f.push_back((std::uint8_t)t[i]); };
    auto le = [&f](std::uint32_t v, int n) {
        for (int i = 0; i < n; ++i) f.push_back((std::uint8_t)((v >> (8 * i)) & 0xff));
    };
    tag("RIFF"); le(36u + dataBytes, 4); tag("WAVE");
    tag("fmt "); le(16u, 4); le(3u, 2); le(2u, 2);
    le((std::uint32_t)renderRate, 4); le((std::uint32_t)renderRate * 8u, 4);
    le(8u, 2); le(32u, 2);
    tag("data"); le(dataBytes, 4);
    for (std::uint32_t i = 0; i < frames; ++i)
        for (int c = 0; c < 2; ++c) {
            const float v = c == 0 ? left[i] : right[i];
            std::uint32_t bits; std::memcpy(&bits, &v, 4); le(bits, 4);
        }
    std::ofstream out(path, std::ios::binary);
    out.write((const char*)f.data(), (std::streamsize)f.size());
    out.close();
    if (!out) throw std::runtime_error("cannot write WAV: " + path);
}

struct Note { int note; double on; double off; };
struct Move { double t; float cutoff; };

EngineParameters parametersFor(const sysex::Patch& patch)
{
    EngineParameters p {};
    p.lfoRate = patch.lfoRate; p.lfoDelay = patch.lfoDelay;
    p.dcoLfoDepth = patch.dcoLfo; p.pwmDepth = patch.pwm;
    p.pwmSource = patch.pwmSource; p.range = patch.range;
    p.sawEnabled = patch.saw; p.pulseEnabled = patch.pulse;
    p.subLevel = patch.sub; p.noiseLevel = patch.noise;
    p.highPass = patch.highPass; p.cutoff = patch.cutoff;
    p.resonance = patch.resonance; p.envPolarity = patch.envPolarity;
    p.envDepth = patch.vcfEnv; p.vcfLfoDepth = patch.vcfLfo;
    p.keyFollow = patch.keyFollow; p.vcaMode = patch.vcaMode;
    p.vcaLevel = patch.vcaLevel; p.attack = patch.attack;
    p.decay = patch.decay; p.sustain = patch.sustain;
    p.release = patch.release; p.chorus = patch.chorus;
    p.volume = 1.0f; p.polyphony = 6; p.calibration = 1.0f;
    ProductFidelityProfile::applyTo(p);
    return p;
}
} // namespace

int main(int argc, char** argv)
{
  try {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <preset e.g. A11> <out.wav> "
                            "[--score notes.txt] [--sample-rate 96000]\n", argv[0]);
        return 2;
    }
    std::string scorePath;
    for (int i = 3; i < argc; ++i) {
        const std::string option = argv[i];
        if (i + 1 >= argc) throw std::runtime_error("missing option value");
        if (option == "--score") scorePath = argv[++i];
        else if (option == "--sample-rate") {
            const std::string value = argv[++i];
            std::size_t used = 0;
            renderRate = std::stod(value, &used);
            if (used != value.size()
                || (renderRate != 44100 && renderRate != 48000
                    && renderRate != 88200 && renderRate != 96000
                    && renderRate != 176400 && renderRate != 192000))
                throw std::runtime_error("unsupported sample rate");
        } else throw std::runtime_error("unknown option: " + option);
    }
    const auto* preset = presets::findByNumber(argv[1]);
    if (preset == nullptr) {
        std::fprintf(stderr, "unknown preset %s\n", argv[1]);
        return 2;
    }

    // The shared score. Times in seconds.
    std::vector<Note> notes;
    notes.push_back({ 48, 0.5, 2.5 });                 // a sustained low note
    notes.push_back({ 60, 3.0, 5.0 });                 // a sustained middle note
    const int melody[] = { 60, 63, 67, 70, 72, 70, 67, 63 };
    for (int i = 0; i < 8; ++i)                        // a melody, 0.45 s apart
        notes.push_back({ melody[i], 5.5 + 0.45 * i, 5.5 + 0.45 * i + 0.4 });
    for (int n : { 48, 55, 60, 64 })                   // a four-note chord
        notes.push_back({ n, 9.5, 12.5 });
    for (int n : { 45, 52, 57, 61 })                   // a second voicing
        notes.push_back({ n, 13.0, 16.0 });
    for (int n : { 36, 48, 55 })                       // held under the sweep
        notes.push_back({ n, 16.5, 23.5 });
    double endSeconds = 26.0;

    // The cutoff slider swept up and back across the held chord, 100 steps,
    // which is what a player's hand on the FREQ slider actually produces.
    std::vector<Move> moves;
    for (int i = 0; i <= 100; ++i) {
        const double t = 17.0 + 6.0 * i / 100.0;
        const double phase = i / 100.0;
        const float value = (float)(phase < 0.5 ? preset->patch.cutoff
                                        + (1.0 - preset->patch.cutoff) * (phase * 2.0)
                                      : 1.0 - (1.0 - preset->patch.cutoff)
                                        * ((phase - 0.5) * 2.0));
        moves.push_back({ t, value });
    }

    if (!scorePath.empty()) {
        notes.clear(); moves.clear(); endSeconds = 0;
        std::ifstream score(scorePath);
        if (!score) throw std::runtime_error("cannot open score: " + scorePath);
        std::string line;
        int lineNumber = 0;
        while (std::getline(score, line)) {
            ++lineNumber;
            line = line.substr(0, line.find('#'));
            std::istringstream fields(line);
            std::string kind, extra;
            if (!(fields >> kind)) continue;
            bool valid = false;
            if (kind == "note") {
                Note n {};
                valid = bool(fields >> n.on >> n.off >> n.note)
                    && std::isfinite(n.on) && std::isfinite(n.off)
                    && n.on >= 0 && n.off > n.on && n.off <= 600
                    && std::llround(n.off * renderRate) > std::llround(n.on * renderRate)
                    && n.note >= 0 && n.note <= 127;
                if (valid) notes.push_back(n);
            } else if (kind == "end") {
                valid = bool(fields >> endSeconds) && std::isfinite(endSeconds)
                    && endSeconds > 0 && endSeconds <= 600;
            }
            if (!valid || (fields >> extra))
                throw std::runtime_error("invalid score line " + std::to_string(lineNumber));
        }
        if (notes.empty()) throw std::runtime_error("empty score");
        double lastOff = 0;
        for (const auto& n : notes) lastOff = std::max(lastOff, n.off);
        if (endSeconds > 0 && endSeconds < lastOff)
            throw std::runtime_error("score ends before its notes are released");
        endSeconds = std::max(endSeconds, lastOff
            + YouKnowEngine::envelopeReleaseSeconds(preset->patch.release) + 0.5);
    }

    auto engine = std::make_unique<YouKnowEngine>();
    ProductFidelityProfile::configureBeforePrepare(*engine);
    engine->selectConverterTimingProfile(
        YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
    engine->prepare(renderRate, 256, 4);
    auto parameters = parametersFor(preset->patch);
    parameters.vcfTanhMode = VcfTanhMode::Exact;
    parameters.vcfSolverMode = VcfSolverMode::MersonHalfSteps;
    parameters.aging = 0;
    engine->setParameters(parameters);
    engine->setPitchBend(0);
    engine->setModWheel(0);

    struct Event { double t; int kind; int note; float cutoff; };
    std::vector<Event> events;
    for (const auto& n : notes) {
        events.push_back({ n.on, 0, n.note, 0.0f });
        events.push_back({ n.off, 1, n.note, 0.0f });
    }
    for (const auto& m : moves) events.push_back({ m.t, 2, 0, m.cutoff });
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        return a.t != b.t ? a.t < b.t : a.kind == 1 && b.kind != 1;
    });
    std::set<int> held;
    int heldPeak = 0;
    for (const auto& e : events) {
        if (e.kind == 0) {
            if (!held.insert(e.note).second) throw std::runtime_error("overlapping duplicate key");
            heldPeak = std::max(heldPeak, static_cast<int>(held.size()));
            if (heldPeak > 6) throw std::runtime_error("score exceeds six held keys");
        } else if (e.kind == 1 && held.erase(e.note) != 1)
            throw std::runtime_error("unmatched note release");
    }

    std::vector<float> left, right;
    std::array<float, 256> bl {}, br {};
    std::size_t next = 0;
    if (!scorePath.empty())
        for (int i = 0; i < static_cast<int>(0.37 * renderRate); i += 256)
            engine->process(bl.data(), br.data(), 256);
    std::int64_t rendered = 0;
    const auto frameAt = [](double time) { return static_cast<std::int64_t>(std::llround(time * renderRate)); };
    const auto totalFrames = frameAt(endSeconds);
    int activePeak = 0;
    while (rendered < totalFrames) {
        while (next < events.size() && frameAt(events[next].t) <= rendered) {
            const auto& e = events[next++];
            if (e.kind == 0) engine->noteOn(e.note, 1.0f);
            else if (e.kind == 1) engine->noteOff(e.note);
            else { parameters.cutoff = e.cutoff; engine->setParameters(parameters); }
        }
        const auto boundary = next < events.size()
            ? std::min(frameAt(events[next].t), totalFrames) : totalFrames;
        const auto count = static_cast<int>(std::min<std::int64_t>(256, boundary - rendered));
        if (count <= 0) throw std::runtime_error("non-progressing score");
        engine->process(bl.data(), br.data(), count);
        left.insert(left.end(), bl.begin(), bl.begin() + count);
        right.insert(right.end(), br.begin(), br.begin() + count);
        activePeak = std::max(activePeak, engine->getActiveVoiceCount());
        rendered += count;
    }

    double peak = 0.0;
    for (const auto* channel : {&left, &right})
        for (float v : *channel) {
            if (!std::isfinite(v)) throw std::runtime_error("nonfinite audio");
            peak = std::max(peak, static_cast<double>(std::abs(v)));
        }
    if (peak < 1e-6 || activePeak == 0 || activePeak > 6)
        throw std::runtime_error("invalid audio/voice count");
    if (!scorePath.empty() && engine->getActiveVoiceCount() != 0)
        throw std::runtime_error("reference score has an unfinished release");
    writeFloatWav(argv[2], left, right);
    std::uint8_t tone[sysex::toneByteCount] {};
    sysex::toneBytesFromPatch(preset->patch, tone);
    std::ofstream manifest(std::string(argv[2]) + ".json");
    manifest << std::setprecision(10)
        << "{\n  \"preset\": " << std::quoted(preset->number)
        << ", \"sample_rate\": " << renderRate
        << ", \"quality_selected\": 4, \"oversampling_applied\": " << engine->getOversamplingFactor()
        << ", \"internal_rate\": " << renderRate * engine->getOversamplingFactor()
        << ",\n  \"vcf_tanh\": \"Exact\", \"vcf_solver\": \"MersonHalfSteps\","
           " \"aging\": 0, \"unit_character\": 1, \"volume\": 1,"
           " \"key_mode\": \"Poly1\", \"portamento\": 0, \"pitch_bend\": 0,"
           " \"mod_wheel\": 0, \"master_tune_cents\": 0,"
           " \"factory_tone_bytes\": [";
    for (int i = 0; i < sysex::toneByteCount; ++i) {
        if (i) manifest << ", ";
        manifest << static_cast<int>(tone[i]);
    }
    manifest << "],\n  \"seconds\": " << left.size() / renderRate
             << ", \"peak_held_keys\": " << heldPeak
             << ", \"peak_active_voices\": " << activePeak
             << ", \"raw_peak_dbfs\": " << 20 * std::log10(peak)
             << ", \"score\": " << std::quoted(scorePath) << "\n}\n";
    manifest.close();
    if (!manifest) throw std::runtime_error("cannot write score manifest");
    std::printf("%-4s %-28s %6.2f s  peak %7.2f dBFS  internal %.0f Hz\n", preset->number,
                preset->name, left.size() / renderRate,
                peak > 0 ? 20 * std::log10(peak) : -144.0,
                renderRate * engine->getOversamplingFactor());
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Preset score: %s\n", e.what());
    return 1;
  }
}
