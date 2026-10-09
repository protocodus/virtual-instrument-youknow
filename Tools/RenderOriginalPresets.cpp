// Raw performances for the sixteen authored product presets. Tone and saved
// performance controls come from PresetScores.h; the Python companion owns
// composition, MIDI, static gain and delivery formats. Scores use beats:
// tempo BPM / length BEATS / gate BEAT DURATION KEY MIDI_VELOCITY.
#include "PresetScores.h"

#include <bit>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>

namespace
{
using namespace youknow;
constexpr int sampleRate = 96000;
constexpr int blockSize = 256;
constexpr double leadInSeconds = 0.05;

void check(bool ok, const std::string& message)
{
    if (!ok) throw std::runtime_error(message);
}

struct Note { double beat {}, duration {}; int key {}, velocity {}; };
struct Score { double tempo {}, beats {}; std::vector<Note> notes; };
struct Gate { std::int64_t frame {}; int key {}, velocity {}; };

const presets::Preset& productPreset(const char* slot)
{
    for (const auto& preset : presets::productBank())
        if (std::string(slot) == preset.number) return preset;
    throw std::runtime_error("Unknown original preset: " + std::string(slot));
}

std::vector<Gate> gatesFor(const Score& score)
{
    const auto frame = [&](double beat) {
        return static_cast<std::int64_t>(std::llround(
            sampleRate * (leadInSeconds + beat * 60.0 / score.tempo)));
    };
    std::vector<Gate> gates;
    for (const auto& note : score.notes)
    {
        gates.push_back({frame(note.beat), note.key, note.velocity});
        gates.push_back({frame(note.beat + note.duration), note.key, 0});
    }
    std::sort(gates.begin(), gates.end(), [](const auto& a, const auto& b) {
        if (a.frame != b.frame) return a.frame < b.frame;
        return a.velocity == 0 && b.velocity != 0;
    });
    return gates;
}

Score readScore(std::istream& input, const EngineParameters& parameters)
{
    Score score;
    bool hasTempo = false, hasLength = false;
    std::string line;
    while (std::getline(input, line))
    {
        check(line.size() <= 1024, "Score line is too long");
        std::istringstream fields(line);
        std::string kind, extra;
        if (!(fields >> kind) || kind.front() == '#') continue;
        if (kind == "tempo")
        {
            check(!hasTempo, "Duplicate tempo");
            fields >> score.tempo;
            hasTempo = true;
        }
        else if (kind == "length")
        {
            check(!hasLength, "Duplicate length");
            fields >> score.beats;
            hasLength = true;
        }
        else if (kind == "gate")
        {
            Note note;
            fields >> note.beat >> note.duration >> note.key >> note.velocity;
            check(std::isfinite(note.beat) && std::isfinite(note.duration)
                && note.beat >= 0.0 && note.beat <= 64.0
                && note.duration >= 0.005 && note.duration <= 64.0
                && note.key >= 0 && note.key <= 127
                && note.velocity >= 1 && note.velocity <= 127,
                "Invalid gate: " + line);
            score.notes.push_back(note);
            check(score.notes.size() <= 4096, "Too many notes");
        }
        else throw std::runtime_error("Unknown score command: " + kind);
        check(!fields.fail() && !(fields >> extra), "Malformed score line: " + line);
    }
    check(input.eof() && hasTempo && hasLength && !score.notes.empty(),
          "Score needs tempo, length and notes");
    check(std::isfinite(score.tempo) && score.tempo >= 30.0 && score.tempo <= 240.0
        && std::isfinite(score.beats) && score.beats > 0.0 && score.beats <= 64.0,
        "Tempo or length outside supported bounds");
    for (const auto& note : score.notes)
        check(note.beat + note.duration <= score.beats + 1e-8,
              "A note extends past the score length");
    std::array<bool, 128> held {};
    int count = 0;
    // Unison is a last-note monophonic keyboard, so overlapping different
    // keys are valid legato input even when it uses one physical voice.
    const int heldLimit = parameters.keyMode == KeyMode::Unison ? 128 : parameters.polyphony;
    for (const auto& gate : gatesFor(score))
    {
        const bool on = gate.velocity != 0;
        auto& key = held[static_cast<std::size_t>(gate.key)];
        check(key != on, "Overlapping or unmatched same-key note gates");
        key = on;
        count += on ? 1 : -1;
        check(count <= heldLimit, "Score exceeds preset polyphony");
    }
    check(count == 0, "Score leaves keys held");
    return score;
}

Score loadScore(const char* path, const EngineParameters& parameters)
{
    std::ifstream input(path);
    check(input.good(), "Cannot open score: " + std::string(path));
    return readScore(input, parameters);
}

void render(const presets::Preset& preset, const Score& score,
            const std::filesystem::path& path, double settleSeconds = 0.25)
{
    static_assert(std::endian::native == std::endian::little,
                  "Raw output uses little-endian IEEE float");
    const auto parameters = preset_demos::loadPresetParameters(preset.number);
    auto engine = std::make_unique<YouKnowEngine>();
    ActiveProductFidelityProfile::configureBeforePrepare(*engine);
    engine->selectConverterTimingProfile(YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
    engine->prepare(sampleRate, blockSize, YouKnowEngine::maximumOversampleFactor);
    engine->setParameters(parameters);
    engine->setPitchBend(0.0f);
    engine->setModWheel(0.0f);
    check(engine->getRequestedOversamplingFactor() == 4 && engine->getOversamplingFactor() == 2,
          "Expected maximum quality at a 192 kHz internal rate");
    std::array<float, blockSize> left {}, right {};
    for (int remaining = static_cast<int>(sampleRate * settleSeconds); remaining > 0;)
    {
        const int count = std::min(remaining, blockSize);
        engine->process(left.data(), right.data(), count);
        remaining -= count;
    }
    const double tail = std::max(0.8, YouKnowEngine::envelopeReleaseSeconds(parameters.release) + 0.5);
    const auto frames = static_cast<std::int64_t>(std::llround(sampleRate *
        (leadInSeconds + score.beats * 60.0 / score.tempo + tail)));
    auto gates = gatesFor(score);
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    check(output.good(), "Cannot open raw audio output");
    std::array<float, 2 * blockSize> stereo {};
    std::size_t next = 0;
    double peak = 0.0, energy = 0.0;
    int activePeak = 0;
    for (std::int64_t frame = 0; frame < frames;)
    {
        while (next < gates.size() && gates[next].frame == frame)
        {
            const auto& gate = gates[next++];
            if (gate.velocity != 0) engine->noteOn(gate.key, gate.velocity / 127.0f);
            else engine->noteOff(gate.key);
        }
        auto count = std::min<std::int64_t>(blockSize, frames - frame);
        if (next < gates.size()) count = std::min(count, gates[next].frame - frame);
        check(count > 0, "Invalid event schedule");
        engine->process(left.data(), right.data(), static_cast<int>(count));
        activePeak = std::max(activePeak, engine->getActiveVoiceCount());
        for (int i = 0; i < count; ++i)
        {
            const auto index = static_cast<std::size_t>(i);
            const double l = left[index], r = right[index];
            check(std::isfinite(l) && std::isfinite(r), "Non-finite audio");
            stereo[2 * index] = left[index];
            stereo[2 * index + 1] = right[index];
            peak = std::max({peak, std::abs(l), std::abs(r)});
            energy += l * l + r * r;
        }
        output.write(reinterpret_cast<const char*>(stereo.data()), count * 2 * sizeof(float));
        frame += count;
    }
    output.close();
    check(output.good() && next == gates.size(), "Incomplete raw audio");
    check(peak > 1e-7 && activePeak > 0 && activePeak <= parameters.polyphony
          && engine->getActiveVoiceCount() == 0, "Silent audio or unreleased voices");
    std::cout << std::setprecision(12)
        << "{\"slot\":" << std::quoted(preset.number)
        << ",\"frames\":" << frames << ",\"sample_rate\":" << sampleRate
        << ",\"channels\":2,\"quality_requested\":4,\"oversampling_applied\":2"
        << ",\"lead_in_seconds\":" << leadInSeconds << ",\"settle_seconds\":" << settleSeconds
        << ",\"tail_seconds\":" << tail << ",\"active_voice_peak\":" << activePeak
        << ",\"raw_peak_dbfs\":" << 20.0 * std::log10(peak)
        << ",\"raw_rms_dbfs\":" << 10.0 * std::log10(energy / (2.0 * frames))
        << ",\"engine_profile\":\"ProductHardwareRealismProfile\""
        << ",\"performance_timing\":\"Direct / MeasuredChartGeometry\"}\n";
}

int run(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--list")
    {
        std::cout << '[';
        bool first = true;
        for (const auto& preset : presets::productBank())
        {
            if (!first) std::cout << ',';
            first = false;
            const auto p = preset_demos::loadPresetParameters(preset.number);
            const int offset = p.range == DcoRange::Sixteen ? -12 : p.range == DcoRange::Four ? 12 : 0;
            std::cout << "{\"slot\":" << std::quoted(preset.number)
                << ",\"name\":" << std::quoted(preset.name)
                << ",\"category\":" << std::quoted(preset.category == presets::Preset::Category::Bass ? "Bass" : "Pad")
                << ",\"offset\":" << offset << ",\"polyphony\":" << p.polyphony
                << ",\"key_mode\":" << std::quoted(p.keyMode == KeyMode::Unison ? "Unison" : "Poly1")
                << ",\"attack_seconds\":" << YouKnowEngine::envelopeAttackSeconds(p.attack)
                << ",\"release_seconds\":" << YouKnowEngine::envelopeReleaseSeconds(p.release) << '}';
        }
        std::cout << "]\n";
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--smoke")
    {
        const auto& preset = productPreset("YB2");
        const auto parameters = preset_demos::loadPresetParameters(preset.number);
        const std::string valid = "tempo 240\nlength 0.25\ngate 0 0.125 60 96\n";
        std::istringstream input(valid);
        const auto score = readScore(input, parameters);
        for (const auto& invalid : {
                 "tempo 100\nlength 1\ngate 0 nan 60 96\n",
                 "tempo 100\ntempo 100\nlength 1\ngate 0 0.5 60 96\n",
                 "tempo 100\nlength 1\ngate 0 0.5 60 96 extra\n",
                 "tempo 100\nlength 1\ngate 0 0.5 128 96\n",
                 "tempo 100\nlength 1\ngate 0 0.5 60 0\n",
                 "tempo 100\nlength 1\ngate 0 2 60 96\n",
                 "tempo 100\nlength 1\ngate 0 0.8 60 96\ngate 0.5 0.25 60 96\n"})
        {
            bool rejected = false;
            try { std::istringstream bad(invalid); (void) readScore(bad, parameters); }
            catch (const std::runtime_error&) { rejected = true; }
            check(rejected, "Invalid score was accepted");
        }
        const auto path = std::filesystem::path(argv[2]) / "original-preset-smoke.f32";
        render(preset, score, path, 0.0);
        check(std::filesystem::file_size(path) > 0, "Smoke render is empty");
        return 0;
    }
    if (argc == 4 && std::string(argv[1]) == "--check")
    {
        const auto& preset = productPreset(argv[2]);
        (void) loadScore(argv[3], preset_demos::loadPresetParameters(preset.number));
        return 0;
    }
    check(argc == 4, "Usage: YouKnowRenderOriginalPresets SLOT SCORE OUTPUT.f32 | --list | --check SLOT SCORE | --smoke DIR");
    const auto& preset = productPreset(argv[1]);
    render(preset, loadScore(argv[2], preset_demos::loadPresetParameters(preset.number)), argv[3]);
    return 0;
}
}

int main(int argc, char** argv)
{
    try { return run(argc, argv); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
