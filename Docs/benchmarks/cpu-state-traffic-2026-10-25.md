# CPU: state traffic — 2026-10-25

YouKnow 1.3.5 removes copying, zero-filling and out-of-line library calls from the engine's audio path without changing a rendered sample. The baseline is `62bf8c2` (1.3.4 plus its generated media). This is an unreleased source change measured on a shared development container; no installed plug-in or Rack Extension was replaced and nothing here is a Reason-host measurement.

## Why only exact changes

The 2026-10-23 round showed that YouKnow's fifteen self-oscillating factory presets turn a one-ulp control change into a divergent oscillation within 40 ms ([cpu-trims-2026-10-23.md](cpu-trims-2026-10-23.md), "Why last-bit changes are not null-testable"). A change that is not bit-exact cannot be graded by a residual here, so this round considered only mechanisms that keep every double the engine computes. Each one is graded *exact*: the float32 output of all 144 presets is byte-identical to the baseline's.

## Changes

One mechanism per commit, each verified bit-identical (all 144 presets) and against the unit suites covering it before the next was started.

1. **`3a2f548` Assigner instructions execute on the live state** (`YouKnowFirmwareAssignerScheduler.cpp`). The A-5 scheduler staged every emulated instruction in a copy of its 173-byte execution state and copied it back on success. The instruction now executes on the live state, and one committed copy is restored on every failure path (unsupported or invalid instruction, `OutputFull`, UART write failure), so a failed instruction still leaves the frontier as it was. RAM and patch RAM still change only through the bounded write ledger. Exact.
2. **`024b6e2` The bridge's discarded CPU event ledger is kept** (`YouKnowFirmwareAssignerAudioBridge.*`). Every 4096-state source slice built a fresh, zero-filled 24 KiB scheduler event buffer only to discard its entries. The buffer is now bridge state, zeroed once; only its count is reset per slice. The scheduler writes each entry before anything reads it and the bridge reads none. Exact.
3. **`69c75ec` Each voice's filter frame is built in place** (`prepareVoiceFilter`). Two early `return {}` paths stopped the compiler from building the 256-byte `VoiceFilterFrame` in the caller's storage, so it was copied out on every voice sample, and a hold trajectory was assembled in a separate zero-filled object and copied in. Every return now names the one frame, and the trajectory is written straight into it, whose own trajectory starts zeroed as the separate object did. Exact.
4. **`08591e0` The remaining audio-path square roots are in line.** Under `-fno-builtin` a double `std::sqrt` is a libm call (in the SDK's libc a software routine). The output summer's three nested roots per clipped sample, the VCF tableau planner's closed-loop spectral factor, the chorus bucket-noise renormalization and the output summer's wet-ratio noise blend now use `inlineSqrt`, the same correctly rounded root as one instruction. Control- and rate-path roots are unchanged. Exact (IEEE square root is correctly rounded in both forms).

Circuit equations, kernels, tableaux, processing rates, latency, noise models, random draws, parameter ranges and every test are unchanged.

## Audio parity

`Tools/cpu-parity/nulltest.cpp` renders every factory and product preset through the product configuration — `ActiveProductFidelityProfile`, measured converter chart, PolyZoned/Cubic/RK4Single, Aging 50 %, six voices, 1x, Original timing, 64-frame callbacks — for one 3.6 s phrase (a six-note chord, two bass notes, a three-note chord under a pitch-bend sweep and mod wheel) after a 0.37 s pre-roll, and writes raw stereo float32. `compare_null.py` subtracts baseline from candidate. Both sides were built with the Rack-like flags below.

- 48 kHz, Normal Circuit Rate: **all 144 presets bit-identical after every one of the four commits** ([final per-preset table](cpu-state-traffic-2026-10-25-null-test.csv); every residual column reads −999 dB, the comparison's "no difference" marker). The fifteen self-oscillating presets are among them.
- 48 kHz, Max Circuit Rate: the 24 presets A11–A24 and B21–B34 (B21 is one of the two presets the 2026-10-23 one-ulp probe made diverge), baseline against the final tree, bit-identical.
- Benchmark stream hashes (FNV-1a over every output sample) equal between baseline and final at 48 kHz Normal (A11, A61, B31, YB3 and the synthetic resonant patch) and Max (A11, B55), 44.1 kHz (A11) and 96 kHz (B31) Normal, 96 kHz Direct timing with 2x oversampling (A11), and 48 kHz with 4x (A35); the GCC-Rack, Clang-Rack and GCC-host builds produce the same hashes.

## Instruction counts

`Tools/cpu-parity/bench.cpp` (its protocol is at the head of the file) plays the product configuration with six notes retriggered and transposed every 512 callbacks, 128 warm-up callbacks, then the measured callbacks under `valgrind --tool=callgrind --collect-atstart=no`, collection toggled around the measured loop. The primary build is the Rack's as nearly as a host without the SDK allows: every DSP translation unit at C++17, `-O3 -fno-builtin -DYOUKNOW_EMBEDDED_TARGET=1`, GCC 13.3, x86-64. Instructions per 64-frame callback, 48 kHz, six voices:

| Commit | A11 Normal | A61 Normal | B31 Normal | YB3 Normal | A11 Max |
| --- | ---: | ---: | ---: | ---: | ---: |
| baseline `62bf8c2` | 6,334,367 | 6,048,128 | 6,177,082 | 6,457,537 | 9,795,031 |
| 1. `3a2f548` live-state instructions | 6,217,183 (1.85 %) | 5,930,944 (1.94 %) | 6,059,898 (1.90 %) | 6,340,318 (1.82 %) | 9,678,005 (1.19 %) |
| 2. `024b6e2` kept event ledger | 6,167,613 (2.63 %) | 5,881,375 (2.76 %) | 6,010,329 (2.70 %) | 6,290,667 (2.58 %) | 9,628,271 (1.70 %) |
| 3. `69c75ec` frame in place | 6,143,106 (3.02 %) | 5,856,983 (3.16 %) | 5,985,938 (3.09 %) | 6,266,236 (2.96 %) | 9,603,763 (1.95 %) |
| 4. `08591e0` in-line roots | 6,117,501 (3.42 %) | 5,835,871 (3.51 %) | 5,961,113 (3.50 %) | 6,209,659 (3.84 %) | 9,573,692 (2.26 %) |

Percentages are cumulative against the baseline; 300 measured callbacks for Normal, 200 for Max. The same workload, baseline against the final tree, on two other builds:

| Build | A11 Normal | A61 Normal | B31 Normal | YB3 Normal | A11 Max |
| --- | ---: | ---: | ---: | ---: | ---: |
| Clang 18, the Rack-like flags | 1.29 % | 1.34 % | 1.31 % | 1.28 % | 0.83 % |
| GCC 13, plug-in flags (C++20 `-O3`) | 3.17 % | 3.32 % | 3.26 % | 3.12 % | 2.10 % |

[Raw table](cpu-state-traffic-2026-10-25-instructions.csv).

**Read the instruction counts with this caveat.** GCC copies and clears the scheduler's 173-byte state and the bridge's buffer with `rep movsb` / `rep stos`, and callgrind counts every iteration of a repeated string instruction as one instruction, so mechanisms 1–3 weigh more in the GCC counts than in cycles: hardware moves those bytes in far fewer cycles than one per byte. Clang copies the same structures without a per-byte string instruction, which is why its reduction is about a third of GCC's. The removed work — one of two state copies per emulated instruction, 24 KiB of zeroing per slice, one 256-byte copy per voice sample, the libm square-root calls — is real on both. The SDK's own lowering of these copies and of `sqrt` was not available to measure.

No thread-CPU timing is reported: the four-core container ran five other instruments' builds and tests throughout (load average 11–17), which puts run-to-run noise far above a change of this size.

## Considered and not shipped

- **A rollback-free assigner commit.** The one remaining copy per emulated instruction (about 1.9 % of the GCC count) is the rollback point for `OutputFull` and the other failure paths the scheduler's contract requires to leave the frontier untouched; avoiding it needs the instruction's writes enumerated per opcode. Not attempted.
- **`bitCast` through `__builtin_memcpy`.** Under Clang with `-fno-builtin`, the C++17 `bitCast` fallback's `std::memcpy` is a library call for each cache key; the builtin removes it (exact) but saved only 0.07 % of the Clang count. Not worth its own commit this round.
- **Builtins for `std::isfinite`, `std::abs`, `std::floor` and `std::copysign`** (about 250 sites). Exact by construction and, per the portfolio's Acustra round, a large win where the SDK's libc++ calls them out of line, but libstdc++ already inlines them on this host, so no host build can measure the change. Left for a round with the Rack SDK installed.
- **Skipping the unused zero-fill of the island's local frame and context** (about 0.5 % of the GCC count): needs a frame type without default member initializers, which every other `VoiceFilterFrame` user relies on.
- **More exact-equality memos.** Probed on A11 and rejected: the voice VCA junction's RK4 step never saw the same charge, target, interval and temperature twice in 437,494 calls, and the cutoff chain's existing memo misses on almost every voice sample because the rail-ripple term in its input moves every sample (211,152 `vcfEffectiveCutoffHz` evaluations in 556 callbacks, 12,828 of them from the hold-trajectory path, where no two consecutive calls had equal inputs).
- **Any last-bit or approximate mechanism** (rational kernels, Newton stop thresholds, per-sample divisions as coefficients, the collector-noise covariance refresh offered as an A–Z set on 2026-10-23): not null-testable on the self-oscillating presets, so out of scope for a round that ships only exact changes.

## Tests

The DSP test tree (`-DYOUKNOW_BUILD_PLUGIN=OFF -DBUILD_TESTING=ON`, Release, GCC 13, x86-64):

- Per commit, before the next was started: the 144-preset null test bit-identical, the benchmark hashes equal, and the unit suites covering the change — `FirmwareAssignerScheduler`, `FirmwareAssignerAudio`, `FirmwareAssignerAudioBridge`, `FirmwareAssignerTrace`, `OriginalPerformance`, `FirmwareLiveAssignerAudio`, `FirmwareSchedulerAudio` (1, 2); `Engine`, `EngineRegression`, `Circuit`, `OtaShotNoise`, `VoiceRateIsland`, `VcfNoise` (3); `ChorusBucketNoise`, `OutputSummerMagnitude`, `VcfIntegrator`, `DspPortability` (4) — all passed.
- Final tree: the whole tree built (including `YouKnowDSPCxx17`, the C++17 embedded DSP target); `unit` tier 122/122 and `fuzz` tier (`YouKnow.DspFuzz`) passed. In the first `ctest -L unit -j2` pass, run while five other instruments' builds and tests loaded the four cores (load average 11–17), 116 passed and six did not complete: `Engine` (1,800 s timeout), `PresetLevels` (1,800 s), `OversamplingAuditContract` and `DcoScanQualityContract` (300 s each) hit their CTest timeouts, and `Ba662TailNoise` and `VoiceVcaAntialias` were not run because an interrupted parallel build had left their executables unlinked. After the two were rebuilt, all six passed run one at a time on the unchanged tree with the same timeouts (`Engine` 693 s, `PresetLevels` 408 s, the contracts 100 s and 81 s). No timeout or test was changed.
- The `audio` tier (`YouKnow.AudioDemos`, about two hours on four cores) and the plug-in build's `render` tier were not run in this round.

## Reproduce

```sh
H=Tools/cpu-parity
$H/build.sh /path/to/baseline-checkout bench /tmp/bench-base rack
$H/build.sh . bench /tmp/bench-cand rack
$H/measure.sh /tmp/bench-base /tmp/base.csv
$H/measure.sh /tmp/bench-cand /tmp/cand.csv
$H/build.sh /path/to/baseline-checkout nulltest /tmp/null-base rack
$H/build.sh . nulltest /tmp/null-cand rack
/tmp/null-base /tmp/out-base original 48000 normal
/tmp/null-cand /tmp/out-cand original 48000 normal
python3 -I $H/compare_null.py /tmp/out-base /tmp/out-cand --csv /tmp/null.csv
```

`CXX=clang++` selects the second compiler and `plugin` in place of `rack` the plug-in's flags; use the same compiler and profile on both sides. `bench.cpp` includes `<valgrind/callgrind.h>` for the collection toggles (no-ops outside valgrind).
