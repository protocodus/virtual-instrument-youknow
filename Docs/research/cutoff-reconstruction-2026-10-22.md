# Continuous cutoff reconstruction - 2026-10-22

The product now reconstructs the existing VCF cutoff control during RC
settling between converter writes. This reduces an identified numerical
error without changing circuit constants, settled cutoff, resonance, service
trim, stored parameters, gain laws or latency. Expected listening differences
are subtle; this is not a newly calibrated original Juno-106 voice model.

## Circuit and numerical boundary

Roland's [JUNO-106 Service Notes, module schematic p.13](https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=13)
draw the C61 cutoff-control network with its WIDTH trimmer, R110 and positor.
The existing engine represents its nominal settling with a 522 us time
constant. That retained value describes the current model, not a new
installed-unit measurement. With held target `cT`, its continuous control is

```text
c(t) = cT + (c0 - cT) exp(-t/tau)
omega(t) = 2 pi F(c(t)) / internalRate
```

`F` is the existing anti-log/current-limit mapping. The engine already
provided exact exponential control nodes in intervals containing converter
writes. Later settling intervals instead interpolated their mapped frequency
endpoints linearly. The correction targets that latter boundary inside the
existing voice VCF/C59/VCA rate island.

Exact interior mappings would repeatedly evaluate the cutoff chain's exp2
and powers. [CutoffReconstruction](../../Source/DSP/YouKnowCutoffReconstruction.h)
instead uses four already computed endpoints at times `{-2,-1,0,1}`. For
current endpoint `a`, previous `b`, and older `c,d`, let

```text
step = a-b, previous = b-c, older = c-d
A = step/3 + 5 previous/6 - older/6
B = (step-previous)/2
C = (step-2 previous+older)/6
p(t) = b + t (A + t (B + t C)), 0 <= t <= 1
```

This is the causal cubic interpolant in difference form. Its coefficients
are prepared once per base frame; samples need fixed arithmetic only. The
result is bounded to the current interval's two endpoints, and those
endpoints are returned exactly. The existing seven-node island control
transport and its piecewise-linear local reads remain in place.

## Guards and product selection

- Converter-event intervals retain their existing exact trajectories and
  restart cubic history. Held-target changes also restart it.
- Host parameter edits, explicit tone recalls, rate rebuilds and resting
  island resumes discard numerical history while retaining the existing
  physical-state policy. A card-drift jump also restarts the history.
- Reconstruction waits for four endpoints from one segment. Equal endpoints,
  direction reversals, cap departures and an adjacent endpoint ratio above
  two retain the existing linear fallback. The one-octave ratio is a numerical
  admission guard, not a new hardware parameter.
- `enableVcfSettlingReconstruction` defaults false in raw diagnostic engines;
  `ProductHardwareRealismProfile` enables it. There is no new public control
  or serialized patch property, lookahead, allocation or delay.

## Independent checks

[YouKnowCutoffReconstructionTests](../../Tests/YouKnowCutoffReconstructionTests.cpp)
independently reduces the existing anti-log/current-limit equation and
evaluates exact exponential control. Its 150-case sweep combines five initial
and five target coordinates with 8/44.1/48/88.2/96/192 kHz rates. Every interval
is checked at 25 positions, including bounds and exact endpoints.

At standard rates, summed relative control-error power falls to **33.6923%**
of linear interpolation. Including the extreme 8 kHz cases gives **98.3724%**;
those unresolved transitions deliberately retain linear fallback. In this
tested matrix the largest relative error remains **0.589748** for both paths.
These aggregate results do not assert improvement at every possible control
position.

The audio oracle separately integrates the nonlinear four-stage OTA equations
with exact exponential control and a 220 Hz sine input. Independent RK4
references use 64 and 128 subdivisions per base frame and converge below the
candidate error. The production cascade is exercised through both 2x and 4x
island grids, including their seven-node piecewise-linear control reads.
The table reports waveform RMS error relative to the 128-step oracle, in dBr;
more negative is closer. Initial/target cutoff coordinates are 5000/12000
for Up and the reverse for Down. RES is feedback gain, not panel travel.

| Base rate | RES | Direction | Linear 2x | Cubic 2x | Linear 4x | Cubic 4x |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| 44.1 kHz | 0 | Up | -94.15 | -120.99 | -94.15 | -120.99 |
| 44.1 kHz | 0 | Down | -65.39 | -87.64 | -65.39 | -87.65 |
| 44.1 kHz | 3.5 | Up | -82.58 | -106.10 | -82.60 | -109.26 |
| 44.1 kHz | 3.5 | Down | -74.35 | -80.69 | -74.34 | -80.65 |
| 48 kHz | 0 | Up | -95.62 | -122.56 | -95.62 | -122.56 |
| 48 kHz | 0 | Down | -66.86 | -89.81 | -66.86 | -89.81 |
| 48 kHz | 3.5 | Up | -84.06 | -108.38 | -84.07 | -110.89 |
| 48 kHz | 3.5 | Down | -75.83 | -83.23 | -75.82 | -83.20 |
| 96 kHz | 0 | Up | -107.66 | -135.02 | -107.66 | -135.03 |
| 96 kHz | 0 | Down | -78.91 | -105.24 | -78.91 | -105.24 |
| 96 kHz | 3.5 | Up | -96.11 | -123.27 | -96.11 | -123.51 |
| 96 kHz | 3.5 | Down | -87.90 | -104.41 | -87.90 | -104.39 |

All 24 audio comparisons improve. A separate actual-product wiring probe
checks stored curved midpoints against the selected reconstruction and
requires more than 100 exercised contexts. Target/event resets and exact settled values are
also checked. Existing VCF-integrator and voice-island contracts passed in
the focused run. The same test executable's `--benchmark` option supplies
alternating mechanism-off/on runs at 48 kHz, 1x, Normal, six notes and
64-frame blocks; final integrated performance and output-level evidence are
reported with the release validation.

## Limits

These checks qualify interpolation of the existing equations. They do not
identify original-card transfer parameters, establish a perceptual threshold,
or prove an audible preference. The cubic remains an approximation, has
explicit fallback regions, and is subsequently read through the existing
seven-node interpolation boundary. This pass does not add a physical pole,
change steady loudness, refit hardware constants or introduce independent
oscillator drift. Unnormalized musical comparisons and final native-host
performance remain separate evidence.
