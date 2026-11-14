# CPU optimizations — 2026-11-14

YouKnow 1.4.8 removes work from the engine's voice islands, chorus board, output network and emulated firmware scheduler while leaving every rendered sample bit-for-bit identical to `5ccfafa` (1.4.7). This is a source change measured on an Apple-silicon macOS development machine; no installed plug-in or Rack Extension was replaced and nothing here is a Reason-host measurement.

## Changes

1. **Portable 8-byte-aligned `Double2` SIMD across native and `phdsp64`** (`YouKnowCompatibility.h`). `YOUKNOW_HAS_DOUBLE2_SIMD` is enabled directly under `__clang__` and `__GNUC__` without requiring host `<arm_neon.h>` or `<emmintrin.h>` headers, wrapped in `struct Double2 { typedef double Lanes __attribute__((__vector_size__(16))); Lanes v; ... } __attribute__((__packed__, __aligned__(8)))` (`alignof(Double2) == 8 && sizeof(Double2) == 16`) with `memcpy`-backed unaligned load/store helpers so SDK Clang 15.0.7 never emits aligned `movapd` loads behind pointers or references.
2. **Paired polyphonic voice solvers and 1:4 / 1:2 voice rate islands** (`YouKnowEngine.h`, `YouKnowEngine.cpp`). `tryProcessVoiceFilterSubstepPair` steps two active voices together in `Double2` across slewing and thermal-warmup controls (using bitwise `allSameBits` fast-paths when controls are flat, skipping open-loop `tanh` when `feedback == 0`, and deferring Merson fallback shot-noise preparation until a single-step tableau error check fails). `renderVoiceIslandPair` pairs active voices across the entire local voice island—polyphase upsampler FIR, local-rate nonlinear VCF/VCA solver, and symmetric folded FIR decimator—so the shipped `1x` default runs paired SIMD.
3. **Batched polyphase upsamplers and folded `Double2` FIR decimators** (`YouKnowChorusRateIsland.h`, `YouKnowEngine.cpp`). `ChorusPolyphaseUpsampler` and `PolyphaseUpsampler` skip the center-tap phase-0 dot product, load each history tap once to accumulate remaining phases in registers, and skip interpolation while `!ready`. Stereo chorus and dual-voice FIR decimators fold symmetric tap pairs across four interleaved `Double2` accumulators while matching scalar FMA expression boundaries.
4. **Chorus BBD clock hiss without `libm` `pow`, plus paired stereo chorus and output stages** (`YouKnowChorus.cpp`, `YouKnowChorusRateIsland.h`, `YouKnowOutputNetwork.h`, `YouKnowHeadphoneOutput.h`). Over the physical BBD clock range `[1 kHz, 260 kHz]`, the 192 kHz clock-hiss law `std::pow(clock, -0.5 * hissClockExponent)` is evaluated by IEEE-754 exponent/mantissa extraction and a 16-segment degree-9 polynomial in `Double2` (verified bit-exact against `std::pow` across all rendered takes) with bit-exact caching across dual lines, and `Chorus Off` short-circuits stopped-clock `std::log1p` trajectories and `noiseScale == 0` hiss math while advancing PRNG states identically. Dual chorus clock-driver integrators, pre-emphasis/anti-aliasing SVFs, recovery filters, stereo output-jack stages and headphone load/blocking filters step in `Double2`, and identical dual-mono chorus/summer samples deduplicate nonlinear clipping.
5. **Coupled mixer, sub-level diode solve, OTA shot noise, and firmware scheduler trims** (`YouKnowCoupledMixer.h`, `YouKnowSubLevel.h`, `YouKnowOtaShotNoise.h`, `YouKnowFirmwareAssignerScheduler.cpp`). All-zero oscillator mixer frames short-circuit saturation and bleed solves, `subOscillator Diode` caches 1-entry bit-exact voltages across held half-cycles, host-grid thermal/shot-noise prep is skipped when shadowed by an active voice island, and `FirmwareAssignerScheduler::advanceTo` hoists active voice counts/pointers and reuses the decoded ROM instruction across each cycle.

Circuit equations, filter topologies, oversampling rates, latency, noise laws, random-draw chronology and emulated firmware behaviour are unchanged.

## Native calling-thread CPU benchmark (`Tools/CompareCpu.py`)

Measured with `Tools/CompareCpu.py` (`YouKnowOversamplingAudit`, 48 kHz, `1x` quality, `A/B/A` bracketing baselines, statistic `1 - candidate median / faster bracketing baseline median`):

| Mode | Scenario | Baseline median (ms) | Candidate median (ms) | Reduction | Stream fingerprint |
| --- | --- | ---: | ---: | ---: | --- |
| `original` | `idle-dry` | 285.55 | 264.36 | −7.4 % | `31e2fbc5c4746028` (match) |
| `original` | `single-voice-plain-dry` | 292.43 | 267.30 | −8.6 % | `f8b3538b45a9eab5` (match) |
| `original` | `six-voice-low-cutoff-dry` | 301.73 | 280.13 | −7.2 % | `36786347e3e41f43` (match) |
| `original` | `six-voice-plain-dry` | 299.28 | 275.12 | −8.1 % | `45fcce7673821c8a` (match) |
| `original` | `single-voice-resonant-dry` | 298.62 | 274.28 | −8.2 % | `8c875ece629b92f3` (match) |
| `original` | `six-voice-resonant-dry` | 310.45 | 286.31 | −7.8 % | `f0d18716b4f11390` (match) |
| `original` | `six-voice-full-mixer-chorus-ii` | 442.38 | 382.14 | **−13.6 %** | `926168d61023e368` (match) |
| `direct` | `idle-dry` | 270.29 | 245.87 | −9.0 % | `f54a9eed63ae2b14` (match) |
| `direct` | `single-voice-plain-dry` | 276.22 | 252.68 | −8.5 % | `9a99c589537cd6f8` (match) |
| `direct` | `six-voice-low-cutoff-dry` | 287.40 | 265.28 | −7.7 % | `2cbeafc67e5fe9e6` (match) |
| `direct` | `six-voice-plain-dry` | 282.96 | 260.58 | −7.9 % | `540c8d508200118d` (match) |
| `direct` | `single-voice-resonant-dry` | 287.06 | 262.20 | −8.7 % | `b5696a63b7c4953e` (match) |
| `direct` | `six-voice-resonant-dry` | 296.76 | 273.54 | −7.8 % | `6c2f7d6473233991` (match) |
| `direct` | `six-voice-full-mixer-chorus-ii` | 429.73 | 368.54 | **−14.2 %** | `2aba16c94420f10c` (match) |
| `direct` | `sixteen-voice-full-mixer-chorus-ii` | 966.33 | 811.07 | **−16.1 %** | `6c891b0d7b25da7f` (match) |

All 15 scenarios report `fingerprints_match: true`.

## Audio parity & test gates

- **`Tools/CompareProductAudio.py`** against `5ccfafa` (`1.4.7`): **20 of 20 product validation renders `identical`** (`original` and `direct` modes across `44.1 kHz`, `48 kHz`, `96 kHz` at `1x`, `2x`, `4x` and `173` / `64` frame blocks; `0 ULP` difference).
- **`Tools/CheckDspPortability.py --cxx clang++`**: passed strict native and embedded (`-DYOUKNOW_EMBEDDED_TARGET=1 -std=c++17 -O3 -fno-builtin -fno-exceptions -Wall -Wextra -Wpedantic -Werror -pedantic-errors`) compilation, symbol-table checks (zero `__cxa_guard_acquire` or writable globals), C++17/C++20 view parity and 123-field equality checks.
- **DSP CTest suite** (`-DYOUKNOW_BUILD_PLUGIN=OFF -DBUILD_TESTING=ON`, Release):
  - `YouKnowDSPCxx17` and `YouKnowDSPWorkAudit` built cleanly.
  - `unit` 132/132 passed.
  - `audio` 2/2 passed (`YouKnow.AudioDemos`, `YouKnow.MoreDemos`).
  - `perf` 2/2 passed (`YouKnow.EnginePerformance`, `YouKnow.CpuBudget`).

## Reproduce

```sh
cmake -S /path/to/1.4.7-checkout -B build-base -DCMAKE_BUILD_TYPE=Release -DYOUKNOW_BUILD_PLUGIN=OFF -DBUILD_TESTING=OFF
cmake --build build-base --parallel --target YouKnowRenderProductValidation YouKnowOversamplingAudit
cmake -S . -B build-cand -DCMAKE_BUILD_TYPE=Release -DYOUKNOW_BUILD_PLUGIN=OFF -DBUILD_TESTING=ON
cmake --build build-cand --parallel
python3 Tools/CheckDspPortability.py --cxx clang++
python3 Tools/CompareProductAudio.py --baseline build-base/YouKnowRenderProductValidation --candidate build-cand/YouKnowRenderProductValidation --output /tmp/audio-parity
python3 Tools/CompareCpu.py --baseline build-base/YouKnowOversamplingAudit --candidate build-cand/YouKnowOversamplingAudit --out-dir /tmp/cpu-bench
```
