// CPU benchmark / callgrind driver for the YouKnow engine at the Rack product
// configuration. Usage: bench original|direct RATE max|normal BLOCKS [chorus 0|1|2] [factor 1|2|4] [preset index, -1 = synthetic patch]
// Prints an FNV-1a hash of every rendered sample, then per-callback thread-CPU
// statistics. Under callgrind run with --collect-atstart=no: collection is
// toggled around the measured callbacks only.
// Environment (2026-10-30 additions):
//   YK_NOTES=n       play the first n notes of the chord (default 6; 0 = idle)
//   YK_SETTLED=1     thermally settled start, as a session past its first minute
//   YK_WARM=n        warm-up callbacks before measuring (default 128)
//   YK_PLAYFIRST=n   play the full chord for n callbacks and release it first
//   YK_FTZ=1         set flush-to-zero and denormals-are-zero (x86)
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowActiveProductFidelity.h"
#include "DSP/YouKnowPresets.h"
#include "PresetScores.h"
#include <valgrind/callgrind.h>
#include <algorithm>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif
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
    const int noteCount = std::getenv("YK_NOTES") ? std::atoi(std::getenv("YK_NOTES")) : 6;

    auto engine = new YouKnowEngine();
    if (!ActiveProductFidelityProfile::tryConfigureBeforePrepare(*engine)) { std::fprintf(stderr, "configure failed\n"); return 1; }
    engine->selectConverterTimingProfile(YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
    engine->setInitialCircuitRate(normal ? CircuitRate::Normal : CircuitRate::Max);
    // A session past its first minute: the 3 s chassis warm-up has reached 1.0f.
    if (std::getenv("YK_SETTLED") && !engine->configureThermalStart(true)) { std::fprintf(stderr, "thermal failed\n"); return 1; }
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
        for (int k = 0; k < noteCount; ++k) engine->noteOff(chord[k] + transposes[static_cast<std::size_t>(cycle % 6)]);
        ++cycle;
        for (int k = 0; k < noteCount; ++k) engine->noteOn(chord[k] + transposes[static_cast<std::size_t>(cycle % 6)], 0.9f);
    };
#if defined(__SSE__)
    if (std::getenv("YK_FTZ")) _mm_setcsr(_mm_getcsr() | 0x8040);
#endif
    if (std::getenv("YK_PLAYFIRST"))
    {
        // Play the chord, release it, and let the tails decay before the
        // measured (silent) section: an instance that has played notes.
        for (int note : chord) engine->noteOn(note, 0.9f);
        for (int block = 0; block < std::atoi(std::getenv("YK_PLAYFIRST")); ++block)
            engine->process(left.data(), right.data(), blockSize);
        for (int note : chord) engine->noteOff(note);
    }
    for (int k = 0; k < noteCount; ++k) engine->noteOn(chord[k], 0.9f);
    // Warm-up: the chord settles and the firmware assigner is running.
    const int warm = std::getenv("YK_WARM") ? std::atoi(std::getenv("YK_WARM")) : 128;
    for (int block = 0; block < warm; ++block) engine->process(left.data(), right.data(), blockSize);

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
