# Four shipped assumptions against the #439522 captures

2026-11-16. The known-gaps research
([combined report](known-gaps-research-2026-11-16/youknow-known-gaps-2026-11-16-combined.md),
section 4, "Session 0") named four assumptions that recordings already in hand
can test without new hardware time: the rail-ripple and converter-hold priors
in the cutoff, the noise source's distribution, idle hum by chorus state (the
one disputed claim), and the DCO clock's drift. This pass measures each one on
the capture and on the engine's render of the same MIDI by one protocol, so
any estimator bias is shared. It changes no DSP.

| Assumption (engine 1.4.9) | Capture says | Verdict |
| --- | --- | --- |
| Rail ripple: 0.003 cents peak cutoff FM at 120 Hz (`enableRailRipple`) | No 120 Hz cutoff FM above a 0.10-cent floor | Neither: the floor is 30 times the prior |
| Converter holds: typical-leakage droop, 16.8 µV per 4.2 ms pass, no charge injection (`enableConverterHoldDroop`, OQ-07) | Sawtooth-like cutoff FM at the pass rate, 0.19–0.20 cents peak; the render shows none above 0.005 cents | **Contradicts** the size of the per-pass disturbance |
| Noise source: Gaussian | Skewness within 0.02, tails within 0.15 σ of Gaussian, like the render | **Confirms** |
| Idle hum (KR-106's "+18 dB above dry at 120 Hz" against the owner's "I don't hear it") | A 120 Hz line 69–73 dB under a C4 note with the chorus on, entering at the delay lines; none with it off; the engine has none | **Confirms** the hum rises with the chorus; **contradicts** the engine, which has no chorus-path ripple |
| DCO clock: settled CSA8.00MTZ temperature shape after a 3 s software warm-up | No common drift over 37.6 minutes: 0.012 ± 0.013 cents per minute | Consistent, but not a test of the proxy |

## Captures and renders

All five takes are the same serviced unit, #439522: original DCOs, chorus
board and converters, Borish replacement VCF/VCA cards
([recordings register](hardware-recordings.md)). Its recording chain has its
own floor and hum, and nothing separates the chain from the instrument except
what a contrast between the unit's own states does. None of these takes was
made for these questions, so a capture that shows nothing bounds an effect only
to its own floor.

| Take | Hash (SHA-256) | Format | Used for |
| --- | --- | --- | --- |
| `vcf_fullsweep_bip.aif` with `vcf_fullsweep.mid` | `27ab9ed0…67956be`, MIDI `2d5eb741…1fa6a2f` | 192 kHz, 582 s | sidebands |
| `bank_{A1x…B8x}_bip.aif` in `Juno-10-test-audio-96K.zip`, MIDI in `106_calibration.zip` | per bank in `Tools/AnalyzeChorusBankEvidence.py` `BANKS`; zip `fe3fb80d…8e5af0` | 96 kHz, ≈104 s each | hum, pitch |
| `osc_test_bip.aif` with `osc_test.mid` | `b0309cd3…e39093f`, MIDI `051ea38e…0ad2d43` | 96 kHz, 96.27 s | noise |
| `osc_calibrate_new_bip.aif` with `osc_calibrate_new.mid` | `35323722…525aabf` | 192 kHz, 118 s | noise |

The renders come from `YouKnowRenderCalibrationEvents` built at `6b92c51`
(YouKnow 1.4.9, unmodified), Release, GCC 13.3.0 `-O3 -DNDEBUG`, with the
exact MIDI each take played (`Tools/AnalyzeHardwareCalibration.py`
`export_events`):

- fullsweep: `1 product 1 nominal fixed-service serviced439522 192000 4 rotary6`,
  Unit Character on, so the rail ripple and the hold droop are both active;
- banks for hum: `1 product` (48 kHz, 4x, the product's cold start and clock proxy);
- banks for pitch: `1 product 1 nominal fixed-service serviced439522 48000 1 normal 8000000 fixed-clock settled`,
  the fixed nominal 8 MHz clock, so hardware minus render is the unit's own offset;
- osc_test and isolators: `1 product`.

Analysis: [`Tools/AnalyzeExistingCaptures.py`](../../Tools/AnalyzeExistingCaptures.py),
whose head states each protocol; Python 3.13, NumPy 2.5.3, SciPy 1.18.1,
soundfile 0.14.0. Its outputs are in
[`existing-capture-analysis-2026-11-16/`](existing-capture-analysis-2026-11-16/).

## 1. Cutoff modulation: rail ripple and the converter pass

The fullsweep holds RES 127 with every source off, so each steady window is
the self-oscillating filter alone and its frequency is the cutoff. The tool
demodulates each window's tone into its frequency (cents) and envelope, and
averages the modulation spectra over the windows of each tone band. Below
2 kHz the demodulation band holds the switched-off 16′ C4 DCO and sub's
feedthrough harmonics, whose beats read as lines in capture and render alike,
so only tones from 2 kHz up are used. The estimator recovers synthetic FM of
0.003–0.3 cents at 120–232 Hz to within 2 %.

| Line | Capture, 2–6 kHz tones (87 windows) | Capture, above 6 kHz (30) | Render, 2–6 kHz (90) | Render, above 6 kHz (30) |
| --- | --- | --- | --- | --- |
| 120 Hz FM | not detected, floor 0.10 c | not detected, floor 0.25 c | not detected, floor 0.007 c | not detected, floor 0.007 c |
| pass FM | **0.19 c at 231.9 Hz** (17 dB over floor) | **0.20 c at 231.4 Hz** (8.5 dB) | not detected, floor 0.005 c | not detected, floor 0.005 c |
| twice the pass, FM | **0.13 c at 464 Hz** | **0.12 c at 464 Hz** | not detected, floor 0.003 c | not detected, floor 0.004 c |
| pass AM | not detected, floor 0.006 % | not detected, floor 0.02 % | not detected | not detected |

**Rail ripple: not tested.** The shipped 0.003 cents is under both floors,
the capture's (0.10 cents, set by the self-oscillation's own frequency noise)
and the render's (0.007 cents under Unit Character). The capture bounds any
120 Hz cutoff FM on this unit to under about 0.1 cents peak, so a ripple up to
30 times the prior would pass unseen. The prior's sideband at 1 kHz is
−103 dBc.

**Converter pass: the prior's size is contradicted.** The capture carries a
frequency modulation of the cutoff at 231.4–231.9 Hz whose size in cents is
the same for 3.4 and 7.4 kHz tones, so it moves the cutoff by a fixed fraction,
with no measurable amplitude modulation beside it. Its second harmonic stands
at 0.64 of the fundamental, close to a sawtooth's 0.5: a ramp between writes
and a step at each one, about 0.6 cents peak to peak. The engine's render shows
nothing at the pass rate above 0.005 cents, so this unit's per-pass disturbance
is about 40 times anything the engine produces there. The shipped droop is typical leakage
(16.8 µV per pass on the VCF hold, under a hundredth of the 2.45 mV LSB), with
charge injection unmodelled; leakage well above typical or a charge-injection
step per write would each explain the size. The sidebands are small, −62 dBc
at 3.4 kHz and −55 dBc at 7.4 kHz, under a held note's partials.

The line also times the pass directly: 231.4–231.9 Hz is a 4.312–4.322 ms
pass, 2.7–2.9 % longer than the chart's 4.2 ms and just past the 4.231–4.304 ms
that KR-106's LFO rates imply for this unit (README OQ-08). This sweep runs
one card at a time, and the pass is branch-dependent, so the figure belongs to
this state.

## 2. Noise distribution

NOISE-only windows, band-passed alike (200 Hz–12 kHz) with the capture
resampled to the render's 48 kHz; values in standard deviations, with a
20-block standard error of 0.01–0.05.

| Window | Skewness, capture / render | Excess kurtosis, capture / render | 0.1 % and 99.9 % quantiles, capture | render |
| --- | --- | --- | --- | --- |
| osc_test NOISE 102 | −0.016 / −0.004 | −0.065 / −0.039 | −3.08, 2.99 | −3.06, 3.04 |
| osc_test NOISE 114 | −0.001 / −0.006 | −0.109 / −0.090 | −2.99, 2.99 | −3.04, 2.99 |
| osc_test NOISE 127 | 0.003 / 0.003 | −0.122 / −0.003 | −2.92, 2.98 | −3.07, 3.06 |
| isolators | 0.017 / 0.002 | −0.035 / −0.064 | −2.96, 3.14 | −3.09, 2.98 |

A Gaussian source gives 0, 0 and ∓3.09. **Confirmed**: the unit's noise has
no heavy tail and no burst (popcorn) component, which would give a positive
kurtosis, and the render matches it. One difference stands at three standard
errors: the capture's kurtosis falls as NOISE rises (−0.065, −0.109, −0.122),
while the render's does not, which would fit a soft compression after the
noise source at its highest level. It is too small to act on from one take.

## 3. Idle hum by chorus state

Every idle window of the sixteen bank takes with RELEASE ≤ 40 and NOISE 0:
each mains line's level from a least-squares sine, against the same fit 5 and
7 Hz either side, pooled per chorus state, with left/right coherence. Levels
are dB under each patch's own C4 note, which cancels the recording gain.

| State (windows) | 120 Hz over its floor, L / R | 120 Hz re C4, L / R | L/R coherence, phase | Render |
| --- | --- | --- | --- | --- |
| Off (37) | 0.3 / −1.7 dB: no line | floor −105 / −106 dB | — | no line in any state; floor −110 dB |
| I (19) | **17.4 / 16.8 dB** | **−69.8 / −69.0 dB** | 0.29, −10° | no line |
| II (5) | **16.5 / 9.0 dB** | **−73.0 / −68.6 dB** | 0.55, −30° | no line |

With chorus I the 240 Hz line also stands 4.8 / 9.6 dB over its floor; no
other line passes 6 dB in either chorus state. With the chorus off, 180 and
300 Hz reach 5–6 dB in both channels, just under the threshold.

**The disputed claim, settled on this unit.** The hum rises with the chorus:
the 120 Hz line is at least 35 dB over the chorus-off floor, and it is the
instrument's, not the recording chain's, since the chain is the same in every
state. The owner not hearing it is consistent with its level, about 69 dB
under a C4 note. KR-106's "+18 dB above dry" is not directly comparable: here
the dry state shows no line at all to be above. The coherence places the entry.
A line entering before or after the chorus reaches both channels alike
(coherence near 1 at 0°). The chorus is two MN3009 lines whose delays the one
triangle sweeps in anti-phase, one to each channel, so a line entering both
delay lines reaches the channels with a relative phase that wanders with the
triangle, and its pooled coherence falls. Both chorus states show that: low
coherence with a wandering phase. The line enters at the delay lines (their
bias, clock or supply), not the dry path.

**The engine has no such line.** Its rail ripple reaches only the VCF cutoff
(section 1), so with the chorus on the render stays at its noise floor. A
chorus-path ripple candidate would need a coupling mechanism the schematic
supports; this one take gives its level, not its path. It is recorded as a
Known gap.

## 4. DCO pitch against elapsed time

Each chorus-off, DCO-LFO-0, NOISE-0 patch's C1–C5 notes, the fundamental by
interpolated periodogram, against the fixed-clock render. Elapsed time is each
AIFF's archive time minus its duration: the recorder's file times, a proxy for
when each take ended, not a log.

- 150 notes, 143 within 15 cents of the median (the seven others are notes
  where the periodogram took the resonant peak, five of them A8x's).
- Offset: −9.16 cents against the nominal 8 MHz clock, MASTER TUNE included.
- Drift: 0.012 ± 0.013 cents per minute over 37.6 minutes; per-note scatter
  1.9 cents rms. Apart from A8x, whose notes all took the resonant peak,
  every bank's median lies within 0.7 cents of the offset.

**Consistent, but not a test of the proxy.** The capture shows a settled
clock. The shipped warm-up is a 3 s software startup with a 15 °C rise, about
one cent through the CSA8.00MTZ shape; the unit was recorded 80 minutes
before the first bank (106_calibration_bip.aif's file time), so it was most
likely warm; and the
drift bound, about ±1 cent over the session at two standard errors, is as
large as the proxy's whole excursion. A cold-start session with a logged
clock is in the [capture protocol](../plans/hardware-capture-protocol-2026-11-16.md).
