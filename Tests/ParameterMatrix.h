// Shared Protocodus U4 parameter-extremes matrix.
//
// Master copy: reason-rack-extensions/Tools/portfolio/Tests/ParameterMatrix.h.
// Every canonical repo carries a byte-identical copy in Tests/, and
// Tools/check_repo_standard.py fails on a drifted one.
//
// A repo wraps its processor (or, in the Rack, its device's properties) in a
// protocodus::matrix::Device and runs
//
//   return protocodus::matrix::Main(argc, argv, [] { return std::make_unique<MyDevice>(); });
//
// as a CTest test labelled `fuzz`, under ASan + UBSan (see Main for the
// command line). Every automatable
// parameter takes normalised 0, 0.5 and 1 and every discrete parameter takes
// every value, in these cases:
//
//   1. all at min, all at mid, all at max, and the default patch;
//   2. each parameter alone at each of its values, the rest at default;
//   3. an all-pairs covering array: every pair of parameter values appears
//      together in at least one case;
//   4. every factory preset with each parameter pushed to min and to max.
//
// Each case plays a fixed phrase (note-on, held chord, pitch bend, note-off,
// release tail) while the parameters ramp from the case's values towards the
// base patch and back mid-note, at 44.1, 48 and 96 kHz with blocks of 1, 64
// and 512 frames. It fails on non-finite output, a peak at or above the
// declared ceiling, runaway DC, held notes gone silent before note-off,
// sound left after the release where the design implies silence (or a tail
// that grows where it does not), or a state that does not survive save and
// restore.
//
// When including JUCE, include it before this header to get JuceDevice, which
// takes the parameter list, programs and state from a juce::AudioProcessor.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace protocodus::matrix {

struct Parameter {
    std::string name;
    int steps = 0;            // 0: continuous; n >= 2: discrete with n values
    float defaultValue = 0.0f; // normalised
};

struct Event {
    int offset; // frame within the block
    std::uint8_t bytes[3];
    int size;
};

// What a case must stay within. A device returns tighter or looser values per
// case where its design calls for it, for example no release silence while a
// hold switch or an infinite-feedback delay is on.
struct Limits {
    float ceiling = 1.0f;         // linear peak, exclusive
    float dcLimit = 0.05f;        // |mean| of each channel over the whole run
    // Silence is due this long after the last note-off. Below 0 none is due,
    // and the last 250 ms must only stay within 3 dB of the held chord's peak.
    double releaseSeconds = 2.0;
    float silenceFloor = 1.0e-3f; // -60 dBFS
    // The held chord must still sound just before note-off, so a device that
    // drops held notes cannot pass by being silent. false where a case is
    // silent by design, such as level at zero.
    bool soundsWhileHeld = true;
};

struct Case {
    std::string label;
    int preset = -1;           // loaded before `values` are applied; -1: the default patch
    std::vector<float> values; // normalised, one per parameter
    std::vector<float> base;   // the patch automation ramps towards
};

class Device {
public:
    virtual ~Device() = default;
    virtual std::vector<Parameter> Parameters() const = 0;
    virtual int PresetCount() const = 0;
    virtual void Prepare(double sampleRate, int maxBlockFrames) = 0;
    virtual void LoadPreset(int index) = 0;
    virtual float GetParameter(std::size_t index) const = 0;
    virtual void SetParameter(std::size_t index, float normalised) = 0;
    // Writes `frames` samples of the main stereo output; `events` are sorted by offset.
    virtual void Process(float* left, float* right, int frames, const Event* events, int eventCount) = 0;
    virtual std::vector<std::uint8_t> SaveState() = 0;
    virtual void RestoreState(const std::vector<std::uint8_t>& state) = 0;
    // Automation ramps from c.values towards c.base mid-note, so a limit that
    // depends on a parameter may need the larger of the two.
    virtual Limits LimitsFor(const Case& /*c*/) const { return {}; }
};

struct Options {
    // false runs each case at one rate and block size in rotation (the four
    // whole-patch cases still run at all nine), for a quick pre-merge pass;
    // the standard's full matrix is true.
    bool everyConfiguration = true;
    int jobs = 1; // above 1, the factory must be safe to call from several threads
    // Runs only the cases it accepts, given each case's index in Cases(), for
    // sharding a large preset bank across CI jobs or sampling it.
    std::function<bool(std::size_t index, const Case&)> select;
};

constexpr double kRates[] = { 44100.0, 48000.0, 96000.0 };
constexpr int kBlocks[] = { 1, 64, 512 };

inline std::vector<float> Values(const Parameter& p) {
    if (p.steps < 2) return { 0.0f, 0.5f, 1.0f };
    std::vector<float> values;
    for (int i = 0; i < p.steps; ++i) values.push_back(static_cast<float>(i) / static_cast<float>(p.steps - 1));
    return values;
}

// Greedy all-pairs covering array. Each row is the best of several
// candidates, one seeded from each pair still uncovered between the first two
// parameters that have one; a candidate fills every other parameter with the
// value covering the most new pairs against those already set, preferring the
// value used least so far. Deterministic, so a failing row keeps its number.
inline std::vector<std::vector<int>> AllPairs(const std::vector<int>& counts) {
    const std::size_t n = counts.size();
    std::vector<std::vector<std::vector<std::vector<bool>>>> covered(n);
    std::vector<std::vector<int>> used(n);
    std::size_t uncovered = 0;
    for (std::size_t i = 0; i < n; ++i) {
        covered[i].resize(n);
        used[i].assign(static_cast<std::size_t>(counts[i]), 0);
        for (std::size_t j = i + 1; j < n; ++j) {
            covered[i][j].assign(static_cast<std::size_t>(counts[i]),
                                 std::vector<bool>(static_cast<std::size_t>(counts[j]), false));
            uncovered += static_cast<std::size_t>(counts[i] * counts[j]);
        }
    }
    const auto isCovered = [&](std::size_t i, int a, std::size_t j, int b) {
        return i < j ? covered[i][j][a][b] : covered[j][i][b][a];
    };
    const auto gainOf = [&](const std::vector<int>& row) {
        std::size_t gain = 0;
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t j = i + 1; j < n; ++j) gain += !covered[i][j][row[i]][row[j]];
        return gain;
    };
    std::vector<std::vector<int>> rows;
    while (uncovered > 0) {
        std::size_t si = 0, sj = 0;
        bool found = false;
        for (std::size_t i = 0; i < n && !found; ++i)
            for (std::size_t j = i + 1; j < n && !found; ++j)
                for (int a = 0; a < counts[i] && !found; ++a)
                    for (int b = 0; b < counts[j] && !found; ++b)
                        if (!covered[i][j][a][b]) {
                            si = i;
                            sj = j;
                            found = true;
                        }
        std::vector<int> best;
        std::size_t bestGain = 0;
        for (int a = 0; a < counts[si]; ++a)
            for (int b = 0; b < counts[sj]; ++b) {
                if (covered[si][sj][a][b]) continue;
                std::vector<int> row(n, -1);
                row[si] = a;
                row[sj] = b;
                for (std::size_t k = 0; k < n; ++k) {
                    if (row[k] >= 0) continue;
                    int choice = 0, choiceGain = -1;
                    for (int v = 0; v < counts[k]; ++v) {
                        int gain = 0;
                        for (std::size_t m = 0; m < n; ++m)
                            if (row[m] >= 0 && !isCovered(k, v, m, row[m])) ++gain;
                        if (gain > choiceGain || (gain == choiceGain && used[k][v] < used[k][choice])) {
                            choice = v;
                            choiceGain = gain;
                        }
                    }
                    row[k] = choice;
                }
                const std::size_t gain = gainOf(row);
                if (gain > bestGain) {
                    best = row;
                    bestGain = gain;
                }
            }
        for (std::size_t i = 0; i < n; ++i) {
            ++used[i][best[i]];
            for (std::size_t j = i + 1; j < n; ++j)
                if (!covered[i][j][best[i]][best[j]]) {
                    covered[i][j][best[i]][best[j]] = true;
                    --uncovered;
                }
        }
        rows.push_back(std::move(best));
    }
    return rows;
}

// Every case of the matrix, in a fixed order. `probe` supplies the parameter
// list and each preset's values.
inline std::vector<Case> Cases(Device& probe) {
    const auto parameters = probe.Parameters();
    const std::size_t n = parameters.size();
    std::vector<float> defaults;
    std::vector<std::vector<float>> values;
    for (const auto& p : parameters) {
        defaults.push_back(p.defaultValue);
        values.push_back(Values(p));
    }
    std::vector<Case> cases;
    const auto add = [&](std::string label, int preset, std::vector<float> v, std::vector<float> base) {
        cases.push_back({ std::move(label), preset, std::move(v), std::move(base) });
    };

    // Mid is 0.5, or the middle value of a discrete parameter (the lower of two).
    std::vector<float> mids(n);
    for (std::size_t i = 0; i < n; ++i) mids[i] = values[i][(values[i].size() - 1) / 2];
    add("all-min", -1, std::vector<float>(n, 0.0f), defaults);
    add("all-mid", -1, mids, defaults);
    add("all-max", -1, std::vector<float>(n, 1.0f), defaults);
    add("default", -1, defaults, defaults);

    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t v = 0; v < values[i].size(); ++v) {
            auto row = defaults;
            row[i] = values[i][v];
            char suffix[32];
            std::snprintf(suffix, sizeof suffix, "=%g", static_cast<double>(row[i]));
            add(parameters[i].name + suffix, -1, row, defaults);
        }

    std::vector<int> counts;
    for (const auto& v : values) counts.push_back(static_cast<int>(v.size()));
    const auto rows = AllPairs(counts);
    for (std::size_t r = 0; r < rows.size(); ++r) {
        std::vector<float> row(n);
        for (std::size_t i = 0; i < n; ++i) row[i] = values[i][static_cast<std::size_t>(rows[r][i])];
        add("pairs#" + std::to_string(r + 1), -1, row, defaults);
    }

    for (int preset = 0; preset < probe.PresetCount(); ++preset) {
        probe.LoadPreset(preset);
        std::vector<float> patch(n);
        for (std::size_t i = 0; i < n; ++i) patch[i] = probe.GetParameter(i);
        for (std::size_t i = 0; i < n; ++i)
            for (const float extreme : { 0.0f, 1.0f }) {
                auto row = patch;
                row[i] = extreme;
                add("preset " + std::to_string(preset) + " " + parameters[i].name + (extreme > 0 ? "=1" : "=0"),
                    preset, row, patch);
            }
    }
    return cases;
}

// One run of one case at one rate and block size; empty when it passes.
inline std::string Play(const std::function<std::unique_ptr<Device>()>& make, const Case& c, double rate,
                        int block) {
    constexpr double kNoteOff = 0.8, kRampStart = 0.25, kRampEnd = 0.75, kBendStart = 0.2, kBendEnd = 0.6;
    const auto device = make();
    device->Prepare(rate, block);
    if (c.preset >= 0) device->LoadPreset(c.preset);
    const std::size_t n = c.values.size();
    for (std::size_t i = 0; i < n; ++i) device->SetParameter(i, c.values[i]);
    const Limits limits = device->LimitsFor(c);
    const double seconds = kNoteOff + (limits.releaseSeconds >= 0.0 ? limits.releaseSeconds + 0.25 : 0.5);
    const auto total = static_cast<long long>(std::llround(seconds * rate));
    const auto frame = [&](double t) { return static_cast<long long>(std::llround(t * rate)); };

    struct Timed { long long at; Event event; };
    std::vector<Timed> notes {
        { frame(0.0), { 0, { 0x90, 60, 100 }, 3 } },
        { frame(0.1), { 0, { 0x90, 64, 80 }, 3 } },
        { frame(0.1), { 0, { 0x90, 67, 127 }, 3 } },
        { frame(kNoteOff), { 0, { 0x80, 60, 0 }, 3 } },
        { frame(kNoteOff), { 0, { 0x80, 64, 0 }, 3 } },
        { frame(kNoteOff), { 0, { 0x80, 67, 0 }, 3 } },
    };
    const auto triangle = [](double t, double start, double end) {
        if (t <= start || t >= end) return 0.0;
        const double x = (t - start) / (end - start);
        return x < 0.5 ? 2.0 * x : 2.0 * (1.0 - x);
    };

    std::vector<float> left(static_cast<std::size_t>(block)), right(static_cast<std::size_t>(block));
    std::vector<Event> events;
    std::size_t nextNote = 0;
    int lastBend = 8192;
    bool rampDone = false;
    double sum[2] = { 0.0, 0.0 }, peak = 0.0, heldPeak = 0.0, tailPeak = 0.0;
    const long long heldStart = frame(kNoteOff - 0.25), heldEnd = frame(kNoteOff);
    const long long tailStart = limits.releaseSeconds >= 0.0 ? frame(kNoteOff + limits.releaseSeconds)
                                                             : total - frame(0.25);
    for (long long done = 0; done < total; done += block) {
        const int frames = static_cast<int>(std::min<long long>(block, total - done));
        const double t = static_cast<double>(done) / rate;
        events.clear();
        // Automation and bend move at block starts, as a host delivers them.
        if (t > kRampStart && !rampDone) {
            // The last step lands exactly back on the case's values.
            const double ramp = triangle(t, kRampStart, kRampEnd);
            rampDone = t >= kRampEnd;
            for (std::size_t i = 0; i < n; ++i)
                device->SetParameter(i, static_cast<float>(c.values[i] + (c.base[i] - c.values[i]) * ramp));
        }
        const int bend = 8192 + static_cast<int>(std::lround(8191.0 * triangle(t, kBendStart, kBendEnd)));
        if (bend != lastBend) {
            events.push_back({ 0, { 0xE0, static_cast<std::uint8_t>(bend & 0x7F), static_cast<std::uint8_t>(bend >> 7) }, 3 });
            lastBend = bend;
        }
        while (nextNote < notes.size() && notes[nextNote].at < done + frames) {
            Event e = notes[nextNote].event;
            e.offset = static_cast<int>(std::max<long long>(0, notes[nextNote].at - done));
            events.push_back(e);
            ++nextNote;
        }
        std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.offset < b.offset; });
        device->Process(left.data(), right.data(), frames, events.data(), static_cast<int>(events.size()));
        for (int i = 0; i < frames; ++i) {
            const float l = left[static_cast<std::size_t>(i)], r = right[static_cast<std::size_t>(i)];
            if (!std::isfinite(l) || !std::isfinite(r))
                return "non-finite output at " + std::to_string(static_cast<double>(done + i) / rate) + " s";
            const double magnitude = std::max(std::fabs(l), std::fabs(r));
            peak = std::max(peak, magnitude);
            if (done + i >= heldStart && done + i < heldEnd) heldPeak = std::max(heldPeak, magnitude);
            if (done + i >= tailStart) tailPeak = std::max(tailPeak, magnitude);
            sum[0] += l;
            sum[1] += r;
        }
    }

    char problem[160] = "";
    const double dc = std::max(std::fabs(sum[0]), std::fabs(sum[1])) / static_cast<double>(total);
    if (peak >= limits.ceiling)
        std::snprintf(problem, sizeof problem, "peak %.2f dBFS reaches the ceiling", 20.0 * std::log10(peak));
    else if (limits.soundsWhileHeld && heldPeak < limits.silenceFloor)
        std::snprintf(problem, sizeof problem, "held notes silent (%.2f dBFS) before note-off",
                      20.0 * std::log10(std::max(heldPeak, 1.0e-12)));
    else if (dc >= limits.dcLimit)
        std::snprintf(problem, sizeof problem, "DC offset %.4f", dc);
    else if (limits.releaseSeconds >= 0.0 && tailPeak >= limits.silenceFloor)
        std::snprintf(problem, sizeof problem, "%.2f dBFS left %.2f s after release", 20.0 * std::log10(tailPeak),
                      limits.releaseSeconds);
    else if (limits.releaseSeconds < 0.0 && tailPeak >= limits.silenceFloor && tailPeak > 1.4125 * heldPeak)
        std::snprintf(problem, sizeof problem, "the tail grows to %.2f dBFS, above the held %.2f dBFS",
                      20.0 * std::log10(tailPeak), 20.0 * std::log10(std::max(heldPeak, 1.0e-12)));
    if (*problem) return problem;

    const auto state = device->SaveState();
    const auto restored = make();
    restored->Prepare(rate, block);
    restored->RestoreState(state);
    if (restored->SaveState() != state) return "state differs after save and restore";
    // A tolerance, not equality: a skewed JUCE range stores plain values, and
    // one float step of plain value near a skewed end moves the normalised
    // value by up to about 0.001 (Electry's Bend time at its minimum). A lost
    // parameter moves it much further.
    for (std::size_t i = 0; i < n; ++i)
        if (std::fabs(restored->GetParameter(i) - device->GetParameter(i)) > 0.005f)
            return "parameter " + std::to_string(i) + " is " + std::to_string(restored->GetParameter(i))
                + " after save and restore, not " + std::to_string(device->GetParameter(i));
    return {};
}

// Runs the matrix, printing each failure with its case, rate and block size.
// Returns the number of failed runs.
inline int Run(const std::function<std::unique_ptr<Device>()>& make, const Options& options = {}) {
    const auto probe = make();
    probe->Prepare(48000.0, 512);
    const auto cases = Cases(*probe);
    const auto parameters = probe->Parameters();

    struct Job { std::size_t index; double rate; int block; };
    std::vector<Job> jobs;
    for (std::size_t k = 0; k < cases.size(); ++k)
        for (int config = 0; config < 9; ++config)
            if ((!options.select || options.select(k, cases[k]))
                && (options.everyConfiguration || k < 4 || static_cast<std::size_t>(config) == k % 9))
                jobs.push_back({ k, kRates[config / 3], kBlocks[config % 3] });

    std::atomic<std::size_t> next { 0 };
    std::atomic<int> failures { 0 };
    std::mutex print;
    const auto work = [&] {
        for (std::size_t j; (j = next++) < jobs.size();) {
            const auto& job = jobs[j];
            const auto& c = cases[job.index];
            const std::string problem = Play(make, c, job.rate, job.block);
            if (problem.empty()) continue;
            ++failures;
            std::lock_guard<std::mutex> lock(print);
            std::printf("FAIL %s @ %g Hz, block %d: %s\n", c.label.c_str(), job.rate, job.block, problem.c_str());
            if (c.label.rfind("pairs#", 0) == 0)
                for (std::size_t i = 0; i < c.values.size(); ++i)
                    std::printf("    %s = %g\n", parameters[i].name.c_str(), static_cast<double>(c.values[i]));
            std::fflush(stdout);
        }
    };
    std::vector<std::thread> workers;
    for (int i = 1; i < options.jobs; ++i) workers.emplace_back(work);
    work();
    for (auto& worker : workers) worker.join();
    std::printf("%zu parameters, %zu cases, %zu runs, %d failed\n", parameters.size(), cases.size(), jobs.size(),
                failures.load());
    return failures.load();
}

// The standard command line (U9), so every repo's matrix test and every
// workflow that shards it read the same:
//
//   [--jobs N] [--rotation] [--shard K/N] [--case LABEL] [--count] [--list]
//
// --rotation runs each case at one rate and block size; --shard K/N runs the
// cases whose index modulo N is K, numbered from 0; --case runs the one case
// with that label; --count and --list print the number of cases and the
// parameters instead of running. `options` carries the product's own settings
// (a product strips its own flags before calling this). Returns the exit code.
inline int Main(int argc, char** argv, const std::function<std::unique_ptr<Device>()>& make, Options options = {}) {
    unsigned long shard = 0, shards = 1;
    bool count = false, list = false;
    std::string only;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--rotation") options.everyConfiguration = false;
        else if (arg == "--count") count = true;
        else if (arg == "--list") list = true;
        else if (arg == "--jobs" && i + 1 < argc) options.jobs = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--case" && i + 1 < argc) only = argv[++i];
        else if (arg == "--shard" && i + 1 < argc && std::sscanf(argv[++i], "%lu/%lu", &shard, &shards) == 2
                 && shard < shards) {}
        else {
            std::fprintf(stderr, "usage: %s [--jobs N] [--rotation] [--shard K/N] [--case LABEL] [--count] [--list]\n",
                         argc > 0 ? argv[0] : "ParameterMatrix");
            return 2;
        }
    }
    if (count || list || !only.empty()) {
        const auto probe = make();
        probe->Prepare(48000.0, 512);
        const auto cases = Cases(*probe);
        if (count) std::printf("%zu cases\n", cases.size());
        if (list) {
            const auto parameters = probe->Parameters();
            for (std::size_t p = 0; p < parameters.size(); ++p)
                std::printf("%zu %s (%d steps)\n", p, parameters[p].name.c_str(), parameters[p].steps);
        }
        if (count || list) return 0;
        // A mistyped label would otherwise select nothing and pass.
        if (std::none_of(cases.begin(), cases.end(), [&](const Case& c) { return c.label == only; })) {
            std::fprintf(stderr, "no case is labelled %s\n", only.c_str());
            return 2;
        }
    }
    if (shards > 1 || !only.empty())
        options.select = [shard, shards, only, product = options.select](std::size_t index, const Case& c) {
            return index % shards == shard && (only.empty() || c.label == only) && (!product || product(index, c));
        };
    return Run(make, options) == 0 ? 0 : 1;
}

#if defined(JUCE_AUDIO_PROCESSORS_H_INCLUDED)
// A juce::AudioProcessor as a Device: its own parameter list (discrete
// parameters by getNumSteps()), its programs as the factory presets and its
// state chunk. Output is the first two channels of the main bus.
class JuceDevice : public Device {
public:
    explicit JuceDevice(std::unique_ptr<juce::AudioProcessor> p) : processor(std::move(p)) {
        for (auto* parameter : processor->getParameters())
            if (parameter->isAutomatable()) automatable.push_back(parameter);
    }
    ~JuceDevice() override { processor->releaseResources(); }

    std::vector<Parameter> Parameters() const override {
        std::vector<Parameter> result;
        for (auto* p : automatable) {
            const int steps = p->isDiscrete() || p->isBoolean() ? p->getNumSteps() : 0;
            result.push_back({ p->getName(64).toStdString(), steps >= 2 ? steps : 0, p->getDefaultValue() });
        }
        return result;
    }
    int PresetCount() const override { return processor->getNumPrograms() > 1 ? processor->getNumPrograms() : 0; }
    void Prepare(double sampleRate, int maxBlockFrames) override {
        processor->setRateAndBufferSizeDetails(sampleRate, maxBlockFrames);
        processor->prepareToPlay(sampleRate, maxBlockFrames);
        // A host passes max(inputs, outputs) channels; inputs stay silent.
        buffer.setSize(std::max({ 2, processor->getTotalNumInputChannels(), processor->getTotalNumOutputChannels() }),
                       maxBlockFrames);
    }
    void LoadPreset(int index) override { processor->setCurrentProgram(index); }
    float GetParameter(std::size_t index) const override { return automatable[index]->getValue(); }
    void SetParameter(std::size_t index, float normalised) override {
        automatable[index]->setValueNotifyingHost(normalised);
    }
    void Process(float* left, float* right, int frames, const Event* events, int eventCount) override {
        buffer.setSize(buffer.getNumChannels(), frames, false, false, true);
        buffer.clear();
        midi.clear();
        for (int i = 0; i < eventCount; ++i) midi.addEvent(events[i].bytes, events[i].size, events[i].offset);
        processor->processBlock(buffer, midi);
        std::copy_n(buffer.getReadPointer(0), frames, left);
        std::copy_n(buffer.getReadPointer(1), frames, right);
    }
    std::vector<std::uint8_t> SaveState() override {
        juce::MemoryBlock block;
        processor->getStateInformation(block);
        const auto* bytes = static_cast<const std::uint8_t*>(block.getData());
        return { bytes, bytes + block.getSize() };
    }
    void RestoreState(const std::vector<std::uint8_t>& state) override {
        processor->setStateInformation(state.data(), static_cast<int>(state.size()));
    }

protected:
    std::unique_ptr<juce::AudioProcessor> processor;

private:
    std::vector<juce::AudioProcessorParameter*> automatable;
    juce::AudioBuffer<float> buffer;
    juce::MidiBuffer midi;
};
#endif

} // namespace protocodus::matrix
