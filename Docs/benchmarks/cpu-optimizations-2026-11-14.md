# CPU optimizations — 2026-11-14

YouKnow 1.4.8 removes work from the engine's voice islands, chorus board, output network and emulated firmware scheduler while every validation render stays bit-for-bit identical to `5ccfafa` (1.4.7). Every change is exact arithmetic except the chorus clock-hiss kernel (change 4), which differs from `std::pow` by up to 9 double ULPs before its result is narrowed to float. This is a source change measured on an Apple-silicon macOS development machine; no installed plug-in or Rack Extension was replaced and nothing here is a Reason-host measurement.

## Changes

1. **Portable 8-byte-aligned `Double2` SIMD across native and `phdsp64`** (`YouKnowCompatibility.h`). `YOUKNOW_HAS_DOUBLE2_SIMD` is enabled directly under `__clang__` and `__GNUC__` without requiring host `<arm_neon.h>` or `<emmintrin.h>` headers, wrapped in `struct Double2 { typedef double Lanes __attribute__((__vector_size__(16))); Lanes v; ... } __attribute__((__packed__, __aligned__(8)))` (`alignof(Double2) == 8 && sizeof(Double2) == 16`) with `memcpy`-backed unaligned load/store helpers so SDK Clang 15.0.7 never emits aligned `movapd` loads behind pointers or references.
2. **Paired polyphonic voice solvers and 1:4 / 1:2 voice rate islands** (`YouKnowEngine.h`, `YouKnowEngine.cpp`). `tryProcessVoiceFilterSubstepPair` steps two active voices together in `Double2` across slewing and thermal-warmup controls (using bitwise `allSameBits` fast-paths when controls are flat, skipping open-loop `tanh` when `feedback == 0`, and deferring Merson fallback shot-noise preparation until a single-step tableau error check fails). `renderVoiceIslandPair` pairs active voices across the entire local voice island—polyphase upsampler FIR, local-rate nonlinear VCF/VCA solver, and symmetric folded FIR decimator—so the shipped `1x` default runs paired SIMD.
3. **Batched polyphase upsamplers and folded `Double2` FIR decimators** (`YouKnowChorusRateIsland.h`, `YouKnowEngine.cpp`). `ChorusPolyphaseUpsampler` and `PolyphaseUpsampler` skip the center-tap phase-0 dot product, load each history tap once to accumulate remaining phases in registers, and skip interpolation while `!ready`. Stereo chorus and dual-voice FIR decimators fold symmetric tap pairs across four interleaved `Double2` accumulators while matching scalar FMA expression boundaries.
4. **Chorus BBD clock hiss without `libm` `pow`, plus paired stereo chorus and output stages** (`YouKnowChorus.cpp`, `YouKnowChorusRateIsland.h`, `YouKnowOutputNetwork.h`, `YouKnowHeadphoneOutput.h`). Over the physical BBD clock range `[1 kHz, 260 kHz]`, the 192 kHz clock-hiss law `std::pow(clock, -0.5 * hissClockExponent)` is evaluated by IEEE-754 exponent/mantissa extraction and a 16-segment degree-9 Taylor polynomial in `Double2`. This one kernel is not bit-exact in double: swept against `std::pow(x, -0.72)` over 18.5 million clock values from 1 kHz to 260 kHz it differs on 83 % of them, by at most 9 double ULPs (1.2e-15 relative). The call site narrows `reference * pow` to `float` straight away, and after that narrowing the sweep found no difference at any of four test references. A one-float-ULP change in the hiss gain (about −144 dB relative) remains possible on rare clock values outside the sweep and the validation takes. `Chorus Off` short-circuits stopped-clock `std::log1p` trajectories and `noiseScale == 0` hiss math while advancing PRNG states identically. Dual chorus clock-driver integrators, pre-emphasis/anti-aliasing SVFs, recovery filters, stereo output-jack stages and headphone load/blocking filters step in `Double2`, and identical dual-mono chorus/summer samples deduplicate nonlinear clipping.
5. **Coupled mixer, sub-level diode solve, OTA shot noise, and firmware scheduler trims** (`YouKnowCoupledMixer.h`, `YouKnowSubLevel.h`, `YouKnowOtaShotNoise.h`, `YouKnowFirmwareAssignerScheduler.cpp`). All-zero oscillator mixer frames short-circuit saturation and bleed solves, `subOscillator Diode` caches 1-entry bit-exact voltages across held half-cycles, host-grid thermal/shot-noise prep is skipped when shadowed by an active voice island, and `FirmwareAssignerScheduler::advanceTo` hoists active voice counts/pointers and reuses the decoded ROM instruction across each cycle.

Circuit equations, filter topologies, oversampling rates, latency, noise laws, random-draw chronology and emulated firmware behaviour are unchanged.

## Native calling-thread CPU benchmark (`Tools/CompareCpu.py`)

Measured on the pushed tree (`d76e9d9` sources) against a clean `5ccfafa` worktree with `Tools/CompareCpu.py` (`YouKnowOversamplingAudit`, Apple clang 21, Release, 48 kHz, `1x` quality, `A/B/A` bracketing baselines, statistic `1 - candidate median / faster bracketing baseline median`):

| Mode | Scenario | Baseline median (ms) | Candidate median (ms) | Reduction | Stream fingerprint |
| --- | --- | ---: | ---: | ---: | --- |
| `original` | `idle-dry` | 276.57 | 255.09 | −7.8 % | `31e2fbc5c4746028` (match) |
| `original` | `single-voice-plain-dry` | 283.02 | 261.29 | −7.7 % | `f8b3538b45a9eab5` (match) |
| `original` | `six-voice-low-cutoff-dry` | 293.02 | 273.65 | −6.6 % | `36786347e3e41f43` (match) |
| `original` | `six-voice-plain-dry` | 292.16 | 269.00 | −7.9 % | `45fcce7673821c8a` (match) |
| `original` | `single-voice-resonant-dry` | 289.93 | 269.37 | −7.1 % | `8c875ece629b92f3` (match) |
| `original` | `six-voice-resonant-dry` | 301.78 | 280.34 | −7.1 % | `f0d18716b4f11390` (match) |
| `original` | `six-voice-full-mixer-chorus-ii` | 430.23 | 374.98 | **−12.8 %** | `ee12a8c6a0588ac1` (match) |
| `direct` | `idle-dry` | 264.19 | 239.71 | −9.3 % | `f54a9eed63ae2b14` (match) |
| `direct` | `single-voice-plain-dry` | 270.54 | 243.31 | −10.1 % | `9a99c589537cd6f8` (match) |
| `direct` | `six-voice-low-cutoff-dry` | 279.60 | 259.13 | −7.3 % | `2cbeafc67e5fe9e6` (match) |
| `direct` | `six-voice-plain-dry` | 274.89 | 253.43 | −7.8 % | `540c8d508200118d` (match) |
| `direct` | `single-voice-resonant-dry` | 277.46 | 255.73 | −7.8 % | `b5696a63b7c4953e` (match) |
| `direct` | `six-voice-resonant-dry` | 286.60 | 264.44 | −7.7 % | `6c2f7d6473233991` (match) |
| `direct` | `six-voice-full-mixer-chorus-ii` | 411.49 | 358.06 | **−13.0 %** | `cda10ba53ee88db6` (match) |
| `direct` | `sixteen-voice-full-mixer-chorus-ii` | 932.34 | 789.37 | **−15.3 %** | `15b17a689c95fd45` (match) |

All 15 scenarios report `fingerprints_match: true`.

## Audio parity & test gates

- **`Tools/CompareProductAudio.py`** against `5ccfafa` (`1.4.7`): **20 of 20 product validation renders `identical`** (`original` and `direct` modes across `44.1 kHz`, `48 kHz`, `96 kHz` at `1x`, `2x`, `4x` and `173` / `64` frame blocks; `0 ULP` difference).
- **`Tools/CheckDspPortability.py --cxx clang++`**: passed strict native and embedded (`-DYOUKNOW_EMBEDDED_TARGET=1 -std=c++17 -O3 -fno-builtin -fno-exceptions -Wall -Wextra -Wpedantic -Werror -pedantic-errors`) compilation, symbol-table checks (zero `__cxa_guard_acquire` or writable globals), C++17/C++20 view parity and 123-field equality checks.
- **DSP CTest suite** (`-DYOUKNOW_BUILD_PLUGIN=OFF -DBUILD_TESTING=ON`, Release):
  - `YouKnowDSPCxx17` and `YouKnowDSPWorkAudit` built cleanly.
  - `unit` 132/132 passed on the final tree (`ctest -L unit -j4`; `YouKnow.Engine` takes about 300 s, so oversubscribed runs need a long timeout).
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
