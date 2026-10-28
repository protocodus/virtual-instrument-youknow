# YouKnow: measured realism research, 2026-10-28

The question was which analog behaviours of the Juno-106 the engine still gets
wrong, with each answer grounded in a source and compared with the engine. The
engine already carries every mechanism the service notes, datasheets and ROM
listings support. Earlier rounds checked about sixty documentary proposals
(`Docs/research/juno-106-fidelity-research.md`, 17 September). So this round
compared the engine against **hash-identified recordings of Juno-106 #439522**.
That unit has its original DCOs, sub switch, noise source, jack board and CPUs,
and Borish replacement VCF/VCA cards. Wherever a residual can be pinned on
original circuitry, it is a measurement rather than an estimate.

**Result: one grounded improvement, not five.** A second, the firmware loop
period, looked measured on one take but did not hold up against the ROM's
interrupt timing or the factory-bank recordings (section 2). Ten further
candidates were measured. They either already match the hardware or cannot be
attributed to original circuitry on the only identified unit. They are listed
below with their numbers. Shipping more would mean inventing values, which the
project's A–Z limit rules out.

All renders came from the engine at `ab96143` (`YouKnowRenderCalibrationEvents`,
product, 192 kHz) and were analysed with the same code as the hardware.

## 1. Sub-oscillator edge skew: measured 5.8 µs (engine 0.2 µs)

**Mechanism.** Tr19 (2SC1815, p. 12 legend) switches the sub square. Its base
is driven straight from MC5534A pin 13, with no speed-up network. A saturated
transistor leaves saturation later than it enters it. So one half-cycle of the
square starts late by the storage time and the other is unaffected. A two-level
square whose edges are skewed by Δt carries even harmonics at
H2/H1 = π·Δt·f_sub, flat across the even series. A level asymmetry gives only
DC, and droop gives a falling series, so only a time skew produces this flat
signature. The engine already models exactly this (`enableSubStorageSkew`),
but with a borrowed 2N3904/2N2222A test-condition class value of 0.2 µs.

**Measurements**, all on sub-only tones at 65.04 Hz (C4, 16′):

| Recording | Rate | Method | Result |
| --- | --- | --- | --- |
| `osc_calibrate_new_bip.aif` (isolators; MIDI sha256 `48311413…`) | 192 kHz | even/odd harmonic ratio, 290 harmonics, 1–35 kHz, pure-delay fit | **5.82 µs**, RMS residual 0.036 dB |
| same | 192 kHz | flat even series H2–H40 | −58.45 dBc → 5.87 µs |
| same | 192 kHz | time domain, interpolated edges, 84 periods | high − low half = −11.53 µs → 5.77 µs |
| `osc_test_bip.aif` (SUB sweep; AIFF sha256 `b0309cd3…`) | 96 kHz | flat even series, SUB bytes 51–127 | −58.9 dBc → 5.55 µs |
| `106_calibration_bip.aif` (AIFF sha256 `a9282c4a…`) | 96 kHz | flat even series | −58.7 dBc → 5.68 µs |

- **The method is validated.** The same analysis applied to the engine's own
  render recovers 0.204 µs, and the engine's value is 0.2 µs.
- **The sign agrees.** Hardware and model recordings have the same polarity,
  and in both the half-cycle the engine assigns to D6 conduction is the shorter
  one.
- **The skew does not depend on level.** It is flat over SUB bytes 51–127, so
  it is not a D6 or level effect.
- **It is independently corroborated.** The skew also predicts the sub's odd
  harmonics: a cos(π·f·Δt) loss relative to the saw, which shares the same
  filter/VCA/output chain. The measured sub-minus-saw treble loss follows that
  prediction within 0.1–0.3 dB up to 36 kHz.
- **The replacement cards are excluded as the cause.** Slew asymmetry in the
  downstream chain would also slow the saw's reset, and the saw's harmonics
  bound that reset at ≤ about 3 µs (below).
- **The edge-shape refinement is not adopted.** A delay plus a 1.5 µs
  exponential edge fits slightly better (0.015 dB), but the gain is inside the
  measurement's own scatter, so the pure delay the engine already implements
  is kept.

**Change.** A product flag selects the measured 5.82 µs in place of the class
value. The raw engine keeps 0.2 µs.

**Effect.** Even harmonics of the sub rise by 29 dB:

| Key | Even harmonics, engine (0.2 µs) | Even harmonics, measured (5.82 µs) |
| --- | --- | --- |
| 16′ bottom key (sub 16 Hz) | −100 dBc | −70 dBc |
| C4 at 16′ (sub 65 Hz, the measured case) | −88 dBc | −58 dBc |
| 4′ top key (sub 2.1 kHz) | −58 dBc | −28 dBc |

A render of the 4′ top key reads −57.7 and −28.5 dBc. The level does not
change: RMS-matched previews needed trims under 0.01 dB.

**Status.** The owner chose to ship it on 2026-10-28, as 1.4.1. That was a
choice on the measurement; no listening verdict is recorded.

**Caveat.** Every capture is at one pitch, so the ∝ f scaling follows from the
mechanism rather than being measured directly. The value comes from one unit's
Tr19 and is not a population figure.

## 2. Firmware loop period: not a single figure on this unit (engine stays at 4.2 ms)

**Mechanism.** B-2 runs its control loop continuously; nothing regulates the
pass length. Envelope steps, LFO and delay rates, portamento and the converter
scan all advance once per pass. The engine uses the "4.2 ms" printed on the
p. 8 timing chart. The LFO is the cleanest probe: the accumulator code at
`$074E`–`$07A0` adds the `$0C60` coefficient once per pass and clamps at each
end, so one period is exactly 4·ceil(8192/coefficient) passes. The engine's
`advanceLfo` follows that code branch for branch.

**The take that suggested 4.31 ms.** `pwm_test_bip.aif` (192 kHz; AIFF sha256
`90dc997c…`, MIDI `affcd71c…`) holds MIDI 55 at 16′ with LFO byte 30 driving
PWM, with no other MIDI traffic. Over five segments the LFO measures
2.23081–2.23084 Hz. Entry 30 is 320, so a period is 104 passes, which gives
**4.3102 ms per pass**, 2.6% slower than the chart.

**What the ROM predicts.** The engine's serial-replay path executes the B-2 ROM
with the free-running converter and its interrupt routine. In steady state the
routine runs once per pass (172 + 16 = 188 states, about 1.1% of a pass), and
the pass comes out at:

| Voices held | Pass |
| --- | --- |
| 0 | 4.219 ms |
| 1 | 4.224 ms |
| 3 | 4.233 ms |
| 6 | 4.237 ms |

Without the interrupt the same path gives 4.159–4.188 ms. **Interrupt service
therefore accounts for about 0.05 ms, not the 0.11 ms gap to 4.31 ms.** An
earlier draft of this note attributed the gap to interrupts; that was wrong.
The CPU's 12 MHz ceramic resonator (CSA 12MHz, p. 13) is a ±0.5% part, so a 2%
clock error is not a credible explanation either.

**What the factory bank says.** The factory-bank recordings of the same
unit (`Juno106_a11-b88.zip`, 44.1 kHz) play each preset's own LFO byte. Each
take was compared with an engine render that plays the same detected notes
through the same preset, because every note-on restarts the B-2 loop and costs
part of a pass (about 0.24% on this phrase). The ratio of the two rates is the
implied pass:

| Take | LFO byte (chart rate) | Observable | Implied pass |
| --- | --- | --- | --- |
| `pwm_test` (one held note, no traffic) | 30 (2.289 Hz) | level, long segments | 4.310 ms |
| B76 Rolling Wah | 36 (2.706 Hz) | level, long segments | 4.186 ms |
| B64 Reedy Organ | 35 (2.588 Hz) | fundamental | 4.259 ms |
| A21 Organ I | 54 (3.968 Hz) | H2/H1 | 4.295 ± 0.003 ms |
| A63 Frontier Organ | 79 (5.952 Hz) | H2/H1 | 4.318 ± 0.018 ms |
| A52 Lead II | 24 (1.860 Hz) | H2/H1; fundamental | 4.340 ± 0.016; 4.410 ± 0.038 ms |
| B15 Harpsichord 1 | 23 (1.804 Hz) | fundamental; H2/H1 | 4.25 ± 0.07; 4.42 ± 0.13 ms |
| B37 Sharp Reed | 21 (1.653 Hz) | fundamental; H2/H1 | 4.28 ± 0.13; 4.47 ± 0.16 ms |
| KR-106's four counted-cycle readings | various | cycle count, about 1% | 4.23–4.30 ms |

The ± figures are jackknife errors over the notes of a take. A77 Sustainer was
dropped: its notes retrigger under a long release, so its level modulation sits
19 dB under the note envelope.

**Reading.** The precise takes disagree with each other by far more than their
own errors: B76 implies 4.19 ms and `pwm_test` 4.31 ms, a 3% spread. Within A52
the two observables differ by about 4σ, so the per-take errors understate the
systematic ones. Every take but B76 sits above the chart's 4.2 ms. The mean is
roughly 1–2% above the chart, with about ±1.5% from take to take. The cause of
that spread is not identified. Two causes would fit, and neither is checked:
- the unit's patch memory may not hold the factory bytes;
- an unmodelled firmware path, such as panel or MIDI handling, may cost a
  variable part of each pass.

**Verdict: withdrawn.** A fixed 4.31 ms would fit one take of a quantity that
moves by ±1.5% between takes on the same unit. A mean of the takes would be a
fitted constant, which the A–Z limit rules out. The ROM's own 4.22–4.24 ms is
derivable, but it is within 1% of the chart, and these takes neither confirm
nor reject it. The engine keeps the chart's 4.2 ms. The work-in-progress
implementation was not merged.

## Measured and rejected

| Candidate | Measurement | Verdict |
| --- | --- | --- |
| Longer saw reset (MC5534A slew) | Saw minus sub-derived chain is within 0.1 dB to 30 kHz, so the reset is ≤ about 3 µs | The engine's 2.2 µs is consistent. No change. An early 1.2 dB residual at 20 kHz turned out to be an f0-estimation artefact. |
| Pulse edges | Odd harmonics match the model within 0.05 dB to 12 kHz. The even series is the duty trim (hardware ε 0.25%, model 0.28%). | A unit trim, not a mechanism |
| Source levels | Saw, pulse and sub against the model agree within 0.2 dB of each other | Already calibrated to this unit |
| Noise colour | After chain correction, a −1 dB broad tilt with ±0.5 dB third-octave scatter. A moved C41 pole, an extra pole and a plain tilt fit equally (0.29–0.37 dB). | Not identifiable |
| Mains hum | 120 Hz at about −125 dBFS per Hz bin, about 90 dB under a saw, also present with sources off | Cannot be separated from the recording chain. The engine's ripple model stays. |
| PWM byte-to-duty | Span 1.7% wider, 0.6% kink at byte 0 | VR31 and component trim of this unit |
| Sub low even harmonics | H2/H4 roughly 30 dB above the model | Depends on the C56/D6/WAVE-node impedance and on the replacement card's input (OQ-15). The coupled-mixer code forbids fitting those constants to a clip. Kept as a lead. |
| HPF positions | Noise through each position matches | No change |
| Keyboard sweeps (`c2-c7`, `c4-c6`, `c0-c8*`) | VCF self-oscillation tracking | Replacement cards, not original circuitry |
| Firmware interrupt load | The ROM's converter interrupt runs once per pass, 188 states, about 1.1% | Already in the engine's serial-replay path. It does not explain the 4.31 ms take (section 2). |

## Captures that would unlock more

- **Sub-only notes at several pitches on the 4′ range.** These would confirm
  the ∝ f skew and measure Tr19 across keys.
- **The same held-note LFO take repeated across a session, plus a SysEx
  dump of the unit's patch memory.** The repeats would show whether the
  ±1.5% loop-period spread is take-to-take drift or something the firmware
  does. The dump would show whether the bank presets carry the factory bytes.
- **The same isolator take on an original 80017A card.** This would cover
  resonance law (OQ-09), main-noise level (OQ-16) and voice VCA (OQ-19).
