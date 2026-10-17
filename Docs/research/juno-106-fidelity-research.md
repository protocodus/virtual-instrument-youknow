# Juno-106 fidelity research

Moved from the [README](../../README.md) on 2026-10-17; its [How it works](../../README.md#how-it-works) section describes the
shipping model.

The 15 October audit compared the oscillator, unison and chorus paths against
Roland's [service schematics](https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf),
the [recovered voice firmware](https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt),
[Raffel and Smith's BBD paper](https://www.dafx.de/paper-archive/2010/DAFx10/RaffelSmith_DAFx10_P42.pdf),
[Holters and Parker's variable-clock model](https://www.dafx.de/paper-archive/2018/papers/DAFx2018_paper_12.pdf)
and [Välimäki, Pekonen and Nam's oscillator reconstruction paper](https://mac.kaist.ac.kr/pubs/ValimakiPeknenNam-jasa2012.pdf).
Shared DCO timing, free-running phase, waveform coupling, firmware Unison
assignment, bandlimited edges and the main BBD support circuits were already
implemented. Seven remaining corrections comprise five performance/controller
bugs and two subtle timing errors, listed in the latest release entry. They
are not seven newly calibrated analog effects. The separate MIDI All Sound
Off/Reset All Controllers behavior follows the
[MIDI Association's message summary](https://midi.org/summary-of-midi-1-0-messages).

`Tools/RenderFidelityQuickWins.cpp` and `Tools/MakeFidelityQuickWins.py` render
the active product path from frozen baseline and candidate builds, retain raw
float audio and exact event manifests, and produce whole-file stereo-RMS-matched
`A.wav`/`B.wav` pairs with a separate `key.md`. A is always the unchanged
baseline. Per-mechanism builds isolate individual fixes; a grouped final-engine
comparison is labelled separately. Listening files stay outside the repository,
and no listening preference or original-unit calibration is inferred from them.

The second 15 October pass targets audible numerical errors after those
correctness fixes. The coupled SUB switch was clipping its reconstructed
edge, the default-rate chorus support differed appreciably from its
high-rate circuit solution, and the VCF's low-grid pole limit rolled off
open-filter sounds prematurely. These are three supported corrections to
existing circuits, demonstrated with seven auditions. B15's maximum-cutoff,
maximum-resonance tone also exposed an artificial resonance around 19.9 kHz
under the old pole limit. A direct 192 kHz engine reference resonates around
45.6 kHz; independently downsampling its output to 48 kHz matches the corrected
default path's short-note RMS within 0.00035 dB. This qualifies the numerical
implementation against a finer-grid model, rather than a hardware capture.
The Unison audit found the shared master clock, independent
free-running phases, six-card assignment and firmware glide already present;
adding independent oscillator detune would contradict that evidence.
`Tools/RenderAudibleRealism.cpp` supplies fresh musical scores, with isolated
mechanism builds and a separate combined Unison comparison. The listening
package keeps small external-audio pages plus portable two-clip pages, raw
and RMS-matched audio, exact events and build hashes. Earlier listening
evidence remains frozen.

The listening HTML embeds the original WAV bytes and evidence documents so
sandbox previews can play them without access to sibling files. Native controls
load audio on demand; selecting the other member of a playing A/B pair preserves
its position and pauses the previous clip. Raw pairs retain their original gain.
`Tools/MakeListeningPagePortable.py` can convert existing local listening pages
without changing their WAVs, identity keys or manifests.

An earlier service and state audit corrects two further discrepancies.
Roland's consecutive [p. 19 adjustments 5 and 6](https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=19)
require 4.8 Vp-p at TP19 and 6 Vp-p at TP8 under the same bank/key setting.
The former voice-amplifier path applied its physically derived distortion
shape at unity gain and produced about 4.69076 Vp-p. A fixed +2.1379 dB
service gain restores the 6 Vp-p target. The node-level regression injects
before C59, whereas TP19 is after it, so the test allows the capacitor’s tiny
248 Hz loss and discrete peak-sampling error. A reciprocal trim follows
all modeled analog stages at the digital output, preserving ordinary
loudness and leaving the stronger HPF/chorus drive intact. Physical noise
voltages stay fixed, so that final trim also reduces their digital level.

The HOLD correction uses B-2's latched running-voice state when deciding a
new note's oscillator reset and LFO delay. A pedal release followed by a new
note before the next snapshot no longer falsely observes an idle instrument.
The regression fails 30 checks on the prior build and passes after correction;
actual keyboard/serial timing remains a separate capture question.

The product now selects `enableChorusClockMuteCircuit`: C15 loads the
existing C16/C13 mute driver, conducting base junctions load their capacitors,
and stopped BBD clocks retain bucket charge and noise-generator state.
The independent nodal reference agrees within 0.35 microvolts. Its nominal
stop/restart predictions, 287.7/12.3 ms, use the existing 0.6 V junction prior
and are **not hardware measurements**. The bounded product-cost audit found
additional chorus-off CPU use: median idle CPU shares moved from 3.9% to 4.5%
at 1× and 15.3% to 16.9% at 4× on an M1 Max, with substantial background-load
scatter. This is not a real-time deadline guarantee. Steady engaged audio
remains bit-identical; the raw engine retains the old comparison default.
Derivations and capture requirements live beside the circuit code. The
completed before/after listening evidence remains frozen under `Docs/audio`.

Earlier corrections addressed filter-control arithmetic, the chorus input's
mutual loading and the output jack's treble attenuation. Five further
[corrections](#further-circuit-and-firmware-corrections) address switching
current, firmware state order and distributed filter noise. The accompanying
[instability research](#voice-instability-and-pitch-drift) distinguishes common
DCO-clock drift from per-card analog variation.
These corrections address identifiable implementation errors without fitting a new
instrument character to an unrelated recording. Their expected listening
impact is modest; the measurements below are local numerical and circuit
comparisons, not a new blind listening result or an overall hardware-fidelity
score.

The useful form of novelty here is applying established methods to previously
unqualified boundaries of this particular model. A more complex oscillator,
larger oversampling multiplier or neural replacement is not automatically
more accurate. The existing engine already represents integer note timers,
nonlinear OTA feedback, scanned control holds, physical BBD sampling and
coupled output reconstruction. The remaining question is which discrepancy
can be independently identified and reduced.

## Implemented findings

| Improvement | Evidence and independent check | Measured change | What a listener can expect |
| --- | --- | --- | --- |
| Filter ENV/KEY precision | Pinned B-2 instructions; exhaustive byte-product oracles and actual converter-write fixtures | 10,485,760 operand/depth combinations now agree; an exposed combined case corrects two DAC codes, equivalent to 8.40 cents of cutoff | Small changes in resonant attacks, decays and portamento stepping; no general brightness boost |
| Coupled chorus input | Service-note topology; two-node complex-admittance reference separate from the production state matrix | Former maximum error 0.18856 dB; new 56-case maximum 0.003474 dB / 0.007079° against the declared reference at each rate | Slightly less low/mid wet contribution and correspondingly less BBD drive; same modulation timing and routing |
| Output-jack discretization | Existing component-derived RC corner; independent impulse-response comparison with the analog transfer | Worst tested audio-band magnitude error 1.17177 → 0.09743 dB across twelve host rates and five volume positions | A little less excess top-end on bright material, with closer tonal agreement between sample rates |

**Filter control.** The [B-2 listing at `$05C5–$05D2` and
`$05E1–$0637`](https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L923-L992)
establishes the ENV partial products and KEY shifts. The implementation now
uses `floor(envelope14 * (2 * ENVbyte) / 256)` and forms KEY from the two
separately shifted 8.8 pitch terms before signed multiplication. Both retain
their low-order carries until the final summed cutoff word reaches the DAC.
This is ROM-resolved for the named revision, not a statement about every
possible firmware revision.

The former path reused the VCA's already-truncated envelope fraction and
continuous key-follow scaling. Its individual ENV and KEY terms could differ
by up to 3.916 and 2.604 internal counts in the audited domains. Those errors
can combine across a final four-count DAC boundary. For panel base 8192,
inverted full ENV, envelope RAM 895 and pitch word 15374, the former result
was 7312; the recovered arithmetic yields 7304. Octave scaling and the full
ENV endpoint are unchanged. Velocity remains the documented software
extension, applied after the firmware ENV product.

**Chorus input.** On [Roland's jack-board drawing, p. 15](https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=15),
the coupling node is connected to both its bias return and the downstream
RC branch. There is no buffer separating those two sections. Solving their
currents together adds the loading term missing from the earlier cascade.
With coupling capacitor `Cc`, bias resistor `Rb`, series resistor `Rs` and
shunt capacitor `Cp`, the derived transfer is
`s*Rb*Cc / (1 + s*(Rb*Cc + Rs*Cp + Rb*Cp) + s²*Rb*Rs*Cc*Cp)`.

The continuous six-state support model now includes that connection. The
lower-rate implementation solves the same two capacitor currents together
while keeping its previously qualified prewarping policy; its state carries
use double precision to avoid persistent numerical DC remnants. The
independent reference uses node admittances rather than reproducing the
production matrix. The resulting nominal wet correction is −0.18856 dB at
400 Hz, −0.18590 dB at 1 kHz and −0.06601 dB at 10 kHz. These values describe
the signal entering the BBD; final stereo-output changes depend on the dry/wet
sum and nonlinear drive. They do not calibrate absolute chorus gain, bias
trimmers, clock endpoints or hiss.

**Output jack.** A corner above Nyquist can still attenuate audible frequencies.
The former exponential recursion reproduced a sampled decay but largely
missed that attenuation at normal host rates. The replacement applies the
low-pass limit of [Vicanek's matched one-pole design, equations 3–12](https://vicanek.de/articles/ShelvingFits.pdf).
It matches DC, low-frequency magnitude curvature and one upper-band point;
the matching point is capped at 20 kHz to keep high-rate approximation
accuracy within the audited audio band. This cap is numerical policy.

At 48 kHz and full volume, 20 kHz now receives the modelled −0.75 dB
attenuation instead of approximately −0.04 dB. Half volume receives −1.34 dB
instead of approximately −0.21 dB because the pot changes source resistance.
The filter follows the glided volume position, retains signal histories,
adds no sample delay and preserves unity DC while coefficients move. The
test compares actual impulse output with a separately reduced analog RC
network, checks host-rate wiring at 1×/2×/4×, and exercises reset and
block-boundary invariance during volume automation. This is a magnitude
improvement; the remaining phase mismatch stated above is substantial near
Nyquist. External output loading remains outside the nominal model.

## Control DAC circuitry

The DAC is a control-voltage converter, with no audio sample stream passing
through it. Three corrections follow the [detailed module drawing, p. 13](https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=13)
and the pinned B-2 firmware. They improve the nominal circuit model; they do
not establish a fully measured converter or a universal original-unit tone.

| Correction | Grounding and implementation | Listener impact |
| --- | --- | --- |
| ENV/GATE span | B-2 presents `0x3fff >> 2 = 4095` at an envelope peak or GATE, while stored byte 127 produces code 4064. The analog VCA/C58 model now uses the corresponding physical span, preserving its existing absolute knee and normalized peak | The isolated span fix changes normalized gain by +0.133 dB at 2% control, +0.016 dB at 10%, and zero at the peak; principally a quiet-tail correction |
| Common VCA buffer | IC28a's R130 4.99 kΩ, R131 10 kΩ and R129 39 kΩ give `Vout = 15 × 10/39 − Vdac × 10/4.99`; this replaces the block diagram's rounded +4…−6 V labels | Raw common gain increases by 0.326–0.368 dB at 25 °C across stored levels. Its location before chorus and the final summer also changes their drive slightly |
| Loaded positive buffer | IC27b's R135/R136 10 kΩ pair also sees R127 470 kΩ and VR34's finite source resistance. Solving the loaded +0.26 V zero-code trim yields gain 2.021092556, rather than 2 | Small changes to low noise/resonance controls and envelope tails; service-derived voice-VCA headroom is recalculated consistently |

The [firmware's ENV/GATE selection](https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L909-L920)
and [DAC writer](https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L1292-L1302)
settle the digital endpoints. `ControlDac` keeps the ladder's 4096-step
convention separate from those endpoints. With the loaded positive buffer,
stored maximum spans 10.026514 V and ENV/GATE maximum 10.102996 V above
the existing +0.26 V standoff. The absolute VCA knee remains its earlier
0.148828 V prior; it is not refitted. Peak-normalized gain therefore stays
one. The two positive-path corrections together change gain by +0.314 dB
at 2% control, +0.037 dB at 10%, +0.004 dB at 50%, and zero at full control.
Those figures exclude the separate common-VCA level increase.

The [service adjustment, pp. 18–19](https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=18)
uses full **sustain**, so its signal-stage calculation retains code 4064,
not the envelope peak's 4095. The corrected span gives a 302.1 µA nominal
control tail, trim drive 0.21453375 and 11.19 V signal headroom while retaining
the same 6 Vp-p service output target. The common VCA's nominal 25 °C range
is now −15.994…+5.077 dB using [NEC's −5.9 mV/dB typical law](https://bitsavers.trailing-edge.com/components/nec/_dataBooks/1983_NEC_Integrated_Circuits_for_Consumer_Use.pdf#page=262).
Existing selected VCF calibration, PWM endpoints, negative-branch SUB/DCO
calibrations and factory tone bytes retain their coordinates.

**Verification.** `YouKnow.ControlDacBuffer` independently solves the three
positive-buffer nodes and trim, checking all 4096 codes against the reduced
production network (worst double-precision discrepancy 3.55e−15 V). It also
solves the common-VCA nodes across all stored levels and intermediate states.
The joined voltage-domain C58 tests reject both earlier span implementations.
These numerical discrepancies qualify the implementation of the equations;
they are not claims of physical accuracy at that precision.

`Tools/RenderControlDacProbe.cpp` renders three diagnostic passages through
the product at 96 kHz/maximum quality, Exact tanh and Merson half steps.
Build its identical source against each revision's matching headers/library.
`Tools/AnalyzeControlDacComparison.py` checks metadata and formats, preserves
raw levels, and exports 24-bit A/B pairs matched on whole-file stereo RMS
with identical endpoint guards and shared peak headroom. Its `--self-test`
checks gain-only and transient differences, encoding and rejected inputs.
The five unchanged factory scores A11/A48/A53/B11/A68 show raw RMS increases
of 0.348–0.367 dB; level-matched residuals are −62…−70 dBc and whole-file
magnitude spectral-centroid shifts remain below 0.5 Hz. This does **not**
resolve a substantial tonal-balance difference with an unknown recording.
The maintained numbered demos, five musical showcases and full composition
have been rerendered with the corrected product path.

**Limits.** The existing three filter carry offsets remain an effective
calibration. Their [primary source](https://github.com/kayrockscreenprinting/ultramaster_kr106/blob/bc15caee5843ab238a25d0969e68d57db2b1615f/Source/DSP/J106DACHzTable.h#L1-L22)
contains 93 measured voice-four codes and infers the steps from local
log-frequency extrapolation. It is not a direct 4096-code TP4 voltage sweep;
spreading the offsets to every destination would overstate the evidence.
Full-scale voltage percentages also do not bound audibility near a nonlinear
control threshold. `Tools/AuditControlDacTiming.py` verifies pinned firmware
sequencing and conditional settling arithmetic. It corrects the former
under-10 µs acquisition claim: at the datasheet's 15 V reference coordinate,
280 Ω and 10 nF need 25.23 µs for an ideal full-scale half-LSB settling, and
25 mA is an absolute maximum rather than guaranteed charging current. NOISE's
no-interrupt window is only 35.25/39.75 µs. Installed supplies, source drive,
exact latch edges, droop and charge injection remain measurement questions;
no arbitrary switching glitch or random ladder error was introduced.

## Further circuit and firmware corrections

Five further changes target discrepancies that can be resolved without fitting
unknown hardware characteristics. Their checks compare against circuit
relations or the recovered B-2 control flow. They establish implementation
accuracy within the stated model; they do not constitute a new end-to-end
original-instrument accuracy score.

| Correction | Independent evidence and check | Audible or behavioral consequence |
| --- | --- | --- |
| Finite chorus discharge | R46 330 Ω; two-node current integration agrees within 0.35 µV at 8–768 kHz, including interrupted commands | Nominal settled wet-return opening moves from 113.2 to 120.6 ms; both capacitor charges survive switching |
| Envelope store order | B-2 `$0503–$05C5`; distinct per-card states and interrupted PWM/VCA/DCO windows | Abandoning a DCO scan cannot accelerate attack/release; completed later envelope stores survive a restart |
| DCO range current | Selected 399/200/100 kΩ charging resistance; independent charge integrals and same-cycle comparator decisions for every directed range change at six rates | The first partial cycle follows the new charging current, retaining its charge; 16′ also restores the 400/399 nominal ramp-height ratio |
| Four-stage resistor noise | Original-module 68 kΩ/560 Ω readings and Johnson–Nyquist density; independent per-stage transfer functions and integrated output PSD | The filter receives its own noise at every stage, with the correct downstream filtering and self-oscillation excitation |
| Portamento store order | B-2 `$03E0–$0410`; six distinct glide states, interrupted passes and callback partitions | A partially completed DCO train cannot give only its first few cards an extra glide step |

**Switching current and stored charge.** The two analog changes repair a
specific boundary condition. C16 cannot discharge instantly through a finite
resistor, and selecting a different C54 charging resistor changes current
without instantaneously changing capacitor voltage. The chorus uses prepared
state transitions outside the callback; the DCO retains its event-based slope
correction, independent clock reload, reset policy and supply limit. The
0.6 V transistor threshold and unknown analog-switch injection remain model
assumptions, not newly measured properties.

**Firmware state.** The prior envelope/pitch helper advanced two unrelated
kinds of RAM at each DCO transaction. The corrected scheduler respects their
separate positions in the recovered loop. Envelopes advance after the DCO
train: ENV0 before PWM, subsequent envelopes before the preceding card’s VCA
write. Portamento advances all six words before SUB and the DCO train. Shared
per-pass guards make a fractional look-ahead and its later converter poll one
operation. Completed RAM updates remain completed when a logical Voice On/Off
restarts the pass; unvisited work is discarded. These are ordinal guarantees
on the established chart grid, not a complete CPU or serial-wire emulator.
The collapsed phase-zero diagnostic is a separate synthetic test profile.

**Distributed noise.** The original hybrid’s resistor readings corroborate
the existing 68 kΩ/560 Ω stage network and also the VCA’s 47 kΩ load and
4.7 kΩ/560 Ω input. The VCA’s parallel capacitor remains unidentified; no
new capacitor value is inferred from a replacement design. For each filter
stage, the resistor-derived node density at 25 °C is approximately
3.02 nV/√Hz, or 370 nV/√Hz referred through the stage divider. Four separate
sources enter their corresponding differential inputs, outside the external
signal’s resonance compensation.

Their amplitude follows `sqrt(Tcard / 298.15 K)` using the same local
temperature as the voice's other analog paths. Warming from 25 to 40 °C
therefore raises resistor-noise power by 5.03%, or 0.213 dB; nominal
Unit Character 0 remains at 25 °C. This extends the
[Johnson–Nyquist temperature law](https://www.ti.com/document-viewer/lit/html/SBOA345/GUID-F87CE11A-8998-4FB4-BEA6-8D520E81351E)
to the existing resistor sources. The temperature trajectory remains the
documented software character model, not an installed-unit measurement.

An independent small-signal reference gives each source the transfer
`H^(4-j)/(1+k*H^4)`, where `H=1/(1+s/ωc)` and `j=0..3`. Independent powers
add. At zero resonance this yields four times the former low-frequency PSD,
fifteen times its PSD at cutoff, and a total noise-power integral of
`density² * 35πfc/32`. The stochastic test at 192 kHz/1 kHz agrees with that
integral within 0.04 dB. This closes the omitted resistor sources; it does
not calibrate the complete installed-card floor, semiconductor noise or the
shared NOISE slider. Scalar and vector kernels use the same reconstructed
sources, and idle cards preserve the random-stream histories. An isolated
five-pair 48 kHz/4× benchmark with 64-frame blocks and chorus Off measures
+13.36% median CPU for six voices (pair range 12.49–15.60%) and +4.68% idle.
This is a material runtime cost, accepted for the four physically distinct
noise paths; it is not a hardware accuracy or listening score.

The source basis is [Roland’s service drawing](https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf),
the pinned [B-2 listing](https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt),
the [original-module measurements](https://www.sounddoctorin.com/synthtec/roland/juno106.htm),
and [Johnson’s thermal-noise paper](https://doi.org/10.1103/PhysRev.32.97).

## Oscillator and envelope circuit completion

The six O/E findings are implemented at distinct circuit or firmware
boundaries. O1 integrates the held DCO current without anticipating future
CV writes. O2 preserves joint capacitor/resistor identity across RANGE.
E1 keeps B-2's independent attack, decay, gate and run latches, including
retrigger from the current level and the pass-held gate decision.

O3 adds an explicitly configured exponential reset: gate duration,
discharge resistance and clamp voltage are mandatory inputs. It preserves
residual C54 voltage, derives PWM crossings from that voltage and carries the
reset through waveform means and antialiasing. E3 adds six independent 10 nF
hold capacitors upstream of C58/Tr20, with finite acquisition and explicitly
supplied bias, off leakage and turn-off charge. C58 consumes the changing
upstream voltage throughout the interval. Neither mechanism introduces an
unmeasured installed-part default: unconfigured engines retain the established
linear reset and ideal acquisition.

E2's `FirmwareControlNoInterrupt` comparison executes the full B-2
`02EC–07B5` control pass, including its helpers, using semantic instruction
descriptors and the engine's existing generated coefficient tables. RAM
stores, envelope/glide updates and DAC/mux events reach the actual engine
in causal order. Branches determine pass length. This qualifies nominal
instruction execution under explicit pass-start input snapshots; physical
ADC interrupts, serial-wire delivery and installed pin-edge delays remain
outside its claim. The product retains the chart geometry previously chosen
by ear. On September 14 the owner heard the unison A/B as close and preferred
whichever matches hardware; A was retained because B has no demonstrated
overall hardware advantage with its interrupt work omitted. B has stronger
nominal instruction timing, while A's chart proportions remain unmeasured;
this decision does not settle their complete hardware match. See the
[timing decision](../decisions.md#2026-09-14--retain-timing-a-pending-a-hardware-comparison).
The full trace remains a calibration/comparison path, not a new saved tone
parameter.

`FirmwareAdcTrace::run` now adds a standalone, serial-quiet comparison with
continuous ADC conversion, persistent phase/results, alternating banks and
the actual interrupt handler. It takes eight explicit raw input bytes; their
hysteresis and endpoint mapping run through B-2 instead of being overwritten
by host-derived values. The original-family NEC timing basis is detailed under
OQ-08 below. All timestamps use CPU states, independent of the audio sample rate.
The shipping chart profile stays unchanged; the serial replay below integrates
this peripheral with audio using explicit inputs.

The new test exhausts all 768 whole-state ADC phases, both starting banks,
three declared ANM-write scenarios and two peripheral-access conventions over
four consecutive passes: 36,864 complete passes. Independent instruction
ledgers check the five ISR RAM stores, register/skip restoration and 188-state
entry-plus-handler duration. Some tested passes contain zero or two services
because interrupts cross pass boundaries; the late NOISE converter aperture
can grow while the protected DCO writes stay intact. A fixed delay appended
to every pass would miss both effects. These constant-input scenarios do not
identify analog acquisition or every internal bus race. The resumable receiver
below supplies partial-pass state and the audio replay consumes its actual
PIT/DAC events with an explicit static raw-input contract.

`FirmwareSerialTrace::advanceTo` carries the CPU, stack, alternate register
banks, ADC phase and receive state through arbitrary time boundaries. It
executes note and HOLD commands from explicit byte-ready timestamps, including
the firmware's stack abandonment on note restarts. Completed pitch-counter
stores remain observable even when the following DAC calculation is abandoned.
CPU stores use instruction completion; DAC/mux events retain the comparison's
declared instruction-start convention. Equal-time ordering is explicit and is
not a measured bus subcycle. The richer table API also executes normal
`8E–A2` parameter payloads and the unarmed `A3` return path. Armed `A3` stops
before diagnostic side effects; cold boot and test mode remain outside its
scope. The original control-table overload retains its note/ADC-only contract.
Unread-byte collisions report unsupported overrun rather than choosing an
undocumented overwrite rule. This remains a comparison API; the product
timing selection is unchanged.

The engine's `FirmwareSerialReplay` comparison connects that receiver to the
existing audio signal path. Configure a static patch before `prepare`, eight
raw ADC bytes and an immutable caller-owned byte-ready schedule. Reset starts
the schedule again; audio quality changes retain CPU time and pending work.
The default raw snapshot is neutral tune, no portamento or bend, and an inactive
LFO trigger. It seeds raw history and processed values once, then lets the
firmware update them. Downstream ADC-derived caches start neutral; custom raw
inputs take effect as later firmware passes compute those caches. This is a
declared main-loop initial state, not a cold boot. It does not invert arbitrary
host control values: the
firmware's conditioning has no exact raw input for processed even values
236–254. Keep host panel controls static during replay; scheduled parameter
bytes change CPU RAM and then the actual bus outputs. Host note, sustain,
pitch-bend and modulation handlers are bypassed. The receiver accepts explicit
byte-ready schedules; the separate assigner models below supply upstream traces.

The separate `FirmwareAssignerTrace` executes A-5's original command/value
sender, queue producer and transmit interrupt, with `FirmwareUartTrace`
following the physical serial output. The queue has 48 slots and 47 usable
entries; publishing a byte and transmitting it are separate operations.
[NEC's original manual](https://drive.google.com/file/d/0B44NKm9yPA1bNDFXZnFrdG1PdDA/view),
printed pp. 7-3–7-4, establishes separate transmit-buffer and shift registers,
buffer overwrite behavior and completion of the active frame when transmission
is disabled. Its empty-buffer interrupt can occur while a frame is still on
the wire. The model retains those two stages and routes actual pin levels
through the PC2-controlled gates in
[Roland's drawing](https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=11).

The [A-5 route guard](https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic1.txt)
takes 1,579 nominal CPU states, 394.75 µs, from its initial `MVI` through
completion of the PC write. Normal transmit dispatch reaches it with an empty
holding buffer and at most one active 1,280-state frame. Receive service takes
priority and can postpone the route change; it does not bypass this guard.
The bounded executor retains instruction, stack, queue and peripheral state
across time slices. It stops when it returns to unimplemented foreground code,
or reaches the full ADC or receive handler, rather than inserting idle time.
Initial transmit-clock phase and equal-time event ordering remain explicit
numerical scenarios. This provides sender and wire traces, not a complete
keyboard/MIDI scheduler or an identified stop-bit-to-RXB latch timestamp.

For isolated calls that empty the queue before returning, an end-to-end
regression drains the remaining UART frames, decodes the module wire's actual
logic edges and feeds the recovered bytes into the existing audio replay. It
preserves the observed spacing within each burst and supplies the first receiver-ready time
explicitly. The test covers both initial routes and all 128 configured clock
phases, then checks note and cutoff commands through audio at 1× and 4× quality.

`FirmwareAssignerScheduler` extends this with the original normal A-5 main
loop, incoming MIDI parser, voice allocator, panel/keyboard scanning and full
ADC service. These paths execute continuously with the sender and UART; the
model supplies no fixed foreground period or interrupt padding. Physical
contacts, raw ADC codes, initial peripheral phases and any accessed patch RAM
are explicit inputs. Cold boot, cassette operations and diagnostic entry remain
outside this warm-state comparison.

The [original A-5 code](https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic1.txt)
first changes a MIDI note bitmap, then merges it with keyboard contacts during
a descending foreground scan. Repeated note-ons therefore collapse into one
bit; on/off pairs can cancel before the scan sees them. New note bits observed
together are considered in descending pitch order, which can change their voice-card
assignment. Roland's [MIDI chart](https://cdn.roland.com/assets/media/pdf/JUNO-106_OM.pdf#page=35)
also warns that notes shorter than 5 ms may not be received. That statement is
not implemented as a fixed rejection threshold: the comparison executes the
actual scan and its dependence on phase and load. The normal plug-in retains
its counted host-note adapter.

An independent interpreter of the original ROM bytes checks the scheduler's
RAM, registers, flags and timed stores. A continuous audio regression sends
MIDI notes and sustain, then moves a physical cutoff ADC input. The original
foreground generates module commands and outgoing MIDI traffic; independent
pin decoding separates the routes and supplies the module bytes to B-2 and
the actual audio engine. It passes at 44.1, 48 and 96 kHz, at 1× and 4× quality,
with exact block-size invariance. This bridge explicitly uses frame end as its
receiver-ready convention; the installed receive-latch subcycle is still
unidentified. UART/ADC phase alternatives are comparison scenarios, not measured
hardware bounds.

`FirmwareAssignerAudioBridge` also runs this path incrementally: each audio
block can supply its incoming byte-ready events and held physical inputs,
advance the original assigner, decode only the module pin, and append the
result to the engine's bounded streaming receiver queue before rendering.
Partial instructions, serial frames and undelivered bytes survive block
boundaries and full output buffers. An inclusive completed time cannot receive
late events; reset clears the stream, while a quality change preserves CPU
time and queued bytes. The audio callback allocates no memory in the tested
paths. The session TIMING menu now exposes this bridge as Original, with
an explicit warmed A-5 panel state and nominal DIN frame-start host timestamps.
Notes, HOLD, bend, modulation and seven-bit tone edits pass through A-5 and
its module UART to B-2. Original uses six cards and hardware velocity behavior;
Direct retains the existing host adapter. New instances default to Original;
explicit Direct states and legacy states without a timing field keep Direct.
Changing timing clears held notes and pending work. The bounded live driver
has allocation, overflow, recovery and block-invariance regressions.

The live regression covers note/HOLD traffic, a physical cutoff change and
six-card unison. Independently executed original ROM bytes fix the expected
module-byte times; streaming and immutable receiver input then give identical
CPU, PIT, capacitor and audio states at all three sample rates and both quality
settings above. One-byte output buffers force actual drain/retry transitions,
including a same-time frame-end/CPU-port-write test. This makes the normal
firmware path usable without precomputing a song-length receiver schedule.
This is a CPU-intensive selectable path: an earlier local M1 Max unison audit
at 48 kHz/4× used about 19% more CPU than receiver-only replay, with the full
path taking a median 1.12 CPU-seconds per second of audio. Those short-scene
timings are conditional measurements, not a universal real-time cost bound.
Direct remains the economical alternative; Original exposes the supported transport
and scan behavior without asserting installed-unit receive-latch timing.

Physical continuous-panel edits now update the held ADC inputs instead of
creating incoming DIN traffic. The original foreground scan applies its raw
deadband and value conditioning; received MIDI and its reflected UI state do
not move those inputs. Full-tone MIDI frames retain their channel, raw payload
and position in the event stream, including same-value dumps. At 31.25 kbaud,
24 bytes take 7.68 ms; expanding all 18 tone bytes into seven-byte parameter
frames took 40.32 ms. A-5 stores values as they arrive, so preserving framing
also preserves the duration of intermediate combinations during patch changes.
The plug-in retains its existing omni-channel input policy by selecting the
ROM receiver channel at each serialized message boundary. Packet bytes remain
intact; this is a host-adapter policy, not a claim about the unit's channel setup.
The on-screen keyboard uses the 61 physical key contacts, with UI-channel holds
combined before releasing a contact. A-5 keeps those contacts independent of
its incoming MIDI key bitmap. Programmatic or host-transposed pitches outside
the physical span retain the legacy DIN fallback rather than adding contacts.

The voice VCA's Tr20 now follows the existing thermal state using a named
2SA1015 estimate of −1.6 mV/°C for reference VBE. Its current and the load on
C58 follow the same junction law; capacitor charge survives temperature changes.
Service gain is fixed at the declared settled temperature, and the existing
BA662 temperature factor is applied once. This chiefly changes quiet sustain
and release tails. The coefficient, TP7 reference stability and accelerated
chassis warm-up remain estimates. The resonance BA662 separately uses a signed
250 µV offset span under Unit Character, replacing the older voiced 1.5 mV
span. Two original BA662A measurements (30/250 µV) motivate this estimate but
do not establish a population bound; IR3109 stage offsets are unchanged.

Actual external control/low-byte/high-byte stores reach each pitch counter at
fractional audio positions, independently of the later pitch-DAC update. The
bus adapter decodes physical PA mux/channel bits, so abandoned scans do not
misroute a subsequent DAC write by ordinal. Existing capacitor trajectories
consume the passive control holds, and configured envelope acquisitions follow
the actual inhibit/enable events. All six cards retain the shared PIT clock.
The nominal instruction graph bounds consecutive DAC enables at at least
216 CPU states, including destructive serial restarts whose separate bound is
395 states. The 32 kHz internal floor therefore admits at most one enable per
audio interval; multiple pitch-counter stores are retained separately. CPU
stores use instruction completion, seven states later than the old comparison's
STAX instruction-start anchors. Neither convention identifies installed pin
propagation delays.

Live RANGE commands drive PF at instruction completion. All six oscillator
walks share the same fractional change and prescaler phase, preserve the ramp
capacitor's voltage, and change charging resistance without waiting for another
oscillator reset. Saw, HPF and chorus commands first update RAM, then reach
IC40 at its later external store. Their existing audio solvers consume that
latch on the internal sample grid, leaving up to one interval of switching-time
uncertainty. Pulse switching follows the PWM converter voltage. These different
paths must not all move at serial-payload arrival.

The [A-5 forwarding code](https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic1.txt)
complements the two stored switch bytes before B-2 receives them. Together with
[Roland's IC40 drawing](https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=13),
this establishes PF `C0/40/00` as 4′/8′/16′ and the active-low waveform and
chorus-enable outputs. Chorus mode selection is independent of mute: mode II
can be selected while off without resetting phase or stored BBD charge. An
initial host Off state seeds mode I because the host patch has no separate
muted I/II selector; subsequent board commands carry that distinction.

An independent original-byte interpreter checks all 128 payload values for
all 22 parameter dispatch entries with two initial flag/latch states. The
candidate also passes continuation, bounded-buffer and mixed note/parameter/
ADC schedules against that reference. All 520 generated attack, LFO-rate,
decay/release, fade and DCO-LFO-depth entries match the archived B-2 tables;
no coefficient retuning follows from this audit. A separate 1,152-case switch
route check covers the normal one-hot RANGE combinations from stored patch
bytes through A-5 and B-2. These establish nominal digital behavior, not an
installed unit's analog switching transient or serial delivery latency.

Its regression tests cover 7,680 twelve-byte unison bursts, exact note/HOLD
handler stores, interrupt priority, one-state continuation and bounded-buffer
draining. The burst sweep varies ADC and arrival phases together across 1,280
offsets and six declared peripheral conventions; it is not every independent
phase combination. A separate 768-scene quiet comparison preserves all
140,638 control events and 192,000 nonstack RAM checks from the established
ADC API. The tests also retain a completed PIT count whose later DAC update
is cancelled by a note restart, which a forecast of whole passes would miss.

`YouKnowFirmwareSerialAudioTests` additionally checks the actual audio bridge
at 8–192 kHz, including the 32 kHz internal floor and 4× oversampling. It
compares CPU state and event ledgers with the qualified standalone executor,
independently decodes raw PIT/DAC addresses, and verifies that all six counters
run and their ramp capacitors move. An exact RC calculation checks the optional
upstream envelope holds through fractional inhibit/enable edges. The abandoned
DAC test runs beyond the old commit time, with leakage disabled only in that
write-ownership fixture. Block partitioning and reset replay are bit-identical;
quality transitions retain the CPU timeline and pending instructions.

`YouKnowFirmwareLiveParameterAudioTests` follows scheduled parameter bytes
through the actual audio path. A separate 8 MHz counter model checks shared
RANGE phase, and a piecewise charging-current integral checks C54 across the
fractional switch time. It also checks delayed IC40 updates, independent chorus
selection while muted, changed audio after control events, and repeatable block
and reset playback. Entering serial replay through a later reset installs its
lookup tables and 32 kHz minimum grid; returning to chart timing restores the
host patch's HPF coefficients, including at an unchanged sample rate.

`YouKnowRenderOscillatorEnvelopeCircuits` makes each candidate audible through
the product signal path. It writes a raw float pair, whole-file RMS-matched
`A.wav`/`B.wav`, and a separate `key.md` with inputs and trims. A is shipping;
B changes one mechanism. Use a new output directory for each run:

```sh
build-fidelity-research/YouKnowRenderOscillatorEnvelopeCircuits timing out/timing-pair
build-fidelity-research/YouKnowRenderOscillatorEnvelopeCircuits timing out/unison-timing-pair --unison
# Supply explicit coordinates; these variable names carry no suggested values.
build-fidelity-research/YouKnowRenderOscillatorEnvelopeCircuits reset out/reset-pair "$RESET_OHMS" "$GATE_US" "$CLAMP_VOLTS"
build-fidelity-research/YouKnowRenderOscillatorEnvelopeCircuits hold out/hold-pair "$HOLD_OHMS" "$BIAS_NA" "$LEAKAGE_NA" "$CHARGE_PC"
build-fidelity-research/YouKnowRenderOscillatorEnvelopeCircuits coupling out/coupling-pair "$TOTAL_OHMS" --complex
```

For reset and coupling comparisons, append `--complex` to use longer material
with exposed oscillator sections or moving PWM and overlapping voices. The
candidate coordinates stay explicit; the flag changes the shared score only.
The original short score remains available without the flag. Each key records
the score and level trims, so repeat auditions can preserve the first set.

The basis is [Roland's DCO and module drawings](https://www.vintagesynthparts.com/wp-content/uploads/2017/03/JUNO-106_SERVICE_NOTES.pdf),
the pinned [B-2 program](https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt),
and the [HD14051B component specification](https://akizukidenshi.com/goodsaffix/hd14051b_e.pdf).
Switch resistance rows are conditional component comparisons; installed
resistance, leakage and charge injection still require calibration inputs.

## Voice instability and pitch drift

A faithful 106 should distinguish **common DCO tuning movement**, **relative
phase**, and **analog voice variation**. Those produce different audible
results and require different state variables. Six unrelated random pitch
walks would not reproduce the documented DCO architecture.

Roland identifies the master as an 8 MHz **ceramic resonator**, part
`12389728 / KMFC1034T1`, and the processors’ separate 12 MHz resonators as
`12389719 / KMFC1007T31` ([parts list, p. 4](https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=4)).
The earlier claim that its crystal tolerance must be smaller than pitch
quantization was unsupported. For a fixed count, `fvoice = fmaster/(range*N)`;
common reference movement therefore shifts all six voices together. Equal
counts retain equal steady frequencies, while staggered count writes during
modulation can temporarily differ and change phase relationships.

How much actual DCO drift occurs remains a measurement gap: no quantitative
cold-to-warm trace or temperature/aging specification for the installed
KMFC1034T1 has been established. For scale only, 500 ppm of common clock
movement is 0.8654 cents, while one count from 4545 to 4546 changes pitch by
0.3809 cents. The firmware’s 1/256-semitone control grid is 0.390625 cents.
These are arithmetic examples, not observed Juno drift amplitudes.

The [Murata CSA/CSB catalogue](https://docs.rs-online.com/9b38/0900766b8053c862.pdf)
lists ±0.5% initial, full-range temperature and ten-year aging limits for its
CSA□MTZ family. A ±0.5% frequency change corresponds to about ±8.65 cents,
but the installed part’s identity, load and operating temperature are not
established by that catalogue. Its full-range bounds cannot be repurposed
as typical warm-up or random-wander settings. Murata also warns that the
oscillator’s actual IC circuit affects frequency
([application manual, printed p. 14](https://mm.digikey.com/Volume0/opasdata/d220001/medias/docus/2531/Ceramic%20Resonator%20(CERALOCK)%20Application%20Manual.pdf#page=16)).

[`AnalyzeResonatorProxy.py`](../../Tools/AnalyzeResonatorProxy.py) models the older
[Murata Erie CSA8.00MT equivalent circuit](https://dn710301.ca.archive.org/0/items/Murata-ErieCeramicResonatorsForTimingControlOCR/Murata-ErieCeramicResonatorsForTimingControlOCR.pdf#page=12)
as an explicit substitute for electrical loading experiments. Run
`python3 Tools/AnalyzeResonatorProxy.py --caps-pf 30 30` for the catalogue's
capacitor pair; repeat `--caps-pf` with another pair to compare shifts in ppm
and cents. The calculation retains motional resistance and reports passive
parallel phase resonance. The complete oscillator also depends on its
inverter and actual loading: the catalogue's MT test uses CD4069UBE at 12 V,
and its 30 pF values do not establish Juno C109's value. This offline tool
does not supply a warm-up curve or change the instrument's tuning parameters.

The engine's comparison-only `configureDcoMasterClockHz()` accepts a settled
reference frequency before `prepare()`. It changes the shared prescaler and
all six DCO timers, while keeping firmware count/CV words and the separate
CPU cadence unchanged. Charging current and finite reset duration retain
their existing model values: a faster reference shortens the rise, lowers the
saw peak and changes PWM at the physical comparator. The idle voice model
also retains the resulting saw/pulse DC charge in C56/C50. Its periodic mean
includes the existing finite linear discharge and +15 V supply limit; those
remain compatibility models, not new measurements of the custom chip.
The accepted 7.2–8.8 MHz reference range is an engineering test domain, not a
hardware tolerance. Raw-engine comparisons default to a fixed reference.
The product now connects its shared temperature to the published
[Murata CSA8.00MTZ temperature curve](https://www.homepages.ed.ac.uk/jwp/radio/projects/p16e9.pdf#page=11),
an explicitly approximate substitute for the undocumented KMFC1034T1 curve.
The reference is 8 MHz at 25°C. Normalizing the digitized percentage curve
as `(1 + s(T)/100)/(1 + s(25)/100)` predicts +585.7 ppm, or +1.014 cents,
at the model's 40°C asymptote with Unit Character 1. This is a component-proxy
prediction, not an original-Juno measurement. The resonator reads the common
chassis temperature; per-card heat offsets continue to affect analog voice
circuits, never six separate DCO clocks.

The existing 15°C rise now uses the user-requested **3-second time constant**
(63.2% after 3 seconds, 95% after 9 seconds). This acceleration is a software
startup choice, not a claim about the hardware's heating rate. Once the
temperature settles, clock frequency settles too; no random pitch walk is
added. `configureThermalStart(true)` starts the entire thermal model at its
settled state immediately. Temperature and frequency survive host stops.
`configureDcoTemperatureProxy(enabled, referenceCelsius)` permits an explicit
reference temperature when a measured clock value is supplied. The calibration
renderer retains fixed-clock comparisons and accepts final optional arguments
`[master-Hz] [fixed-clock|csa8mtz-25c] [cold|settled]`.

The [Kiwi-106 hardware-upgrade manual, p. 19](https://www.kiwitechnics.com/downloads/Kiwi-106/KiwiTechnics_Kiwi106_Manual_v206.pdf#page=19)
describes bass cancellation on some unison notes from the six waveforms’
relative phases, despite their stable common tuning. Preserving timer state,
count-load timing and free-running phase models that behavior. Randomizing
phase on every sample or adding arbitrary voice detune models something else.

The unison observer now measures the rendered timer edges as well as each
card's live count. Equal settled counts preserve relative counter phase;
shared clock drift moves every frequency together. During LFO modulation or
glide, sequential writes and each timer's half-cycle transfer can temporarily
leave cards on different counts. That produces deterministic phase motion:
the test's selected LFO and glide scenarios span about 7.8° and 37.1° between
CH1 and CH6. These are software qualification scenarios, not measured Juno
statistics. Entering Unison from Poly with portamento can also retain distinct
per-card glide histories until they converge.

For timing evidence, select `FirmwareControlNoInterrupt` and
`selectVoiceBoardCommandReplay(true)` before prepare/reset. Then call
`serviceVoiceBoardNoteOn(card, boardPitchByte)` or
`serviceVoiceBoardNoteOff(card)` at the supplied service boundaries, splitting
`process()` at those boundaries. Cards are 0–5, board pitch bytes are 0–127;
the caller owns timestamp-to-sample quantization and must record it. Normal
keyboard assignment is bypassed in this explicit replay mode. The caller
supplies when each completed logical command is serviced and its addressed
card and board pitch byte. Six commands produce six ordered restarts while
PIT, C54 and analog state continue between them. No command directly draws a
new phase or adds detune. These boundaries are not wire-byte or RXB-ready
timestamps: the API retains the model's documented protected-write policy,
without asserting UART buffering or installed interrupt latency. The product's
Direct adapter retains this logical-event convention; Original instead runs
the bounded assigner/UART bridge described above.

VCF tuning can vary independently of DCO pitch. Roland’s ±10-cent service
acceptance after warm-up is a calibration criterion, not a distribution of
thermal drift. A [single repair account](https://sequence15.blogspot.com/2008/01/on-workbench-juno-106-part-2.html)
reports several filters about a quarter-tone flat after years without
calibration; it does not measure oscillator drift or establish population
statistics. The [AS3109 datasheet’s](https://www.alfatriode.lv/eng/sc/AS3109.pdf)
0.33%/°C is a typical frequency-control coefficient for a replacement IC,
not an original 80017A residual-drift bound after the installed compensation.
The product keeps 10% of it, 0.033%/°C, as an explicit estimate of what the
board's R111 positor compensation leaves; no original-card curve measures it.

Temperature also affects voice timbre. The filter's nonlinear OTA headroom
follows absolute temperature, and the voice VCA now applies `T_ref/T` to its
signal after C59 and the fixed input trim. Its reference is each card's settled
service temperature, including the existing spatial gradient. This preserves
the calibrated warm response while giving a cold card more small-signal gain
and distortion. The pair's saturation ceiling stays fixed at unchanged current
([general OTA derivation](https://www.ti.com/lit/ds/symlink/lm13700.pdf#page=9)).
At Character 1 with the gradient disabled, 25→40°C reduces small-signal gain
by about 0.426 dB; this is a conditional circuit prediction, not a Juno capture.
Character zero and settled starts retain the previous signal law exactly.
Setting `enableVoiceVcaTemperature=false` retains the former response;
disabling saturation alone keeps the thermal gain in the linear path.

This is a partial model: Tr20 current still follows the scanned envelope CV
through the existing control law, but its additional temperature dependence
and the movement of its low-current knee are omitted. No transistor temperature
coefficient has been fitted. Local ramp-current, capacitor and comparator
changes can also alter saw amplitude and pulse width without independent DCO
detune. Their installed temperature coefficients remain unknown. A static
temperature gradient gives
static differences; ongoing thermal movement requires changing conditions.

The current Unit Character model’s `0.9992` recurrence at 375 updates/s,
with uniform `±0.004` excitation, corresponds to a 3.332 s correlation time
and, at eight converter counts per state unit, approximately 0.485 cents
stationary cutoff standard deviation at 100%.
Its 15 °C chassis rise, spatial gradient and Aging profile remain voiced priors;
the 3-second thermal time constant is a user-selected software acceleration.
The sample-count scheduler approximates 375 Hz when the
processing rate is not an integer multiple. These numbers describe this
implementation; they are not measurements of a typical Juno-106. The seeded
VCF wander is independent of temperature and belongs to cutoff, not to a
per-card DCO frequency offset. It must not be counted again as an additional
temperature-derived residual.

The DCO timers continue clocking behind closed voice VCAs, so the number of
held notes is not a measurement of digital-rail load. Supply-dependent master
clock pulling is plausible, but its coefficient needs an installed supply-to-
frequency measurement; neither a gate's propagation delay nor a resonator's
temperature curve supplies it. No active-voice-count pitch-sag term is added.

The processed TUNE byte contributes one 1/256-semitone unit per step
(0.390625 cents in the pitch word, before integer PIT quantization). This does
not imply raw ADC code hunting. Roland's
[control drawing](https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=8)
assigns TUNE to AN0, portamento to AN1 and bender CV to AN7; IC30–32 are 4050
buffers. The [jack-board drawing](https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=15)
identifies TUNE as VR4, 50 kΩ B. B-2's
[ADC handler](https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L1256-L1280)
retains the preceding reading when the new raw code differs by at most one,
then applies endpoint conditioning. The later
[TUNE routine](https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L1107-L1119)
reads the stored value and separates sign and magnitude; it does not sample
AN1 and simply subtract 128. Independent adjacent-code noise added
after that handler would bypass its hysteresis. A uniform potentiometer
resistance temperature coefficient also does not establish divider-ratio
drift. Installed noise, contact behavior and differential heating need
measurements before adding an analog TUNE-wander model.

Analog ramp crosstalk can move a PWM crossing or alter waveform shape. It
does not establish mutual DCO injection locking: Roland's
[internal drawing](https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=9)
resets C54 from the PIT edge, while the PWM comparator supplies the pulse
output. It shows no comparator-to-PIT feedback that would pull the counter
phase. The [Kiwi-106 manual's unison discussion](https://www.kiwitechnics.com/downloads/Kiwi-106/KiwiTechnics_Kiwi106_Manual_v206.pdf#page=19)
likewise describes programmable-divider phase relationships, not parasitic
locking. The configured exponential-reset model already retains charge and
its effect on PWM; unmeasured temperature coefficients are not added to it.
For a rising ramp at fixed slope and comparator threshold,
`t_cross = (V_PWM - V_base) / slope`: raising the baseline advances the crossing
by `delta_t = -delta_V_base / slope`. Moving that one pulse edge changes duty
cycle and harmonic amplitudes as well as phases; it is not a uniform time
shift of the waveform or a change to the PIT-controlled fundamental.

A resonance peak alone is not a second sustained tone. Below self-oscillation,
a fixed linear filter driven by a steady periodic DCO changes the existing
harmonics' levels and phases; it does not generate a new tone at its cutoff.
This follows the [steady-state filter response](https://dsprelated.com/freebooks/filters/Transient_Response_Steady_State.html).
An attack can excite decaying ringing, noise can excite the resonant band,
and autonomous self-oscillation or time-varying/nonlinear behavior can produce
additional components. Those can interact with DCO harmonics through the
existing filter. A universal extra beating oscillator or drift offset could
double-count existing effects or invent an absent component; either requires
separate evidence.

The connected clock model advances **one common prescaler phase** and preserves
PIT counts through temperature changes. A future calibration can replace the
component proxy with measured reference frequency versus temperature, supply
and age. The separate CPU clock controls firmware cadence and needs its own
measurement. A common DCO-clock change also changes the ramp’s available
charging time at a fixed DAC current, so amplitude and PWM follow the circuit
rather than being renormalized automatically.

A useful identification session records X2 frequency with a calibrated
reference, dry DCO reset periods, local temperature and supply voltage over
repeated cold starts. Separate VCF self-oscillation captures identify analog
filter drift. Voice-by-voice frequency residuals must be split into common
clock movement, discrete count changes and measurement error. Audio recorded
against an unreferenced interface clock cannot by itself establish absolute
synthesizer drift. Until those measurements exist, the 8 MHz reference,
named component proxy and analog character assumptions remain explicitly
separate from measured Juno calibration.

## Clock, thermal and noise proposals

The September 15 assessment distinguishes supported mechanisms from proposed
magnitudes that the cited parts and drawings do not establish:

| Proposal | Assessment and implementation |
| --- | --- |
| Master-clock phase noise and PWM threshold jitter | A common noisy clock is physically plausible, but the claimed 50–180 ps jitter and −80 dBc/Hz spectrum are not established by the Murata reference. Common clock drift, divider phase and independent PIT state already exist. No extra jitter is added. Threshold timing error is voltage noise divided by ramp slew, not the derivative of voltage noise divided by ramp slew. |
| Separate VCF die/tempco thermal lag | Plausible in principle, but the proposed internal construction and 3/25-second time constants are unmeasured. Roland shows an external R111 positor in the control divider. A universal cutoff multiplier omits that divider and the exponential control dependence; existing headroom, service trims and cutoff wander already model distinct analog effects. No extra lag is added. |
| Temperature-dependent DCO reset switch | Finite reset and an explicitly calibrated exponential-reset comparison already exist. Roland's internal drawing shows an analog differentiator driving the reset transistor; it does not establish a 125 ns gate, 50–80 Ω switch or its temperature law. At 2093 Hz even an assumed 250 ns lost charging interval gives only 0.052% amplitude loss, about 0.0045 dB. No switch parameters are invented. |
| Shared avalanche noise plus local Johnson noise | The shared C42/BA662/C41 path and independent per-stage resistor sources already exist. The missing local resistor-temperature scaling is implemented above. Neither a fixed low-frequency burst spectrum nor an avalanche-noise gain coefficient follows from breakdown-voltage tempco alone, so those remain unmodelled. |
| Chorus heterodyne, delay drift and bucket noise | Clocked independent BBD noise and support filtering already exist. MN3009's `td = 256/(2 fCP)` gives 1.07–3.20 ms at 120–40 kHz, not 1.4–6.4 ms. Carrier coupling amplitudes and net clock-circuit temperature drift are unmeasured. Audio-grid MHz carriers would introduce sample-rate-dependent aliases, and aggregate BBD noise cannot be identified as pure kTC noise. No new carriers or thermal coefficients are added. |

Circuit references: Roland's [DCO internals, p. 9](https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=9),
[module board, p. 13](https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=13),
and [chorus board, p. 15](https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=15).
The [Panasonic MN3009 datasheet](https://www.experimentalistsanonymous.com/diy/Datasheets/MN3009.pdf)
defines its clock/delay relation; Analog Devices derives the
[voltage-noise/slew-rate jitter relation](https://www.analog.com/en/resources/design-notes/measuring-the-output-jitter-of-the-max999-comparator.html).
The resistor-noise change corrects the model's temperature dependence;
its audible effect has not been established.

## Proposed analog artefacts, 17 September 2026

A third-party list of twenty-five "analog circuit simulation techniques",
each named after a switch this engine does not have, was checked against the
code and the drawings above. None was implemented. Eight describe mechanisms
the engine already carries; the others name parts, pages or datasheets that
do not exist or rest on invented constants. Where a magnitude is quoted below
it is for the scenario that exposes the mechanism, and it is a report, not a
threshold: a small term with a sourced value is welcome (the rail droop and
the Johnson floors ship computed), while an unsourced one cannot be made
audible without inventing its number. The September 15 limit applies
unchanged.

| Proposal | Assessment |
| --- | --- |
| IR3109 stage input offsets (1.5 mV) giving even harmonics | Already shipped: `enableVcfStageOffsets` draws ±1.5 mV per stage under Unit Character. In the [harmonic record](https://github.com/protocodus/virtual-instrument-youknow/blob/2fbfee540eb41953d3d2f555904ec05ae3938e67/Docs/benchmarks/harmonics-2026-09-05.json) the shipping render's self-oscillation H2 is −57.4 dBc against the identified unit's −61.1 dBc, and −125.7 dBc at Character 0. No second offset term is added. |
| Resonance-loop soft limiting, exponent 1.7 | Already shipped: the implicitly solved loop uses the generalized algebraic clip with that fixed exponent, which the serviced-card fit holds constant (`YouKnowReferenceVcf.h`). |
| D6 dynamic junction resistance modulated by the WAVE node | The product now selects the fully coupled `CoupledSubMixer`, preserving the measured SUB-only aggregate with an explicit source-impedance/bias prior. Mixed-source loading follows the joint diode/node solve; installed impedance and bias remain unmeasured (OQ-15). |
| Large-signal transconductance droop lowering cutoff | Inherent in the tanh stages solved as their continuous equations — the [filter section](../../README.md#what-is-modelled) records that the cascade retains its own amplitude-dependent frequency droop. A separate `tanh(A)/A` factor would count it twice. |
| Bass-boost IC4b saturation | Already shipped: IC4b's ×11 band stops at the shared ±13.5 V swing policy inside the switched-HPF solve. |
| BBD charge-transfer smear and MOS input square law | Already shipped: zero-order hold plus residual transfer loss. The former 0.3 %/0.78 Vrms table-point transfer remains in `ProductFidelityProfile` diagnostic fixtures; the product now selects the explicitly conditional serviced-bias estimate. The proposed `0.999^256` product is not the part's transfer law. |
| Six-voice rail sag compressing cutoff and headroom | Computed and inert, 0.1 cents from one to six voices ([bus and output](../../README.md#what-is-modelled)). The voice circuits are class A and the DCOs run behind closed VCAs, so a chord changes the rail load by little more than six BA662 control currents, about 1.8 mA. |
| 120 Hz rail ripple into cutoff (68 dB rejection) | Now carried as `enableRailRipple` at the sheet's one figure: the rated-load reservoir sawtooth's 120 Hz fundamental through 68 dB is 80 µV peak at the rail, 0.003 cents of cutoff under Unit Character (the [known gap](../../README.md#known-gaps) keeps the caveats). A DCO's pitch is a divided 8 MHz resonator and does not follow a rail. |
| Reset flyback ringing (10 Ω, 15 nH, 8 MHz, 50 ns) | The discharge transistor is inside the MC5534A; Roland prints no switch resistance, trace inductance or glitch waveform, and "Tr23/Tr25" are not on the drawing. A 50 ns, 1 V event carries about 50 nV·s per period, −119 dB against the 12 Vpp ramp. The finite-linear and configurable exponential resets remain. |
| Miller-integrator curvature adding +0.35 dB of H2 | Computed in the [oscillator section](../../README.md#what-is-modelled) for an assumed gain of 2000: 7.65 × 10⁻⁶ dB. Finite gain is a 0.2 Hz high-pass on the ideal saw and cannot reach the claimed magnitude. |
| Comparator slew from an "MC5534A comparator datasheet" (10 V/µs) | No such datasheet exists: the MC5534A is Roland's custom DCO IC. A 1 µs edge is a ~350 kHz corner, −0.014 dB at 20 kHz, and BLEP already renders the edge. |
| 4013 pull-up/pull-down asymmetry adding H2 to the sub | A level difference between the two states of a 50 % square adds only DC, which C56/C50 remove; it has no even harmonics. Timing skew would, but the TC4013B's tens of ns against a half-period no shorter than 0.24 ms sits below −70 dBc, and the sub's leg is Tr19/D6, already modelled. About 5.8 µs of Tr19 storage-time skew would reproduce the identified unit's −58.5 dBc sub H2, but that take passes through replacement cards and no storage time is published for the installed drive, so nothing is fitted to it; the part-class 0.2 µs now ships as `enableSubStorageSkew`, −64 dBc at the 4′ top key and no more. There is no 4013 in that path at all: the sub square leaves the MC5534A itself (pins 13/15 on p. 13) straight into Tr19, and IC40, the one latch beside the timing chain, is a 40174. |
| Inter-voice trace crosstalk (5 pF) and ground-bus coupling (15 mΩ, −70 dBc) | The Juno-106 has one module board, not voice cards in sockets, and the cited pp. 22–24 are not in the service notes. 5 pF × 12 V into the 3.969 kΩ WAVE load is 0.24 µV·s per reset, −106 dB against the ramp. Both coupling values are invented. |
| WAVE-node stray-capacitance pole at 300 kHz | R101/R102 are 27 kΩ/33 kΩ, not 100 kΩ/200 kΩ, and the modelled load is 3.969 kΩ, so 20 pF gives a 2 MHz pole: −0.0004 dB at 20 kHz. Even the proposed 300 kHz is −0.019 dB. |
| Avalanche 1/f knee at 1.5 kHz | The row above stands: the C42/BA662/C41 shaping is anchored and no low-frequency spectrum follows from the parts. The knee is an invented number; an original-card TP8 PSD (OQ-16) would settle it. |
| Inter-stage buffer slew (+18/−11 V/µs) | The stage capacitors charge from the transconductor's limited output current, which the tanh stages already bound — `4π·V_T·f_c` over the 560/68 560 divider is about 2 V/µs at the 50 kHz cap and 0.2 V/µs at 5 kHz — so a buffer five to ninety times faster never limits. |
| Audio-rate thermal self-FM in the 80017A; VCA thermal "glue" (0.8 dB) | Class-A dissipation is signal-independent to first order and die time constants are milliseconds; 0.8 dB of gm change needs a 29 K signal-dependent junction excursion. The shared chassis warm-up and the BA662 T_ref/T term already exist. |
| VCA x² rectification "thump"; Tr20 C_bc control feedthrough (5 pF) | Tr20's collector feeds the BA662 control pin, not the audio output. The real mechanism is the input-offset feedthrough that VR30/R112 null, whose calibrated residual is unmeasured; a thump heuristic was already [removed on review](../../README.md#known-gaps). The C58/Tr20 control solve and C59 coupling remain. |
| TC4052 Ron(V) in the HPF | 110 Ω against the legs' 47 kΩ series parts: a 30 % on-resistance swing is about −63 dB, and Toshiba's curves are drawn at ±5/±7.5 V, not the installed +5 V/Tr3 rail. Deliberately uncalibrated (OQ-21). |
| TA75558 class-AB deadband (4 mV) | 4 mV is −71 dB before the loop gain the modelled 527 kHz closed-loop pole implies; Toshiba publishes no crossover figure. The summer's swing, slew, pole and Johnson noise are modelled. |
| Chorus LFO 48/52 duty from "Tr19/Tr22 charging C53" | Those parts are the BBD *clock* oscillator (OQ-01). The LFO is a TL062CP integrator plus Schmitt comparator whose triangle is symmetric by construction; its ± saturation asymmetry is unmeasured and the duty figure is invented. |

## Sibling-checked proposals, 17 September 2026

A second list of eighteen followed the same day, with two directions from the
owner: quote each mechanism's magnitude for the scenario that exposes it
rather than against one threshold, and look for the number in a sibling
Roland drawing or a comparable part before calling it unsourced. This pass
therefore read the JUNO-6 CPU-board drawing (p. 9), the 106's own module-board
legend (p. 12) and module drawing (p. 13), and the Hitachi HD14051B, TI TL08x,
Rohm BA6110 and Panasonic MN3009 sheets. One proposal is implemented — the
resonance BA662's input offset, see the release notes — and one is recorded as
the capture that would decide it. The rest are reported with their numbers.

| Proposal | Sibling or part evidence | Assessment |
| --- | --- | --- |
| Resonance BA662 untrimmed input offset, gm·V_os feedthrough | Rohm BA6110FS sheet, same family: "Low offset voltage (VIO = 3 mV max)"; nothing on the 106 nulls it — VR30/R112 null the voice VCA only | **Implemented** under Unit Character, ±1.5 mV per card inside the sibling maximum: a RES change moves the loop's DC operating point and C59 turns it into audio; exactly inert with the loop open. In a level-matched A/B the difference sits about −29 dB under a low note whose RES alternates every 0.4 s and −50 to −55 dB while RES rests. |
| MC5534A reset as a 40–75 Ω NMOS RC flyback | JUNO-6 p. 9: the discrete DCO resets C7 0.001G through TR5 and R35 2.2 Ω, driven by C6 270 pF against R34 10 kΩ, on a ½ TL082 integrator | With 2.2 Ω the RC is 2 ns; the flyback is the amplifier's 12 V slew — about 0.9 µs, straight, at the TL082's 13 V/µs. That is the shipping linear form; its 2.2 µs and the sibling's 0.9 µs differ by under 0.004 dB at 20 kHz. Recorded beside `rampResetSeconds`; no exponential is added. |
| PWM comparator noise flutter; 238 Hz hold ripple on the threshold | TL08x-class input noise, 18 nV/√Hz; HD14051B 10 pA off-leakage and TL08xC 65 pA bias, already in the converter comment as 31.5 µV per 4.2 ms on 10 nF | 20 µV of comparator noise over 1 MHz against a 12 V-per-period ramp is 46 ns of edge jitter at C1, −100 dB re the fundamental. The hold droop — 1.4 ppm of duty per pass behind IC23's TL064 follower, sidebands near −117 dBc — now ships for every hold as `enableConverterHoldDroop` at the sheets' typicals; the comparator noise is not added, and the `hold` audition still takes explicit coordinates. |
| Tr19 storage-time skew of the sub edge | Module-board legend: Tr19 is a 2SC1815-Y/GR, its base driven straight from MC5534A pin 13; Toshiba's 2SC1815 sheet and the 2SC945 sheets list no switching times; comparable era NPN switches specify about 0.2 µs | H2 = π·t_s·f_sub: −88 dBc at the 16′ bottom, −64 dBc at the 4′ top key. The identified unit's −58.5 dBc at 65 Hz would need 5.8 µs, so it is not this mechanism at part-class values. Added at the class value as `enableSubStorageSkew` (a render at the 4′ top key measures −63.9 dBc against the −63.6 dBc the law predicts); nothing is fitted to the take. |
| C54 polystyrene dielectric absorption | The p. 12 layout draws C54 with the legend's Mylar (film) outline and p. 13 gives it the tolerance letter G (±2 %) and no class letter; the notes carry no parts list. Dow's polystyrene model (Pease) puts 0.09 % in branches of 40 ms to 490 s, but the drawn part is not identified as polystyrene, and for PET film Pease gives a percentage class with no branches | The fastest branch exchanges tens of picoamps with the ramp against at least 0.3 µA of charging current: below −84 dB of slope, inside the range resistors' 1 % class. Not added: it waits on the part's dielectric (the layout says film, not which) and a branch model for it. |
| µPD7810 port-driver R-2R DNL | The ladder's real carry errors are already measured on the identified unit: −4.64/+23.31/−4.48 cents at codes 1024/2048/3072 (`vcfConverterCarryCounts`) | The measured profile ships; the proposed cause is not identified by it. Nothing to add. |
| HD14051B charge-injection bow | Hitachi sheet: 0.18 pF feedthrough, 10 pA leakage, 80/280 Ω on-resistance, no charge-injection row | A gate-overlap charge of the 10 pC class into 10 nF is about 1 mV, mostly a constant the FREQ/WIDTH trims absorb; the residual bow is unmeasured. The `hold` audition accepts an explicit charge. Not added. |
| Voice-sequential 238 Hz droop phases | Same leakage figures; the six VCF holds are already written at their chart positions | The droop now ships (`enableConverterHoldDroop`) and inherits the stagger: the VCF holds sit behind TL064 followers (IC15/18, p. 13), so 16.8 µV per pass, 0.029 cents peak-to-peak on cutoff at 25 °C, sidebands near −95 dBc on a bare self-oscillation. |
| µPD7810 ADC deadband and hysteresis | ROM-resolved (OQ-13) | The MIDI bend path already carries the assigner's reduction and B-2's dead zone; panel bytes are the stored bytes by product policy, and the plug-in has no lever ADC. |
| 80017A resin inter-pin leakage | A failure mode of aged modules, reported by technicians without pin-to-pin readings | A fault, not a fresh-service behaviour; the Aging control follows one documented recalibration. Left to a product decision with measured modules. |
| R111 positor thermally decoupled from the hybrid | No sheet gives the hybrid's thermal resistance; the JUNO-6 mounts its positor beside a DIP IR3109 | The one proposal whose predicted size is not small: with 20–30 mW of control-dependent dissipation (about 0.6 mA more IR3109 control at a 20 kHz cutoff than at 5 kHz, plus a 0.3 mA VCA tail, at 30 V) and a comparable SIP hybrid's 50–100 K/W, the die could move 1–3 °C uncompensated, about 20 cents per °C at a cutoff five octaves up. Experience with the instrument suggests less, which would mean a lower thermal resistance or internal compensation; only a self-oscillation pitch log after a large cutoff or level change on a warmed unit decides it. Recorded as that capture request, not modelled. |
| IR3109 lateral-PNP mirror pole | No IR3109 or AS3109 figure; a 3 MHz class pole | Self-oscillation would fall 1.3 % at a 20 kHz cutoff and 0.3 % at 5 kHz, inside the per-card ceiling the serviced-card fit absorbs; the audio-band magnitude is unchanged. Not added. |
| Tr20 dynamic emitter impedance on C58 | This README's own text | Already shipped as `enableCoupledVoiceVcaControl`. |
| IC1a summing-node crosstalk | Module drawing p. 13: IC1a is an M5218L, not a TA75558; six 33 kΩ legs into 3.3 kΩ | The voices drive the node from low-impedance module outputs, so its error voltage is a finite-gain term on the sum, not a path into the other voices; at a 3 MHz class gain-bandwidth the closed-loop pole is above 1.8 MHz, −0.0005 dB at 20 kHz. Not added. |
| C14 electrolytic soakage and C(V) | The C(V) candidate exists and was withdrawn from the default on Chemi-Con's bias guidance (2026-08-29) | C14 carries 1.2 % of the signal at 40 Hz (398 Ω against 33 kΩ), so even a 0.3 %/V² coefficient gives distortion near −126 dB, and aluminium soakage acts on sub-hertz memory. Not added. |
| Sallen-Key op-amp slewing on BBD steps | Roland's jack-board Tr15–Tr18 are PNP emitter followers; the tap has 3.3 kΩ output legs, 47 kΩ and 2.2 nF; its effective source impedance remains provisional (OQ-04) | The earlier op-amp slew comparison used the wrong device type. No op-amp slew limit belongs here; finite transistor gain, loading and parasitics are now quantified separately under OQ-04. Nominal forward-active exponential follower currents now run inside the capacitor network; original-unit large-signal distortion remains unmeasured. |
| OUT1/OUT2 mismatch leaving a clock carrier | MN3009 sheet: "clock component cancellation capability", no residual figure; the jack board has no balance pot, only the two 3.3 kΩ legs | With an assumed 50 mV follower DC mismatch the carrier at the 20 kHz clock end would sit near −49 dB re a 1 V wet signal after the tap RC and the ×4 reconstruction; the only capture, KR-106's chorus-on idle floor, does not isolate it (OQ-03). The signal's physical clock images are already rendered. Not added. |
| BBD bias trimmer drift and asymmetric clipping | Jack-board VR1/VR2 set the MN3009 input bias; fresh service nulls the asymmetry, and the 0.3 %/0.78 Vrms sheet point is already fitted | The drifted state is a measurement on an aged unit; the identified unit's chorus is original but no wet-only capture isolates H2. A candidate for the Aging control once measured. |

## Ten circuit proposals, 17 September 2026

A third list arrived the same day, ten circuit mechanisms each with a
component reference, values and a pitch, with the request that none be
dismissed quickly. Each was checked against the engine source rather than
against this document, and the one numeric disagreement was settled on the
jack-board drawing (p. 15, read at 400 dpi). Every one of the ten is already
in the shipping engine, nine of them on by default in the raw engine and the
tenth turned on by the product profile; in eight cases the proposal quotes
the engine's own numbers.

| Proposal | Shipping as | Where the proposal and the engine differ |
| --- | --- | --- |
| 1. IR3109 stage input offsets, ±0.8–1.5 mV per stage, inside the tanh | `enableVcfStageOffsets` (on): ±1.5 mV per stage, seeded per card, referred to the node through the 560/68560 divider, scaled by Unit Character, added to each stage's differential input before its tanh; the resonance BA662's own offset joined it on 17 September | Nothing. |
| 2. Voice VCA translinear saturation with the 4.8 → 6 Vp-p TP8 service gain | `enableVoiceVcaSignalSaturation` (on): the BA662 pair's I_tail·tanh(V_d / 2V_t) law under the JUNO-6/60 drawing's 47 kΩ load; `enableVoiceVcaServiceGain` (on): the p. 19 6 Vp-p TP8 gain; the older softplus stand-in has been retired | Nothing. |
| 3. Sub through D6 as a unipolar half-wave current, AC-coupled by C56 into the hybrid input | The product selects the full `CoupledSubMixer` with declared source priors, the 4.7 kΩ ∥ (24 kΩ + 1.5 kΩ) load and continuous C56 charge. `enableSubHalfWaveNodeCoupling` and `enableSubDiodeControl` retain the earlier independent path for reference fixtures | The proposal's "R102 10 kΩ pull-up" misreads p. 13: R102 is Tr19's collector load; the leg is R101 and D6. |
| 4. Tr20's dynamic emitter impedance loading C58, 1 ms → 687 µs | `enableCoupledVoiceVcaControl` (on): C58 and Tr20 as one loaded circuit, 1 ms as Tr20 closes, (10 kΩ ∥ 22 kΩ)·0.1 µF = 687 µs at high current, the emitter junction solved by the Wright omega law; already listed as shipped in the previous table | Nothing. |
| 5. Switched HPF with continuous capacitor states and departing-leg tails | Continuous capacitor state: C10 15 nF and C11 4.7 nF legs bleeding through their 1 MΩ and 47 kΩ into IC4a, 15.71 ms leaving Two and 4.92 ms leaving Three; Boost as the C9/C8/C6 network with 0.37 and 2.77 ms eigenmodes; the TC4052 at 110 Ω in the product profile | The proposal's Boost capacitors (C8 0.068 µF, C9 0.01 µF) are wrong: p. 15 prints C9 = .047, C8 = .01, C10 = .015, C11 = .0047 and C6 = .022, and IC4 is an M5218L, not a TA75558. Its eigenmodes are the engine's. |
| 6. MN3009 insertion-gain spread ±4 dB per line plus the BBD noise floor | Per-line insertion gain: one fixed-seed relative draw between the two lines inside Panasonic's Min −4 / Typ 0 / Max +4 dB row, 1.6 dB full span, scaled by Unit Character; the 0.200 mVrms part endpoint shaped by the reconstruction filter is the noise master `chorusNoise` | The proposal draws each line anywhere inside the ±4 dB limits, so up to 8 dB between lines. The sheet gives limits, not a distribution, so either span is a convention; the shipped one is the same conservative fraction of the bound the IR3109 stage capacitors use. Widening it is a listening decision (A 1.6 dB, B 4 dB, C 8 dB spans), offered, not taken. |
| 7. Differential resonance drive with BA662 tanh limiting, 0.2751–0.3078 compensation, 4.8 Vp-p at TP19 | Differential circuit solve with the compensation bracket's floor as `ResonanceCompensationShape::Reconstruction`, the loop tanh headroom from the 100 kΩ/1.5 kΩ divider, and the p. 19 4.8 Vp-p self-oscillation trim | Nothing. |
| 8. Six-card calibration coordinates from serviced #439522 | Selected on 2026-09-11, now retained in `ProductFidelityProfile` diagnostic fixtures. The product's `useOriginalCardVcfCalibration` selects the nominal original-hybrid/service frequency law instead; seeded Unit Character draws remain | The unit's cards are Borish replacements, so the fit is comparison coordinates rather than original-module facts; the proposal presents it as a lab measurement of original hardware. |
| 9. Pulse Off leaves the MC5534A pinned high; WAVE node DC removed by C56 | The comparator's rail stays on the shared WAVE node; the selected coupled solve lets its bias affect D6 before continuous C56 coupling removes settled DC | Nothing. |
| 10. Johnson noise of the 68 kΩ/560 Ω network into each OTA stage, plus the µPC1252H2's −94 dBV floor | `enableCardJohnsonFloor` (on): four independent sqrt(4kTR) sources at the live card temperature into their own differential nodes; `enableCommonVcaNoise` (on): NEC's −94 dBV typical on the bus ahead of the chorus split | The proposal injects fresh noise at every Runge–Kutta evaluation, which would make the noise power depend on the step size; the engine draws once per sample and lets the stages shape it. |

**What is off, why, and what enabling it takes.** The serviced-unit frequency
calibration (row 8) remains available in `ProductFidelityProfile` diagnostic fixtures because
the capture is of replacement cards; the current product selects nominal
original-hybrid/service coordinates. OQ-03 still requires measured clock-bleed
amplitude and evidence for any rate-dependent noise source. C14 needs a
measured installed 10 µF non-polar part before introducing voltage-dependent
capacitance; manufacturer guidance says bias does not change its capacitance.
The unsupported placeholder implementations have been removed, along with
the superseded softplus VCA law and the hyperbolic delay sweep, which described
an oscillator this board does not have.

## Static behaviours swept for physical dynamics, 17 September 2026

A sweep of the engine source for quantities held constant that a real circuit
moves — by temperature, by signal, by time — assessed each against the
instrument's own physics and the evidence rules above. What could be made to
move without inventing a number was implemented; the rest is recorded with
the measurement or decision it waits on.

| Static quantity | Grounds | Verdict |
| --- | --- | --- |
| Resonance return headroom frozen at 25 °C while the stage headroom warmed | Both are 2 Vt through a resistor ratio, one module, one die temperature | Implemented (`enableResonanceHeadroomTemperature`), with the RES adjustment (`enableResonanceServiceTrim`) it makes necessary; see the filter section. |
| Card gradient present at full strength at power-on | The gradient is the supply's heat; everything is at ambient when the instrument is switched on | Implemented: the gradient rides the warm-up clock, the trims stay at the service reference. |
| Output-stage resistor floors pinned at 298.15 K while the cards' floors and IC5's control constant read the jack board | Johnson's law; the same chassis law the jack board already reads | Implemented: √(T/298.15 K) on the summer and wiper floors; IC5's floor stays a 25 °C sheet figure. |
| Sub edge instantaneous | Tr19 storage time | Implemented at the part class (`enableSubStorageSkew`). |
| RES trimmer modelled as one loop-gain endpoint for every card | p. 19 sets an amplitude, per card, warm | Implemented as above. |
| The voiced ±2 % RES-travel post-trim residual | It realises up to ±10 % of self-oscillation amplitude on some cards, larger than a scope reading of 4.8 Vp-p, and the FREQ solve does not see it | Left as it is: re-pinning it as an amplitude reading error needs the procedure's tolerance, which the notes do not state; a full account needs the amplitude it realises, an inverse solve the automatable Character setter must not run. Recorded, not changed. |
| IR3109 Early effect gated by Unit Character | Character's documented semantics: zero is the calibrated nominal with no device nonlinearity beyond the stage tanh | Kept; the switch and the coefficient are documented. |
| Uniform rather than Gaussian Johnson draws | Same RMS; the crest factor of a −90 dBFS floor is inaudible | Kept. |
| Hard resonance onset at the grounded-base junction | The onset's shape belongs to OQ-09's measured family | Waits on OQ-09. |
| Serial control-wire timing as a nominal | Sourced nominal; a spread is a listening candidate, not a measurement | Offered as an A–Z listening decision; nothing shipped. |
| Tr20's control law with Vt only | The full V_be(T) law needs the junction's own calibration | The product now uses the named 25 °C 2SA1015 junction prior; installed parameters and temperature dependence still need the voice-VCA capture (OQ-19). |
| C59's corner as one nominal per unit | VR27's position and the 1 µF part's tolerance | The product now derives a nominal per-card load from the fixed service input trim (1.324 Hz at 25 °C); arbitrary capacitor spread remains omitted. The raw reference retains the R108-only 1.94 Hz upper bound. |
| Voice-sequential 238 Hz scan droop | The VCF holds sit behind TL064 followers (IC15/18, p. 13): 0.029 cents per pass at 25 °C, not the 0.054 the TL08x figure gave | Implemented for every hold (`enableConverterHoldDroop`); see the converter and below-audibility sections. |
| Panel ADC laws | Product policy, not circuit | Kept. |
| Chorus line insertion-gain spread as a 1.6 dB convention | The MN3009 sheet gives limits, not a distribution | Spans of 1.6, 4 and 8 dB offered as letters A, B and C for a listening decision; the convention ships. |
| Chorus and DCO clock items | OQ-15, OQ-03, OQ-04 | Wait on those captures. |
| 80017A die self-heating | A borrowed thermal resistance predicts 47–140 cents at the top of the range, which experience with the instrument contradicts | The capture request above stands; not modelled. |

## Below-audibility mechanisms, 17 September 2026

Six mechanisms the audits above had assessed as inaudible were re-examined
for a sourced number: those that have one now ship, because a small term
with a sourced value is welcome; the rest are recorded with the exact input
they wait on.

| Mechanism | Sourced input | Verdict |
| --- | --- | --- |
| Converter hold droop | HD14051B 10 pA typical off-leakage; TI's 25 °C typical input bias for the followers p. 13 names — TL08x 65 pA (IC16/19/20, IC17b) behind IC24's DCO and SUB holds, TL064 30 pA (IC15/18, IC21/22) behind IC23's and IC26's; the 10 nF holds; TI's doubling per 10 °C | Implemented in the product (`enableConverterHoldDroop`, a product selection like the serviced VCF calibration; the engine's reference configuration keeps ideal holds): 0.013 or 0.007 LSB per 4.2 ms pass, falling by a stated convention, more on a warm board. |
| 120 Hz rail ripple into the cutoff | p. 16's 3300 µF reservoirs and 0.25 A secondary rating; the M5230L's typical 68 dB rejection at 120 Hz; the reference unit's 60 Hz mains from its 120 Hz hum | Implemented (`enableRailRipple`): 80 µV peak at the rail, 0.003 cents of cutoff under Unit Character, nothing at zero. |
| µPC1252H2 even-order distortion | NEC's THD figure is for the symmetry-trimmed part; Roland fits no trimmer, and no untrimmed figure or distribution is published | Recorded. Waits on a level-swept THD capture of the bus after IC5, or NEC's untrimmed figure. |
| C54 dielectric absorption | The p. 12 layout's Mylar (film) outline, ±2 % (G), no parts list; Pease's branch model is for polystyrene | Recorded. Waits on the part's dielectric and a branch model for it. |
| 4013 edge skew on the sub | No 4013 in the sub path: the square is the MC5534A's own (pins 13/15) into Tr19; IC40 is a 40174 latch | Nothing to add beyond the Tr19 storage skew that already ships. |
| Chorus clock and LFO capacitor tempcos | p. 15's C53 150 pF (Tr19's current source) and the LFO integrator's C3 0.1 µF carry no class letter; the resistors around them are R20J carbon film with no printed tempco (the notes print 100 ppm only for the FX metal-oxide parts) | Recorded. Waits on the jack-board layout's outlines (p. 14) for the classes, a datasheet curve for each, and the carbon-film tempco. |

## Ten circuit proposals checked, 17 September 2026

A second third-party list of ten "grounded analog circuit behaviors", each
with a confidence figure, checked against the drawings and the engine as it
stands. Seven describe mechanisms that already ship, one an option that
already exists, and two were assessed above; none adds a sourced number the
engine lacks, so nothing was implemented from it.

| Proposal | Assessment |
| --- | --- |
| 1. WAVE-node saw-to-sub intermodulation through D6 | The product now selects `CoupledSubMixer`: the sources and D6 share one nonlinear WAVE-node solve, with explicit source priors and the existing SUB-only calibration. Installed impedance and bias remain OQ-15. The proposal's parts are misread: R101/R102 are 27 kΩ/33 kΩ on p. 13, not 22 kΩ/10 kΩ, and its separate modulation coefficient is unsourced. |
| 2. Chorus-off BBD clock clamp as a tape-stop pitch sag | The product's D3/R41/R47/C15 network into Tr23/Tr28 (`enableChorusClockMuteCircuit`) stops the clocks behind the wet mute. Its nominal coupled model gives about 288 ms to stop and 82 ms to mute after Off; it does not sweep clock frequency, so the proposed audible pitch ramp is not the circuit. Installed switching times remain unmeasured. |
| 3. IC6 TA75558S headroom and slew | Shipped: the 13.5 V asymptote with the algebraic clip and the 1.0 V/µs slew (`enableOpAmpSlewLimiting`). A tanh is exactly what the code note rejects — it has no linear region. |
| 4. C61 and VR28 smoothing the cutoff staircase | Shipped since the first converter model: `vcfHoldSlewSeconds` is 522 µs inside the 467–553 µs WIDTH bracket. The network is misread: the trimmer's neighbours on p. 13 are R110 8.2 kΩ, the R111 560 Ω positor and R113 10 kΩ, not R91 18.2 kΩ and R92 560 Ω. |
| 5. Output R64/C21 pole and L/MONO normalling | Shipped: one pole per channel (`YouKnowOutputJack.h`) and a mono bus carries the normalled mean. The OUTPUT menu now provides H/M/L and explicit receiver resistance, including a shared load in mono; the default stays High/Open (OQ-17). |
| 6. TC4052 Ron(V) in the HPF | Assessed above: 110 Ω against the legs' 47 kΩ puts a 30 % swing at −63 dB, and the proposed voltage coefficient is invented (OQ-21). |
| 7. Noise level OTA ahead of C41/R79 | Shipped: `enableNoiseLevelBeforeC41` with the 4.82 kHz pole and C42's 33.9 Hz high-pass. |
| 8. Relaxation-oscillator curvature and turnaround rounding | The affine delay law is derived and shipped, and the reset dead time is a stated 0.3–2.5 µs assumption inside OQ-01's conditional interval. Tr19's Early effect needs the part's output conductance, which its sheet does not give, and the 0.05·(1 − cos) rounding is invented: the LFO is a TL062CP integrator and Schmitt comparator whose triangle has no rounded turnaround. Not added. |
| 9. BA662 sub-threshold knee | The product selects the same Wright-omega junction law with a conditional 177.78 mV knee from the named Tr20 specimen. The former voiced 150 mV coordinate remains in `ProductFidelityProfile` diagnostic fixtures; the proposed softplus is a retired approximation. |
| 10. Post-BBD Sallen-Key with the MN3009 source loading | Shipped: one coupled MNA of the provisional 3.5 kΩ effective source boundary, the 47 kΩ/2.2 nF tap and both 22 kΩ sections (OQ-04). The former interpretation as two continuously active 3.7 kΩ sources is withdrawn; the actual impedance remains unidentified. |

## Provenance list checked, 17 September 2026

A third-party summary of "every hardware recording, measurement, service
document and schematic referenced by the engine" was checked against the
repository's own records and, where it disagreed with them, against the
drawings. Its recordings, hashes, issue references, the six
`serviced439522Vcf` rows, the A11 chorus fit and the data-sheet figures are
this repository's own, restated; nothing in it is new evidence and nothing
in it changes a decision. Its wrong readings are recorded here so they are
not proposed again:

| Claim | The record | Verdict |
| --- | --- | --- |
| Boost HPF on "p. 14 (main board)": IC4 a TA75558, C8 0.068 µF, C9 0.01 µF | The switched HPF is on the p. 15 jack board: C9 .047, C8 .01, C10 .015, C11 .0047 and C6 .022 at 400 dpi, and IC4 is an M5218L. The TA75558S on that board is IC6, the output summer. | Wrong page, wrong values, wrong part; the same values were rejected in the ten-proposal audit above. |
| Chorus mute "Tr4/Tr5 delay: C13 10 µF, C16 4.7 µF, R48 47 kΩ" | p. 15 at 300 dpi: R50 10 kΩ, C16 2.2/50, R48 150 kΩ, R49 560 kΩ, C13 1/50, R42 39 kΩ, R43 100 kΩ, R46 330 Ω, R47 330 kΩ, D3, and D4/D5 into the Tr11/Tr12 2SK30A gates — the values `YouKnowChorus.h` carries. | Wrong parts. The raw two-node model gives 80.2/120.6 ms; the complete-circuit binary comparison gives 81.8/116.7 ms. The product's named finite Tr4/JFET priors give 88.1/107.8 ms; these are model predictions. |
| DCO ramp parts on "p. 9 (DCO & VCF)": C54 0.001 µF, R85 399 kΩ, R86 200 kΩ, R87 100 kΩ | The values are right; the 106's module board is p. 12. Page 9 is the JUNO-6/60 CPU board the engine cites for the 240PJ stage capacitors and the R42 47 kΩ load. | Wrong page, right values. |
| JUNO-6/60 p. 9 "differential resonance input network R5/R2 and R3/R1" | The engine reads the resonance input-side compensation from the 106 module drawing and the Open80017a netlist; the JUNO-6/60 page supplies the 240 pF capacitors and the 47 kΩ load. | Unverified designators; not a source the engine uses for that network. |
| OKI MSM82C53-2 databook "p. 186, count edge on the TP5 falling edge driving C54 discharge", later restated as "coincidence policies A and B" in this README | The engine discharges C54 on the M82C53 OUT low-to-high transition; TP5 is the count clock. No databook page is cited anywhere in the records, and no such policy pair exists in them. | Unverified page, and the edge description does not match the drawing-derived model; nothing adopted. |
| NEC µPD7810 "172-state ADC service vector, 43 µs at 4 MHz", attributed to an NEC instruction-timing manual | The figure is this repository's own: the 2026-09-14 entry in `Docs/decisions.md` counts 172 nominal states, 43 µs at the model's 4 MHz state rate, for the recovered B-2 ADC vector and handler — the work the `FirmwareControlNoInterrupt` comparison profile (letter B) omits, which is why letter A, `MeasuredChartGeometry`, stays the product default. | The 172-state vector/handler count remains ROM-derived. The newly recovered original-family NEC manual now adds the independently specified 16-state automatic entry, for 188 occupied states; see the OQ-08 discussion below. |

Everything else in the list — unit #439522 with Borish replacement cards
installed and calibrated in 2022, the M-Track 2x2 interface, the 192 kHz
six-card sweep (`27ab9ed0…`), the 96 kHz calibration capture (`a9282c4a…`)
and its MIDI archive (`c9727669…`), the six card rows, the A11 spectral fit
and click series, p. 19's trim targets, p. 13's D6, R101/R102, C56 10 µF and
C58 0.1 µF, the Open80017a 3.969 kΩ input impedance, the owner's manual's
`0x31` message, the ROM line ranges, the MN3009 ±4 dB row and the µPC1252H2
−94 dBV floor — restates `Docs/validation/hardware-validation.md`,
`Source/DSP/YouKnowReferenceVcf.h` and the comments beside the constants.

## Other methods assessed

The following decisions distinguish a useful research method from evidence
that it should replace a shipping block. The method summaries are sourced;
their adoption decisions are engineering judgments for this repository.

| Method and primary research | Relevance | Decision |
| --- | --- | --- |
| [Differentiable white-box modeling — Esqueda, Kuznetsov & Parker, 2021](https://dafx.de/paper-archive/2021/proceedings/papers/DAFx20in21_paper_39.pdf) | Learns circuit parameters from measured input/output while retaining the circuit equations | Appropriate for future bounded identification of uncertain component values. No new fit is justified without suitable original-unit captures and held-out controls. |
| [Differentiable all-pole filters — Yu et al., 2024](https://diffapf.github.io/web/) | Gives sample-wise gradients for time-varying recursive filters | Useful for offline envelope/filter identification. Its synth example is not evidence that a simplified filter should replace this nonlinear four-stage model. |
| [BBD antialiasing with BLEP — Gabrielli, D'Angelo & Squartini, 2025](https://dafx25.dii.univpm.it/wp-content/uploads/2025/09/DAFx25_paper_29.pdf) | Separates physical BBD aliasing from additional artifacts introduced by digital simulation | Already represented by the clock-event reconstruction and existing audits. Preserve physical clock folding; qualify the corrected input network through those same audits. |
| [Interpolation filters for ADAA — Zheleznov & Bilbao, 2024](https://dafx.de/paper-archive/2024/papers/DAFx24_paper_33.pdf) | Shows gains from cubic interpolation for memoryless nonlinearities, with different stability/results for stateful systems | No blanket insertion into OTA feedback. A feedback-loop modification must requalify resonance, amplitude, modulation and stability; a memoryless result does not supply that evidence. |
| [Neural multi-port elements in wave-digital structures — Massi et al., 2025](https://dafx.de/paper-archive/2025/DAFx25_paper_69.pdf) | Trains a nonlinear element within a physical circuit structure | A possible residual model after a level-dependent hardware discrepancy is identified. No training data here establishes such a residual for original 80017A cards. |

A local continuous-time VCF probe also examined the linear interpolation used
between successive cutoff-hold endpoints during RC settling. Supplying exact exponential control
trajectories reduced numerical error, but would require six cutoff mappings
per settling sample. At 48 kHz host/4×, the measured waveform corrections in
the tested non-self-oscillating upward/downward cases (feedback 0 and 3.5)
were approximately −85 to −110 dB relative to signal. Self-oscillating cases
had phase-sensitive differences that cannot support the same bound. These
are circuit-probe differences, not perceptual thresholds.

A cheaper bounded cubic candidate improved one 1× musical-sweep oracle error
from −71.87 to −81.66 dBr. However, sending those trajectories through the
current interface disables the multi-voice SIMD path; an extreme upward
case remained dominated by the solver and showed no useful improvement.
Neither candidate is adopted. A future implementation needs a bounded
scalar/pair/quad method with state continuity and measured callback cost,
not merely a lower offline residual. This leaves a measured
numerical research opportunity without introducing an unqualified CPU
regression into the product.

## Limits and next hardware evidence

Numerical agreement with a circuit equation proves that the implementation
solves that model more accurately. It does not prove that all uncertain
component values describe a particular forty-year-old instrument. The
existing [identified-unit report](../validation/hardware-validation.md) uses a serviced
Juno-106 with replacement voice cards, and the approved product profile
retains that scope. None of this pass's numerical improvements changes that
calibration into an original-card population measurement.

The next experiments with the strongest potential to change audible fidelity
remain original-card noise/source balance, chorus delay/level/noise, and
filter transfer versus input level. Each needs exact patch bytes, an
unchanged recording gain, a documented output connection, warm-up state and
separate control settings reserved for validation. A fitting method should
first recover nuisance gain and alignment without letting per-patch EQ hide
model error. Repeat captures are needed to separate measurement variation,
unit condition and a wrong nominal law. These are evidence requirements,
not additional unimplemented sound-design choices.

## Reproduction and listening

Build with the existing DSP-only recipe and run `ctest --output-on-failure`.
`YouKnowChorusMuteTests`, `YouKnowDcoRangeTests` and `YouKnowVcfNoiseTests`
check the further circuit corrections. The focused PIT engine suite also
checks interrupted envelope/portamento stores and host-block invariance.
`YouKnowOutputJackTests` reports the magnitude comparison;
`YouKnowChorusInputLoadingTests` reports AC errors and the loading correction;
`YOUKNOW_PIT_TESTS_ONLY=1 YouKnowEngineTests` includes the exhaustive ENV/KEY
oracle and production control-write fixtures.
`YOUKNOW_OUTPUT_TESTS_ONLY=1 YouKnowEngineTests` isolates the volume law,
fixed output corpus and rendered jack-response integration checks. The general
engine, circuit, oversampling and rendering suites check their integration. All references
and derivations needed by the implementation are also beside the code.

The five further corrections pass all 36 DSP/tool CTest contracts after
an affected-audit rerun, including the complete engine suite. The dynamic
VCF wiring oracle now replays the four additional noise inputs; its connected
state remains bit exact and its disconnected-control mutation still fails.
The rebuilt macOS arm64 VST3, Audio Unit and CLAP pass the processor and all
three bundle checks. The isolated noise test additionally proves that full-
and half-step RK4 pairs and Merson pairs/quads execute. No new original-unit
capture or blind listening score is claimed.

The earlier output/filter pass re-rendered the maintained ten demos and
*Low Sun* composition with the product profile and renderer settings.
Those eleven files passed format, duration, peak and DC validation; they
are not new listening evidence for the five further corrections.
Their tables below are generated by the renderers. Frozen historical
comparison recordings remain historical evidence. For listening, bright
upper-register material exposes the output correction, chorus pads expose
the small wet-balance change, and resonant envelopes or portamento expose
the firmware correction. Small corrections can disappear in a dense mix;
no new blind audibility or preference verdict is claimed.

## Research sources

1. Roland Corporation. [JUNO-106 Service Notes, first edition](https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=15), July 31, 1984, printed p. 15. Original circuit topology and component values; the scan is hosted by KiwiTechnics.
2. ErroneousBosh. [Juno-106 voice-CPU B-2 disassembly, `ic29.txt`](https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L923-L992), immutable revision `26926a04ff1939106820313e71e34b4ca2f67070`, accessed September 12, 2026. ENV/KEY instruction order and integer arithmetic; a reverse-engineered primary technical artifact, not an official firmware specification.
3. Martin Vicanek. [Matched One-Pole Digital Shelving Filters](https://vicanek.de/articles/ShelvingFits.pdf), September 7, 2019, revised September 24, 2019, equations 3–12. Magnitude-matching method, applied here in its low-pass limit.
4. Fabián Esqueda, Boris Kuznetsov and Julian D. Parker. [Differentiable White-Box Virtual Analog Modeling](https://dafx.de/paper-archive/2021/proceedings/papers/DAFx20in21_paper_39.pdf), DAFx20in21, September 8–10, 2021, pp. 41–48. Parameter identification through circuit simulation.
5. Chin-Yun Yu and collaborators. [Differentiable All-pole Filters for Time-varying Audio Systems](https://diffapf.github.io/web/), 2024, paper and accompanying research examples. Time-varying recursive-filter gradients.
6. Leonardo Gabrielli, Stefano D'Angelo and Stefano Squartini. [Antialiasing in BBD Chips Using BLEP](https://dafx25.dii.univpm.it/wp-content/uploads/2025/09/DAFx25_paper_29.pdf), DAFx25, September 2–5, 2025, pp. 71–77. Physical versus simulation-generated BBD aliases.
7. Victor Zheleznov and Stefan Bilbao. [Interpolation Filters for Antiderivative Antialiasing](https://dafx.de/paper-archive/2024/papers/DAFx24_paper_33.pdf), DAFx24, September 3–7, 2024. Memoryless and stateful interpolation results and stability qualifications.
8. Oliviero Massi, Alessandro Ilic Mezza, Riccardo Giampiccolo and Alberto Bernardini. [Training Neural Models of Nonlinear Multi-Port Elements Within Wave Digital Structures Through Discrete-Time Simulation](https://dafx.de/paper-archive/2025/DAFx25_paper_69.pdf), DAFx25, September 2–5, 2025. Learned circuit-element models; no Juno-106 calibration result is claimed.
