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
// The same Device runs the U11 parameter-reach test (RunReach(), `--reach`): every
// parameter, the editor's and the host's alike, must audibly change notes
// held through the change, new notes after it and, after long play, every
// later change back and forth. See the comment above namespace reach.
//
// When including JUCE, include it before this header to get JuceDevice, which
// takes the parameter list, programs and state from a juce::AudioProcessor.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
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

// How a parameter must reach the sound in the U11 reach test.
enum class Reach {
    Held,     // a change also audibly alters notes already sounding (the default)
    NewNotes, // by design a change shapes only the notes struck after it
    Exempt,   // by design inaudible on the reach phrase; the reason names what covers it
};

struct ReachRule {
    Reach reach = Reach::Held;
    // Required for NewNotes and Exempt: the mechanism that makes it so, as the
    // design states it. The test prints it with the results.
    std::string reason;
    // Other parameters (index, normalised value) set before every run, so a
    // parameter the base patch leaves inaudible can be heard: a compressor's
    // time constants with the compressor on. Preferred to an exemption.
    std::vector<std::pair<std::size_t, float>> with;
    // Set when what the parameter changes runs on state the playing before
    // it leaves (free-running voices beating in the phase their old tuning
    // left, a slow LFO's phase, a glide starting wherever its voice is): the
    // reason probe 1 need not sound like T, so it is not compared with T,
    // and a later probe whose run without its change already sounds as the
    // value it sets need not move. On an Exempt rule: what the long play
    // exercises and the probes do not, so R and T differ by that history
    // alone (see reach).
    std::string history;
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

    // Parameters the editor can change but a host cannot automate, numbered
    // after Parameters() in GetParameter and SetParameter. The matrix leaves
    // them where they are; the reach test changes them too.
    virtual std::vector<Parameter> EditorOnlyParameters() const { return {}; }
    // SetParameter is the editor's path (an attachment's); this is the host's
    // automation path, for a device where the two differ.
    virtual void AutomateParameter(std::size_t index, float normalised) { SetParameter(index, normalised); }
    // How the parameter at `index` (counting editor-only ones after the rest)
    // must reach the sound when the reach test moves it to `value`. Called
    // from several threads at once.
    virtual ReachRule ReachFor(std::size_t /*index*/, float /*value*/) const { return {}; }
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

// ---------------------------------------------------------------------------
// U11 parameter reach: no parameter goes stale.
//
// A parameter is stale when the editor or the host changes it and the sound
// does not follow, which the matrix above cannot see: it checks that the
// output stays sane, not that it changes. For every parameter and every value
// it can move to from the base patch (the device as prepared; a continuous
// parameter moves to its farther end, a discrete one to each other value),
// RunReach() renders one long run at 48 kHz in 64-frame blocks:
//
//   0 s     6 s of long play at the base value: overlapping notes over C3-C6,
//           chords, a twelve-note cluster beyond most voice limits, bends
//   6.0 s   a chord is held (G3 D4 B4: either side of middle C, where key
//           scaling and key tracking usually pivot)
//   6.5 s   change 1, to the test value, through the editor's path
//   7.0 s   the held chord is released
//   7.5 s   probe 1: G3 E4 C5, bent up and back, released after 0.8 s
//   9.0 s   change 2, back to the base value, through the host's path
//   9.5 s   probe 2
//   11.0 s  change 3, to the test value again, through the host's path
//   11.5 s  probe 3, and the run ends at 13 s
//
// and checks it against the same run with the parameter never changed (R),
// the same run with the parameter at the test value from the start (T) and,
// for each change, the run without that change (the run itself until it,
// R for change 1). They all share their notes, so whatever the long play
// leaves behind (voice and string allocation, tails, a room, random draws)
// is common to them:
//
//   effect  R and T differ audibly during probe 1, or the parameter does
//           nothing a player can hear on a new chord: a failure, unless
//           declared Exempt
//   held    from 6.6 s to 7.5 s the run differs audibly from R, which shares
//           its history exactly until change 1 (Held parameters only)
//   probes  each probe sounds within 20 dB of the quieter of R and T (an
//           instrument that ignores notes after a change fails here rather
//           than passing as a change that moved), differs audibly from the
//           run without its change, and probe 1 moved towards T: its
//           distance to T, plus a quarter of how far it moved, is no more
//           than R's. Where they differ in phase
//           but not in spectrum (a delay, an allpass) the difference signal
//           decides, by 3 dB, and where something slow is out of phase (an
//           LFO that ran at another rate before the change) the spectrum
//           averaged over the probe does. A rule that names the history its
//           parameter acts on skips this comparison: free-running voices
//           detuned at change 1 beat in the phase their old tuning left, and
//           a glide starts wherever its voice is, unlike T's. Later probes
//           need not sound like T or R either: their history differs from
//           both, and an instrument that remembers its playing (a hand
//           position on the neck, a string retuned while it rings) need not
//           sound like either; the run without the change still shows
//           whether it took, unless the named history had already brought
//           the instrument to the value the change sets (a probe's bend
//           ends at the centre a Pitch Bend parameter returns to).
//
// "Audibly" is a mean 1/12-octave band-level difference of at least 0.5 dB
// (about one level JND) over the cells within 60 dB of the loudest, or a
// difference signal no more than 20 dB below the reference. A parameter that
// by design shapes only new notes (an attack property, a voice count) is
// declared NewNotes, and one the phrase cannot exercise is declared Exempt,
// each with its reason (Device::ReachFor, per value). One the base patch
// leaves inaudible (a modulator's rate while its depth is zero) is tested on
// the first factory preset where it is audible, as a player meets it, or with
// the other parameters its rule names set first (a compressor's attack with
// the compressor on). An exemption is only honoured while the parameter stays
// inaudible on the base patch: an audible one is checked anyway, unless its
// rule names the history it acts on and probe 1 does not follow the change
// (sharping levers, which only keys between strings use: the long play plays
// such keys, the probes do not). R and T then differ by that history alone,
// and the product's own tests must show the parameter reaching it.
namespace reach {

constexpr double kRate = 48000.0;
constexpr int kBlock = 64; // Reason's batch, and a small host buffer
constexpr double kWarm = 6.0, kHeldOn = kWarm, kChange1 = kWarm + 0.5, kHeldOff = kWarm + 1.0;
constexpr double kProbes[3] = { kWarm + 1.5, kWarm + 3.5, kWarm + 5.5 };
constexpr double kChanges[3] = { kChange1, kWarm + 3.0, kWarm + 5.0 };
constexpr double kEnd = kWarm + 7.0;
constexpr double kProbeSeconds = 1.5;
constexpr double kAudibleDb = 0.5, kAudibleDifferenceDb = -20.0, kMargin = 0.25, kDifferenceMarginDb = 3.0;
constexpr double kLostDb = 20.0;
constexpr double kRangeDb = 60.0;
constexpr int kFftSize = 2048;

struct Timed { long long at; Event event; };
struct Change { long long at; std::size_t index; float value; bool automation; };
struct Audio { std::vector<float> left, right; };

inline long long Frame(double seconds) { return static_cast<long long>(std::llround(seconds * kRate)); }

inline void Note(std::vector<Timed>& out, double on, double off, int note, int velocity) {
    out.push_back({ Frame(on), { 0, { 0x90, static_cast<std::uint8_t>(note), static_cast<std::uint8_t>(velocity) }, 3 } });
    out.push_back({ Frame(off), { 0, { 0x80, static_cast<std::uint8_t>(note), 0 }, 3 } });
}

// A triangle of pitch bend from centre to `depth` (-1..1) and back, in 20 ms steps.
inline void Bend(std::vector<Timed>& out, double start, double end, double depth) {
    for (int k = 0; start + 0.02 * k <= end + 1.0e-9; ++k) {
        const double x = 0.02 * k / (end - start);
        const double shape = x < 0.5 ? 2.0 * x : 2.0 * (1.0 - x);
        const int value = std::clamp(8192 + static_cast<int>(std::lround(8191.0 * depth * shape)), 0, 16383);
        out.push_back({ Frame(start + 0.02 * k),
                        { 0, { 0xE0, static_cast<std::uint8_t>(value & 0x7F), static_cast<std::uint8_t>(value >> 7) }, 3 } });
    }
}

inline void Probe(std::vector<Timed>& out, double at) {
    Note(out, at, at + 0.8, 55, 100);
    Note(out, at, at + 0.8, 64, 80);
    Note(out, at, at + 0.8, 72, 127);
    Bend(out, at + 0.2, at + 0.6, 1.0);
}

inline void Sort(std::vector<Timed>& events) {
    std::stable_sort(events.begin(), events.end(), [](const Timed& a, const Timed& b) { return a.at < b.at; });
}

inline std::vector<Timed> LongPhrase() {
    std::vector<Timed> events;
    for (int i = 0; i < 48; ++i) // eight notes a second, walking in fifths over C3-C6
        Note(events, 0.125 * i, 0.125 * i + 0.375, 48 + (7 * i) % 37, 40 + (37 * i) % 88);
    for (const double at : { 0.5, 2.5, 4.5 })
        for (const int note : { 50, 57, 62, 66 }) Note(events, at, at + 0.75, note, 110);
    for (int k = 0; k < 12; ++k) Note(events, 3.25, 3.75, 48 + 3 * k, 90);
    Bend(events, 1.0, 2.0, 1.0);
    Bend(events, 4.0, 4.5, -1.0);
    for (const int note : { 55, 62, 71 }) Note(events, kHeldOn, kHeldOff, note, 100);
    for (const double at : kProbes) Probe(events, at);
    Sort(events);
    return events;
}

// Changes land at block starts, as a host delivers them.
inline Audio Render(Device& device, const std::vector<Timed>& events, std::vector<Change> changes, double seconds) {
    const auto total = Frame(seconds);
    Audio audio { std::vector<float>(static_cast<std::size_t>(total)), std::vector<float>(static_cast<std::size_t>(total)) };
    std::vector<Event> block;
    std::size_t nextEvent = 0, nextChange = 0;
    for (long long done = 0; done < total; done += kBlock) {
        const int frames = static_cast<int>(std::min<long long>(kBlock, total - done));
        for (; nextChange < changes.size() && changes[nextChange].at <= done; ++nextChange) {
            const Change& c = changes[nextChange];
            if (c.automation) device.AutomateParameter(c.index, c.value);
            else device.SetParameter(c.index, c.value);
        }
        block.clear();
        for (; nextEvent < events.size() && events[nextEvent].at < done + frames; ++nextEvent) {
            Event e = events[nextEvent].event;
            e.offset = static_cast<int>(std::max<long long>(0, events[nextEvent].at - done));
            block.push_back(e);
        }
        device.Process(&audio.left[static_cast<std::size_t>(done)], &audio.right[static_cast<std::size_t>(done)], frames,
                       block.data(), static_cast<int>(block.size()));
    }
    return audio;
}

inline void Fft(std::vector<std::complex<double>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t length = 2; length <= n; length <<= 1) {
        const double angle = -6.283185307179586 / static_cast<double>(length);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < n; i += length) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < length / 2; ++k, w *= step) {
                const auto u = a[i + k], v = a[i + k + length / 2] * w;
                a[i + k] = u + v;
                a[i + k + length / 2] = u - v;
            }
        }
    }
}

// Band levels in dB, per Hann frame of kFftSize (hop half that), 1/12-octave
// band from 40 Hz to 16 kHz (bands narrower than a bin merge) and channel;
// with `average`, each band's power over all the frames instead.
struct Levels { std::vector<float> db; };

inline Levels Analyse(const Audio& audio, double from, double seconds, bool average = false) {
    std::vector<std::size_t> edges;
    for (int k = 0;; ++k) {
        const double hz = 40.0 * std::pow(2.0, k / 12.0);
        if (hz > 16000.0) break;
        const auto bin = static_cast<std::size_t>(std::ceil(hz * kFftSize / kRate));
        if (edges.empty() || bin > edges.back()) edges.push_back(bin);
    }
    const std::size_t bands = edges.size() - 1;
    Levels levels;
    std::vector<double> sums(2 * bands, 0.0);
    const long long start = Frame(from), stop = Frame(from + seconds);
    std::vector<std::complex<double>> spectrum(kFftSize);
    for (long long frame = start; frame + kFftSize <= stop; frame += kFftSize / 2)
        for (int c = 0; c < 2; ++c) {
            const auto& channel = c == 0 ? audio.left : audio.right;
            for (int i = 0; i < kFftSize; ++i) {
                const double hann = 0.5 - 0.5 * std::cos(6.283185307179586 * i / kFftSize);
                spectrum[static_cast<std::size_t>(i)] = hann * channel[static_cast<std::size_t>(frame + i)];
            }
            Fft(spectrum);
            for (std::size_t b = 0; b < bands; ++b) {
                double power = 0.0;
                for (std::size_t bin = edges[b]; bin < edges[b + 1]; ++bin) power += std::norm(spectrum[bin]);
                if (average) sums[c * bands + b] += power;
                else levels.db.push_back(static_cast<float>(10.0 * std::log10(power + 1.0e-30)));
            }
        }
    if (average)
        for (const double power : sums) levels.db.push_back(static_cast<float>(10.0 * std::log10(power + 1.0e-30)));
    return levels;
}

// Mean absolute level difference between x and y, in dB, over the cells
// where either reference is within kRangeDb of the references' loudest cell;
// a cell quieter than that floor counts as the floor.
inline double Distance(const Levels& x, const Levels& y, const Levels& a, const Levels& b) {
    float top = -1.0e30f;
    for (const auto* r : { &a, &b })
        for (const float v : r->db) top = std::max(top, v);
    const float floor = top - static_cast<float>(kRangeDb);
    double sum = 0.0;
    std::size_t cells = 0;
    for (std::size_t c = 0; c < a.db.size(); ++c) {
        if (a.db[c] < floor && b.db[c] < floor) continue;
        sum += std::fabs(std::max(x.db[c], floor) - std::max(y.db[c], floor));
        ++cells;
    }
    return cells ? sum / static_cast<double>(cells) : 0.0;
}

// The level of x - y relative to y, in dB, over [from, from + seconds) of x
// and [at, at + seconds) of y.
inline double DifferenceDb(const Audio& x, double from, const Audio& y, double at, double seconds) {
    const long long n = Frame(seconds), i0 = Frame(from), j0 = Frame(at);
    double difference = 0.0, reference = 0.0;
    for (long long k = 0; k < n; ++k)
        for (const auto channel : { &Audio::left, &Audio::right }) {
            const double u = (x.*channel)[static_cast<std::size_t>(i0 + k)], v = (y.*channel)[static_cast<std::size_t>(j0 + k)];
            difference += (u - v) * (u - v);
            reference += v * v;
        }
    if (!(difference > 0.0)) return -INFINITY;
    return reference > 0.0 ? 10.0 * std::log10(difference / reference) : INFINITY;
}

inline double EnergyDb(const Audio& audio, double from, double seconds) {
    double energy = 0.0;
    for (long long k = Frame(from), end = Frame(from + seconds); k < end; ++k)
        for (const auto channel : { &Audio::left, &Audio::right })
            energy += (audio.*channel)[static_cast<std::size_t>(k)] * (audio.*channel)[static_cast<std::size_t>(k)];
    return 10.0 * std::log10(energy + 1.0e-30);
}

inline bool Audible(double distanceDb, double differenceDb) {
    return distanceDb >= kAudibleDb || differenceDb >= kAudibleDifferenceDb;
}

// R on each factory preset, rendered the first time a change needs it.
struct PresetRun {
    std::once_flag once;
    std::vector<float> values;
    Audio unchanged;
};

struct Base {
    std::vector<Parameter> parameters; // Parameters() then EditorOnlyParameters()
    std::size_t automatable = 0;
    std::vector<float> values;         // the base patch
    std::unique_ptr<Device> rules;     // asked for each change's ReachRule
    Audio unchanged;                   // R
    mutable std::deque<PresetRun> presets;
};

inline const PresetRun& OnPreset(const std::function<std::unique_ptr<Device>()>& make, const Base& base, int preset) {
    PresetRun& run = base.presets[static_cast<std::size_t>(preset)];
    std::call_once(run.once, [&] {
        auto device = make();
        device->Prepare(kRate, kBlock);
        device->LoadPreset(preset);
        for (std::size_t i = 0; i < base.parameters.size(); ++i) run.values.push_back(device->GetParameter(i));
        run.unchanged = Render(*device, LongPhrase(), {}, kEnd);
    });
    return run;
}

// The values each parameter moves to from the base patch: a continuous one
// (or a discrete one of more than 12 values) to its farther end, a discrete
// one to every other value.
inline std::vector<float> Targets(const Parameter& p, float base) {
    if (p.steps < 2 || p.steps > 12) return { base < 0.5f ? 1.0f : 0.0f };
    std::vector<float> targets;
    const int at = static_cast<int>(std::lround(base * static_cast<float>(p.steps - 1)));
    for (int k = 0; k < p.steps; ++k)
        if (k != at) targets.push_back(static_cast<float>(k) / static_cast<float>(p.steps - 1));
    return targets;
}

// One parameter moving to one value; empty when it reaches. `report` gets the
// figures either way.
inline std::string Check(const std::function<std::unique_ptr<Device>()>& make, const Base& base, std::size_t index,
                         float target, std::string& report) {
    const ReachRule rule = base.rules->ReachFor(index, target);
    if (rule.reach != Reach::Held && rule.reason.empty()) return "declared NewNotes or Exempt without a reason";
    for (const auto& [other, value] : rule.with)
        if (other == index || other >= base.parameters.size() || !(value >= 0.0f && value <= 1.0f))
            return "a ReachRule sets the parameter itself, one that does not exist or a value outside 0..1";
    const bool editorOnly = index >= base.automatable;

    int preset = -1; // the base patch, or the factory preset the change is tested on
    const auto render = [&](const std::vector<Change>& changes) {
        std::vector<Change> all;
        for (const auto& [other, value] : rule.with) all.push_back({ 0, other, value, false });
        all.insert(all.end(), changes.begin(), changes.end());
        auto device = make();
        device->Prepare(kRate, kBlock);
        if (preset >= 0) device->LoadPreset(preset);
        return Render(*device, LongPhrase(), all, kEnd);
    };
    const auto analyse = [](const Audio& audio, double at) { return Analyse(audio, at, kProbeSeconds); };

    float from = base.values[index];
    const Audio withUnchanged = rule.with.empty() ? Audio {} : render({});
    const Audio* unchanged = rule.with.empty() ? &base.unchanged : &withUnchanged;
    Audio atTarget = render({ { 0, index, target, false } }); // T
    double effect = 0.0, effectDifference = 0.0;
    const auto measure = [&] {
        const Levels r1 = analyse(*unchanged, kProbes[0]), t1 = analyse(atTarget, kProbes[0]);
        effect = Distance(r1, t1, r1, t1);
        effectDifference = DifferenceDb(atTarget, kProbes[0], *unchanged, kProbes[0], kProbeSeconds);
        return Audible(effect, effectDifference);
    };
    // A parameter the base patch leaves inaudible (a modulator's rate while
    // its depth is zero) is tested on the first factory preset that does not,
    // as a player would meet it, unless the rule sets its own context.
    const bool search = !measure() && rule.with.empty() && rule.reach != Reach::Exempt && !base.presets.empty();
    if (search) {
        const double baseEffect = effect, baseDifference = effectDifference;
        for (int p = 0; p < static_cast<int>(base.presets.size()) && preset < 0; ++p) {
            const PresetRun& on = OnPreset(make, base, p);
            if (on.values[index] == target) continue;
            preset = p;
            from = on.values[index];
            unchanged = &on.unchanged;
            atTarget = render({ { 0, index, target, false } });
            if (!measure()) preset = -1;
        }
        if (preset < 0) {
            effect = baseEffect;
            effectDifference = baseDifference;
        }
    }
    char text[256];
    if (preset >= 0) std::snprintf(text, sizeof text, "on preset %d (from %g), ", preset, static_cast<double>(from));
    else std::snprintf(text, sizeof text, "%s", rule.with.empty() ? "" : "with others set, ");
    report = text;
    std::snprintf(text, sizeof text, "new chord %.2f dB apart (difference %.0f dB)", effect, effectDifference);
    report += text;
    if (!Audible(effect, effectDifference)) {
        if (rule.reach == Reach::Exempt) {
            report += "; exempt: " + rule.reason;
            return {};
        }
        return report + (search ? ": no audible effect on a new chord, on the base patch or any factory preset"
                                : ": no audible effect on a new chord")
                      + " (declare it Exempt only if the design says so)";
    }

    const std::vector<Change> changes {
        { Frame(kChanges[0]), index, target, false },
        { Frame(kChanges[1]), index, from, !editorOnly },
        { Frame(kChanges[2]), index, target, !editorOnly },
    };
    const Audio run = render(changes);
    if (rule.reach == Reach::Exempt && !rule.history.empty()) {
        const Levels p = analyse(run, kProbes[0]), w = analyse(*unchanged, kProbes[0]);
        const double moved = Distance(p, w, p, w);
        const double movedDifference = DifferenceDb(run, kProbes[0], *unchanged, kProbes[0], kProbeSeconds);
        std::snprintf(text, sizeof text, "; probe 1 moved %.2f dB (%.0f dB)", moved, movedDifference);
        report += text;
        if (Audible(moved, movedDifference))
            return report + ": declared Exempt for what the long play leaves, yet probe 1 follows the change";
        report += "; exempt: " + rule.reason + "; R and T differ by " + rule.history;
        return {};
    }
    const double heldFrom = kChange1 + 0.1, heldSeconds = kProbes[0] - heldFrom;
    const Levels heldRun = Analyse(run, heldFrom, heldSeconds), heldUnchanged = Analyse(*unchanged, heldFrom, heldSeconds);
    const double held = Distance(heldRun, heldUnchanged, heldRun, heldUnchanged);
    const double heldDifference = DifferenceDb(run, heldFrom, *unchanged, heldFrom, heldSeconds);
    std::snprintf(text, sizeof text, "; held notes %.2f dB (%.0f dB)", held, heldDifference);
    report += text;
    if (rule.reach == Reach::NewNotes) report += "; new notes only: " + rule.reason;
    else if (!Audible(held, heldDifference)) return report + ": notes held through the change do not follow it";

    for (int k = 0; k < 3; ++k) {
        const double at = kProbes[k];
        // A dropped note is far from the run without the change, so it
        // would pass as one that moved: it must sound as R or T does.
        const double level = EnergyDb(run, at, kProbeSeconds);
        const double quieter = std::min(EnergyDb(*unchanged, at, kProbeSeconds), EnergyDb(atTarget, at, kProbeSeconds));
        if (level < quieter - kLostDb) {
            std::snprintf(text, sizeof text, "; probe %d %.0f dB under R and T: the notes played after change %d were lost",
                          k + 1, quieter - level, k + 1);
            return report + text;
        }
        // The run without change k: until it, the run itself; from it on,
        // the value it would have kept.
        const Audio without = k == 0 ? *unchanged : render(std::vector<Change>(changes.begin(), changes.begin() + k));
        const Levels p = analyse(run, at), w = analyse(without, at);
        const double moved = Distance(p, w, p, w), movedDifference = DifferenceDb(run, at, without, at, kProbeSeconds);
        std::snprintf(text, sizeof text, "; probe %d moved %.2f dB (%.0f dB)", k + 1, moved, movedDifference);
        report += text;
        if (!Audible(moved, movedDifference)) {
            // A declared history can bring the instrument back to the value
            // change k sets before the change comes (each probe's bend ends
            // at the centre a Pitch Bend parameter rests at): the run without
            // it then already sounds as that value does. Undeclared, the same
            // sign is a value lost a while after it was set, and fails.
            if (k > 0 && !rule.history.empty()) {
                const Audio& value = k == 1 ? *unchanged : atTarget;
                const Levels v = analyse(value, at);
                if (!Audible(Distance(w, v, w, v), DifferenceDb(without, at, value, at, kProbeSeconds))) {
                    std::snprintf(text, sizeof text, "; probe %d already sounds as change %d leaves it: ", k + 1, k + 1);
                    report += text + rule.history;
                    continue;
                }
            }
            std::snprintf(text, sizeof text, ": probe %d sounds as it would without change %d", k + 1, k + 1);
            return report + text;
        }
        if (k > 0) continue;
        if (!rule.history.empty()) {
            report += "; not compared with T: " + rule.history;
            continue;
        }
        // Change 1 must also move probe 1 towards T, the run held at the
        // test value: closer to it by a quarter of the way it moved. Where
        // they differ in phase but not in spectrum (a delay, an allpass) the
        // difference signal decides, by 3 dB; where they differ in the phase
        // of something slow (an LFO, beating between detuned voices) that
        // ran at another rate before the change, the probe's spectrum
        // averaged over its frames does.
        const Levels t = analyse(atTarget, at);
        double after = 0.0, before = 0.0;
        bool towards = false;
        if (moved >= kAudibleDb) {
            after = Distance(p, t, p, t);
            before = Distance(w, t, w, t);
            towards = after + kMargin * moved <= before;
            if (!towards) {
                const auto average = [&](const Audio& audio) { return Analyse(audio, at, kProbeSeconds, true); };
                const Levels pa = average(run), wa = average(without), ta = average(atTarget);
                const double movedAverage = Distance(pa, wa, pa, wa);
                const double afterAverage = Distance(pa, ta, pa, ta), beforeAverage = Distance(wa, ta, wa, ta);
                towards = movedAverage >= kAudibleDb && afterAverage + kMargin * movedAverage <= beforeAverage;
                std::snprintf(text, sizeof text, " (averaged %.2f from %.2f)", afterAverage, beforeAverage);
                report += text;
            }
        } else {
            after = DifferenceDb(run, at, atTarget, at, kProbeSeconds);
            before = DifferenceDb(without, at, atTarget, at, kProbeSeconds);
            towards = after + kDifferenceMarginDb <= before;
        }
        std::snprintf(text, sizeof text, ", to %.2f from %.2f dB of the run held at %g", after, before,
                      static_cast<double>(target));
        report += text;
        if (!towards) return report + ": probe 1 moves no closer to the run held at the value it was set to";
    }
    return {};
}

inline Base Prepare(const std::function<std::unique_ptr<Device>()>& make) {
    Base base;
    auto device = make();
    device->Prepare(kRate, kBlock);
    base.parameters = device->Parameters();
    base.automatable = base.parameters.size();
    for (const auto& p : device->EditorOnlyParameters()) base.parameters.push_back(p);
    for (std::size_t i = 0; i < base.parameters.size(); ++i) base.values.push_back(device->GetParameter(i));
    for (int p = 0; p < device->PresetCount(); ++p) base.presets.emplace_back();
    base.unchanged = Render(*device, LongPhrase(), {}, kEnd);
    base.rules = make();
    base.rules->Prepare(kRate, kBlock);
    return base;
}

} // namespace reach

// Runs the reach test, printing every change with its figures, and returns
// the number that fail. `only`, when not empty, names the one parameter to test.
inline int RunReach(const std::function<std::unique_ptr<Device>()>& make, int jobs = 1, const std::string& only = {}) {
    const reach::Base base = reach::Prepare(make);
    struct Job { std::size_t index; float target; };
    std::vector<Job> work;
    for (std::size_t i = 0; i < base.parameters.size(); ++i)
        if (only.empty() || base.parameters[i].name == only)
            for (const float target : reach::Targets(base.parameters[i], base.values[i])) work.push_back({ i, target });
    std::vector<std::string> lines(work.size());
    std::atomic<std::size_t> next { 0 };
    std::atomic<int> failures { 0 };
    const auto run = [&] {
        for (std::size_t j; (j = next++) < work.size();) {
            std::string report;
            const std::string problem = reach::Check(make, base, work[j].index, work[j].target, report);
            failures += !problem.empty();
            char head[160];
            std::snprintf(head, sizeof head, "%s %s %g -> %g: ", problem.empty() ? "PASS" : "FAIL",
                          base.parameters[work[j].index].name.c_str(), static_cast<double>(base.values[work[j].index]),
                          static_cast<double>(work[j].target));
            lines[j] = head + (problem.empty() ? report : problem);
        }
    };
    std::vector<std::thread> workers;
    for (int i = 1; i < jobs; ++i) workers.emplace_back(run);
    run();
    for (auto& worker : workers) worker.join();
    for (const auto& line : lines) std::printf("%s\n", line.c_str());
    std::printf("%zu parameters, %zu changes, %d failed\n", base.parameters.size(), work.size(), failures.load());
    return failures.load();
}

// The standard command line (U9), so every repo's matrix test and every
// workflow that shards it read the same:
//
//   [--jobs N] [--rotation] [--shard K/N] [--case LABEL] [--count] [--list]
//   --reach [--jobs N] [--case PARAMETER]
//
// --rotation runs each case at one rate and block size; --shard K/N runs the
// cases whose index modulo N is K, numbered from 0; --case runs the one case
// with that label; --count and --list print the number of cases and the
// parameters instead of running. --reach runs the U11 reach test instead of
// the matrix (registered as <Name>.ParameterReach, label unit), where --case
// names one parameter. `options` carries the product's own settings (a
// product strips its own flags before calling this). Returns the exit code.
inline int Main(int argc, char** argv, const std::function<std::unique_ptr<Device>()>& make, Options options = {}) {
    unsigned long shard = 0, shards = 1;
    bool count = false, list = false, reachTest = false;
    std::string only;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--rotation") options.everyConfiguration = false;
        else if (arg == "--reach") reachTest = true;
        else if (arg == "--count") count = true;
        else if (arg == "--list") list = true;
        else if (arg == "--jobs" && i + 1 < argc) options.jobs = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--case" && i + 1 < argc) only = argv[++i];
        else if (arg == "--shard" && i + 1 < argc && std::sscanf(argv[++i], "%lu/%lu", &shard, &shards) == 2
                 && shard < shards) {}
        else {
            std::fprintf(stderr, "usage: %s [--jobs N] [--rotation] [--shard K/N] [--case LABEL] [--count] [--list]\n"
                                 "       %s --reach [--jobs N] [--case PARAMETER]\n",
                         argc > 0 ? argv[0] : "ParameterMatrix", argc > 0 ? argv[0] : "ParameterMatrix");
            return 2;
        }
    }
    if (reachTest) {
        if (count || list || shards > 1) {
            std::fprintf(stderr, "--reach takes only --jobs and --case\n");
            return 2;
        }
        if (!only.empty()) {
            const auto probe = make();
            probe->Prepare(48000.0, 512);
            auto parameters = probe->Parameters();
            for (const auto& p : probe->EditorOnlyParameters()) parameters.push_back(p);
            if (std::none_of(parameters.begin(), parameters.end(), [&](const Parameter& p) { return p.name == only; })) {
                std::fprintf(stderr, "no parameter is named %s\n", only.c_str());
                return 2;
            }
        }
        return RunReach(make, options.jobs, only) == 0 ? 0 : 1;
    }
    if (count || list || !only.empty()) {
        const auto probe = make();
        probe->Prepare(48000.0, 512);
        const auto cases = Cases(*probe);
        if (count) std::printf("%zu cases\n", cases.size());
        if (list) {
            auto parameters = probe->Parameters();
            const std::size_t automatable = parameters.size();
            for (const auto& p : probe->EditorOnlyParameters()) parameters.push_back(p);
            for (std::size_t p = 0; p < parameters.size(); ++p) {
                // The rule for the value the reach test moves it to first.
                const ReachRule rule = probe->ReachFor(p, reach::Targets(parameters[p], probe->GetParameter(p)).front());
                std::printf("%zu %s (%d steps)%s%s%s\n", p, parameters[p].name.c_str(), parameters[p].steps,
                            p >= automatable ? ", editor only" : "",
                            rule.reach == Reach::NewNotes ? ", reaches new notes only: " : rule.reach == Reach::Exempt ? ", reach exempt: " : "",
                            rule.reason.c_str());
            }
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
// parameters by getNumSteps(); the non-automatable ones as editor-only), its
// programs as the factory presets and its state chunk. Output is the first
// two channels of the main bus. SetParameter is an attachment's
// setValueNotifyingHost; AutomateParameter is what a plug-in wrapper does
// with host automation, setValue and then the listeners.
class JuceDevice : public Device {
public:
    explicit JuceDevice(std::unique_ptr<juce::AudioProcessor> p) : processor(std::move(p)) {
        for (auto* parameter : processor->getParameters())
            (parameter->isAutomatable() ? automatable : editorOnly).push_back(parameter);
    }
    ~JuceDevice() override { processor->releaseResources(); }

    std::vector<Parameter> Parameters() const override { return Describe(automatable); }
    std::vector<Parameter> EditorOnlyParameters() const override { return Describe(editorOnly); }
    int PresetCount() const override { return processor->getNumPrograms() > 1 ? processor->getNumPrograms() : 0; }
    void Prepare(double sampleRate, int maxBlockFrames) override {
        processor->setRateAndBufferSizeDetails(sampleRate, maxBlockFrames);
        processor->prepareToPlay(sampleRate, maxBlockFrames);
        // A host passes max(inputs, outputs) channels; inputs stay silent.
        buffer.setSize(std::max({ 2, processor->getTotalNumInputChannels(), processor->getTotalNumOutputChannels() }),
                       maxBlockFrames);
    }
    void LoadPreset(int index) override { processor->setCurrentProgram(index); }
    float GetParameter(std::size_t index) const override { return At(index)->getValue(); }
    void SetParameter(std::size_t index, float normalised) override { At(index)->setValueNotifyingHost(normalised); }
    void AutomateParameter(std::size_t index, float normalised) override {
        At(index)->setValue(normalised);
        At(index)->sendValueChangedMessageToListeners(normalised);
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
    static std::vector<Parameter> Describe(const std::vector<juce::AudioProcessorParameter*>& list) {
        std::vector<Parameter> result;
        for (auto* p : list) {
            const int steps = p->isDiscrete() || p->isBoolean() ? p->getNumSteps() : 0;
            result.push_back({ p->getName(64).toStdString(), steps >= 2 ? steps : 0, p->getDefaultValue() });
        }
        return result;
    }
    juce::AudioProcessorParameter* At(std::size_t index) const {
        return index < automatable.size() ? automatable[index] : editorOnly[index - automatable.size()];
    }

    std::vector<juce::AudioProcessorParameter*> automatable, editorOnly;
    juce::AudioBuffer<float> buffer;
    juce::MidiBuffer midi;
};
#endif

} // namespace protocodus::matrix
