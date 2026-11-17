# CPU in the Rack toolchain — 2026-11-17

YouKnow 1.4.11 makes the Rack Extension's build use about 36 % less CPU than
1.4.10 while every render stays bit-for-bit identical. Every change is exact
arithmetic. The baseline is `4eb8fa1`, the engine the 1.4.10f2 Rack device
pins. The figures come from development machines running the Jukebox SDK's
own toolchain; no installed Rack Extension was replaced and nothing here is a
Reason-host measurement.

## Why the Rack build was slow

`build45.py` compiles the engine with `-fno-builtin -ffreestanding` against the
SDK's own C library. Under `-fno-builtin` the compiler may not treat a library
function as an intrinsic, so `std::memcpy`, `std::isfinite` and `std::abs`
become real calls into that library, which copies short blocks a byte at a
time. Native plug-in builds never see this: their compilers expand the same
calls inline.

1.4.8 added the packed `Double2` pair type, whose `double2Load` and
`double2Store` used `std::memcpy` so that no aligned vector move could reach an
8-byte-aligned member. In the Rack build, every one of those 16-byte loads and
stores became a library call. The 1.4.10f2 arm64 Deployment dylib has these
library call sites:

| Call | Sites | From |
| --- | ---: | --- |
| `memcpy` | 240 | `Double2` loads and stores, `bitCast` |
| `__fpclassifyd` | 187 | `std::isfinite` |
| `fabs` | 155 | `std::abs` |
| `floorf` | 96 | `std::floor` on `float` |
| `__signbitd` | 42 | `std::signbit` |

## Changes

1. **`Double2` loads and stores, and `bitCast`, use `__builtin_memcpy`**
   (`YouKnowCompatibility.h`, and the chorus hiss kernel in
   `YouKnowChorus.cpp`). A fixed 16-byte builtin copy lowers to one unaligned
   vector move under `-fno-builtin` too, so the type keeps its alignment
   guarantee and no aligned move is introduced.
2. **Finite checks use the engine's `finiteValue` bit test**, which replaces
   `std::isfinite` in the chorus, bucket noise, mute drive, coupled mixer,
   DCO, envelope hold, high-pass switch, output network and headphone
   output.
3. **The paired RK4 OTA solve stops zeroing its lane scratch**
   (`YouKnowEngine.cpp`, `tryProcessSettledRk4Pair`). Both lanes' node arrays
   were value-initialised on every pair step, a 1.6 KB `memset`. The solve
   reads only the tableau's nodes, and `checkLane` and `buildLaneNodes` write
   each of them before it is read.
4. **The VCA charge table's cached cell keeps its left edge as a `double`**
   (`YouKnowEvidenceVca.h`), and `sampleCached` is forced inline. The
   interpolation fraction reads the cached edge, which is the same value as
   the cell number it computed before. The integer round trip now feeds only
   the cache-hit check, not the RK stages' serial chain.
5. **The OTA shot-noise covariance norms and Cholesky pivot check use
   `inlineAbs`** (`YouKnowOtaShotNoise.h`).

Circuit equations, filter topologies, oversampling, latency, noise laws,
random-draw order and firmware behaviour are unchanged. The remaining `fabs`,
`floorf` and `signbit` calls are left for a later pass.

## Mac: the SDK's Deployment builds

Apple-silicon MacBook Pro (M1 Max). Each variant was built as a Deployment
`local45` build through `build45.py` from the Rack adapter (arm64 and
x86_64 dylibs, not installed), then played through the adapter's patch host,
one process per render, 48 kHz, on identical MIDI. Nothing else was building
or validating during the timings. "f2" is the shipped 1.4.10f2 Deployment
build, as a reference. "base" is `4eb8fa1` built on the same machine. "c3"
is changes 1 and 2, and "c4" is all five changes, the 1.4.11 source.

The realtime factor is CPU seconds per second of audio, as a median over five
rounds in alternating variant order. The worst block is the slowest 256-frame
block's time as a share of its 5.33 ms of audio.

| Phrase | f2 | base | c3 | c4 | c4 against base |
| --- | ---: | ---: | ---: | ---: | ---: |
| 001-bass-rubber-pulse | 0.4513 | 0.4415 | 0.2891 | 0.2668 | −39.6 % |
| 001-bass-rubber-pulse-stress | 0.3674 | 0.3671 | 0.2482 | 0.2371 | −35.4 % |
| 018-brass-bright-stab | 0.3893 | 0.4115 | 0.2903 | 0.2713 | −34.1 % |
| 018-brass-bright-stab-stress | 0.4370 | 0.4260 | 0.2771 | 0.2513 | −41.0 % |
| 033-effects-rising-voltage | 0.6365 | 0.6224 | 0.4745 | 0.4156 | −33.2 % |
| 033-effects-rising-voltage-stress | 0.6340 | 0.6716 | 0.4654 | 0.4466 | −33.5 % |
| 046-keys-velvet-electric | 0.5255 | 0.5261 | 0.3494 | 0.3397 | −35.4 % |
| 046-keys-velvet-electric-stress | 0.6090 | 0.6203 | 0.4163 | 0.3785 | −39.0 % |
| 061-pads-lunar-drift | 0.5825 | 0.5742 | 0.3898 | 0.3754 | −34.6 % |
| 061-pads-lunar-drift-stress | 0.5953 | 0.5693 | 0.4051 | 0.3943 | −30.7 % |
| 076-self-oscillating-sine | 0.3693 | 0.3751 | 0.2350 | 0.2206 | −41.2 % |
| 076-self-oscillating-sine-stress | 0.4295 | 0.4257 | 0.2845 | 0.2720 | −36.1 % |
| 079-strings-pwm-ensemble | 0.6148 | 0.6086 | 0.4068 | 0.3885 | −36.2 % |
| 079-strings-pwm-ensemble-stress | 0.6013 | 0.6391 | 0.4309 | 0.3975 | −37.8 % |
| 100-vibrato-lead | 0.5228 | 0.5212 | 0.3659 | 0.3563 | −31.6 % |
| 100-vibrato-lead-stress | 0.5357 | 0.5596 | 0.3860 | 0.3657 | −34.7 % |
| **Sum** | 8.301 | 8.359 | 5.714 | 5.377 | **−35.7 %** |

| Variant | Median realtime factor | Worst block, median | Worst block, max |
| --- | ---: | ---: | ---: |
| f2 | 0.5306 | 431 % | 1390 % |
| base | 0.5428 | 608 % | 2899 % |
| c3 | 0.3760 | 498 % | 1018 % |
| c4 | 0.3610 | 225 % | 674 % |

f2 and base agree to within 0.7 % on the sum, so the Mac's build of `4eb8fa1`
stands for the shipped device. The worst blocks are single-thread renders on
the patch host, given for the comparison, not as a Reason meter reading.

**All 100 factory patches**, playing the standard phrase, base and c4, arm64:
the median realtime factor is 0.5034 for base and 0.3199 for c4. The median
c4/base ratio is 0.637, ranging from 0.532 to 0.806, so every patch saves
between 19 % and 47 %. The worst-block median is 129 % against 73 %, and the
maximum is 1520 % against 845 %.

## Container: an SDK-faithful x86_64 build

Linux VM, Intel Xeon. Each variant is compiled the way `build45.py` builds a
Deployment dylib: the SDK's own C and C++ headers,
`-fno-builtin -ffreestanding -O3`, `llvm-link` with the SDK's `libcpp.bc`
and `libc.bc`, and `llc -O2`, targeting x86_64 with SSE3. Like `phdsp64`
code it has no FMA and no auto-vectorisation. The SDK's `memcpy`, `fabs`,
`__fpclassifyd` and `floorf` run as their own implementations. Only `exp`,
`log`, `pow`, `sin`, `cos` and `sqrt`, which Reason's dylibs take from the
host, come from glibc. The device runs over the adapter's mock host. The
figure is thread CPU in `RenderBatch` over the same sixteen phrases, pinned
to one core, as a median of alternating rounds: base 73.08 s, c4 38.94 s,
**−46.7 %**. This build is an approximation of the Rack toolchain and is
given only as a second machine's direction. The Mac figures are the
measurement.

## Parity

- **Mac:** the sixteen phrases' rendered audio hashes are identical to base
  for f2, c3 and c4, arm64 and x86_64 (Rosetta), 0 of 32 different. All 100
  patches are identical for c4 on arm64.
- **Container:** the SDK-faithful build of c4 renders identical to base for
  all 100 patches, and for the sixteen phrases at 44.1 kHz, at 96 kHz, at
  Quality 2x and 4x, with Circuit Rate switched, at 44.1 kHz with Quality 4x
  and at 96 kHz with Circuit Rate switched.
- **x86_64 object code:** no aligned packed SSE move on non-stack memory
  appears in the candidate that is not already in base. The one aligned
  `movaps` in `advanceFirmwareSerialInterval` is unchanged from base.

## Tests

On the 1.4.11 source, Linux, GCC 13.3, Release, `-DYOUKNOW_BUILD_PLUGIN=OFF
-DBUILD_TESTING=ON`:

- `unit` 132/132 passed.
- `audio` 2/2 passed (`YouKnow.AudioDemos`, `YouKnow.MoreDemos`).
- `perf` 2/2 passed (`YouKnow.EnginePerformance`, `YouKnow.CpuBudget`).
- `YouKnowDSPCxx17` built, and `Tools/CheckDspPortability.py` passed.

## Reproduce

In the Rack repository, point `Examples/YouKnow/Instrument` at each revision
in turn and build a Deployment `local45` build with `build45.py`, without
installing. Play the eight demo patches' MIDI and the stress phrase through
the adapter's patch host, one process per render, alternating the variants'
order each round. Compare the rendered audio's hashes against base before
reading any timing.
