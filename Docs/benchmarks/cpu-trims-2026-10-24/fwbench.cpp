// Times the A-5 assigner foreground through FirmwareAssignerAudioBridge, one
// 64-sample host block (5333 or 5334 CPU states at 48 kHz) per call, from the
// scheduler regression's warm fixture, and hashes the emulated RAM, registers
// and decoded bytes so two revisions can be compared. A three-byte note-on
// and a note-off arrive every 256 blocks. Usage: fwbench [BLOCKS]
// Build against one checkout, as the Rack compiles the firmware sources:
//   g++ -std=c++17 -O3 -fno-builtin -DNDEBUG -DYOUKNOW_EMBEDDED_TARGET=1 -ITREE/Source fwbench.cpp
//       TREE/Source/DSP/YouKnowFirmware{AssignerAudioBridge,AssignerScheduler,UartTrace,SerialPinDecoder}.cpp
#include "DSP/YouKnowFirmwareAssignerAudioBridge.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <vector>
using namespace youknow;
using S = FirmwareAssignerScheduler; namespace U = FirmwareUartTrace; namespace Io = FirmwareAssignerIo;
using B = FirmwareAssignerAudioBridge;
static double now() { timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }
int main(int argc, char** argv) {
    const int blocks = argc > 1 ? std::atoi(argv[1]) : 4000;
    S::State warm; U::State uart; S::Configuration configuration; S::Tables tables; Io::Inputs inputs;
    inputs.panelAdc.fill(132); warm.io.conversion = {0, 0, 0, 255};
    auto& ram = warm.ram;
    std::fill(ram.begin() + 0x58, ram.begin() + 0x68, 132); std::fill(ram.begin() + 0x6c, ram.begin() + 0x7c, 132);
    ram[0x6b] = 255;
    for (unsigned card = 0; card < 6; ++card) { ram[0x80 + card] = 0x88 + card; ram[0x88 + card] = 0x80; }
    ram[0x86] = 0x88; std::fill(ram.begin() + 0x90, ram.begin() + 0xa0, 64);
    ram[0x4f] = 0x21; ram[0xb6] = 2; ram[0x8e] = ram[0xb8] = ram[0xc7] = 0x2a; ram[0xba] = 16; ram[0xbb] = 1;
    ram[0xbc] = 8; ram[0xbe] = 12; ram[0xc5] = ram[0xc6] = 4; ram[0xc8] = 0x42; ram[0xcb] = 0xfc;
    auto state = std::make_unique<B::State>();
    if (!B::reset(*state, warm, uart)) { std::fprintf(stderr, "reset failed\n"); return 1; }
    // Note on/off bytes every 256 blocks keep the receive parser and sender busy.
    std::vector<double> micros(static_cast<std::size_t>(blocks));
    std::array<FirmwareSerialPinDecoder::ByteReady, 64> ready{};
    std::uint64_t hash = 1469598103934665603ull, bytes = 0;
    const auto mix = [&](std::uint64_t v) { hash = (hash ^ v) * 1099511628211ull; };
    for (int block = 0; block < blocks; ++block) {
        const std::uint64_t target = (static_cast<std::uint64_t>(block) + 1) * 4000000ull * 64 / 48000;
        std::array<S::InputEvent, 3> in{};
        std::size_t count = 0;
        if (block % 256 == 10) { const auto t = state->assigner.now + 1; const unsigned note = 48 + (block / 256) % 24;
            in = {{{t, 0x90}, {t + 1280, static_cast<std::uint8_t>(note)}, {t + 2560, 100}}}; count = 3; }
        if (block % 256 == 140) { const auto t = state->assigner.now + 1; const unsigned note = 48 + (block / 256) % 24;
            in = {{{t, 0x90}, {t + 1280, static_cast<std::uint8_t>(note)}, {t + 2560, 0}}}; count = 3; }
        std::size_t offset = 0;
        const double start = now();
        for (int attempt = 0; attempt < 64; ++attempt) {
            B::Output output{ready, 0};
            const auto result = B::advanceTo(*state, configuration, tables, inputs,
                Span<const S::InputEvent>(in.data() + offset, count - offset), target, output);
            offset += result.consumedInputs; bytes += output.count;
            for (std::size_t i = 0; i < output.count; ++i) { mix(ready[i].states); mix(ready[i].value); }
            if (result.status == B::Status::ReachedTarget) break;
            if (result.status != B::Status::OutputFull && result.status != B::Status::WorkLimit) {
                std::fprintf(stderr, "bridge status %u at block %d\n", unsigned(result.status), block); return 1; }
        }
        micros[static_cast<std::size_t>(block)] = now() - start;
    }
    for (auto v : state->assigner.ram) mix(v);
    mix(state->assigner.registers.pc); mix(state->assigner.foregroundPasses); mix(state->uart.frameOrdinal);
    std::sort(micros.begin(), micros.end());
    double total = 0; for (double m : micros) total += m;
    std::printf("hash %016llx bytes %llu passes %llu\n", (unsigned long long)hash, (unsigned long long)bytes,
                (unsigned long long)state->assigner.foregroundPasses);
    std::printf("blocks %d median_us %.3f mean_us %.3f\n", blocks, micros[micros.size() / 2], total / blocks);
}
