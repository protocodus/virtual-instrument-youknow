// Raw, unmastered performances of the original YouKnow bank. The companion
// MakeOriginalPresetDemos.py owns the compositions, MIDI and delivery masters.
// Score format: tempo BPM / end BEAT / note BEAT LENGTH MIDI_KEY VELOCITY.
// Max selects 4x; the shipping rate policy applies 2x at a 96 kHz host, giving
// a 192 kHz internal grid. Exact tanh and two-half-step Merson stay enabled.
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
constexpr double rate = 96000.0;
constexpr int blockSize = 256;
constexpr double leadIn = 0.08;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

int run(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--list")
    {
        for (const auto& preset : presets::productBank())
        {
            const auto& p = preset.patch;
            const int offset = p.range == DcoRange::Sixteen ? -12
                             : p.range == DcoRange::Four ? 12 : 0;
            std::cout << preset.number << '\t' << preset.name << '\t' << offset
                      << '\t' << YouKnowEngine::envelopeAttackSeconds(p.attack)
                      << '\t' << YouKnowEngine::envelopeReleaseSeconds(p.release)
                      << '\n';
        }
        return 0;
    }
    require(argc == 4, "usage: YouKnowRenderOriginalPresets SLOT SCORE OUTPUT.f32 | --list");
    const auto* preset = presets::findByNumber(argv[1]);
    require(preset != nullptr && preset->number[0] == 'Y', "Expected an original YouKnow preset");
    auto parameters = preset_demos::loadPresetParameters(argv[1]);
    struct Note { double beat, length; int key; float velocity; };
    std::vector<Note> notes;
    double tempo = 0.0, end = 0.0;
    std::ifstream input(argv[2]);
    require(static_cast<bool>(input), "Cannot read score");
    std::string line;
    while (std::getline(input, line))
    {
        if (line.empty() || line.front() == '#') continue;
        std::istringstream stream(line);
        std::string type, extra;
        stream >> type;
        if (type == "tempo") stream >> tempo;
        else if (type == "end") stream >> end;
        else if (type == "note")
        {
            Note note {};
            stream >> note.beat >> note.length >> note.key >> note.velocity;
            require(std::isfinite(note.beat) && std::isfinite(note.length)
                    && std::isfinite(note.velocity) && note.beat >= 0.0
                    && note.length > 0.0 && note.key >= 0 && note.key <= 127
                    && note.velocity > 0.0f && note.velocity <= 1.0f,
                    "Invalid note: " + line);
            notes.push_back(note);
        }
        else throw std::runtime_error("Unknown score line: " + line);
        require(!stream.fail() && !(stream >> extra), "Malformed score: " + line);
    }
    require(std::isfinite(tempo) && tempo >= 30 && tempo <= 240
            && std::isfinite(end) && end > 0 && end <= 64 && !notes.empty(),
            "Invalid or empty score");
    struct Gate { std::int64_t frame; int key; float velocity; bool on; };
    const auto frameAt = [tempo](double beat) {
        return static_cast<std::int64_t>(std::llround((leadIn + beat * 60.0 / tempo) * rate));
    };
    std::vector<Gate> gates;
    for (const auto& note : notes)
    {
        require(note.beat + note.length <= end + 1e-6, "Note exceeds score ending");
        gates.push_back({frameAt(note.beat), note.key, note.velocity, true});
        gates.push_back({frameAt(note.beat + note.length), note.key, 0.0f, false});
    }
    std::stable_sort(gates.begin(), gates.end(), [](const auto& a, const auto& b) {
        return a.frame != b.frame ? a.frame < b.frame : a.on < b.on;
    });
    // Check MIDI lifetime independently of the engine: repeated same-key
    // overlaps otherwise make a later note-off truncate the wrong note.
    std::array<bool, 128> held {};
    int heldCount = 0, heldPeak = 0;
    for (const auto& gate : gates)
    {
        require(held[static_cast<std::size_t>(gate.key)] != gate.on,
                "Overlapping or unmatched same-key MIDI events");
        held[static_cast<std::size_t>(gate.key)] = gate.on;
        heldCount += gate.on ? 1 : -1;
        heldPeak = std::max(heldPeak, heldCount);
    }
    require(heldCount == 0 && heldPeak <= (parameters.keyMode == KeyMode::Unison
                                             ? 2 : parameters.polyphony),
            "Score exceeds the preset's playable polyphony");

    auto engine = std::make_unique<YouKnowEngine>();
    ProductFidelityProfile::configureBeforePrepare(*engine);
    engine->selectConverterTimingProfile(YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
    engine->prepare(rate, blockSize, YouKnowEngine::maximumOversampleFactor);
    engine->setParameters(parameters);
    engine->setPitchBend(0);
    engine->setModWheel(0);
    require(engine->getRequestedOversamplingFactor() == 4
            && engine->getOversamplingFactor() == 2,
            "Expected maximum-quality 192 kHz internal processing");
    std::array<float, blockSize> left {}, right {};
    // Let the physical nodes settle before recording the attack.
    for (int i = 0; i < static_cast<int>(rate); i += blockSize)
        engine->process(left.data(), right.data(), blockSize);
    const double tail = std::max(0.8, YouKnowEngine::envelopeReleaseSeconds(parameters.release) + 0.4);
    const auto frames = frameAt(end) + static_cast<std::int64_t>(std::ceil(tail * rate));
    static_assert(std::endian::native == std::endian::little);
    const std::filesystem::path output(argv[3]);
    if (output.has_parent_path()) std::filesystem::create_directories(output.parent_path());
    std::ofstream audio(output, std::ios::binary);
    require(static_cast<bool>(audio), "Cannot open raw output");
    std::array<float, blockSize * 2> interleaved {};
    std::size_t next = 0;
    double peak = 0.0, energy = 0.0;
    int voicePeak = 0;
    for (std::int64_t frame = 0; frame < frames;)
    {
        while (next < gates.size() && gates[next].frame <= frame)
        {
            const auto& gate = gates[next++];
            if (gate.on) engine->noteOn(gate.key, gate.velocity);
            else engine->noteOff(gate.key);
        }
        auto count = std::min<std::int64_t>(blockSize, frames - frame);
        if (next < gates.size()) count = std::min(count, gates[next].frame - frame);
        require(count > 0, "Score made no forward progress");
        engine->process(left.data(), right.data(), static_cast<int>(count));
        voicePeak = std::max(voicePeak, engine->getActiveVoiceCount());
        for (int i = 0; i < count; ++i)
        {
            const double l = left[static_cast<std::size_t>(i)];
            const double r = right[static_cast<std::size_t>(i)];
            require(std::isfinite(l) && std::isfinite(r), "Non-finite synth output");
            peak = std::max({peak, std::abs(l), std::abs(r)});
            energy += 0.5 * (l * l + r * r);
            interleaved[static_cast<std::size_t>(2 * i)] = static_cast<float>(l);
            interleaved[static_cast<std::size_t>(2 * i + 1)] = static_cast<float>(r);
        }
        audio.write(reinterpret_cast<const char*>(interleaved.data()),
                    static_cast<std::streamsize>(count * 2 * sizeof(float)));
        frame += count;
    }
    audio.close();
    require(static_cast<bool>(audio), "Failed writing complete raw output");
    require(next == gates.size() && voicePeak > 0 && voicePeak <= parameters.polyphony
            && engine->getActiveVoiceCount() == 0, "Unreleased or missing synth voices");
    require(peak > 1e-6 && peak < 1.0, "Silent or clipped raw performance");
    std::cout << std::setprecision(12)
              << "{\"slot\": " << std::quoted(preset->number)
              << ", \"preset\": " << std::quoted(preset->name)
              << ", \"sample_rate\": 96000, \"channels\": 2, \"quality_selected\": 4,"
                 " \"oversampling_applied\": 2, \"internal_rate\": 192000,"
                 " \"vcf_tanh\": \"Exact\", \"vcf_solver\": \"MersonHalfSteps\","
              << " \"seconds\": " << frames / rate << ", \"tail_seconds\": " << tail
              << ", \"raw_peak_dbfs\": " << 20.0 * std::log10(peak)
              << ", \"raw_rms_dbfs\": " << 10.0 * std::log10(energy / frames)
              << ", \"peak_held_keys\": " << heldPeak
              << ", \"peak_active_voices\": " << voicePeak
              << ", \"final_active_voices\": " << engine->getActiveVoiceCount() << "}\n";
    return 0;
}
}

int main(int argc, char** argv)
{
    try { return run(argc, argv); }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
