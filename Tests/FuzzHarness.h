// Shared Protocodus U4 input-fuzzing support, after Electry's DspFuzz.
//
// Master copy: reason-rack-extensions/Tools/portfolio/Tests/FuzzHarness.h.
// Every canonical repo carries a byte-identical copy in Tests/, and
// Tools/check_repo_standard.py fails on a drifted one.
//
// A repo's Tests/DspFuzz.cpp defines LLVMFuzzerTestOneInput: from fresh
// objects every time, it reads commands from a protocodus::fuzz::Input
// (parameter writes with raw and extreme floats, notes, bends, lifecycle
// calls) and renders a little after each one through a Sentinels buffer,
// calling protocodus::fuzz::Require on every invariant. Work and allocation
// stay bounded whatever the input length, so a saved crash replays alone.
// Tests/portfolio/ToyFuzz.cpp in the Rack repo is the skeleton.
//
// Built with Clang -fsanitize=fuzzer it is a libFuzzer target. Built with
// PROTOCODUS_FUZZ_MAIN defined (any compiler) it gets this header's main():
//
//   <Name>DspFuzz [-seed=N] [-runs=N] [-max_len=N] [file ...]
//
// which replays each file (committed crash reproducers, as U1 regression
// tests) and then runs N seeded random inputs, so GCC and MSVC builds can
// register the same harness with CTest.
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace protocodus::fuzz {

// Aborts, so libFuzzer and the sanitizers report the input that broke it.
inline void Require(bool value) {
    if (!value) std::abort();
}

// The fuzz input as a stream of decisions; reads past the end return zeros.
struct Input {
    const std::uint8_t* data;
    std::size_t size;

    bool Empty() const { return size == 0; }
    std::uint8_t Byte() {
        if (size == 0) return 0;
        --size;
        return *data++;
    }
    std::uint32_t Word() {
        std::uint32_t result = 0;
        for (int shift = 0; shift != 32; shift += 8) result |= static_cast<std::uint32_t>(Byte()) << shift;
        return result;
    }
    // A value in [0, count).
    std::size_t Pick(std::size_t count) { return count == 0 ? 0 : Byte() % count; }
    // Mostly a normalised control value, sometimes any float bit pattern or an extreme.
    float Scalar() {
        switch (Byte() % 8) {
            case 0: { const std::uint32_t bits = Word(); float f; std::memcpy(&f, &bits, sizeof f); return f; }
            case 1: return std::numeric_limits<float>::max();
            case 2: return -std::numeric_limits<float>::max();
            case 3: return std::numeric_limits<float>::quiet_NaN();
            case 4: return std::numeric_limits<float>::infinity();
            case 5: return -std::numeric_limits<float>::infinity();
            default: return static_cast<float>(Byte()) / 255.0f;
        }
    }
    // A sample rate a host might pass, including nonsense ones.
    double Rate() {
        static constexpr std::array<double, 12> rates {
            8000.0, 44100.0, 48000.0, 96000.0, 96001.0, 192000.0, 384000.0,
            0.0, -1.0, 1.0e300, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()
        };
        return rates[Pick(rates.size())];
    }
};

// A stereo block with guard samples on both sides: a write outside the
// caller's frame count that stays inside the allocation would evade
// AddressSanitizer, but not this.
template <std::size_t MaxFrames>
struct Sentinels {
    static constexpr float kGuard = 12345.0f;
    std::array<float, MaxFrames + 2> left {}, right {};

    float* Left() { return left.data() + 1; }
    float* Right() { return right.data() + 1; }
    void Arm() {
        left.fill(kGuard);
        right.fill(kGuard);
    }
    // Every rendered sample finite, every guard untouched.
    void Check(std::size_t frames) const {
        Require(frames <= MaxFrames);
        for (std::size_t i = 0; i < left.size(); ++i) {
            const bool rendered = i >= 1 && i <= frames;
            if (rendered) Require(std::isfinite(left[i]) && std::isfinite(right[i]));
            else Require(left[i] == kGuard && right[i] == kGuard);
        }
    }
};

} // namespace protocodus::fuzz

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

#if defined(PROTOCODUS_FUZZ_MAIN)
int main(int argc, char** argv) {
    unsigned long long seed = 1, runs = 10000, maxLength = 4096;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&](const char* name) { return std::strtoull(arg.c_str() + std::strlen(name), nullptr, 10); };
        if (arg.rfind("-seed=", 0) == 0) seed = value("-seed=");
        else if (arg.rfind("-runs=", 0) == 0) runs = value("-runs=");
        else if (arg.rfind("-max_len=", 0) == 0) maxLength = value("-max_len=");
        else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "usage: %s [-seed=N] [-runs=N] [-max_len=N] [file ...]\n", argv[0]);
            return 2;
        } else {
            std::ifstream file(arg, std::ios::binary);
            if (!file) {
                std::fprintf(stderr, "error: cannot read %s\n", arg.c_str());
                return 2;
            }
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
            LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
            std::printf("replayed %s (%zu bytes)\n", arg.c_str(), bytes.size());
        }
    }
    // SplitMix64: the same seed gives the same inputs on every platform.
    std::uint64_t state = seed;
    const auto next = [&state] {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };
    std::vector<std::uint8_t> bytes;
    for (unsigned long long run = 0; run < runs; ++run) {
        bytes.resize(static_cast<std::size_t>(next() % (maxLength + 1)));
        for (auto& b : bytes) b = static_cast<std::uint8_t>(next());
        LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
    }
    std::printf("%llu random inputs from seed %llu passed\n", runs, seed);
    return 0;
}
#endif
