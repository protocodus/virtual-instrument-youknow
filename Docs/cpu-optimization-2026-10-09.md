# YouKnow CPU optimization — 2026-10-09

The measurements in this report describe the optimization on baseline
`a0c6b08c0375b325f169e4dae7ece776081395b5`. Before publication, the change was
rebased onto main at `964ad6e`, which also contains independent thermal and
oscillator optimizations. The [integration record](cpu-2026-10-09/integration/validation.json)
identifies that combined source and its checks. The percentages below remain
the original comparison against `a0c6b08`; they do not measure the incremental
gain over the newer main.

The integrated source passed ten focused regressions and the full engine suite
(823.48 seconds). Its twenty audio renders also match the original frozen
baseline byte for byte. These integration checks cover native Linux x86-64;
the upstream ARM NEON path is outside this host's coverage.

At 48 kHz/1×, median CPU savings across the steady-state workloads are
**7.12% in Original mode and 8.65% in Direct mode**. Thirteen of fifteen
workloads exceed 5%; the complete range is 2.99–9.95%. All measured audio
fingerprints match. These are native DSP measurements on the machine described
below.

This change reduces repeated calculations in the current product engine,
relative to `a0c6b08c0375b325f169e4dae7ece776081395b5`. It keeps the existing
circuit model, Original/Direct timing modes, integration steps, noise draws,
oversampling and latency. The optimizations apply automatically. Measured
savings remain below 30%, so no additional UI switch is introduced.

The main improvement reuses the two temperature-interpolated current values
in the voice amplifier's current charge cell. Temperature interpolation is
linear in the grid values and their temperature slopes, so it can precede
charge interpolation. A small per-voice cache supplies all the RK4 evaluations
and subsequent gain/control reads until the cell, exact temperature or circuit
voltage span changes. Charge still interpolates on every call; there is no
reduced grid or control update rate. Charge normalization multiplies by a
precomputed reciprocal instead of repeating a hardware division at each lookup.
The only numerical changes are double rounding from these operations. Cache
state copies with its voice and has no audio-thread allocation.

Two smaller changes replace near-zero chorus transistor residual calculations
with a bounded degree-12 polynomial, retaining `expm1` outside ±0.25, and remove
a redundant UART advance in the Original firmware scheduler. The chorus Newton
tolerance is unchanged. The UART still drains each time boundary before CPU
events and preserves output-pressure retry ordering.

The CPU comparison uses GCC 14.2 Release/IPO on Linux x86-64, AMD EPYC 7763,
with both binaries pinned to the same logical CPU. The existing product audit
uses 48 kHz, 1× quality, Poly/Cubic/Normal, Aging 0.5 and 256-frame blocks.
Each scenario has two seconds of preroll and seven 32,768-frame measured
renders. Runs are serial baseline/candidate/baseline; the comparison uses the
faster of the two baseline medians. Initialization, snapshot copies and audio
hashing are outside the thread-CPU timer. These are native engine timings,
not a DAW CPU-meter or Reason-host measurement. The steady-state matrix does
not by itself measure note-on or automation cost.

| Workload | Original CPU reduction | Direct CPU reduction |
| --- | ---: | ---: |
| Idle, dry | 9.06% | 9.75% |
| Single voice, plain, dry | 8.99% | 9.95% |
| Six voices, low cutoff, dry | 7.12% | 9.06% |
| Six voices, plain, dry | 7.30% | 8.44% |
| Single voice, resonant, dry | 6.46% | 8.86% |
| Six voices, resonant, dry | 5.15% | 6.83% |
| Six voices, full mixer, Chorus II | 4.32% | 5.99% |
| Sixteen voices, full mixer, Chorus II | — | 2.99% |

The medians above give equal weight to seven Original and eight Direct
workloads. The heavy Original chorus
case and Direct sixteen-voice extension improve by less than 5%. The
[timing record](cpu-2026-10-09/timing/summary.json) retains each run's median,
minimum, median absolute deviation and fingerprint, with raw trial times in
the adjacent text files. Every reported reduction uses the faster bracketing
baseline.

The separate four-second control-transition score saves **6.49% in Original
mode and 6.93% in Direct mode** at 48 kHz/1× with 173-frame callbacks. Each
value is the median of three baseline/candidate/baseline comparisons, again
using the faster baseline in each bracket. All six reductions are between
5.71% and 7.28%, and all corresponding audio files are byte-identical. This
measures `process()` CPU while voices respond to the score; preparation,
synchronous note/control setters, resets, validation and file I/O are outside
the timer. The process CPU clock includes clock-read overhead and does not
measure callback wall latency. The [transition timing record](cpu-2026-10-09/transition-timing/summary.json)
retains each measurement; the [transition driver](cpu-2026-10-09/benchmark-transitions.py)
reproduces it after compiling the render fixture below.

The [build manifest](cpu-2026-10-09/manifest.json) records source/compiler/binary
identities, with [build instructions](cpu-2026-10-09/build-reproduction.txt).
Run the [comparison driver](cpu-2026-10-09/benchmark.py) against
matching Release builds, with other builds and renders stopped:

```sh
python3 Docs/cpu-2026-10-09/benchmark.py \
  --baseline /path/to/baseline/YouKnowOversamplingAudit \
  --candidate /path/to/candidate/YouKnowOversamplingAudit \
  --output /path/to/new-evidence-directory --rate 48000 --factor 1
```

On Linux, `--cpu N` optionally pins both processes to one allowed CPU.

All **20 dynamic audio comparisons, totaling 4,898,400 stereo frames, are
byte-identical**. They cover Original and Direct timing, 44.1/48/96 kHz,
requested quality 1×/2×/4×, 173-frame callbacks and two additional 64-frame
cases. The existing engine policy caps the 96 kHz/4× request at 2×.
The four-second score includes single notes/chords, sustain/release,
chorus changes, cutoff/resonance, Unit Character/Aging, pitch/modulation and
resets. This is equivalence for the tested renders and toolchain, not a
universal bit-equality guarantee for floating-point reassociation. The
[fixture](cpu-2026-10-09/RenderComparison.cpp),
[reproduction commands](cpu-2026-10-09/render-reproduction.txt) and
[hashes and null measurements](cpu-2026-10-09/audio-comparison.json) are retained.

Focused regressions passed for the temperature-dependent amplifier, Original
performance, firmware scheduler/bridge, chorus nonlinear/input/recovery paths
and product profile. Cached versus uncached current differed by at most
`1.59107e-15` relative; the capacitor trajectory differed by at most
`1.11022e-16` charge units. Independent physical-circuit oracles retain their
original limits. The scheduler passed 112,759 assertions, and the bridge
passed 46,288. A separate 60-case scheduler comparison retained every event
and serialized final state; its [generator](cpu-2026-10-09/SchedulerLedger.cpp)
and [commands/results](cpu-2026-10-09/firmware-validation.txt) are retained.
GCC AddressSanitizer/UndefinedBehaviorSanitizer checks passed
for the cache and scheduler; the scheduler test's malloc/free allocation
counter now also supplies matching sized-delete overloads.

See the [validation record](cpu-2026-10-09/validation.json),
[focused test log](cpu-2026-10-09/regression-tests.txt) and
[sanitizer log](cpu-2026-10-09/sanitizer-tests.txt). The full
[engine regression](cpu-2026-10-09/full-engine-test.txt) also passed on the
measured source before integration with newer main in 827.54 seconds.
Windows/macOS binaries,
interactive DAW performance and the Reason Rack Extension package were not
tested by this pass. Changes are in the canonical instrument source; packaged
releases are separate from these native measurements.
