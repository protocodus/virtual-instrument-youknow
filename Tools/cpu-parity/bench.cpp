// CPU benchmark / callgrind driver for the YouKnow engine at the Rack product
// configuration (first written for the 2026-10-23 round; this copy is the
// 2026-10-25 round's, Docs/benchmarks/cpu-state-traffic-2026-10-25.md).
//
// Measurement protocol
// - Build every Source/DSP translation unit as the Rack does, as nearly as a
//   host without the SDK allows: `build.sh TREE bench OUT rack` (C++17,
//   -O3 -fno-builtin -DYOUKNOW_EMBEDDED_TARGET=1). CXX=clang++ gives the
//   second compiler. Baseline and candidate use the same compiler and flags.
// - Product configuration: ActiveProductFidelityProfile, measured converter
//   chart, PolyZoned / Cubic / Rk4Single, Aging 50 %, six voices, 1x,
//   Original timing, 64-frame callbacks, the preset's stored controls.
// - Six notes held (C2 C3 G3 C4 E4 G4, velocity .9), 128 warm-up callbacks,
//   then BLOCKS measured callbacks, the chord retriggered and transposed
//   every 512 callbacks.
// - The primary metric is instructions (Ir) per callback from
//   `valgrind --tool=callgrind --collect-atstart=no`: collection is toggled
//   around the measured loop only. Counts are reproducible on a shared
//   machine; thread-CPU timings printed below are not, and are only ever
//   reported as development-machine figures.
// - The FNV-1a hash of every rendered sample must equal the baseline's; the
//   full parity check is nulltest + compare_null.py (all 144 presets).
//
// Usage: bench original|direct RATE max|normal BLOCKS [chorus 0|1|2] [factor 1|2|4]
//              [preset index, -1 = synthetic patch]
// Prints an FNV-1a hash of every rendered sample, then per-callback thread-CPU
// statistics.
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowActiveProductFidelity.h"
#include "DSP/YouKnowPresets.h"
#include "PresetScores.h"
#include <valgrind/callgrind.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {
constexpr int blockSize = 64;
constexpr std::array<int, 6> chord { 36, 48, 55, 60, 64, 67 };
constexpr std::array<int, 6> transposes { 0, 5, -3, 7, 2, -5 };

double nowMicros() {
    timespec ts {};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1.0e6 + ts.tv_nsec / 1.0e3;
}

void hashFloat(std::uint64_t& h, float v) {
    std::uint32_t bits; std::memcpy(&bits, &v, 4);
    for (int i = 0; i < 4; ++i) { h ^= (bits >> (8 * i)) & 0xffu; h *= 1099511628211ull; }
}
}

int main(int argc, char** argv) {
    using namespace youknow;
    if (argc < 5) { std::fprintf(stderr, "usage: bench original|direct RATE max|normal BLOCKS [chorus] [factor]\n"); return 2; }
    const bool original = std::string(argv[1]) == "original";
    const int rate = std::atoi(argv[2]);
    const bool normal = std::string(argv[3]) == "normal";
    const int blocks = std::atoi(argv[4]);
    const int chorusMode = argc > 5 ? std::atoi(argv[5]) : 2;
    const int factor = argc > 6 ? std::atoi(argv[6]) : 1;
    const int presetIndex = argc > 7 ? std::atoi(argv[7]) : -1;

    auto engine = new YouKnowEngine();
    if (!ActiveProductFidelityProfile::tryConfigureBeforePrepare(*engine)) { std::fprintf(stderr, "configure failed\n"); return 1; }
    engine->selectConverterTimingProfile(YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
    engine->setInitialCircuitRate(normal ? CircuitRate::Normal : CircuitRate::Max);
    engine->prepare(static_cast<double>(rate), blockSize, factor);
    EngineParameters p;
    if (presetIndex >= 0) {
        const auto& factory = presets::factoryBank();
        const auto& preset = static_cast<std::size_t>(presetIndex) < factory.size()
            ? factory[static_cast<std::size_t>(presetIndex)] : presets::productBank()[static_cast<std::size_t>(presetIndex) - factory.size()];
        p = preset_demos::loadPresetParameters(preset.number);
        p.polyphony = 6;
        std::printf("preset %s %s\n", preset.number, preset.name);
    }
    else ActiveProductFidelityProfile::applyTo(p);
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfFastEarlyMode = VcfFastEarlyMode::Cubic;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    p.aging = 0.5f;
    if (presetIndex < 0) {
        p.calibration = 1.0f; p.polyphony = 6;
        p.sawEnabled = true; p.pulseEnabled = true;
        p.subLevel = 0.5f; p.noiseLevel = 0.10f;
        p.attack = 0.05f; p.decay = 0.4f; p.sustain = 0.8f; p.release = 0.3f;
        p.cutoff = 0.62f; p.resonance = 0.5f; p.envDepth = 0.35f;
        p.chorus = chorusMode == 0 ? ChorusMode::Off : chorusMode == 1 ? ChorusMode::One : ChorusMode::Two;
        p.volume = 0.8f;
    }
    engine->setParameters(p);
    engine->setOriginalPerformanceMode(original);

    std::array<float, blockSize> left {}, right {};
    std::uint64_t hash = 1469598103934665603ull;
    int cycle = 0;
    const auto retrigger = [&] {
        for (int note : chord) engine->noteOff(note + transposes[static_cast<std::size_t>(cycle % 6)]);
        ++cycle;
        for (int note : chord) engine->noteOn(note + transposes[static_cast<std::size_t>(cycle % 6)], 0.9f);
    };
    for (int note : chord) engine->noteOn(note, 0.9f);
    // Warm-up: the chord settles and the firmware assigner is running.
    for (int block = 0; block < 128; ++block) engine->process(left.data(), right.data(), blockSize);

    std::vector<double> micros(static_cast<std::size_t>(blocks));
    CALLGRIND_TOGGLE_COLLECT;
    for (int block = 0; block < blocks; ++block) {
        if (block % 512 == 0) retrigger();
        const double start = nowMicros();
        engine->process(left.data(), right.data(), blockSize);
        micros[static_cast<std::size_t>(block)] = nowMicros() - start;
        for (int i = 0; i < blockSize; ++i) { hashFloat(hash, left[i]); hashFloat(hash, right[i]); }
    }
    CALLGRIND_TOGGLE_COLLECT;
    if (original && !engine->originalPerformanceHealthy()) { std::fprintf(stderr, "original performance unhealthy\n"); return 1; }
    std::sort(micros.begin(), micros.end());
    double total = 0; for (double m : micros) total += m;
    const auto q = [&](double f) { return micros[static_cast<std::size_t>(f * (micros.size() - 1))]; };
    std::printf("hash %016llx voices %d factor %d\n", static_cast<unsigned long long>(hash), engine->getActiveVoiceCount(), engine->getOversamplingFactor());
    std::printf("blocks %d median_us %.3f mean_us %.3f p95_us %.3f max_us %.3f total_ms %.3f\n",
                blocks, q(0.5), total / blocks, q(0.95), micros.back(), total / 1000.0);
    delete engine;
    return 0;
}
