// Deterministic current-product validation, built as YouKnowRenderProductValidation.
// Usage: YouKnowRenderProductValidation OUTPUT.f32 original|direct RATE FACTOR [BLOCK=173]
//
// For cross-revision comparisons, compile this SAME source against each revision's
// own Source headers and matching Release DSP library. Use the library's compiler,
// architecture and IPO flags; never mix one revision's headers with another's
// archive. With CMake, enable YOUKNOW_BUILD_TOOLS and build this target. For an
// older source tree without the target, an equivalent GCC Release/IPO command is:
// g++ -std=c++20 -O3 -DNDEBUG -flto -I /path/to/revision/Source Tools/RenderProductValidation.cpp /path/to/build/libYouKnowDSP.a -o /tmp/render
//
// Four seconds exercise idle, single/six-voice notes, sustain/release, chorus
// Off/I/II, cutoff/resonance, Character/Aging, pitch/modulation, cold reset,
// host-stop reset, local keyboard and panic/restart. Both timing modes select the
// active product profile, Poly/Cubic/Normal kernels, and the measured converter
// chart. Events land at floor(tick * rate / 8), including at 44.1 kHz. The output
// has exactly 4 * rate * 2 * sizeof(float) bytes of native-endian, interleaved
// stereo float32, without normalization, silence trimming or latency alignment.
// Existing files are refused; failed runs can leave incomplete output. Only a
// complete=true record with finite audio, six active voices and nonzero energy
// qualifies. Original firmware health is checked after every callback.
//
// process_cpu_seconds and max_callback_cpu_us use std::clock around process().
// POSIX supplies process CPU time; MSVC supplies elapsed wall time, so on Windows
// these historical field names carry wall-clock diagnostics only. The transition
// CPU benchmark requires POSIX; raw-audio comparison is valid on either platform.
// Preparation, synchronous event/control delivery, validation and I/O are outside
// those intervals. Clock overhead is not subtracted; process CPU can include
// other threads and is not a DAW callback-deadline measurement. Use matching
// callback sizes and serial, quiet-machine baseline/candidate/baseline brackets.
// CompareProductAudio.py checks the 20-case audio matrix; BenchmarkTransitions.py
// runs the fixed transition CPU protocol. Keep source/binary hashes alongside
// results. The historical protocol identifier and diagnostic tag remain stable
// so previously captured evidence remains comparable after this file's move.
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowActiveProductFidelity.h"
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

int integer(const char* text) {
    const std::string_view input(text);
    int value{};
    const auto result = std::from_chars(input.data(), input.data() + input.size(), value);
    if (result.ec != std::errc{} || result.ptr != input.data() + input.size())
        throw std::invalid_argument("expected an integer argument");
    return value;
}

int main(int argc, char** argv) {
    try {
        using namespace youknow;
        static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
        static_assert(CLOCKS_PER_SEC > 0);
        if (argc != 5 && argc != 6)
            throw std::invalid_argument("usage: RenderComparison OUTPUT.f32 original|direct RATE FACTOR [BLOCK=173]");
        const std::string_view mode(argv[2]);
        const int rate = integer(argv[3]), quality = integer(argv[4]);
        const int block = argc == 6 ? integer(argv[5]) : 173;
        if ((mode != "original" && mode != "direct") ||
            (rate != 44100 && rate != 48000 && rate != 96000) ||
            (quality != 1 && quality != 2 && quality != 4) || block < 1 || block > 256)
            throw std::invalid_argument("unsupported mode, rate, quality or block (1..256)");
        if (std::filesystem::exists(argv[1]))
            throw std::runtime_error("output already exists; preserve prior evidence");
        std::ofstream output(argv[1], std::ios::binary);
        if (!output) throw std::runtime_error("cannot open output");

        auto engine = std::make_unique<YouKnowEngine>();
        ActiveProductFidelityProfile::configureBeforePrepare(*engine);
        engine->selectConverterTimingProfile(YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
        engine->prepare(rate, 256, quality);
        EngineParameters p;
        ActiveProductFidelityProfile::applyTo(p);
        p.vcfTanhMode = VcfTanhMode::PolyZoned;
        p.vcfFastEarlyMode = VcfFastEarlyMode::Cubic;
        p.vcfSolverMode = VcfSolverMode::Rk4Single;
        p.aging = 0.5f; p.calibration = 1.0f; p.polyphony = 6;
        p.sawEnabled = p.pulseEnabled = true;
        p.subLevel = 0.55f; p.noiseLevel = 0.08f;
        p.attack = 0.02f; p.decay = 0.3f; p.sustain = 0.8f; p.release = 0.18f;
        p.cutoff = 0.65f; p.resonance = 0.45f; p.envDepth = 0.25f;
        p.chorus = ChorusMode::Off; p.volume = 0.65f;
        engine->setParameters(p);
        engine->setOriginalPerformanceMode(mode == "original");
        const int applied = engine->getOversamplingFactor();
        constexpr std::array<int, 6> chord{36, 48, 55, 60, 64, 67};
        const auto startChord = [&] { for (int note : chord) engine->noteOn(note, 0.82f); };
        const auto stopChord = [&] { for (int note : chord) engine->noteOff(note); };
        std::array<float, 256> left{}, right{};
        std::array<float, 512> interleaved{};
        std::int64_t position = 0;
        double energy = 0.0, peak = 0.0;
        double processCpuTicks = 0.0, maximumCallbackCpuTicks = 0.0;
        float maximumTemperature = engine->getDisplayTemperatureC();
        int maximumVoices = 0;
        for (int tick = 0; tick < 32; ++tick) {
            switch (tick) {
            case 1: engine->noteOn(60, 0.65f); break;
            case 3: engine->noteOff(60); startChord(); break;
            case 5: engine->setSustainPedal(true); break;
            case 6: stopChord(); break;
            case 8:
                engine->setSustainPedal(false);
                p.cutoff = 0.3f; p.resonance = 0.78f; p.chorus = ChorusMode::One;
                p.aging = 0.85f; p.calibration = 1.4f;
                engine->setParameters(p); break;
            case 9: engine->noteOn(55, 1.0f); engine->noteOn(67, 0.7f); break;
            case 12:
                p.chorus = ChorusMode::Two; p.calibration = 0.0f;
                engine->setParameters(p); break;
            case 13: engine->noteOff(55); engine->noteOff(67); break;
            case 14:
                p.chorus = ChorusMode::Off; p.resonance = 0.1f;
                p.calibration = 0.5f; p.aging = 0.1f;
                engine->setParameters(p); break;
            case 16: engine->reset(); break;
            case 17: startChord(); break;
            case 20: engine->setPitchBend(0.3f); engine->setModWheel(0.5f); break;
            case 22:
                p.calibration = 2.0f; p.aging = 1.0f; p.cutoff = 0.82f;
                engine->setParameters(p); break;
            case 24: p.chorus = ChorusMode::One; engine->setParameters(p); break;
            case 25: stopChord(); engine->setSustainPedal(false); break;
            case 26: engine->resetForHostStop(); break;
            case 27: engine->noteOnFromLocalKeyboard(76, 0.8f); break;
            case 29: engine->noteOffFromLocalKeyboard(76); break;
            case 30: engine->allNotesOff(); break;
            case 31: engine->noteOn(60, 0.7f); break;
            default: break;
            }
            // Integer boundaries retain exactly four seconds, including44.1kHz.
            const std::int64_t end = static_cast<std::int64_t>(tick + 1) * rate / 8;
            while (position < end) {
                const int count = static_cast<int>(std::min<std::int64_t>(block, end - position));
                const auto cpuStart = std::clock();
                engine->process(left.data(), right.data(), count);
                const auto cpuEnd = std::clock();
                if (cpuStart == std::clock_t(-1) || cpuEnd == std::clock_t(-1) || cpuEnd < cpuStart)
                    throw std::runtime_error("unavailable or nonmonotonic process CPU clock");
                const double cpuTicks = static_cast<double>(cpuEnd) - static_cast<double>(cpuStart);
                if (!std::isfinite(cpuTicks)) throw std::runtime_error("non-finite process CPU interval");
                processCpuTicks += cpuTicks;
                maximumCallbackCpuTicks = std::max(maximumCallbackCpuTicks, cpuTicks);
                if (mode == "original" && !engine->originalPerformanceHealthy())
                    throw std::runtime_error("Original firmware failed at frame " + std::to_string(position));
                maximumVoices = std::max(maximumVoices, engine->getActiveVoiceCount());
                maximumTemperature = std::max(maximumTemperature, engine->getDisplayTemperatureC());
                for (int i = 0; i < count; ++i) {
                    for (int channel = 0; channel < 2; ++channel) {
                        const float sample = channel == 0 ? left[i] : right[i];
                        if (!std::isfinite(sample)) throw std::runtime_error("non-finite audio");
                        interleaved[2 * i + channel] = sample;
                        peak = std::max(peak, std::abs(static_cast<double>(sample)));
                        energy += static_cast<double>(sample) * sample;
                    }
                }
                output.write(reinterpret_cast<const char*>(interleaved.data()), count * 2 * sizeof(float));
                position += count;
            }
        }
        output.close();
        if (!output || maximumVoices < 6 || energy < 1.0e-12)
            throw std::runtime_error("incomplete output, silent fixture or missing six-voice chord");
        if (!std::isfinite(processCpuTicks) || !(processCpuTicks > 0.0))
            throw std::runtime_error("invalid accumulated process CPU time");
        std::cout << std::setprecision(12) << "complete=true protocol=youknow-cpu-2026-10-09-v1"
                  << " profile=active-product timing=" << mode << " rate=" << rate
                  << " requested_quality=" << quality << " applied_quality=" << applied
                  << " kernel=poly-zoned early=cubic solver=rk4-single block=" << block
                  << " frames=" << position << " channels=2 format=f32 endian="
                  << (std::endian::native == std::endian::little ? "little" : "big")
                  << " normalized=false peak=" << peak << " rms=" << std::sqrt(energy / (position * 2))
                  << " max_voices=" << maximumVoices << " max_temperature_c=" << maximumTemperature
                  << " process_cpu_seconds=" << processCpuTicks / CLOCKS_PER_SEC
                  << " max_callback_cpu_us=" << maximumCallbackCpuTicks * 1.0e6 / CLOCKS_PER_SEC << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RenderComparison: " << error.what() << '\n';
        return 1;
    }
}
