# Continuous chorus mute loading — 22 October 2026

The chorus now reconstructs the changing output load within each numerical
interval. This reduces a switching error in the existing circuit; it does not
change the engaged chorus's delay, depth, rate, distortion, noise or gain.

## Evidence and correction

Roland's [JUNO-106 Service Notes, jack board p. 15](https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=15)
draw C25/C28 as 1 µF coupling capacitors with 22 kΩ bleed paths and the
39 kΩ summer inputs connected through Tr11/Tr12. The existing finite mute
driver supplies their continuously changing channel conductance. Previously
the output circuit used the conductance at the end of a sample as a constant
load throughout that sample. This right-endpoint quadrature has first-order
time error during switching.

`Chorus::finiteWetTransition` now uses the mean of the two physical endpoint
conductances for state transport. Endpoint voltages and the implicit nonlinear
follower currents still use the actual final load. Separating those quantities
avoids delaying the physical output divider or using midpoint junction
voltages at the endpoint. Existing prepared circuit maps remain unchanged;
their endpoint current maps are recombined only while the load moves.

No circuit value, gate trajectory or wet attenuation was fitted. The engine
comparison flag defaults to the former behavior; the active product profile
selects the correction. Steady intervals use the same cached maps as before.
One additional double stores the cache's endpoint-load key.

## Qualification

[`YouKnowChorusContinuousMuteTests.cpp`](../../Tests/YouKnowChorusContinuousMuteTests.cpp)
contains an independent analytic current oracle. With the upstream nodes
held at unity, C25 follows
`x' = (1-x) × (1/22000 + g(t)/39000) / 1e-6`.
For a 2 ms linear conductance ramp, its exact solution follows from the
integral of `g(t)`. The maximum normalized capacitor-voltage errors are:

| Numerical rate | Previous endpoint load | Continuous load |
| --- | ---: | ---: |
| 8 kHz | 0.00142512 | 7.15145e-8 |
| 48 kHz | 0.000237670 | 8.13389e-9 |
| 192 kHz | 5.94234e-5 | 2.07312e-9 |

The remaining scalar error is bounded by the separately qualified 16-cell
conductance-grid interpolation. This is a model-equation comparison, not
an original-instrument voltage measurement.

The full finite-follower network is also exercised on the physical mute
trajectory against a 768 kHz reference, retaining the same exact held-output
forcing at every compared rate:

| Numerical rate | Previous maximum wet-output error | Continuous load |
| --- | ---: | ---: |
| 192 kHz | 1.27056e-6 | 9.35979e-8 |
| 384 kHz | 6.12577e-7 | 2.29338e-8 |

These amplitudes are model units (2.6 V/unit), before the digital output
boundary. At 192 kHz the maximum C25 state error falls from 3.66542e-7 to
1.65352e-8 model units. The increasing improvement at the finer rate supports
the expected time-order correction.

Actual nonlinear chorus processing covers On/Off/On and interrupted commands,
with both random noise and finite clock-mute circuitry enabled. It produces
a nonzero correction (maximum 1.19321e-5 model units in that fixture), no
nonlinear solver recovery, and identical physical control-capacitor and
noise-generator chronology. Cold, reset and settled Off/I/II audio remain
bit-identical to the former implementation. This is a subtle switching
correction; no large audible difference or listening preference is claimed.

## Mode-II timing investigation: retained current calibration

A separate bounded investigation tested an independent Mode-II timing fit
before considering a retune. The original chorus board of Juno-106 #439522
is documented by its [owner's recording issue](https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/16)
and [component provenance clarification](https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/41#issuecomment-4189199115).
Its voice cards are Borish replacements. B3x/B4x audio from the
[April capture archive](https://www.lewisfrancis.com/nwio/Juno-10-test-audio-96K.zip)
and the [matching MIDI](https://kayrock.org/kr106/106_calibration.zip) passed
the existing analyzer's pinned AIFF, PCM and per-bank MIDI hashes. No capture
gain, EQ, or patch tone byte was changed.

The existing channel-ratio method was screened with an independently rendered
actual clocked Chorus fixture. It recovered the known centre/depth/rate with
errors of approximately 4.13 µs / 1.78 µs / 5.04e-6 Hz; its held-out residual
was 0.00350. Complete C2/C4 note windows were withheld from timing fits.

The real patches did not justify new constants. B4x p88's fitted rate was
0.83655 Hz versus the current 0.83761 Hz, with its centre within 11 µs;
its held-out residual only changed from 0.29303 to 0.28326. Resonance and
vibrato contaminate the upper notes and the free robust fit worsens ordinary
training RMS. B3x p81 has long release and overlapping pitch content; its
fit also leaves large residuals and gain nuisances near their search bounds.
B4x p94 reaches a search boundary and worsens held-out error to 0.42792.
Depth estimates differ across these confounded patches. These residuals are
far above synthetic recovery, so no Mode-II retune was applied. The current
rate and centre are corroborated to the limited precision of this screen;
independently calibrated Mode-II delay endpoints remain an open measurement.
