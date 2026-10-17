// Shared Protocodus preset demo renderer (README G3, G3a, G3c, G6).
//
// Master copy: reason-rack-extensions/Tools/portfolio/Tools/PresetDemoRenderer.h.
// Every canonical repo carries a byte-identical copy in Tools/, and
// Tools/check_repo_standard.py fails on a drifted one: change the master and
// copy it everywhere in the same round.
//
// A repo's Tools/RenderPresetDemos.cpp describes its instrument and hands over:
//
//   int main(int argc, char** argv) { return protocodus::demos::Main(argc, argv, instrument); }
//
// and builds it as Render<Name>PresetDemos:
//
//   Render<Name>PresetDemos [--jobs N] <midi-dir> <out-dir>
//       one <NN>-<preset-slug>.mp3 per factory preset, from
//       <midi-dir>/<NN>-<preset-slug>.mid, plus demos.json, which
//       update_readme_blocks.py turns into the README demo table
//   Render<Name>PresetDemos [--jobs N] --check <midi-dir>
//       the audio tier: renders every preset twice and fails unless the
//       second render is bit-identical
//
// Both forms fail unless <midi-dir> holds exactly one .mid per preset and
// every demo is finite, audible, below the declared ceiling and 10-20 s long.
// Every demo renders at the same rate, block size and seed, so an unchanged
// engine produces identical bytes. MP3s are encoded by ffmpeg (the FFMPEG
// environment variable overrides the executable) with fixed settings and no
// metadata, so an unchanged render also encodes to identical bytes.
#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace protocodus::demos {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockFrames = 256;
constexpr std::uint32_t kSeed = 20261017u;
constexpr double kMinSeconds = 10.0;
constexpr double kMaxSeconds = 20.0;
constexpr double kTailSeconds = 2.5; // rendered after the last MIDI event, if it fits
constexpr double kFadeSeconds = 0.75; // only where kMaxSeconds cuts a tail
constexpr float kAudibleFloor = 1.0e-3f;

// One preset, prepared at kSampleRate. Midi() receives channel messages at the
// sample they are due, between Render() calls of at most kBlockFrames.
class Voice {
public:
    virtual ~Voice() = default;
    virtual void Midi(const std::uint8_t* bytes, int size) = 0;
    virtual void Render(float* left, float* right, int frames) = 0;
};

struct Instrument {
    std::string name;                  // product name, for messages
    std::vector<std::string> presets;  // factory presets in program order
    float ceiling = 1.0f;              // declared peak ceiling (linear); 1 = 0 dBFS
    // Called from worker threads, so it must not share mutable state between voices.
    std::function<std::unique_ptr<Voice>(std::size_t preset, double sampleRate, int maxBlockFrames,
                                         std::uint32_t seed)> open;
};

struct MidiEvent {
    double seconds;
    std::uint8_t bytes[3];
    int size;
};

namespace detail {

namespace fs = std::filesystem;

inline std::string Slug(const std::string& name) {
    std::string slug;
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            slug += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (!slug.empty() && slug.back() != '-')
            slug += '-';
    }
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    return slug;
}

// <NN>-<slug>, numbered from 01 and widened for banks of 100 or more presets
// so the files still sort in program order.
inline std::string Stem(const Instrument& instrument, std::size_t preset) {
    const int width = std::max<int>(2, static_cast<int>(std::to_string(instrument.presets.size()).size()));
    std::string number = std::to_string(preset + 1);
    number.insert(0, static_cast<std::size_t>(width) - number.size(), '0');
    return number + "-" + Slug(instrument.presets[preset]);
}

} // namespace detail

// A Standard MIDI File (format 0 or 1) as channel messages in time order, with
// the tempo map applied. Meta and system-exclusive events are skipped.
inline std::vector<MidiEvent> ReadMidi(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error(path.string() + ": cannot read");
    const std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(file)), {});
    std::size_t at = 0;
    const auto need = [&](std::size_t n) {
        if (at + n > data.size()) throw std::runtime_error(path.string() + ": truncated");
    };
    const auto u16 = [&] { need(2); const int v = data[at] << 8 | data[at + 1]; at += 2; return v; };
    const auto u32 = [&] {
        need(4);
        const std::uint32_t v = std::uint32_t(data[at]) << 24 | std::uint32_t(data[at + 1]) << 16
            | std::uint32_t(data[at + 2]) << 8 | data[at + 3];
        at += 4;
        return v;
    };
    const auto vlq = [&] {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            need(1);
            const std::uint8_t b = data[at++];
            v = v << 7 | (b & 0x7F);
            if (!(b & 0x80)) return v;
        }
        throw std::runtime_error(path.string() + ": bad variable-length quantity");
    };
    const auto tag = [&](const char* expected) {
        need(4);
        const bool match = std::memcmp(&data[at], expected, 4) == 0;
        at += 4;
        return match;
    };

    if (!tag("MThd")) throw std::runtime_error(path.string() + ": not a MIDI file");
    const std::uint32_t headerLength = u32();
    const std::size_t headerStart = at;
    const int format = u16(), tracks = u16(), division = u16();
    if (format > 1 || headerLength < 6 || (division & 0x8000) || division == 0)
        throw std::runtime_error(path.string() + ": needs format 0 or 1 with ticks per quarter note");
    at = headerStart + headerLength;

    struct TickEvent { std::uint64_t tick; int order; std::uint32_t tempo; MidiEvent event; };
    std::vector<TickEvent> events;
    int order = 0;
    for (int track = 0; track < tracks; ++track) {
        if (!tag("MTrk")) throw std::runtime_error(path.string() + ": missing MTrk");
        const std::uint32_t length = u32();
        need(length);
        const std::size_t end = at + length;
        std::uint64_t tick = 0;
        std::uint8_t status = 0;
        while (at < end) {
            tick += vlq();
            need(1);
            if (data[at] & 0x80) status = data[at++];
            if (status == 0xFF) {
                need(1);
                const std::uint8_t type = data[at++];
                const std::uint32_t metaLength = vlq();
                need(metaLength);
                if (type == 0x51 && metaLength == 3)
                    events.push_back({ tick, order++,
                                       std::uint32_t(data[at]) << 16 | std::uint32_t(data[at + 1]) << 8 | data[at + 2],
                                       {} });
                at += metaLength;
                status = 0; // meta events cancel running status
            } else if (status == 0xF0 || status == 0xF7) {
                const std::uint32_t sysexLength = vlq();
                need(sysexLength);
                at += sysexLength;
                status = 0;
            } else if (status >= 0x80) {
                const int size = (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 ? 2 : 3;
                need(static_cast<std::size_t>(size - 1));
                MidiEvent event { 0.0, { status, data[at], size > 2 ? data[at + 1] : std::uint8_t(0) }, size };
                at += static_cast<std::size_t>(size - 1);
                events.push_back({ tick, order++, 0, event });
            } else {
                throw std::runtime_error(path.string() + ": data byte without a status");
            }
        }
        at = end;
    }

    // Tracks merge by tick; within a tick, file order wins.
    std::stable_sort(events.begin(), events.end(), [](const TickEvent& a, const TickEvent& b) {
        return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
    });
    std::vector<MidiEvent> result;
    double seconds = 0.0, secondsPerTick = 0.5 / division; // 120 bpm until a tempo event
    std::uint64_t lastTick = 0;
    for (const auto& e : events) {
        seconds += static_cast<double>(e.tick - lastTick) * secondsPerTick;
        lastTick = e.tick;
        if (e.tempo != 0) {
            secondsPerTick = e.tempo / 1.0e6 / division;
        } else {
            result.push_back(e.event);
            result.back().seconds = seconds;
        }
    }
    return result;
}

namespace detail {

struct Demo {
    std::string stem;
    std::vector<float> left, right;
    float peak = 0.0f;
    std::uint64_t hash = 1469598103934665603ull; // FNV-1a over the float bits
};

inline Demo Render(const Instrument& instrument, const fs::path& midiDir, std::size_t preset) {
    Demo demo;
    demo.stem = Stem(instrument, preset);
    const auto events = ReadMidi(midiDir / (demo.stem + ".mid"));
    if (events.empty()) throw std::runtime_error(demo.stem + ".mid: no MIDI channel events");
    const double end = std::min(kMaxSeconds, events.back().seconds + kTailSeconds);
    const auto frames = static_cast<std::size_t>(std::llround(end * kSampleRate));
    demo.left.assign(frames, 0.0f);
    demo.right.assign(frames, 0.0f);

    const auto voice = instrument.open(preset, kSampleRate, kBlockFrames, kSeed);
    const auto due = [&](std::size_t i) { return static_cast<std::size_t>(std::llround(events[i].seconds * kSampleRate)); };
    std::size_t next = 0, done = 0;
    while (done < frames) {
        const std::size_t blockEnd = std::min(frames, done + static_cast<std::size_t>(kBlockFrames));
        // Each event lands on its own sample, as a plug-in host delivers it.
        while (done < blockEnd) {
            while (next < events.size() && due(next) <= done) {
                voice->Midi(events[next].bytes, events[next].size);
                ++next;
            }
            const std::size_t until = next < events.size() ? std::min(blockEnd, due(next)) : blockEnd;
            voice->Render(&demo.left[done], &demo.right[done], static_cast<int>(until - done));
            done = until;
        }
    }

    if (end < events.back().seconds + kTailSeconds) {
        const auto fade = std::min(frames, static_cast<std::size_t>(kFadeSeconds * kSampleRate));
        for (std::size_t i = 0; i < fade; ++i) {
            const float gain = static_cast<float>(i) / static_cast<float>(fade);
            demo.left[frames - 1 - i] *= gain;
            demo.right[frames - 1 - i] *= gain;
        }
    }
    for (std::size_t i = 0; i < frames; ++i) {
        for (const float sample : { demo.left[i], demo.right[i] }) {
            if (!std::isfinite(sample)) demo.peak = INFINITY;
            else demo.peak = std::max(demo.peak, std::fabs(sample));
            std::uint32_t bits;
            std::memcpy(&bits, &sample, sizeof bits);
            for (int b = 0; b < 4; ++b) demo.hash = (demo.hash ^ ((bits >> (8 * b)) & 0xFF)) * 1099511628211ull;
        }
    }
    return demo;
}

inline double Seconds(const Demo& demo) { return static_cast<double>(demo.left.size()) / kSampleRate; }

inline double Dbfs(float peak) { return 20.0 * std::log10(std::max(1.0e-9, static_cast<double>(peak))); }

inline const char* Problem(const Instrument& instrument, const Demo& demo) {
    if (!std::isfinite(demo.peak)) return "non-finite samples";
    if (demo.peak < kAudibleFloor) return "silent";
    if (demo.peak >= instrument.ceiling) return "peak at or above the declared ceiling";
    if (Seconds(demo) < kMinSeconds || Seconds(demo) > kMaxSeconds) return "not 10-20 s long";
    return nullptr;
}

inline void WriteWav(const fs::path& path, const Demo& demo) {
    const auto frames = static_cast<std::uint32_t>(demo.left.size());
    const std::uint32_t dataBytes = frames * 2 * 3;
    std::vector<std::uint8_t> out;
    out.reserve(44 + dataBytes);
    const auto put = [&](std::uint32_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    const auto text = [&](const char* s) { out.insert(out.end(), s, s + 4); };
    const auto rate = static_cast<std::uint32_t>(kSampleRate);
    text("RIFF"); put(36 + dataBytes, 4); text("WAVE");
    text("fmt "); put(16, 4); put(1, 2); put(2, 2); put(rate, 4); put(rate * 6, 4); put(6, 2); put(24, 2);
    text("data"); put(dataBytes, 4);
    for (std::uint32_t i = 0; i < frames; ++i)
        for (const float sample : { demo.left[i], demo.right[i] }) {
            const double scaled = std::round(std::clamp(static_cast<double>(sample), -1.0, 1.0) * 8388607.0);
            put(static_cast<std::uint32_t>(static_cast<std::int32_t>(scaled)), 3);
        }
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!file) throw std::runtime_error(path.string() + ": cannot write");
}

inline void EncodeMp3(const fs::path& wav, const fs::path& mp3) {
    const char* ffmpeg = std::getenv("FFMPEG");
    std::string command = std::string("\"") + (ffmpeg && *ffmpeg ? ffmpeg : "ffmpeg")
        + "\" -nostdin -v error -y -i \"" + wav.string()
        + "\" -c:a libmp3lame -b:a 192k -bitexact -map_metadata -1 -id3v2_version 0 \"" + mp3.string() + "\"";
#ifdef _WIN32
    command = "\"" + command + "\""; // cmd.exe strips one outer pair of quotes
#endif
    if (std::system(command.c_str()) != 0 || !fs::exists(mp3))
        throw std::runtime_error(mp3.string() + ": ffmpeg failed");
}

inline std::string JsonString(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof escaped, "\\u%04x", static_cast<unsigned>(c));
            out += escaped;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

struct Summary {
    std::string stem;
    double seconds = 0.0;
    float peak = 0.0f;
    std::uint64_t hash = 0;
    std::string error;
};

// Renders every preset on `jobs` threads, keeping one demo's samples per
// thread; `keep` sees each demo before its samples are dropped.
inline std::vector<Summary> RenderAll(const Instrument& instrument, const fs::path& midiDir, int jobs,
                                      const std::function<void(const Demo&)>& keep) {
    std::vector<Summary> summaries(instrument.presets.size());
    std::atomic<std::size_t> next { 0 };
    std::vector<std::thread> workers;
    for (int j = 0; j < std::max(1, jobs); ++j)
        workers.emplace_back([&] {
            for (std::size_t i; (i = next++) < summaries.size();) {
                auto& summary = summaries[i];
                summary.stem = Stem(instrument, i);
                try {
                    const Demo demo = Render(instrument, midiDir, i);
                    summary = { demo.stem, Seconds(demo), demo.peak, demo.hash, {} };
                    if (const char* problem = Problem(instrument, demo)) summary.error = problem;
                    else if (keep) keep(demo);
                } catch (const std::exception& e) {
                    summary.error = e.what();
                }
            }
        });
    for (auto& worker : workers) worker.join();
    return summaries;
}

inline int Report(const std::vector<Summary>& summaries) {
    int failures = 0;
    for (const auto& s : summaries) {
        std::printf("%s %s: %.1f s, peak %.2f dBFS%s%s\n", s.error.empty() ? "PASS" : "FAIL", s.stem.c_str(),
                    s.seconds, Dbfs(s.peak), s.error.empty() ? "" : ": ", s.error.c_str());
        failures += !s.error.empty();
    }
    return failures;
}

// Exactly one <stem>.mid per preset and no strays, so a renamed or removed
// preset cannot leave a source behind.
inline bool CheckSources(const Instrument& instrument, const fs::path& midiDir) {
    std::vector<std::string> expected, found;
    for (std::size_t i = 0; i < instrument.presets.size(); ++i) expected.push_back(Stem(instrument, i) + ".mid");
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(midiDir, error))
        if (entry.path().extension() == ".mid") found.push_back(entry.path().filename().string());
    std::sort(expected.begin(), expected.end());
    std::sort(found.begin(), found.end());
    if (!error && found == expected && !expected.empty()) return true;
    std::fprintf(stderr, "FAIL: %s must hold exactly one <NN>-<preset-slug>.mid per factory preset\n",
                 midiDir.string().c_str());
    for (const auto& name : expected)
        if (!std::binary_search(found.begin(), found.end(), name)) std::fprintf(stderr, "  missing %s\n", name.c_str());
    for (const auto& name : found)
        if (!std::binary_search(expected.begin(), expected.end(), name)) std::fprintf(stderr, "  no preset for %s\n", name.c_str());
    return false;
}

inline int Check(const Instrument& instrument, const fs::path& midiDir, int jobs) {
    if (!CheckSources(instrument, midiDir)) return 1;
    auto first = RenderAll(instrument, midiDir, jobs, nullptr);
    const auto second = RenderAll(instrument, midiDir, jobs, nullptr);
    for (std::size_t i = 0; i < first.size(); ++i)
        if (first[i].error.empty() && first[i].hash != second[i].hash) first[i].error = "a second render differs";
    return Report(first) == 0 ? 0 : 1;
}

inline int Write(const Instrument& instrument, const fs::path& midiDir, const fs::path& outDir, int jobs) {
    if (!CheckSources(instrument, midiDir)) return 1;
    fs::create_directories(outDir);
    const auto summaries = RenderAll(instrument, midiDir, jobs, [&](const Demo& demo) {
        WriteWav(outDir / (demo.stem + ".wav"), demo);
    });
    if (Report(summaries) != 0) return 1;

    // std::system is not safe to call from several threads, so encode here.
    for (const auto& s : summaries) {
        EncodeMp3(outDir / (s.stem + ".wav"), outDir / (s.stem + ".mp3"));
        fs::remove(outDir / (s.stem + ".wav"));
    }

    std::string json = "{\n  \"instrument\": " + JsonString(instrument.name)
        + ",\n  \"sample_rate\": " + std::to_string(static_cast<int>(kSampleRate))
        + ",\n  \"block_frames\": " + std::to_string(kBlockFrames)
        + ",\n  \"seed\": " + std::to_string(kSeed) + ",\n  \"demos\": [";
    for (std::size_t i = 0; i < summaries.size(); ++i) {
        char numbers[96];
        std::snprintf(numbers, sizeof numbers, ", \"seconds\": %.9g, \"peak_dbfs\": %.9g}", summaries[i].seconds,
                      Dbfs(summaries[i].peak));
        json += std::string(i ? "," : "") + "\n    {\"number\": " + std::to_string(i + 1)
            + ", \"preset\": " + JsonString(instrument.presets[i])
            + ", \"file\": " + JsonString(summaries[i].stem + ".mp3") + numbers;
    }
    json += "\n  ]\n}\n";
    std::ofstream manifest(outDir / "demos.json", std::ios::binary);
    manifest << json;
    if (!manifest) throw std::runtime_error((outDir / "demos.json").string() + ": cannot write");
    return 0;
}

} // namespace detail

inline int Main(int argc, char** argv, const Instrument& instrument) {
    const std::string usage = "usage: " + std::string(argc > 0 ? argv[0] : "RenderPresetDemos")
        + " [--jobs N] <midi-dir> <out-dir> | [--jobs N] --check <midi-dir>";
    try {
        if (instrument.presets.empty() || !instrument.open) throw std::runtime_error("the instrument declares no presets");
        int jobs = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
        bool check = false;
        std::vector<std::filesystem::path> paths;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--check") check = true;
            else if (arg == "--jobs" && i + 1 < argc) jobs = std::stoi(argv[++i]);
            else if (!arg.empty() && arg[0] != '-') paths.emplace_back(arg);
            else throw std::runtime_error(usage);
        }
        if (paths.size() != (check ? 1u : 2u)) throw std::runtime_error(usage);
        return check ? detail::Check(instrument, paths[0], jobs) : detail::Write(instrument, paths[0], paths[1], jobs);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}

} // namespace protocodus::demos
