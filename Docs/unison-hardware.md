# Original six-voice Unison behavior

Evidence audit: 2026-10-08. This contract concerns the original Roland
JUNO-106's normal Solo Unison mode, using the recovered A-5 assigner and B-2
voice firmware. It separates recovered digital behavior from host event
policy and from unmeasured physical timing. It does not certify a physical
hardware A/B comparison.

## Evidence and scope

The manufacturer's [Owner's Manual, p. 22](https://cdn.roland.com/assets/media/pdf/JUNO-106_OM.pdf#page=22)
describes pressing both POLY buttons to make a monophonic stack of six voices.
Page 23 states that enabling portamento applies it to pitch changes.
The manual does not specify detailed Unison key priority or oscillator phase.

The [Service Notes, pp. 7-8](https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=7)
show the combined keyboard/MIDI bitmaps, a shared DCO master oscillator feeding
six programmable timer counters, and separate per-card control holds. Their
4.2 ms converter diagram is a nominal control-cycle description, not a promise
of identical note-event latency for every scan position.

The finer behavior below is derived from instructions in the pinned recovered
[A-5 listing](https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic1.txt)
and [B-2 listing](https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt),
not merely their unofficial comments. These are recovered firmware evidence,
not manufacturer-authored behavioral documentation. The locally inspected
listing SHA-256 values are:

- A-5 `ic1.txt`: `495988b5222ba6174d7de37746b0404d00272e03785bfc868bad35b720c0e156`.
- B-2 `ic29.txt`: `69c7a92de514b8a2021d80cff7314476e1096918c755bb8f22e9aaf3dd419122`.

The upstream firmware regression fixtures separately identify their original
A-5 ROM as `d43cce5578ee2f16b27c8b06bff30743e3e2dffc796d033811e565d5d578c52e`.
`Tools/AuditFirmwareControlTrace.py` pins the B-2 listing above and
an optional independent ROM-byte check. This audit read the listings and
existing fixtures; it did not independently execute a raw ROM dump.

## Key selection and release

A-5 scans combined key changes in descending pitch order at `$01E0-$0206`.
Its per-scan flag at `FFD0` admits one Unison assignment through `$0A42-$0A48`.
`$0B55-$0B6F` sends the chosen pitch to all six cards. A real key-up runs
`$0A69-$0A72`, stops the stack, and clears the cached combined bitmap for a
new scan. Sustain is a separate voice-board command; it does not preserve
released keys in that bitmap.

The following table assumes distinct transitions separated enough for the
assigner to observe each scan. "Attack" means an envelope attack request,
not zeroing its current level.

| Input/state | Selected pitch and stack behavior |
| --- | --- |
| First key down | That pitch on all six cards; attack. |
| One new key while others remain held | Newly pressed pitch, even when lower; attack. |
| Several new keys observed in one scan | Highest newly pressed pitch wins. |
| Winning key up, others held | Stack off commands, then highest remaining key; attack. |
| Nonwinning held key up, others held | The same rescan, including a new attack if pitch stays unchanged. |
| Last key up, pedal up | All six gates clear and envelopes release. |
| Last key up, pedal down | Gates clear; the voice board retains the sustained stack. |
| Already-high combined key bit, or unmatched key-up | No changed bit, hence no new assignment. |

Example: press C4, then G4, then E4. E4 is current. Releasing C4 selects G4
with a fresh attack, even though C4 was not sounding. This is why replacing
the release rule with ordinary last-note-priority legato changes the model.

With the pedal down, a release while other keys remain still causes the
assigner rescan. B-2's hold state can keep the voices running between the off
and on commands; an audible gate dip is therefore not a universal requirement.
Pedal-up releases cards without a held gate, and does not itself request a new
attack for a still-held key. Released, pedal-held keys are not fallback keys.

## Envelope, pitch memory, and oscillator state

B-2 `$010D-$014C` requests attack without clearing envelope charge; the attack
loop `$0563-$0590` adds to the residual level. Its gate/hold paths are
`$009F-$00C2` and `$02F2-$02FF`.

The glide loop `$03D7-$0406` advances six independent 8.8 pitch words, including
idle cards. Note-on changes targets without replacing those words. Cold start
`$028C-$02B5` sets all six targets to 60; the initial zero-coefficient pass
settles their words to `0x3C00` before the coefficient update at `$06F2`.
This applies to Unison too: six settled, unused cards first glide from 60.
Switching from Poly preserves each card's own current history, including an
unfinished glide. Events arriving before startup settles are outside that
settled-state claim.

The shared clock and equal settled timer counts provide no basis for synthetic
per-card pitch detune. Independent counter phase and ordered writes remain;
equal frequency does not imply equal phase. B-2's same-pitch/running-state
branches determine reset requests, consumed at subsequent timer writes.
An ordinary held-note retarget must not reset every counter or erase analogue
capacitor state at the host event. Temporary count differences during glide or
modulation are distinct from a permanent Unison detune spread.

## Natural voice differences

The original has analogue differences between cards, but the circuit does not
support six independently detuned DCO fundamentals. One 8 MHz resonator feeds
the six timer counters. At equal settled counts, their frequencies agree;
their phases need not. The ramp capacitor and charging resistors affect the
waveform after those timer edges, rather than supplying independent pitch
clocks. See the manufacturer's [clock and waveform descriptions, pp. 8-9](https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=8).

The current core already represents the principal distinctions:

| Mechanism | Hardware evidence | Existing core treatment |
| --- | --- | --- |
| DCO waveform and pulse width | Per-card ramp parts; common PWM trim, with each channel checked at 48–52% and 93–97% duty. | `YouKnowDcoComponents.h` uses the marked C54 ±2% and range-resistor ±1% classes for ramp-current differences. Comparator residuals obey the two service windows jointly. These change waveform voltage and duty, without changing timer frequency. |
| VCF frequency and tracking | Individual FREQ/WIDTH trims target 248/992 Hz, both within ±10 cents. | Per-card two-point residuals, four-stage capacitor/offset differences, and service calibration of the complete nonlinear filter. Fixed errors absorbed by a service adjustment are not added again afterward. |
| Resonance and VCA | Separate resonance, gain and offset adjustments: 4.8 Vpp, 6 Vpp, and minimum thumps. | Per-card resonance trim/residual, VCA gain/control offset, and stage nonlinearities; analogue envelope response can differ despite common digital envelope arithmetic. |
| Noise | One shared noise generator feeds the voice paths. | Shared musical noise, per-card path gain, and independent inherent circuit noise. This does not substitute six unrelated sources for the shared generator. |
| Temperature | VCF adjustment requires at least ten minutes of warm-up. | One temperature-dependent DCO master clock, plus per-card filter/headroom temperature effects and slow cutoff variation. Card temperature does not create separate DCO clocks. |

The topology and component markings are in the
[module-board drawings, pp. 12-13](https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=12);
the adjustment targets and acceptance windows are in
[pp. 18-19](https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=18).
Consequently, **self-oscillating filters can differ in pitch**, even though
equal-count DCOs share frequency. Analogue amplitude, harmonic balance and
phase differences can also change the summed Unison sound.

Those service windows and component tolerance classes are bounds, not measured
random distributions. `buildVoiceCards()` chooses fixed deterministic card
draws. Their statistics, correlations and several magnitudes remain modeling
priors: notably post-calibration resonance/VCA/sub/noise residuals, the chosen
filter-stage capacitor spread inside its marked tolerance, and original
80017A offset populations. Unit Character above 1 exaggerates the reference
variation; Aging is an explicit product extension. Neither is a claim about
the distribution of factory or surviving instruments.

Temperature likewise has an evidence limit: `YouKnowDcoTemperature.h` uses a
named Murata resonator proxy, not a measured curve of the installed KMFC1034T1.
The accelerated three-second warm-up, four-degree card gradient, compensated
filter temperature response and slow cutoff wander are not measured original
unit trajectories. MC5534A reset-pulse duration, discharge resistance and
clamp voltage are also uncalibrated; `YouKnowDcoReset.h` provides a comparison
circuit requiring explicit values, rather than establishing an installed
part's behavior. These gaps call for per-card measurements, including warm-up
and post-service checks to identify a particular instrument. Without those
measurements, the conservative estimates below limit existing residuals; they
do not identify a new mechanism that justifies synthetic DCO detune.

`Tests/YouKnowUnisonPhaseTests.cpp` now also exercises the active product
circuit profile at Character/Aging pairs `(0, 0)`, `(1, 0.5)` and `(2, 1)`.
It checks that analogue ramp scales disperse with Character while settled
timer counts remain equal and relative counter phase remains stable. This
guards the distinction in the current core; it does not measure the audible
spread or thermal behavior of a physical JUNO-106.

## Conservative residual estimates without hardware measurements

At the user's request on 2026-10-08, the core uses a deliberately small
serviced-unit variation budget, centralized in
`Source/DSP/YouKnowVoiceResidualEstimates.h`. These are engineering priors,
not measured distributions, confidence intervals, or guaranteed limits on
original instruments. They apply at Unit Character 1; Character above 1 is
an explicit exaggeration and Aging remains a separate product extension.

| Residual | Previous model | Conservative estimate | Basis and limits |
| --- | --- | --- | --- |
| VCF FREQ/WIDTH endpoint draw | ±10 cents | ±2.5 cents | One quarter of the printed ±10-cent acceptance window. The wider window remains the complete-filter acceptance gate; drawing a smaller residual does not prove a measured population spread. |
| Slow VCF cutoff wander | About 2.425 cents RMS | About 0.485 cents RMS | The existing converter-count multiplier changes 40 → 8. The AR(1) process, cadence and correlation are unchanged. This is filter movement, not oscillator detune. |
| Resonance trim residual | ±0.02 normalized CV | ±0.002 normalized CV | Applied before the nonlinear resonance law and clamp. It is not a percentage of loop gain. The existing endpoint clipping remains. |
| VCA input trim | ±3% | ±1% | About ±0.087 dB. The same estimate drives the input gain and coupling calculation; it is separate from raw resistor tolerances. |
| Aggregate VCA control offset | ±0.004 normalized control | ±0.001 normalized control | A small residual after service adjustment, shared by all processing routes. It does not model an additional audio-input thump null. |
| Compensated VCF temperature coefficient | 0.0033/°C | 0.00033/°C | Retain 10% of the reference part's typical coefficient as an explicit 90%-compensation estimate. No original-card temperature curve establishes this number. |

For the retained random process, `x[n] = 0.9992 x[n-1] + 0.004 u[n]`,
with independent uniform `u` on [-1, 1], the stationary standard deviation is
`0.004 / sqrt(3 (1 - 0.9992²)) = 0.0577466`. Multiplying by eight converter
counts and 1200/1143 cents per count yields about 0.485 cents RMS. At 375 Hz
the correlation time is `-1 / (375 log(0.9992)) ≈ 3.332 s`. These are
mathematical properties of the chosen software process, not recorded hardware
statistics. Startup, Character and deterministic card draws can alter the
finite-window observation.

The temperature reference is the replacement
[AS3109 datasheet](https://www.alfatriode.lv/eng/sc/AS3109.pdf), while the
original module's compensation is visible in the
[service schematic, p. 13](https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=13).
Neither establishes a residual population for original 80017A cards. The
existing 15°C common rise, 4°C spatial gradient and previously user-selected
three-second software warm-up remain explicit model choices. Under this
model the gradient-only cold/service difference falls from about 19.73 to
1.98 cents; this is a calculation for the model, not a physical warm-up test.

Raw marked component classes, inherent circuit noise, common master-clock
behavior, six-card phase histories and the calibrated nonlinear filter solve
remain in place. Independent steady DCO detune remains zero. A self-oscillating
VCF can still differ in pitch between cards. Fixed card differences repeat
deterministically instead of being redrawn on every note.

## Host boundaries and verification limits

The original instrument has six physical cards. The Rack port's VOICES=1 last-note
policy, slots 6-15, and changing the stack size are product extensions, not
hardware evidence. The explicit stack-widening feature may bring joining
cards to the existing stack's pitch, including joining physical cards. An
ordinary fixed-size Poly-to-Unison transition instead preserves every
physical card's own glide word, including the settled startup word of a card
that has never played.

Reason supplies timestamped note events. The original assigner observes
keyboard/MIDI bitmaps and transmits ordered serial voice commands. The Rack port's
equal-timestamp normalization and counted overlapping same-pitch notes are
host adaptations. They must not be described as the original bitmap protocol,
nor used to infer exact simultaneous-key ordering on hardware.

Existing upstream checks cover different parts of this contract:

- `Tests/YouKnowUnisonHardwareTests.cpp` executes the current product's native
  A-5/UART/B-2 path as an independent startup witness, then checks direct-core
  startup, reset, partial Poly history, sample-rate/quality choices and the
  retained mono/extra-card/widening extensions in native and embedded builds.
- `Tests/YouKnowFirmwareAssignerSchedulerTests.cpp` checks a descending Unison
  chord against independently derived A-5 byte/timing fixtures.
- `Tests/YouKnowEngineTests.cpp` checks duplicate/unmatched edges and the
  modeled release/rescan attack interval.
- `Tests/YouKnowUnisonPhaseTests.cpp` checks equal settled counter frequencies,
  distinct phases, retained per-card glide history, and reset requests.
- The Rack port adds product and timestamped host-boundary regressions.
  Those are port checks, not physical hardware measurements.

### Measured event-timing difference

The correction is in the canonical DSP: direct multi-voice Unison now retains
each physical card's powered pitch memory by default. The native
`OriginalPerformance` path already retained those words and keeps its existing
firmware timing. The optional physical-startup policy still governs direct
Poly mode; Unison correctness no longer depends on a Rack configuration flag.

The 2026-10-08 software oracle probe compared the direct core with its default
`NormalizedServiceChart` timing against `OriginalPerformance`'s
A-5-to-UART-to-B-2 execution path at 48 kHz, 1x, and six Unison voices.
The baseline probe did not select the Rack port's `MeasuredChartGeometry` profile. After a 4,096-frame warmup, the timing
fixture disabled glide, pressed 60, rendered 4,096 frames, pressed 64, and
rendered another 4,096 frames before releasing the nonwinning 60. HOLD cases
add pedal-down and 4,096 frames before release. Off/on times below are sample
offsets from that release, in physical-card order:

| Path and pedal | Card gate-off samples | Card gate-on samples |
| --- | --- | --- |
| Direct, pedal up | All 0 | All 194 |
| Firmware execution, pedal up | 130, 145, 160, 176, 191, 207 | 287, 318, 348, 379, 410, 440 |
| Direct, HOLD down | All 0 | All 194 |
| Firmware execution, HOLD down | 129, 144, 159, 175, 190, 205 | 286, 317, 347, 378, 409, 439 |

Under HOLD, the voice-run mask stays `0x3f` despite the gate transitions.
The direct path preserves the release/rescan semantics but does not reproduce
the staggered serial command timing in this fixture. These are phase-dependent
software observations, not universal hardware latency constants or recordings
from a physical unit. The startup-glide correction does not resolve this
separate timing difference; fitting one fixed delay to this scenario would
not establish the original scan and serial protocol.

The same baseline probe identified two pitch-memory errors: a fresh
direct Unison assignment to 48 immediately replaced all six startup words
with 48, while the firmware path retained each card's 60 at its note-on.
After two Poly 2 assignments, 48 then 72, entering Unison similarly replaced
the unused direct cards' words with 72; their firmware-path words remained
60 until their own glide updates. These are state comparisons between
software paths, independently supported by the B-2 startup instructions.

Probe source and baseline output were recorded as
`validation/unison-20261008/oracle_probe.cpp` and
`validation/unison-20261008/baseline-oracle.log`.

Passing these software checks establishes their explicit scenarios. Exact
analogue onset, scan-phase-dependent command timing, and behavior across
different physical units or firmware revisions require separate evidence.
No new physical JUNO-106 listening or recorded A/B test is claimed here.
