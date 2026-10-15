// Reproducible product-level calibration, separate from the archival circuit
// audit. All 144 presets play identical velocity-127 notes (36/48/60/72,
// four seconds on and one off), then a six-note chord for four seconds and a
// two-second tail. This covers attacks, sustained voices, releases and unison.
// Both playback modes: 48 kHz, Direct and Original/1x, Poly/Cubic/Normal, Aging 50%,
// approved component profile, deterministic engine reset. Stored performance
// controls (including transpose, voice mode and analog Volume) stay intact.
// Stereo RMS uses 400 ms windows / 100 ms hops, gated 20 dB below the loudest
// window. Only chorus hiss is muted for the signal measurement; a second pass
// checks the complete preset including hiss. This is unweighted RMS, not LUFS.
// --raw measures unity digital trim; --check renders the stored gains through
// the engine. Both modes run by default; --mode can select one explicitly.
// The common target is -36 dBFS signal RMS +/-1 dB, with a -1 dBFS sample-peak
// ceiling on BOTH passes. The earlier Original-only -34 dBFS target missed
// Direct A86's approximately 32.67 dB crest factor. The quieter reference keeps
// over 2 dB of headroom without compressing its attack.
// One static trim per preset centres the extrema across both modes at 48 kHz;
// the smoke set also includes 44.1/96 kHz. A86's short noise burst spans about
// 2.109 dB across these sample grids, so only A86 at 44.1/96 kHz allows +/-1.1
// dB. Its 48 kHz check and every other preset retain +/-1 dB. Each CSV row names
// its exact tolerance. These are unweighted product-level conventions, not a
// hardware loudness claim. No dynamics, EQ, tone-byte edits or file normalization.
#include "PresetScores.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

namespace
{
using namespace youknow;
constexpr double targetDb = -36.0;
double toleranceDb(const presets::Preset& preset, double rate)
{
    return std::string_view(preset.number) == "A86" && rate != 48000.0 ? 1.1 : 1.0;
}
constexpr double peakCeilingDb = -1.0;
constexpr int blockSize = 256;

double db(double amplitude) { return 20.0 * std::log10(std::max(1.0e-12, amplitude)); }
struct Metrics { double gatedDb, peakDb; };

Metrics render(const presets::Preset& preset, double rate, bool original, bool raw, bool hiss)
{
    auto engine = std::make_unique<YouKnowEngine>();
    ActiveProductFidelityProfile::configureBeforePrepare(*engine);
    engine->selectConverterTimingProfile(YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
    engine->prepare(rate, blockSize, 1);
    auto p = preset_demos::loadPresetParameters(preset.number);
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfFastEarlyMode = VcfFastEarlyMode::Cubic;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    p.aging = 0.5f;
    p.outputTrimDb = raw ? 0.0f : preset.controls.outputTrimDb;
    if (!hiss) p.chorusNoise = 0.0f;
    engine->setParameters(p);
    engine->setOriginalPerformanceMode(original);
    std::vector<double> energy { 0.0 };
    energy.reserve(static_cast<std::size_t>(rate * 26.25) + 1);
    double peak = 0.0;
    const auto play = [&](double seconds) {
        std::array<float, blockSize> left {}, right {};
        auto remaining = static_cast<int>(std::llround(seconds * rate));
        while (remaining > 0)
        {
            const int count = std::min(blockSize, remaining);
            engine->process(left.data(), right.data(), count);
            for (int i = 0; i < count; ++i)
            {
                const double l = left[static_cast<std::size_t>(i)];
                const double r = right[static_cast<std::size_t>(i)];
                if (!std::isfinite(l) || !std::isfinite(r))
                    throw std::runtime_error(std::string(preset.number) + " non-finite output");
                peak = std::max({peak, std::abs(l), std::abs(r)});
                energy.push_back(energy.back() + 0.5 * (l*l + r*r));
            }
            remaining -= count;
        }
    };
    play(0.25);
    for (int note : { 36, 48, 60, 72 })
    {
        engine->noteOn(note, 1.0f);
        play(4.0);
        engine->noteOff(note);
        play(1.0);
    }
    for (int note : { 48, 55, 60, 64, 67, 72 }) engine->noteOn(note, 1.0f);
    play(4.0);
    for (int note : { 48, 55, 60, 64, 67, 72 }) engine->noteOff(note);
    play(2.0);
    if (!engine->originalPerformanceHealthy())
        throw std::runtime_error(std::string(preset.number) + " firmware performance failed");
    const auto window = static_cast<std::size_t>(std::llround(rate * 0.4));
    const auto hop = static_cast<std::size_t>(std::llround(rate * 0.1));
    std::vector<double> windows;
    for (std::size_t start = 0; start + window < energy.size(); start += hop)
        windows.push_back(std::max(0.0, (energy[start + window] - energy[start]) / window));
    const double loudest = *std::max_element(windows.begin(), windows.end());
    double sum = 0.0;
    int count = 0;
    for (double square : windows)
        if (square >= std::max(1.0e-14, loudest * 0.01)) { sum += square; ++count; }
    return { db(count ? std::sqrt(sum / count) : 0.0), db(peak) };
}

int run(int argc, char** argv)
{
    bool raw = false, smoke = false, check = false;
    double rate = 48000.0;
    std::string slot, mode = "both";
    std::filesystem::path output;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg(argv[i]);
        if (arg == "--raw") raw = true;
        else if (arg == "--smoke") smoke = true;
        else if (arg == "--check") check = true;
        else if (arg == "--rate" && i + 1 < argc) rate = std::stod(argv[++i]);
        else if (arg == "--mode" && i + 1 < argc) mode = argv[++i];
        else if (arg == "--slot" && i + 1 < argc) slot = argv[++i];
        else if (arg == "--help")
        {
            std::cout << "usage: YouKnowCalibratePresets [--raw | --check] [--smoke] "
                         "[--mode both|direct|original] [--rate 44100|48000|96000] [--slot A11] OUTPUT.csv\n";
            return 0;
        }
        else if (arg.starts_with("--") || !output.empty()) throw std::runtime_error("Invalid arguments");
        else output = arg;
    }
    if ((mode != "both" && mode != "direct" && mode != "original")
        || output.empty() || (raw && check) || (rate != 44100 && rate != 48000 && rate != 96000))
        throw std::runtime_error("Supply an output CSV, valid rate, and either --raw or --check");
    struct Entry { const presets::Preset* preset; bool original; };
    std::vector<Entry> bank;
    const auto add = [&](const auto& presets) {
        for (const auto& preset : presets)
        {
            if (!slot.empty() && slot != preset.number) continue;
            constexpr std::array smokeSlots { "A11", "A17", "A44", "A48", "A86",
                                             "B12", "B15", "B26", "B31", "B42", "B51", "B85",
                                             "YB1", "YB5", "YB8", "YP2", "YP7" };
            if (smoke && std::none_of(smokeSlots.begin(), smokeSlots.end(),
                [&](const char* s) { return std::string_view(s) == preset.number; })) continue;
            if (mode != "original") bank.push_back({ &preset, false });
            if (mode != "direct") bank.push_back({ &preset, true });
        }
    };
    add(presets::factoryBank()); add(presets::productBank());
    if (bank.empty()) throw std::runtime_error("No matching preset");
    struct Row { Metrics signal {}, complete {}; double recommended = 0; };
    std::vector<Row> rows(bank.size());
    std::atomic<std::size_t> next { 0 };
    std::atomic<bool> failed { false };
    std::mutex log;
    std::vector<std::thread> workers;
    const auto workerCount = std::min<std::size_t>({bank.size(), 6,
        std::max(1u, std::thread::hardware_concurrency())});
    for (std::size_t w = 0; w < workerCount; ++w)
        workers.emplace_back([&] {
            for (;;)
            {
                const auto i = next.fetch_add(1);
                if (i >= bank.size()) return;
                try
                {
                    auto& row = rows[i];
                    row.signal = render(*bank[i].preset, rate, bank[i].original, raw, false);
                    row.complete = render(*bank[i].preset, rate, bank[i].original, raw, true);
                    row.recommended = (raw ? 0.0 : bank[i].preset->controls.outputTrimDb)
                                    + targetDb - row.signal.gatedDb;
                    const double peak = std::max(row.signal.peakDb, row.complete.peakDb);
                    const bool valid = row.signal.gatedDb > -100 && std::abs(row.recommended) <= 48
                        && (!check || (std::abs(row.signal.gatedDb - targetDb) <= toleranceDb(*bank[i].preset, rate)
                                        && peak <= peakCeilingDb));
                    if (!valid) failed = true;
                    std::lock_guard guard(log);
                    std::cout << bank[i].preset->number << (bank[i].original ? " Original" : " Direct") << " RMS " << row.signal.gatedDb
                              << ", peak " << peak << ", trim " << row.recommended
                              << ", tolerance +/-" << toleranceDb(*bank[i].preset, rate)
                              << (valid ? "\n" : " FAILED\n") << std::flush;
                }
                catch (const std::exception& e)
                {
                    failed = true;
                    std::lock_guard guard(log);
                    std::cerr << e.what() << '\n';
                }
            }
        });
    for (auto& worker : workers) worker.join();
    if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
    std::ofstream csv(output);
    if (!csv) throw std::runtime_error("Cannot write CSV");
    csv << "slot,name,mode,sample_rate,volume,applied_trim_db,signal_gated_rms_dbfs,"
           "complete_gated_rms_dbfs,signal_peak_dbfs,complete_peak_dbfs,tolerance_db,recommended_trim_db\n";
    double lowest = 0.0, highest = -240.0, maxPeak = -240.0;
    for (std::size_t i = 0; i < bank.size(); ++i)
    {
        const auto& p = *bank[i].preset; const auto& r = rows[i];
        csv << p.number << ',' << std::quoted(p.name) << ',' << (bank[i].original ? "original" : "direct") << ',' << rate << ','
            << std::fixed << std::setprecision(6) << p.controls.volume << ','
            << (raw ? 0.0f : p.controls.outputTrimDb) << ',' << r.signal.gatedDb << ','
            << r.complete.gatedDb << ',' << r.signal.peakDb << ',' << r.complete.peakDb << ','
            << toleranceDb(p, rate) << ',' << r.recommended << '\n';
        lowest = std::min(lowest, r.signal.gatedDb); highest = std::max(highest, r.signal.gatedDb);
        maxPeak = std::max({maxPeak, r.signal.peakDb, r.complete.peakDb});
    }
    csv.close();
    if (!csv) throw std::runtime_error("Failed writing CSV");
    std::cout << bank.size() << " preset/mode renders: " << lowest << " to " << highest
              << " dBFS gated RMS (" << highest-lowest << " dB spread), maximum peak "
              << maxPeak << " dBFS. " << (check ? "Stored trim verification" : "Measurement")
              << (failed ? " FAILED\n" : " passed\n");
    return failed ? 1 : 0;
}
}
int main(int argc, char** argv)
{
    try { return run(argc, argv); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
