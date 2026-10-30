# CPU in a settled session — 2026-10-30

YouKnow 1.4.2 removes repeated work from the engine's audio path. Five changes leave every rendered sample identical; one, the collector shot-noise factor, is a numerical choice that changes the noise realisation at a median −123 dB re signal. The baseline is `e7fcd1c` (1.4.1). This is an unreleased source change measured on a development container; no installed plug-in or Rack Extension was replaced and nothing here is a Reason-host measurement.

## What a session spends

Profiling the Rack build showed that, unlike the 1.3.4 round, most of the remaining fixed cost is not the voice filter but work that repeats a result it already has:

- all sixteen voice slots keep integrating their VCA junction temperature, and once the chassis warm-up has reached 1.0f (about 51 s after the instrument is created, a 3 s time constant) an idle slot solves the same fixed point on every interval;
- the chorus clock-mute network does the same under a held command;
- the emulated A-5 firmware copies its execution state per instruction and zeroes a 24 KiB event buffer per slice it then discards.

So this round measures two sessions: **cold** (the first minute, the chassis still warming) and **settled** (a thermally settled start, `configureThermalStart(true)`, and 3000 warm-up callbacks), which is what almost all playing time is.

## Changes

One mechanism per commit, each verified before the next.

1. **Assigner instructions in place** (`4b256aa`). Each completed A-5 instruction copied the 176-byte execution state into a transaction and back on success. The transaction now runs on the state itself and saves only the frontier, which a failed transaction restores exactly as before.
2. **No zeroing of the discarded assigner event buffer** (`62cf96a`). Only `[0, count)` of the per-slice 1024-entry buffer is ever read.
3. **Settled clock-mute drive** (`d188de4`). `advanceClockMuteDrive` is a pure function of three capacitor voltages, the command, two drive flags and the support chain. Once an interval returns its own input, that fixed point and its outputs are recorded and replayed for the same key; the record is cleared with the support chain and on reset.
4. **Settled VCA junction temperature** (`bcb2b1e`). The same idea for each slot's RK4 hold-capacitor interval: an interval that returned its own starting charge is recorded with its exact inputs, and the next interval with exactly those inputs returns that charge without solving. A moving slot pays one comparison. `advance()` is forced inline so the check does not cost the integrator its inlining. This is the round's largest bit-identical saving, and it costs 0.3–0.6 % during the cold first minute.
5. **Voice filter frame built in place** (`3402410`). `prepareVoiceFilter` mixed `return {}` with a named frame, which defeated the named return value optimisation: a ~260-byte frame and a control trajectory were zero-filled and copied every internal interval.
6. **Collector shot-noise factor every fourth internal sample** (`df0b630`, not bit-identical). The voice island rebuilt the collector shot-noise covariance factor at the first local interval of every internal sample; it now rebuilds it every fourth and reuses it in between. Every interval still draws its own innovation and no random draw moves. This is the candidate the 1.3.4 round left for the owner's ear; it ships under the project's standing rule and is recorded in [decisions](../decisions.md).

Circuit equations, kernels, tableaux, processing rates, latency, noise laws and densities are unchanged.

## Instruction counts

`bench.cpp` plays the product configuration (A11 Brass Set 1, 48 kHz, Normal Circuit Rate, Original timing, chorus II, 64-frame callbacks) under `valgrind --tool=callgrind --collect-atstart=no`, 200 measured callbacks. Idle is no notes; six voices is the six-note chord retriggered and transposed every 512 callbacks. Compiled as the Rack does: C++17, `-O3 -fno-builtin -DYOUKNOW_EMBEDDED_TARGET=1`, Clang 18 (the Rack toolchain is Clang-based), x86-64. Instructions per 64-frame callback:

| Commit | Cold idle | Cold six voices | Settled idle | Settled six voices |
| --- | ---: | ---: | ---: | ---: |
| baseline 1.4.1 | 3,627,208 | 6,283,989 | 3,632,715 | 6,254,874 |
| 1. Assigner in place | 3,607,760 | 6,264,630 | 3,613,140 | 6,235,380 |
| 2. Event buffer | 3,558,550 | 6,214,920 | 3,563,930 | 6,185,680 |
| 3. Clock-mute fixed point | 3,534,610 | 6,190,980 | 3,539,990 | 6,161,740 |
| 4. VCA junction fixed point | 3,554,520 | 6,211,620 | 3,312,330 | 6,004,590 |
| 5. Filter frame in place | 3,551,310 | 6,208,410 | 3,309,130 | 6,001,380 |
| 6. Shot-noise factor every fourth sample | 3,551,310 | 5,828,170 | 3,309,130 | 5,627,260 |
| **1.4.2 against 1.4.1** | **−2.1 %** | **−7.3 %** | **−8.9 %** | **−10.0 %** |

The five bit-identical commits alone: settled idle −8.9 %, settled six voices −4.1 %, cold idle −2.1 %, cold six voices −1.2 %. Resting cards do not use the shot-noise factor, so the sixth commit leaves idle unchanged.

GCC 13 with the same flags shows the same direction (the commit messages carry its figures). GCC copies these structs with `rep movsb`, which callgrind counts per byte, so its figures overstate the copy savings; Clang's are the ones to quote.

## Audio parity

`nulltest.cpp` renders all 144 presets through the product configuration (the 1.3.4 harness, with `YK_SETTLED` and `YK_PREROLL` added) and writes raw stereo float32; `null.sh` runs it in four shards.

- Commits 1–5: **all 144 presets bit-identical** to the baseline, both cold (0.37 s pre-roll) and thermally settled (3 s pre-roll), and the benchmark stream hashes equal.
- Commit 6: median paired residual −123 dB re signal; 131 presets below −60 dB; whole-file RMS within 0.03 dB on every preset. The self-oscillating presets take a different noise path at the same level, worst B77 −8 dB and B87 −24 dB re signal, which is the divergence any last-bit change produces in them ([why](cpu-trims-2026-10-23.md#considered-and-not-shipped)).

## Denormals

A played-then-silent instance (`YK_PLAYFIRST`) costs the same as one never played: the analogue noise floor keeps every state normal, so there is no denormal tail to flush.

## Thread CPU

Interleaved baseline/candidate runs of the Clang Rack-flag benchmark, 1.4.1 against 1.4.2 with all six commits (1.3.4's `paired_timing.sh` with `YK_SETTLED=1`: five repetitions per configuration, run order alternating, 300 measured callbacks, `CLOCK_THREAD_CPUTIME_ID` around `process()`, six voices), on a four-core container:

| Rate | Circuit Rate | Preset | 1.4.1 median µs/block | 1.4.2 median µs/block | Median paired reduction |
| ---: | --- | --- | ---: | ---: | ---: |
| 48000 | Normal | A11 Brass Set 1 | 641 | 548 | 14.4 % |
| 48000 | Normal | A61 Piano II | 587 | 528 | 10.8 % |
| 48000 | Normal | B31 Brass | 623 | 550 | 10.2 % |
| 48000 | Normal | YB3 Solid Saw | 630 | 503 | 20.6 % |
| 44100 | Normal | A11 Brass Set 1 | 619 | 578 | 11.1 % |
| 96000 | Normal | A11 Brass Set 1 | 369 | 361 | 1.0 % |
| 48000 | Max | A11 Brass Set 1 | 948 | 882 | 8.4 % |
| 48000 | Max | B31 Brass | 941 | 869 | 6.9 % |

Every configuration favours 1.4.2. Timing on this container moves by about ±15 % run to run, so the instruction counts above are the reliable measure; the 96 kHz row is inside that noise. None of this is a Reason-host or Apple-silicon figure. Every run's stream hash was stable across repetitions. [Raw runs](cpu-settled-2026-10-30-timing.csv).

## Tests

The DSP test tree (`-DYOUKNOW_BUILD_PLUGIN=OFF -DBUILD_TESTING=ON`, Release, GCC 13, x86-64), clean rebuild:

- After commit 5: `unit` 132/132 and `audio` 2/2 passed.
- After commit 6: `unit` 132/132 (including `YouKnow.OtaShotNoise`, whose paired residual against per-interval factoring is −79 to −87 dB, against −80 to −86 dB for every-sample sharing and a −25 dB limit) and `audio` 2/2 passed.
- After commit 6: `perf` tier passed (`YouKnow.EnginePerformance`, `YouKnow.CpuBudget`), run on its own.
- `YouKnowDSPCxx17` built. The `fuzz` and `render` tiers belong to the plug-in build and were not run here.

## Reproduce

```sh
# harness: Docs/benchmarks/cpu-settled-2026-10-30/ (compare_null.py and paired_timing.sh are 1.3.4's)
H=Docs/benchmarks/cpu-settled-2026-10-30
CXX=clang++ $H/build.sh /path/to/baseline-checkout bench /tmp/bench-baseline rack
CXX=clang++ $H/build.sh . bench /tmp/bench-candidate rack
YK_NOTES=0 valgrind --tool=callgrind --collect-atstart=no /tmp/bench-candidate original 48000 normal 200 2 1 0
YK_SETTLED=1 YK_WARM=3000 YK_NOTES=6 valgrind --tool=callgrind --collect-atstart=no /tmp/bench-candidate original 48000 normal 200 2 1 0
$H/build.sh /path/to/baseline-checkout nulltest /tmp/nulltest-baseline plugin
$H/build.sh . nulltest /tmp/nulltest-candidate plugin
$H/null.sh /tmp/nulltest-baseline /tmp/null-base
YK_SETTLED=1 YK_PREROLL=3 $H/null.sh /tmp/nulltest-candidate /tmp/null-cand-settled
python3 -I Docs/benchmarks/cpu-trims-2026-10-23/compare_null.py /tmp/null-base /tmp/null-cand --csv /tmp/null.csv
YK_SETTLED=1 Docs/benchmarks/cpu-trims-2026-10-23/paired_timing.sh /tmp/bench-baseline /tmp/bench-candidate /tmp/timing.csv 5 300
```
