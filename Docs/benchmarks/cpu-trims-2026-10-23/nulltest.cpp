// Renders every factory and product preset through the product engine for a
// fixed phrase and writes raw interleaved stereo float32 per preset, so two
// engine revisions can be compared sample for sample.
// Usage: nulltest OUTDIR original|direct RATE max|normal [FIRST LAST]
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowPresets.h"
#include "DSP/YouKnowActiveProductFidelity.h"
#include "PresetScores.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {
constexpr int blockSize = 64;
constexpr double preRollSeconds = 0.37;
constexpr double lengthSeconds = 3.6;

const youknow::presets::Preset& presetAt(std::size_t index) {
    const auto& factory = youknow::presets::factoryBank();
    return index < factory.size() ? factory[index] : youknow::presets::productBank()[index - factory.size()];
}
}

int main(int argc, char** argv) {
    using namespace youknow;
    if (argc < 5) { std::fprintf(stderr, "usage: nulltest OUTDIR original|direct RATE max|normal [FIRST LAST]\n"); return 2; }
    const std::string outDir = argv[1];
    const bool original = std::string(argv[2]) == "original";
    const int rate = std::atoi(argv[3]);
    const bool normal = std::string(argv[4]) == "normal";
    const int count = static_cast<int>(presets::factoryBank().size() + presets::productBank().size());
    const int first = argc > 5 ? std::atoi(argv[5]) : 0;
    const int last = argc > 6 ? std::atoi(argv[6]) : count - 1;

    const int frames = static_cast<int>(lengthSeconds * rate);
    const auto at = [&](double seconds) { return static_cast<int>(seconds * rate) / blockSize; };
    for (int index = first; index <= last && index < count; ++index) {
        const auto& preset = presetAt(static_cast<std::size_t>(index));
        auto engine = std::make_unique<YouKnowEngine>();
        if (!ActiveProductFidelityProfile::tryConfigureBeforePrepare(*engine)) return 1;
        engine->selectConverterTimingProfile(YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
        engine->setInitialCircuitRate(normal ? CircuitRate::Normal : CircuitRate::Max);
        engine->prepare(static_cast<double>(rate), blockSize, 1);
        auto p = preset_demos::loadPresetParameters(preset.number);
        // The product's numerical kernels, as the Rack and plug-in ship them.
        p.vcfTanhMode = VcfTanhMode::PolyZoned;
        p.vcfFastEarlyMode = VcfFastEarlyMode::Cubic;
        p.vcfSolverMode = VcfSolverMode::Rk4Single;
        p.aging = 0.5f;
        if (std::getenv("YK_NO_SHOT")) p.enableOtaShotNoise = false;
        if (std::getenv("YK_NO_CHORUS")) p.chorus = ChorusMode::Off;
        engine->setParameters(p);
        engine->setOriginalPerformanceMode(original);
        engine->setPitchBend(0.0f);
        engine->setModWheel(0.0f);
        std::array<float, blockSize> left {}, right {};
        for (int done = 0; done < static_cast<int>(preRollSeconds * rate); done += blockSize)
            engine->process(left.data(), right.data(), blockSize);

        std::vector<float> out(static_cast<std::size_t>(frames) * 2u);
        double energy = 0.0, peak = 0.0;
        const int blocks = frames / blockSize;
        for (int block = 0; block < blocks; ++block) {
            if (block == at(0.0)) for (int n : { 48, 55, 60, 64, 67, 72 }) engine->noteOn(n, 0.8f);
            if (block == at(1.0)) for (int n : { 48, 55, 60, 64, 67, 72 }) engine->noteOff(n);
            if (block == at(1.3)) engine->noteOn(36, 1.0f);
            if (block == at(1.5)) engine->noteOn(43, 0.6f);
            if (block == at(1.7)) engine->noteOff(36);
            if (block == at(1.9)) engine->noteOff(43);
            if (block == at(2.0)) for (int n : { 60, 64, 67 }) engine->noteOn(n, 0.9f);
            if (block >= at(2.1) && block <= at(2.5))
                engine->setPitchBend(static_cast<float>(block - at(2.1)) / static_cast<float>(at(2.5) - at(2.1)) * 0.5f);
            if (block == at(2.2)) engine->setModWheel(0.6f);
            if (block == at(2.8)) for (int n : { 60, 64, 67 }) engine->noteOff(n);
            engine->process(left.data(), right.data(), blockSize);
            for (int i = 0; i < blockSize; ++i) {
                const std::size_t o = (static_cast<std::size_t>(block) * blockSize + static_cast<std::size_t>(i)) * 2u;
                out[o] = left[static_cast<std::size_t>(i)]; out[o + 1] = right[static_cast<std::size_t>(i)];
                energy += double(left[i]) * left[i] + double(right[i]) * right[i];
                peak = std::max(peak, std::max(std::fabs(double(left[i])), std::fabs(double(right[i]))));
            }
        }
        if (original && !engine->originalPerformanceHealthy()) { std::fprintf(stderr, "preset %d: original performance unhealthy\n", index); return 1; }
        char name[64]; std::snprintf(name, sizeof name, "%03d-%s.f32", index, preset.number);
        const std::string path = outDir + "/" + name;
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) { std::perror(path.c_str()); return 1; }
        std::fwrite(out.data(), sizeof(float), out.size(), f);
        std::fclose(f);
        std::printf("%03d %s rms_dbfs %.2f peak_dbfs %.2f voices %d cutoff %.3f resonance %.3f env %.3f keyfollow %.3f chorus %d hpf %d\n", index, preset.number,
                    20.0 * std::log10(std::sqrt(energy / (2.0 * blocks * blockSize)) + 1e-30),
                    20.0 * std::log10(peak + 1e-30), engine->getActiveVoiceCount(),
                    double(p.cutoff), double(p.resonance), double(p.envDepth), double(p.keyFollow), int(p.chorus), int(p.highPass));
        std::fflush(stdout);
    }
    return 0;
}
