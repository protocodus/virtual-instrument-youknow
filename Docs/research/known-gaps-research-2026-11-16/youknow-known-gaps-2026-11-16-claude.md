<!-- Run header added by the coordinator. The report below it is the Claude subagent's file, unedited. -->

| Field | Value |
| --- | --- |
| Date | 2026-11-16 |
| Question | How can each of YouKnow's documented known gaps be addressed? (brief: `youknow-known-gaps-2026-11-16-prompt.md`) |
| Product | YouKnow 1.4.9, commit 6b92c518355ee6c22ee4fc66936835544909555e |
| Researcher | Claude Code subagent (general-purpose agent, WebSearch and WebFetch) |
| Model | Claude, default effort |
| Run | 2026-11-16 14:13:49Z to 14:39:38Z UTC, 26 min 7 s |
| Tokens and tools | 376,869 tokens; 146 tool calls: 48 web searches, 62 page fetches (13 failed: 403/404, expired certificate, timeouts) |
| Status | Completed |

---

# Closing YouKnow's known fidelity gaps: public evidence and cheapest captures

- **Date:** 2026-11-16
- **Researcher:** Claude Code subagent
- **Subject:** YouKnow 1.4.9, repository protocodus/virtual-instrument-youknow, commit `6b92c518355ee6c22ee4fc66936835544909555e` (read-only snapshot)
- **Question:** How can each of YouKnow's documented known gaps be addressed? For every gap: what public evidence could close or narrow it, how others have dealt with it, and, where no public evidence exists, the cheapest credible measurement or capture that would close it.

**How to read this report.** Web sources are cited as [S1]…[S28] and listed in section 5. Facts about YouKnow itself are cited to snapshot files and lines (for example `README.md:1863`). Each statement is one of three kinds, and I say which:

- **Sourced fact:** carries an [S#] or a snapshot file reference.
- **Inference:** my reasoning from sourced facts; labelled "Inference".
- **Guess:** labelled "Guess"; treat as a lead only.

**Limits of this pass.** Page contents were read through a fetch tool that summarises pages with a small model. Where I could, I checked numbers against the raw text (PDF text extraction, or reading a scanned drawing by eye), and I say so in the source list. Two source classes need a second look before anyone relies on them: OCR text of Roland service notes on ManualsLib [S2][S3], and my by-eye reading of a scanned MKS-30 drawing [S1]. Several pages could not be opened at all (listed in section 6).

---

## 1. Summary: the five highest-value next actions

Ranked by expected audible or correctness benefit per unit of effort.

1. **Re-analyse the captures already in hand (no new hardware time).** Five analyses on the existing #439522 takes would each test a shipped assumption. All are my proposals (inference), built on the repository's own captures:
   - Sidebands at ±120 Hz around a high self-oscillation tone test the `enableRailRipple` magnitude directly (section 2.4).
   - Sidebands at ±238 Hz (the 4.2 ms scan) around the same tone bound hold droop plus converter charge injection together (section 2.5).
   - 60/120/240 Hz idle lines, by chorus state and left/right coherence, separate recording-chain hum from rail ripple in the chorus path (section 2.4).
   - The amplitude histogram of the noise take tests the Gaussian assumption (section 2.5).
   - Common DCO pitch against elapsed time gives the installed resonator's drift relative to the interface clock (section 2.3).
2. **One bench hour on the reference unit with the cover off.** A shunt test at the WAVE node separates the two source-resistance roots (about 4.98 kΩ and 95.7 kΩ) by more than 5 dB, so one reading decides it (section 2.2). The same hour reads the MN3101 clock extremes (absolute chorus delay, both modes), rail ripple at the card and at the BBD pins, and the 8 MHz reference from cold (section 4).
3. **Get one controlled capture from original 80017A cards, ideally a unit with mixed original and replacement cards.** No public controlled original-card capture exists (section 2.6). The MIDI suite already exists, and the KR-106 tracker is a place where the reference unit's owner has already asked other owners for comparison recordings [S8].
4. **Read three Roland drawings the repository has not used.** The MKS-30 uses the same 80017A hybrid [S1]; the GR-700 is said to as well (search results only, not verified); the JX-3P is a 1983 Roland design a year older than the JUNO-106 [S3]. The MKS-30 notes print a second factory window for the hybrid (self-oscillation trimmed to 250 mVp-p, VCA level 600 mVp-p, both at POLY OUT) [S2], and its drawing shows the hybrid's external resonance, gain and positor networks [S1]. Low cost, modest expected yield (section 2.2).
5. **Quantify the rate dependence and model the chorus-on 120 Hz component.** Publish a convergence table for Normal against Max against an offline reference, and add a host-rate floor (section 2.1). Fit the wet-path 120 Hz component the captures already show, which `Docs/decisions.md:2116` records as not modelled (section 2.4).

---

## 2. Gap by gap

### 2.1 Numerical accuracy is still rate-dependent

**Current state.** `CircuitRate::Normal` runs the local VCF and chorus stages at half the reference rate at 44.1/48 kHz, so its open-filter pole cap and nonlinear-product band sit an octave below Max's; unusually low host rates and chorus settings that cannot reuse a support cache keep older limits (`README.md:1863-1873`, `Source/DSP/YouKnowEngine.h:132`, `Source/DSP/YouKnowChorusRateIsland.h:38-40`). The owner heard no difference between Normal and Max in the 2026-10-16 listening round (`Docs/decisions.md:1860-1884`).

**Evidence found.** This gap is numerical, so hardware evidence does not apply. Two papers bear on the options:

- Holters extends antiderivative antialiasing from memoryless nonlinearities to a class of stateful systems, shown on two guitar-distortion circuits [S21].
- La Pastina and D'Angelo report that standard antiderivative methods change the frequency response of stateful systems and can cause instability, and add a compensation filter to fix that; their test case is a diode clipper [S22]. I read the abstract only.
- The repository has already declined blanket antiderivative antialiasing inside OTA feedback, on Zheleznov and Bilbao's results (`Docs/research/juno-106-fidelity-research.md:1068`). Both papers above are consistent with that decision; neither shows a four-stage OTA filter with resonance feedback. (Inference.)

**How others handle it.** KR-106 exposes the trade to the user: VCF oversampling Off, 2x or 4x, with 4x the default, and documents the cost of the lower settings ("may alias at high cutoff + high resonance", "may be unstable at high resonance") [S6]. Its architecture note describes a 2x halfband design instead [S10], so one of the two documents is out of date; I treat [S10] as the weaker.

**Options.**

| Option | What to do | Data needed | Effort | Audible impact | Confidence |
| --- | --- | --- | --- | --- | --- |
| A. Publish the numbers | Add a convergence table to the README and tests: Normal, Max and an offline high-rate reference over a fixed corpus (open filter, high resonance, hot chorus). Turns a qualitative gap into a stated bound. | None beyond renders | S | None | High |
| B. Host-rate floor | Below 44.1 kHz, run the engine at an internal rate of at least 44.1 kHz and resample to the host. Removes the "unusually low host rates" clause. | None | M | Only in rare hosts | High |
| C. Promote on demand | Under Normal, raise a voice's local stages to the Max grid only while its cutoff and resonance sit where Normal's pole cap binds. | The existing Normal/Max difference measurements | M | Small: open filter and high resonance at 44.1/48 kHz | Medium |
| D. Fill the support cache | Build the missing chorus support caches lazily off the audio thread instead of falling back to the older path. | None | M | Small | Medium |

All four are my proposals (inference). None is taken from a source.

**Open questions.** Does option C's switching itself stay inaudible? The 2026-10-16 verdict suggests the whole difference is already below the owner's threshold, so A and B may be all this gap needs.

---

### 2.2 Estimates still open

**Current state.** The product profile ships five estimates: a recording-derived Mode-I chorus fit, the named Tr20 prior, nominal VCF coordinates, a declared WAVE source prior and a conditional serviced-bias BBD curve (`README.md:1874-1881`). The relevant code is `Source/DSP/YouKnowChorus.cpp:1720-1757` (timing profile), `Source/DSP/YouKnowEvidenceVca.h:19-36` (Tr20), `Source/DSP/YouKnowCoupledMixer.h:95-114` (WAVE source root) and `Source/DSP/YouKnowChorus.h:93-104` (BBD bias curve).

#### a. Original-card resonance against byte (OQ-09)

**Evidence found.**

- **A second Roland factory window for the same hybrid.** The MKS-30 uses the 80017A (the drawing labels IC1 as the VCF/VCA hybrid with RESO, GAIN, DC BAL and COF trimmers around it) [S1]. Its service notes trim resonance by turning VR1 fully clockwise until the filter oscillates and then setting 250 mVp-p at POLY OUT, trim cutoff to 1 kHz, and trim VCA level to 600 mVp-p with a square-wave test signal [S2]. These values come from OCR text and need checking against the scan.
- **Roland treats the hybrid as needing per-chip trimming.** The MKS-30 notes say cutoff, resonance, VCA level and DC balance "should be readjusted whenever the chip is renewed" [S2].
- **An older sibling.** Search results describe the JX-3P's filter as built on the IR3109; I did not verify that on a primary page. The JX-3P notes (July 27, 1983) have a "VCF RESONANCE" step that sets a sine to 600 mV p-p [S3]. Its main-board drawing is on the next page; I could not read it (the page text holds no component values) [S3].
- **Capacitor value.** A 2017 simulation write-up gives 240 pF for the Juno-6/60 filter capacitors and 270 pF for the Analogue Renaissance clone [S27], which agrees with the repository's settled reading in OQ-18 (`README.md:1954`). It is a simulation, with no resonance numbers.
- **No public resonance sweep from an original card.** No public source found. I searched for 80017A resonance measurements, de-potted measurements, original-against-clone comparisons and open-source projects with original-card captures (section 6).

**Inference.** The MKS-30 window is a ratio of self-oscillation level to VCA test level on the same hybrid, like the JUNO-106's 4.8 Vp-p and 6 Vp-p trims. It cannot be turned into a byte law: the test signals differ and POLY OUT sits after the mix stage. Its value is as a consistency check on the model's self-oscillation amplitude under a second set of Roland conditions, and as confirmation that the resonance trim absorbs chip-to-chip spread.

**Options.**

| Option | What to do | Data needed | Effort | Audible impact | Confidence |
| --- | --- | --- | --- | --- | --- |
| A. Original-card resonance sweep | Run the existing `res_calibrate.mid` on a unit with original 80017As (see 2.6). | One volunteer capture | S for the project | High: resonance character is what the owner reported as weak | High that it closes the gap for that unit |
| B. Read the sibling drawings | Read resonance-network values from clean scans of the JX-3P main board and the MKS-30 voice circuit (and the GR-700's, if it does use the hybrid); compare with the two readings that bracket the compensation coefficient. | Clean scans | S | Low to medium | Low to medium that values transfer |
| C. Check the model against the MKS-30 window | Render the MKS-30 test conditions and compare 250/600 mVp-p. | Gain of the MKS-30 mix stage from [S1] | S | None directly | Low |

#### b. Installed WAVE impedance and bias (OQ-15)

**Evidence found.** No public source found for the MC5534A's internal values or for a WAVE-node reading. The repository already notes the custom chip's internal values are unpublished (`README.md:1944`). I searched for reverse-engineering of the MC5534A and for the Analogue Renaissance wave-generator clone; that maker's pages were unreachable (expired certificate) (section 6). A 2008 circuit walk-through describes the DCO but reports no node measurements [S13].

**Cheapest measurement (inference).** The two solutions the product chooses between are about 4.98 kΩ and about 95.7 kΩ (`README.md:1941`), a factor of 19. A single loaded reading separates them:

- Measure the saw-only AC level at the WAVE node, then again with a known shunt to ground (100 kΩ through a 10 µF film or bipolar capacitor).
- For a source resistance Rs, the level falls to R/(R + Rs): 0.953 (−0.42 dB) for 4.98 kΩ, 0.511 (−5.83 dB) for 95.7 kΩ.
- Also read the node's DC voltage with a 10 MΩ meter with each source on alone and with all off; this fixes the sub's mean and the diode onset that OQ-15 names.

The node is not a pure resistance (D6 and C56 are on it), so do this with the saw alone at a low note and treat the result as choosing a root, not as a final value.

| Option | Effort | Audible impact | Confidence |
| --- | --- | --- | --- |
| Shunt test plus DC reading on the reference unit | S (cover off, ten minutes) | Medium: the root moves the 50 % pulse by about 0.8 dB and sets a −15.5 dBc half-frequency component (`README.md:1941`) | High |

#### c. Tr20 quiet-current parameters (OQ-19)

**Evidence found.** No public source found for a Rohm BA662 data sheet (searched again; the repository already records that none has been located, `README.md:1947`). The MKS-30 drawing shows the same hybrid's VCA control driven through an external transistor and metal-film resistors [S1]; I could not read the values reliably at the scan's resolution, so I record this as a lead only.

**Cheapest capture.** The repository already names it: a gain sweep at low sustain levels on an original card (`README.md:1947`). The existing 56-point sweep was on replacement cards. This belongs in the original-card session (2.6).

#### d. BBD bias and noise split (OQ-03, OQ-04)

**Evidence found.**

- **Theory with measurement.** Buss, Bailey and Eversole measured noise spectra of p-channel BBDs and found agreement with a calculation that includes correlation between noise packets, imperfect transfer and the output circuit [S23] (abstract only). This supports the model class the repository fitted on 2026-10-04 (`README.md:1939`); it gives no MN3009 numbers.
- **Roland's bias procedure fixes symmetry, not noise.** The JUNO-106 notes adjust the two bias trimmers so that positive and negative clipping are equal [S19]; the MKS-30 notes adjust for a centred waveform [S2]. So a serviced unit's bias is set by a clipping criterion. (The "serviced-bias" framing in `YouKnowChorus.h:93` matches this.)
- **A second reading of chorus-on noise by band.** KR-106's noise header tabulates hardware idle levels with and without chorus: +9, +18 and +10 dB at 60, 120 and 240 Hz, and +21 to +33 dB from 1 to 30 kHz [S4]. The header names no unit or chorus mode (the repository already notes this, `README.md:1939`).
- **No public measurement of MN3009 noise against bias.** No public source found (section 6).

**Options (inference).**

| Option | What to do | Data needed | Effort | Audible impact | Confidence |
| --- | --- | --- | --- | --- | --- |
| A. Infer the bias point without touching the trimmer | Drive the chorus with a self-oscillation sine at two or three levels; take the wet component as left minus right; read even against odd harmonics. A symmetric-clipping bias gives low even harmonics at high level. | New capture, no cover off | S | Low to medium (chorus distortion colour on hot patches) | Medium |
| B. Bench split | With the cover off, record idle chorus noise at the trimmed bias and at two offsets either side, then restore the trim by the service procedure. | Scope, signal generator, technician | M | Low | High, but invasive |

#### e. Recording-derived Mode-I chorus fit (OQ-01)

**Evidence found.**

- **A sibling measurement by direct timing.** An analysis of Juno-60 recordings reads 0.513 Hz and 0.863 Hz for modes I and II and a delay sweep of 1.66 to 5.35 ms in both, by aligning the channels and timing matching points on the delayed signal [S16]. This is a Juno-60, a different chorus board, so it is sibling-class comparison only. Its centre (3.5 ms) and half-swing (1.85 ms) are close to the product's 3.38 ms and ±1.978 ms (`README.md:1938`). (The comparison is my arithmetic.)
- **Roland's own software model** has only OFF, I and II chorus settings [S20], as the repository found.
- **No public JUNO-106 clock or delay measurement.** No public source found. A forum thread titled "Detailed values of the Juno-106 chorus" exists but returned HTTP 403, so I could not read it (section 6).

**Cheapest captures.**

| Option | What to do | Data needed | Effort | Audible impact | Confidence |
| --- | --- | --- | --- | --- | --- |
| A. Read the clock directly | Scope or counter on the MN3101 clock output, both modes: minimum and maximum frequency. Delay follows exactly from the part's relation, td = 256/(2·fCP) (`Docs/research/juno-106-fidelity-research.md:846`). | Cover off, scope | S | Medium: replaces a fit that absorbs recording-path effects | High |
| B. Look for the clock in audio (guess) | The product's sweep of 1.40 to 5.36 ms corresponds to a clock of about 91 to 24 kHz (my arithmetic from the same relation). A 192 kHz capture of idle chorus spans all of it; the existing 96 kHz captures span the part below 48 kHz. If any clock residue survives the output filters, its frequency track is the delay law. | Existing captures first, then a 192 kHz idle take | S | Same as A if it works | Low that the residue is above the noise |

---

### 2.3 DCO master reference: resonator drift, and no programmed inter-voice detune

**Current state.** The product ties the shared 8 MHz reference to a digitised Murata CSA8.00MTZ temperature curve as a named stand-in for the installed KMFC1034T1, predicting about +1.0 cent at the model's 40 °C asymptote, with a 3-second warm-up chosen by the user; there is no per-voice DCO detune (`README.md:1882-1890`, `Source/DSP/YouKnowDcoTemperature.h:9-19`, `Docs/research/juno-106-fidelity-research.md:621-690`).

**Evidence found.**

- **The KMFC number is a Roland house part number used across products.** The MKS-30 drawing prints "KMFC1007T31 12MHz" on its CPU clock [S1], the same number the JUNO-106 parts list gives for its 12 MHz resonators (`Docs/research/juno-106-fidelity-research.md:622`). This does not identify the maker. No public source found for a KMFC1034T1 data sheet or drift trace (section 6).
- **Class limits from a second maker agree with Murata's.** Kyocera's KBR series (3.52 to 8.0 MHz) lists ±0.5 % tolerance and ±0.3 % or ±0.5 % temperature stability [S15]. A Kyocera AVX overview puts ceramic resonators at ±2500 ppm overall at best against under ±200 ppm for quartz [S14]. Neither is the installed part; both are limits, not typical warm-up behaviour.
- **Three secondary write-ups call the reference a crystal** [S11][S12][S13]. Roland's parts list says ceramic resonator (`Docs/research/juno-106-fidelity-research.md:621`), so those three are contradicted on that point and should not be used for stability figures.
- **Voices stay in tune with each other.** Electric Druid: "All oscillators remain in tune between voices, even if the master pitch drifts" [S11]. This matches the repository's reading of the architecture.
- **One owner anecdote.** The Sequence 15 author checks his unit's tuning about every eight weeks and attributes the need to temperature and ageing [S13]. No number is given.
- **Siblings do not help.** The MKS-30 and JX-3P use LC master oscillators tuned by an inductor to 442 Hz [S2][S3].

**How others handle it.** KR-106 offers per-voice DCO pitch variance of up to ±30 cents as a user control, with a "Robot Tune" preset that sets every voice to specification [S6]; its architecture note says ±3 cents [S10] (the two disagree). This is a sound-design control that the hardware's shared clock does not have. Roland's own plug-in has a CONDITION knob described only as setting "the state (condition) of the analog sound engine circuit" [S20].

**Options.**

| Option | What to do | Data needed | Effort | Audible impact | Confidence |
| --- | --- | --- | --- | --- | --- |
| A. Drift from existing captures | Fit every steady note's fundamental against the firmware count law across each long capture; the common residual against time is the resonator's drift relative to the interface clock. | The sixteen bank captures | S | Very low (a cent or two, common to all voices) | Medium: gives warm drift only, and the warm-up state at capture start is unknown |
| B. Cold-start take | From cold power-on, record one held note every 30 s for an hour at 96 kHz, interface already warm; log room temperature. Replace the stand-in curve's amplitude with the measured one. | New capture, no cover off | S | Very low | High for that unit |
| C. Bench reference | Frequency counter with a disciplined or temperature-compensated reference on the 8 MHz line from cold; optionally vary +5 V inside its ±0.5 V window for the supply sensitivity the README says is missing (`README.md:1899-1900`). | Cover off, counter | M | Very low | High |
| D. Detune | Keep none. Any detune control would be a labelled extension like KR-106's, not a fidelity fix. | None | n/a | n/a | High |

Options A to C are inference. They rest on one sourced point: an audio interface's quartz clock is roughly an order of magnitude tighter than a ceramic resonator [S14], so the interface can serve as the reference for drift of this size.

**Open questions.** Whether the 3-second software warm-up should stay as product policy once a real curve exists (hardware warm-up is minutes, per the ten-minute rule in the service notes [S19]).

---

### 2.4 No calibrated mains-ripple or shared-rail transient model

**Current state.** The engine applies the M5230L's typical 68 dB rejection to the 120 Hz fundamental of the rated-load reservoir sawtooth: 80 µV peak at the card rail and 0.003 cents peak of cutoff movement (`README.md:1891-1900`, `Source/DSP/YouKnowEngine.h:675-685` and `:2788-2799`, `Source/DSP/YouKnowEngine.cpp:11498-11524`). Harmonics, actual draw, load transients and ground impedance are not modelled. `Docs/decisions.md:2116` records that the 120 Hz component the captures carry with the chorus on is not modelled.

**Evidence found.**

- **Roland prints no ripple figure.** The service notes give only DC checks: +5 V adjusted to within 10 mV, +15 V verified to within 0.8 V [S19].
- **A band table from hardware shows hum rising with the chorus.** KR-106's header gives idle levels at 60, 120 and 240 Hz of −86, −89 and −91 dB dry and −77, −71 and −81 dB with chorus on, against a saw at −8.5 dBFS [S4]. That puts 120 Hz about 80 dB under a saw dry and about 62 dB under it with chorus on (my arithmetic). The unit is not named in the header.
- **KR-106 models it as explicit hum.** Its current source has separate dry and wet ripple constants at 120, 240 and 360 Hz and a comment that the wet ripple was "measured +18 dB above dry at 120 Hz" [S5]; the user guide exposes a MAINS control [S6].
- **The reference unit's owner did not hear that hum on the hardware.** Comparing "silent" chorus output boosted 50 dB, he reported a hum in KR-106 that he did not hear on the Juno, traced to KR-106's MAINS setting [S7]. This is a first-hand listening report, with no levels. It conflicts with the band table above in spirit, so the real level is unresolved.
- **A replacement supply's maker reports no measurements of the original** [S26] (weak; vendor page).
- **No public rail-ripple measurement in a JUNO-106.** No public source found (section 6).

**Inference.**

- A 60 Hz line cannot come from a full-wave rectifier's ripple, whose lowest component is 120 Hz. The 60 Hz row in [S4] therefore points to magnetic or ground-loop pickup, in the instrument or the recording chain. The repository's own note that hum is present with the sources off (`Docs/research/realism-research-2026-10-28.md:168`) fits this.
- The evidence that exists places the larger mains component in the chorus path, not in the card-rail-to-cutoff path the engine models. That is consistent with `Docs/decisions.md:2116`.

**Options.**

| Option | What to do | Data needed | Effort | Audible impact | Confidence |
| --- | --- | --- | --- | --- | --- |
| A. Test the shipped ripple magnitude | On a self-oscillation take at the highest available pitch, measure sidebands at ±120 Hz. The model's 0.003 cents peak predicts about −103 dBc at 1 kHz and about −85 dBc at 8 kHz (my arithmetic: sideband = half the modulation index). Stronger sidebands mean the rejection assumption is optimistic. | Existing self-oscillation takes | S | None unless it fails | High as a test |
| B. Sort the idle lines | For each of 60, 120, 180, 240 and 360 Hz in the idle windows: level by chorus state, left/right phase and coherence. Common-mode even harmonics that scale with the chorus indicate rail or bias coupling into the BBD path; odd harmonics that ignore the chorus indicate pickup. | Existing bank captures; a chain-only take (2.4 C) helps | S | None directly | Medium |
| C. Chain floor take | Record with the synth's VOLUME at zero, then with the cable connected and the synth off, then input shorted. Subtracts the recording chain. | New capture | S | None | High |
| D. Fit the wet 120 Hz component | One coefficient fitted to the bank captures, shipped under the hardware-evidence profile, as was done for the clock-feedthrough thump. | B and C | S to M | Very low (audible only at boosted idle) | Medium |
| E. Bench rails | AC-coupled scope or audio interface (through 10 µF and a 10:1 divider) on ±15 V at the card connector and at the MN3009 supply and bias pins; idle against six held voices; note-on and note-off steps for load transients. | Cover off | S to M | Low | High |

**Open questions.** Whether the chorus-on 120 Hz is rail ripple reaching the BBD bias or supply, or pickup into the high-impedance BBD nodes. Option B's coherence test should separate them.

---

### 2.5 Explicit priors, and mechanisms left out until measured

**Current state.** Shipped as labelled estimates: mixer level coordinates, sub half-wave DC magnitude, resonance onset and compensation, noise control onset, upper cutoff knee, noise distribution. Left out: wet-mute JFET gate capacitance, click and leakage; HPF multiplexer charge injection; converter charge injection; the µPC1252H2's untrimmed even-order distortion; pulse-off residual, node level and loading (`README.md:1901-1916`). The noise source is drawn Gaussian on a central-limit argument (`README.md:579-582`).

**Evidence found, by item.**

- **Wet-mute JFET gate capacitance.** The 2SK30ATM sheet gives input capacitance 8.2 pF typical (VGS = 0, VDS = 0, 1 MHz) and reverse transfer capacitance 2.6 pF typical (VGD = −10 V) [S17]. I read these from a third-party reprint; the scan the repository cites [S18] is the same family sheet, but its capacitance rows were illegible in my text extraction.
  - *Inference (rough bound).* The engine's derived mute and open times are 81.8 and 116.7 ms (`README.md:1955`). A gate moving on the order of 15 V in 0.1 s through 2.6 pF injects about 0.4 nA; across a 10 kΩ node that is about 4 µV. The 15 V swing and 10 kΩ are my assumptions for scale. On typical values the gate-capacitance click is far below audibility, which supports leaving it out and lets the README say so with a number. Leakage and installed spread still need a capture.
- **Multiplexer and converter charge injection.** No public source found for a charge-injection figure for the TC4052, HD14051 or other 4000-series multiplexers; the sheets give capacitances only, as the README says (`README.md:1956`). Analog Devices' tutorial on the subject could not be fetched (section 6).
  - *Inference (cheap bound).* Anything periodic at the 4.2 ms scan that reaches the cutoff hold appears as sidebands at ±238 Hz and its multiples around a self-oscillation tone. The reference unit's description names only the voice cards as replaced (`Docs/validation/hardware-validation.md:153-163`), so its converter, multiplexer and hold circuits are presumably original, and the existing self-oscillation takes can bound hold droop plus charge injection together and be compared with the engine's own droop prediction (`Source/DSP/YouKnowEngine.cpp:11526`).
- **Noise level and control onset.** The MKS-30 drawing labels its noise transistor "NOISE SEL", a selected part feeding a BA662 stage [S1]; the JUNO-106 trims noise to 4 Vp-p with VR32 [S19]. Together these say the raw source amplitude is a selected and trimmed quantity, so no design constant exists to find. An independent bench study of reverse-biased junctions found noise amplitude about 50 % larger at 0 °C and about 50 % smaller at 80 °C, and wide variation between devices [S25]. That is a different device and bias; it suggests the JUNO's noise level moves with temperature, which the engine does not model (inference).
- **Noise distribution.** A Motorola patent's background section states that zener noise "is not Gaussian, and may have a mean value different from its median value" [S24]. That is a general prior-art statement about a different circuit, so it is weak evidence, but it contradicts the certainty of the central-limit argument. Hobbyist reports found in search results disagree with each other (not opened, not cited).
  - *Cheapest check (inference).* Compute skewness, excess kurtosis and the histogram of the existing noise take, and of the engine's render of the same take through the same analysis. The C41/R79 pole pushes any distribution towards Gaussian, so compare like with like.
- **Mixer level coordinates, sub half-wave DC.** Nothing new beyond the MKS-7 notes the repository already uses. The MKS-30's 600 mVp-p VCA level is at POLY OUT in a different gain structure [S2] and does not transfer (inference). The sub's DC mean needs a DC-coupled reading at the WAVE node (2.2 b).
- **µPC1252H2 even-order distortion.** No public measurement found. *Cheapest capture (inference):* hold a self-oscillation sine and step the stored VCA LEVEL byte across its range; the change in second harmonic with that byte alone isolates the common VCA from the voice path.
- **Pulse-off residual.** No public measurement found. *Cheapest capture (inference):* all sources off, filter open, resonance zero, VCA gated; the residue at the note's fundamental against a saw-only reference is the installed bleed.
- **Upper cutoff knee, resonance onset and compensation.** See 2.2 a and 2.6: these need an original card.

**How others handle it.** KR-106 ships these as user-adjustable variance and noise amounts instead of fixed estimates: per-voice VCF, envelope, VCA and pulse-width ranges, plus ANALOG, MAINS and CLOCK noise levels [S6].

**Options.**

| Option | Effort | Audible impact | Confidence |
| --- | --- | --- | --- |
| State the JFET gate-capacitance bound in the README from the data-sheet typicals | S | None | Medium (typical values, assumed swing) |
| ±238 Hz sideband analysis on existing takes | S | None unless large | High as a bound |
| Noise histogram on the existing take | S | Low (noise texture) | High as a test |
| VCA LEVEL sweep on a self-oscillation sine | S (new capture) | Low | Medium |
| Pulse-off bleed take | S (new capture) | Low | High |
| Noise level against warm-up time, from the cold-start take in 2.3 B with a noise note added | S | Low | Medium |

---

### 2.6 Hardware reference scope: no match to original voice cards

**Current state.** All validation recordings come from JUNO-106 #439522, which has Borish replacement voice cards calibrated in 2022; the oscillator chips and chorus board are original (`README.md:2104-2114`, `Docs/validation/hardware-validation.md:153-163`).

**Evidence found.**

- **No public controlled capture from original 80017A cards.** No public source found. Searches for datasets, original-against-clone comparisons with files, raw-waveform packs and open-source projects with original-card captures returned only commercial sample packs with no unit identification, and the randomised four-set comparison the repository already lists (`Docs/research/hardware-recordings.md`).
- **KR-106's hardware contributions all come from the same unit.** In the long comparison thread, no participant other than the reference unit's owner posted hardware recordings [S28]. Further takes from that unit exist beyond those the repository cites: a self-oscillating-filter demo of factory patch A31, identified as serial #439522 [S8], and three takes of a parameter test [S9].
- **A channel for recruiting exists.** In the A31 thread the owner notes that Borish and Analogue Renaissance clones are understood to differ in self-oscillation behaviour and invites owners of the other clone to post the same patch [S8].
- **Roland expects per-chip trimming of the hybrid** [S2], so a "population match" can only mean behaviour that survives the service trims: the shape of the resonance law, the VCA knee, noise beyond the trim, self-oscillation amplitude against frequency, and distortion (inference).

**Options (inference).**

| Option | What to do | Effort | Audible impact | Confidence |
| --- | --- | --- | --- | --- |
| A. Mixed-card unit | Find a unit with some original and some replacement cards and record each voice alone through the existing MIDI suite. Card type becomes the only variable; converter, chorus and recording chain are shared. Repair technicians see such units. | S for the project, needs a volunteer | High | High |
| B. All-original unit | Same suite on a unit with six working originals, same protocol as the reference captures (96 kHz, one fixed gain, documented output). | S, needs a volunteer | High | High for that unit; unit-to-unit spread remains |
| C. Ask on the KR-106 tracker | Post a one-page protocol and the MIDI files where the existing owner community already is [S8]. | S | n/a | Medium that someone answers |
| D. Mine the randomised four-set recordings | Cluster the four unlabelled takes per patch and compare residual spread with the model's. Cannot label which set is original. | M | Low | Low |

**Open questions.** Original cards that still work may be ones that have drifted or been treated; the analogia.pl author reports untouched originals becoming unreliable after about an hour (`Docs/research/hardware-recordings.md:55-56`). A capture should record the card history and the time since power-on.

---

### 2.7 Other items the README lists

The README's open-questions table (`README.md:1936-1958`) names items beyond the six gaps above. I did not research these in depth; this is what the pass turned up.

| Item | New public evidence in this pass | Cheapest closing measurement |
| --- | --- | --- |
| OQ-01 absolute chorus timing, OQ-03 chorus noise | See 2.2 d and e | Clock readout; idle takes by bias |
| OQ-04 loaded post-BBD transfer | None | Wet-only sweep (left minus right) on an identified unit, as the README says |
| OQ-05 output op-amp swing and slew under load | None | Level and frequency sweep at the jack into two known loads |
| OQ-06 output reference value | None | Product decision; pending in `Docs/decisions.md:2223-2227` |
| OQ-07 converter hold timing, OQ-08 intra-pass timing | None | Scope on a hold follower and on `/WR` against TP5; the ±238 Hz sideband analysis gives a partial bound |
| OQ-10 voice dispersion and thermal wander | Noise-source temperature dependence as a lead [S25] | Repeated self-oscillation takes across a warm-up, per card |
| OQ-12, OQ-13, OQ-14 envelope, LFO and portamento timing | None | The repeated held-note LFO take and SysEx dump the repository already asks for (`Docs/research/realism-research-2026-10-28.md:175-184`) |
| OQ-17 volume tracking and output selector | None | Jack-level sweep in the three selector positions |
| OQ-20 wet-mute transient | Gate-capacitance typicals [S17] | Chorus on/off switching take with a steady tone |
| OQ-21 HPF switching | None | HPF position switching take with a steady tone |

"Removed on review" (`README.md:1917-1926`): I found no new evidence for any of the removed mechanisms and propose none of them again.

---

## 3. Evidence that contradicts or weakens a source

- [S11], [S12] and [S13] call the 8 MHz reference a crystal; Roland's parts list says ceramic resonator. Use them for architecture, not stability.
- [S6] and [S10] disagree on KR-106's oversampling default and per-voice pitch variance. I treat the user guide [S6] as current.
- [S4]/[S5] (hum measured in hardware, rising with chorus) and [S7] (the owner hears no such hum on the hardware) pull in different directions. Neither settles the level.
- [S2] and [S3] are OCR text; the three numbers taken from [S2] (250 mVp-p, 600 mVp-p, 1 kHz) should be checked against the page scan.
- [S1] labels are my by-eye reading of a scanned drawing.

---

## 4. Measurement and capture plan

The smallest set that closes the most gaps, in three sessions. Sessions A and C need no cover off.

### Session 0: analysis only (existing captures)

| Analysis | Closes or narrows |
| --- | --- |
| ±120 Hz and ±238 Hz sidebands on the highest self-oscillation tone | Rail-ripple magnitude (2.4); hold droop plus charge injection (2.5) |
| 60/120/180/240/360 Hz idle lines by chorus state, left/right coherence | Where the mains component enters (2.4) |
| Noise-take histogram, skewness, kurtosis, against the engine's render | Noise distribution (2.5) |
| Common DCO pitch residual against elapsed time | Resonator drift, warm (2.3) |
| Spectrogram of idle chorus from 24 to 48 kHz | Clock track, if visible (2.2 e) |

### Session A: audio only (reference unit, MIDI plus interface)

Equipment: the existing interface at 96 kHz, and at 192 kHz if available; the MIDI suite; a thermometer.

1. **Chain floor.** Input shorted; cable connected with synth off; synth on with VOLUME at zero. Thirty seconds each.
2. **Cold start.** Interface warm, synth cold. Power on and record for 60 minutes: every 30 s one held DCO note and one noise-only note. Log room temperature at start and end.
3. **Idle chorus at 192 kHz.** Chorus off, I, II; one minute each, no notes.
4. **Self-oscillation sine with a VCA LEVEL sweep.** Resonance at maximum, sources off, one high note; step the VCA LEVEL byte in eight steps.
5. **Chorus distortion.** The same sine at three levels through chorus I, for the wet component by left minus right.
6. **Pulse-off bleed.** All sources off, filter open, resonance zero, VCA gated, several pitches.
7. **Switching takes.** A steady tone while switching chorus off/I/II and the HPF positions.

### Session B: bench (reference unit, cover off, qualified technician; mains voltages are exposed)

Equipment: oscilloscope with 10× probes, 10 MΩ meter, frequency counter with a stable reference, 100 kΩ resistor and 10 µF non-polar capacitor, service notes.

1. **WAVE node.** DC voltage with each source alone and with all off. Saw-only AC level open, then with the 100 kΩ shunt (expect −0.4 dB or −5.8 dB).
2. **Chorus clock.** MN3101 clock output frequency, minimum and maximum, in modes I and II; TP4 triangle amplitude and period.
3. **Rails.** AC-coupled ripple on ±15 V at a voice-card connector, and on the MN3009 supply and bias pins: idle, six voices held, and at note-on and note-off.
4. **8 MHz reference.** Counter on the divider input from cold for 30 minutes; optionally repeat with +5 V moved inside its service window.
5. **Trimmers.** With power off, in-circuit resistance of VR28 (WIDTH), VR32 (noise) and VR27. The README says a reading of VR28 would pin C61's time constant (`README.md:1943`); VR32 and VR27 are likewise unread positions behind voiced or inferred values (`README.md:1953`, `:1947`). In-circuit readings run low through parallel paths, so note what is connected.
6. **Hold pedestal.** Scope on one cutoff hold follower, triggered on the scan, averaged.

### Session C: original cards (a second unit, audio only)

The existing MIDI suite, each voice alone where the test mode allows, 96 kHz, one fixed gain, output selector and cable documented, ten minutes of warm-up, card history noted. Preferably a mixed-card unit.

---

## 5. Sources

All accessed 2026-11-16. "Already in repo" means the snapshot's documents or source cite it.

| # | Title; author or publisher; date | URL as visited | Type | Repo status | Supports | Locating quote or note |
| --- | --- | --- | --- | --- | --- | --- |
| S1 | MKS-30 schematic sheet, printed p. 9; Roland; Dec. 1984 | https://el34world.com/charts/Schematics/files/Roland/Roland_mks30.pdf | Primary: service document (scanned drawing) | New | 80017A used in MKS-30 with external RESO, GAIN, DC BAL, COF trims and a positor; KMFC1007T31 on the 12 MHz CPU clock; noise transistor marked as selected; LC master oscillators | Image only. Labels read by eye: "KMFC1007T31 12MHz", "NOISE SEL", "VCF/VCA" |
| S2 | MKS-30 Service Notes, First Edition; Roland; undated on the pages read | https://www.manualslib.com/manual/3342573/Roland-Mks-30.html (also `?page=4`, `?page=5`, `?page=11`, `?page=12`) | Primary: service document (OCR text) | New | Resonance trim to 250 mVp-p, cutoff 1 kHz, VCA level 600 mVp-p; per-chip readjustment; BBD bias centred; LC master at about 4 MHz | "should be readjusted whenever the chip is renewed" |
| S3 | JX-3P (PG-200) Service Notes, First Edition; Roland; July 27, 1983 | https://www.manualslib.com/manual/3497592/Roland-Jx-3p.html (also `?page=7`, `?page=8`) | Primary: service document (OCR text). Weak: schematic page not read | New | A VCF RESONANCE step at 600 mV p-p; master tuned by inductor to 442 Hz | "5. VCF RESONANCE" |
| S4 | `KR106AnalogNoise.h` at commit bc15caee; kayrockscreenprinting; April 2026 | https://raw.githubusercontent.com/kayrockscreenprinting/ultramaster_kr106/bc15caee5843ab238a25d0969e68d57db2b1615f/Source/DSP/KR106AnalogNoise.h | Secondary: emulation source carrying hardware measurements | Already in repo (same file, cited via github.com) | Idle band table dry against chorus at 60/120/240 Hz and 1 to 30 kHz; saw −8.5 dBFS | "Wet path: 48.5 dB below signal" |
| S5 | `KR106AnalogNoise.h` on main; kayrockscreenprinting; undated | https://raw.githubusercontent.com/kayrockscreenprinting/ultramaster_kr106/main/Source/DSP/KR106AnalogNoise.h | Secondary: emulation source | New revision of a cited file | Separate dry and wet ripple constants at 120/240/360 Hz | "measured +18 dB above dry at 120 Hz" |
| S6 | ULTRAMASTER KR-106 User's Guide; kayrock.org; undated | https://kayrock.org/kr106/guide.html | Secondary: emulation documentation | New | Oversampling options and default; MAINS control; variance ranges; Robot Tune | "4x (default) gives clean self-oscillation tracking up to 20 kHz." |
| S7 | KR-106 issue #55, "'Silent' Chorus compared to hardware"; lfrancis; 2026-04-26 | https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/55 | Forum (first-hand owner report, no levels) | New | Owner hears a hum in KR-106's silent chorus that he does not hear on the Juno; traced to MAINS setting | "present in the MAINS Noise" |
| S8 | KR-106 issue #24, "Self-oscillating filter variance demo"; lfrancis; 2026-03-30 | https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/24 | Forum (first-hand recording) | New | A31 take from serial #439522; invitation to other clone owners | "uses a self-oscillating filter" |
| S9 | KR-106 issue #23, "KR106 SysEx Param Test from a Juno-106 with Borish clones"; lfrancis; 2026-03-29 | https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/23 | Forum (first-hand recordings) | New | Three further takes from the Borish-card unit | "Take 3:" |
| S10 | KR-106 `DSP_ARCHITECTURE.md` on main; kayrockscreenprinting; undated | https://raw.githubusercontent.com/kayrockscreenprinting/ultramaster_kr106/main/docs/DSP_ARCHITECTURE.md | Secondary: emulation documentation. Weak: disagrees with S6 | New | 2x halfband VCF oversampling; ±3 cents per-voice pitch | "2x oversampled" |
| S11 | "Roland Juno DCOs"; Electric Druid; 2008-05-21 (page also shows 2025-09-18) | https://electricdruid.net/roland-juno-dcos | Secondary: article. Contradicted on "crystal" | New | Shared master clock; voices stay mutually in tune | "All oscillators remain in tune between voices, even if the master pitch drifts." |
| S12 | "The Design of the Roland Juno oscillators"; Stargirl Flowers; 2020-12-31 | https://blog.thea.codes/the-design-of-the-juno-dco/ | Secondary: article. Contradicted on "crystal" | New | DCO architecture described; no drift figure | "The timers are clocked by an 8 MHz crystal oscillator." |
| S13 | "Analyzing the Juno-106 DCO circuit"; Dave Cornutt, Sequence 15; 2008-02-17 | https://sequence15.blogspot.com/2008/02/analyzing-juno-106-dco-circuit.html | Secondary: article with owner anecdote. Contradicted on "crystal" | New (a different post from the one the repo cites) | Owner retunes about every eight weeks; no node measurements | "an uncompensated crystal oscillator" |
| S14 | Ceramic resonator technical overview; AVX/Kyocera; undated | https://kyocera-avx.com/docs/techinfo/CR-TechPaper.pdf | Primary: manufacturer overview (not the installed part) | New | Ceramic against quartz overall tolerance | "+-2500ppm at best" (checked in extracted text) |
| S15 | MHz Band Ceramic Resonators (KBR-/MSB/MSE Series); Kyocera; 2003 by file name | https://ele.kyocera.com/sites/default/files/assets/static/discontinued/kbr_2003_e.pdf | Primary: datasheet (not the installed part) | New | ±0.5 % tolerance; ±0.3 % or ±0.5 % temperature stability, 3.52 to 8.0 MHz | "Excellent temperature stability (±0.3%)" (checked in extracted text) |
| S16 | Juno60 repository, `Chorus/README.md`; Andy Harman (pendragon-andyh); 2015 by README notice | https://github.com/pendragon-andyh/Juno60/blob/master/Chorus/README.md | Primary: measurement from recordings (Juno-60, sibling) | New | 0.513 and 0.863 Hz; 1.66 to 5.35 ms sweep; method | Named recording "Juno 60 PulseChorus2.wav" |
| S17 | 2SK30ATM data sheet; Toshiba text, reprinted by Tiger Electronic Co.; undated | https://www.retroamplis.com/WebRoot/StoreES2/Shops/62070367/MediaGallery/Datasheets/2SK30A_-_TGS.pdf | Primary: datasheet (third-party reprint) | New | Ciss 8.2 pF typ, Crss 2.6 pF typ, with conditions | "Reverse transfer capacitance Crss" (checked in extracted text) |
| S18 | 2SK30A data sheet scan; Toshiba; undated | https://amptone.pl/templates/images/files/4686/1710237661-2sk30a-toshiba-6465.pdf | Primary: datasheet | Already in repo | Same family sheet; capacitance rows illegible in my extraction | No usable text for those rows |
| S19 | JUNO-106 Service Notes, full OCR text; Roland; first edition 1984 | https://archive.org/stream/synthmanual-roland-juno-106-service-notes/rolandjuno-106servicenotes_djvu.txt | Primary: service document (OCR text) | Already in repo (same document, different host) | DC supply checks only, no ripple figure; ten-minute warm-up; chorus bias by clipping symmetry; noise 4 Vp-p | "Allow at least 10 minutes for warmup period" |
| S20 | JUNO-106 Software Synthesizer Owner's Manual, 01A; Roland; 2017 | https://www.rolandcloud.com/getmedia/3d48e0af-afa3-4368-a68b-6d297cab4625/JUNO-106-Manual-E.pdf?ext=.pdf | Secondary: emulation documentation | New | CONDITION knob; chorus OFF, I, II only | "Specifies the state (condition) of the analog sound engine circuit" (checked in extracted text) |
| S21 | "Antiderivative Antialiasing for Stateful Systems"; Martin Holters; DAFx-19, 2019-09-02 to 06 | https://dafx.de/paper-archive/2019/DAFx2019_paper_4.pdf (found via https://dafx.de/paper-archive/details/lp5GcJOo79OBkfcMfN3N_g) | Primary: paper | New | Extension of the method to stateful systems; test circuits | "it is extended to a class of stateful systems" (checked in extracted text) |
| S22 | "Antiderivative Antialiasing with Frequency Compensation for Stateful Systems"; La Pastina and D'Angelo; DAFx20in22, 2022 | https://dafx.de/paper-archive/details/vhT2Sz-JpSJ4Br8g236cRg | Primary: paper (abstract page only) | New | Standard methods alter frequency response and can destabilise stateful systems | Abstract paraphrased by the fetch tool; no verbatim quote kept |
| S23 | "Noise in MOS bucket-brigade devices"; Buss, Bailey, Eversole; IEEE Trans. Electron Devices 22(11), 977–981, 1975 | https://search.isc.ac/Inventory/49/1051125.htm (DOI https://doi.org/10.1109/T-ED.1975.18256 redirected to IEEE Xplore, not opened) | Primary: paper (index record and abstract only) | New | Measured BBD noise spectra agree with a correlated-packet calculation | DOI 10.1109/T-ED.1975.18256 on the record |
| S24 | US 4,853,884, "Random number generator with digital feedback"; Brown, Danielsen, Dabbish (Motorola); granted 1989-08-01 | https://patents.google.com/patent/US4853884 | Primary: patent background statement. Weak for the JUNO | New | Zener noise described as non-Gaussian | "the zener diode noise output is not Gaussian" |
| S25 | "Avalanche Noise Source Design"; betrusted.io; undated | https://betrusted.io/avalanche-noise | Primary: bench measurement (different devices) | New | Noise amplitude about ±50 % from 0 to 80 °C; wide device variation | Measurement floor "around 70mVp-p" |
| S26 | "Supernova Power Supply for the Roland Juno-106"; Alex, Plasma Music; 2021-01-01, updated 2024 | https://plasmamusic.com/supernova-power-supply-for-the-roland-juno-106 | Secondary: vendor page. Weak | New | No measurements of the original supply are given | "There's NO mains hum either" |
| S27 | "Juno filter analysis"; Joakim, A to Synth; 2017-10-24 | https://atosynth.blogspot.com/2017/10/juno-filter-analysis.html | Secondary: simulation write-up | New (a different post from the one the repo cites) | 240 pF (Juno-6/60) against 270 pF (clone) | "simulated the circuit in LT-spice" |
| S28 | KR-106 issue #44, "2.4.9 comparison to Juno 106, banks A1x and A2x"; lfrancis; opened 2026-04-07 | https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/44 | Forum (first-hand recordings) | Already in repo | No other hardware owner contributed recordings | "Juno 106 w/Borish clones" |

---

## 6. Search log

Counts: 48 web searches and 62 page fetches (about a dozen fetches failed).

**Queries that found something used above**

- Electric Druid Juno DCO; Juno DCO design articles → S11, S12, S13.
- pendragon-andyh Juno60 chorus → S16.
- KR-106 repository: issue lists, issues #23, #24, #44, #55, #57, noise header, architecture note, user guide → S4 to S10, S28.
- Roland MKS-30, JX-3P and GR-700 service notes; el34world Roland index → S1, S2, S3.
- 2SK30A capacitance → S17, S18.
- Kyocera ceramic resonators → S14, S15.
- Holters antiderivative antialiasing; DAFx archive → S21, S22.
- Buss, Bailey, Eversole BBD noise → S23.
- Avalanche and zener noise distribution (two queries) → S24, S25.
- Roland Cloud JUNO-106 manual (CONDITION, Circuit Mod) → S20.
- JUNO-106 service notes full text (archive.org) → S19.
- Juno filter analysis (atosynth) → S27.

**Queries that found nothing usable**

- "KMFC1034T1" / "KMFC1007T31": no data sheet, no maker.
- JUNO-106 master-tune drift, cold-start tuning, owner frequency-counter readings (three queries, one extended): no measurements.
- JUNO-106 power-supply ripple, M5230L ripple and hum, hum only with chorus (three queries): no measurements; one forum thread with no numbers (GroupDIY, opened, not used).
- Original 80017A resonance measurements; de-potted 80017A values (extended); original against clone with files (extended): nothing beyond what the repository has.
- MC5534A reverse engineering; Analogue Renaissance wave-generator clone: nothing.
- Rohm BA662 data sheet: nothing.
- JUNO-106 chorus clock or delay measured on hardware (two queries, one extended): nothing readable; search snippets said a Eurorack clone (SoundForce Chorus 6) was built by sampling a JUNO-106's BBD driver signals. I did not open a primary page for that, so it is an unverified lead, not a finding.
- JUNO-106 research datasets; raw-waveform packs from original cards; open-source emulations with original-card captures: commercial packs only.
- CD4052, HEF4052, TC4052 charge injection: no figure.
- MN3007/MN3009 noise against bias: nothing beyond theory.
- Positor part "ERS83G561" (read from S1): no data sheet.
- Borish replacement card design: nothing.
- Softube Model 84, Cherry Audio DCO-106, TAL-U-NO-LX, Arturia Jun-6 V documentation on drift, variance or calibration: marketing statements only; not used.
- Behringer JN-80: a Juno-60 clone; not relevant.

**Pages that could not be opened**

- analoguerenaissance.com pages for the 80017A clones (expired certificate); web.archive.org copies (blocked for this tool).
- Gearspace thread "Detailed values of the Juno-106 chorus" (HTTP 403).
- Roland support article on plug-in chorus hiss (HTTP 403).
- Analog Devices MT-088 tutorial (connection refused, then timeouts).
- synfo.nl service-manual directory listing (HTTP 403).
- electrosmash.com MN3007 article (host not found).
- scilit.com record for S23 (HTTP 403); IEEE Xplore page for S23 (redirect not followed).
- github.com/joakimskurk/fm1juno and its pull request #2 (HTTP 404).

**Opened but not relied on**

- github.com/chubbard/Juno-XD (chorus parameters with no stated source).
- Synthtopia 2019 note on a chip-replacement demo (no measurements).
- softsolder.com post on a ceramic resonator in a clock (no temperature data).
- ManualsLib MKS-30 owner's manual page (no service content).
