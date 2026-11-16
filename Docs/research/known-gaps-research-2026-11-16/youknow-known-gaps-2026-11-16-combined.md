# YouKnow known gaps: combined research report

| Field | Value |
| --- | --- |
| Date | 2026-11-16 |
| Question | How can each of YouKnow's documented known gaps be addressed: what public evidence could close or narrow it, how others handle it, and what is the cheapest credible measurement where no evidence exists? |
| Product | YouKnow 1.4.9, protocodus/virtual-instrument-youknow, commit `6b92c518355ee6c22ee4fc66936835544909555e` |
| Brief | `youknow-known-gaps-2026-11-16-prompt.md` (identical for all three researchers) |
| Researchers | **C** = Claude, completed, `youknow-known-gaps-2026-11-16-claude.md`. **X** = Codex at effort `ultra`, completed, `youknow-known-gaps-2026-11-16-codex.md`. **G** = Gemini at high effort through the Antigravity CLI: stopped by the Google account's usage quota after 11 minutes, no report; its searches and candidate URLs are kept in `youknow-known-gaps-2026-11-16-gemini.md`. |
| Merge | Claude, coordinating session |

## How to read this

- **Citations** trace back to each researcher's own list: `[C-S3]` is S3 in `youknow-known-gaps-2026-11-16-claude.md`, `[X-S5]` is S5 in `youknow-known-gaps-2026-11-16-codex.md`, `[G]` is a candidate URL in `youknow-known-gaps-2026-11-16-gemini.md`. `R12` is row 12 of the source register (`youknow-known-gaps-2026-11-16-sources.md`), which holds every URL from all three files with who cited it, a link check and an archive snapshot.
- **Status** of each claim or recommendation:
  - **agreed:** Claude and Codex both make it, and a cited source or the snapshot supports it
  - **single:** one researcher makes it; its support is described
  - **disputed:** the sources or researchers conflict; what would settle it is given
  - **unsupported:** the cited source does not resolve or does not say this
- **✔ checked** means I opened the source during the merge and found the statement in it. **◇ not re-checked** means I could not open it (blocked, image-only) or did not check it; the researcher's own reading stands, flagged.
- Gemini contributed no findings, only candidate sources. Where this report says "both", it means Claude and Codex.

## 1. Summary: what to do first

| # | Action | Status | Who | Effort | Why |
| --- | --- | --- | --- | --- | --- |
| 1 | **Settle the WAVE-node source prior.** First a dry-audio falsification test (pulse-only with SUB off, saw-only, SUB-only, mixed at partial SUB), then one bench reading: DC at the node with each source alone, and the saw level with and without a known shunt. | agreed | C #2, X #1 | S audio, S–M bench | The two shipped roots, about 4.98 kΩ and about 95.7 kΩ, both fit the SUB-only calibration but predict different mixes; the selected root adds a −15.5 dBc f₀/2 component until WAVE-node captures settle it (`README.md:1941`, ✔ checked). Claude computes that a 100 kΩ shunt drops the level by −0.42 dB for one root and −5.83 dB for the other, so one reading picks the root [C §2.2b]. |
| 2 | **Get one documented original-card capture session**, ideally on a unit with mixed original and replacement cards, then a second unit held out. | agreed | C #3, X #2 | M; needs a volunteer unit | Neither researcher found any public labelled, calibrated original-80017A dataset. Leads: the KR-106 issue tracker, where the reference unit's owner already posts takes [C-S8, R22], and replacement makers who say they measured originals, for example MIDI Hardware: "just pure measurement of the original" [X-S15, R35, ✔ checked]. |
| 3 | **Measure the chorus directly:** MN3101 clock extremes in modes I and II, LFO, chorus input and both wet returns, captured together. | agreed | C #2, X #3 | S (clock only) to M (full) | The delay law follows from the clock, so it replaces a recording-derived fit. Holters and Parker found a recording-inferred clock trajectory off by up to 0.13 ms on a Juno-60, and +2.3 dB BBD insertion gain [X-S18, R60, ✔ checked]; their page has wet-only Juno-60 recordings "prior to mixing with the original signal" [X-S19, R59, ✔ checked]. |
| 4 | **Run an analysis-only pass on captures already in hand:** ±120 Hz and ±238 Hz sidebands around a high self-oscillation tone, idle hum lines by chorus state and left/right coherence, the noise-take histogram, and common DCO pitch against elapsed time. | single | C #1 | S, no hardware time | Tests four shipped assumptions (rail-ripple magnitude, hold droop plus charge injection, noise distribution, resonator drift) with no new recording. Codex proposes no reuse of existing captures. Gemini found one more hardware take from the same owner that the repo docs don't cite yet: `osc_test_bip.aif` [G, R31; ✔ link serves an AIFF]. |
| 5 | **Close the software-only numerical gaps:** publish a Normal-vs-Max accuracy envelope, build the missing chorus support caches off the audio thread instead of falling back, and give low host rates a minimum internal rate. | agreed | C #5, X #4 | S–M | No hardware needed. The fallback is an explicit memory trade in `YouKnowChorusRateIsland.h:31–43` [X-S7]. Codex also proposes splitting the nonlinear-current integration at BBD events (single, X). |
| 6 | **Add output and load sweeps** to the bench session: jack level and frequency into known loads, all selector and volume positions. | agreed (priority differs) | X #5; C table §2.7 | S–M increment | Covers OQ-05, OQ-06 and OQ-17; Codex ranks it top five, Claude lists it as a cheap add-on. |
| 7 | **Read sibling Roland service documents:** the MKS-30 (same 80017A hybrid) and JX-3P notes. | single, ◇ not re-checked | C #4 | S | Claude reports an MKS-30 factory trim window (self-oscillation 250 mVp-p, VCA level 600 mVp-p at POLY OUT) and a note that the hybrid "should be readjusted whenever the chip is renewed" [C-S2, R65]. ManualsLib refuses automated access and the archive copies hold no page text, so I could not confirm these numbers; Claude itself says to check them against the scan. |

Agreement table (section 3) counts: 13 agreed, 4 single, 1 disputed, none unsupported. Separately, five of Gemini's candidate URLs return 404 (section 5).

## 2. Gap by gap

### 2.1 Numerical accuracy is still rate-dependent

Both researchers read the current state the same way: Normal runs the local VCF and chorus stages at half the reference rate at 44.1/48 kHz, low host rates and uncached chorus settings keep older limits, and the owner heard no Normal/Max difference in the 2026-10-16 audition (`README.md:1863–1873`, `Docs/decisions.md:1842–1884`).

| Claim or option | Status | Detail |
| --- | --- | --- |
| No hardware evidence applies; this is a numerical question | agreed | [C §2.1], [X §2] |
| Blanket ADAA inside the OTA feedback is still not justified | agreed | Zheleznov and Bilbao favour linear interpolation for stateful systems [X-S21, R12]; Holters [C-S21, R10] and La Pastina and D'Angelo [C-S22, R14] extend or correct ADAA for stateful systems, but neither shows a resonant four-stage OTA filter. |
| Publish an accuracy envelope (Normal, Max, offline reference) | agreed | C option A; X row 4. S effort, no sound change. |
| Fill the chorus support cache off the audio thread | agreed | C option D; X row 1. Codex notes the code avoids two extra 57 KiB caches per Rack instance [X-S7]. |
| Minimum internal rate for low host rates | agreed | C option B (resample from at least 44.1 kHz); X row 2 (declared minimum circuit bandwidth). |
| Promote a voice to the Max grid only where Normal's pole cap binds | single (C) | Medium confidence; switching must stay inaudible. |
| Split nonlinear-current integration at actual BBD events | single (X) | The README already names this residual. |
| How others do it | single each | KR-106 offers Off/2x/4x VCF oversampling, 4x default [C-S6, R27]; its architecture note says 2x, so the two KR-106 documents disagree [C-S10, R41]. Diva separates real-time and offline accuracy [X-S22, R48, ✔ checked: "Accuracy and OfflineAcc"]. |

### 2.2 Estimates still open

**a. Original-card resonance versus byte (OQ-09).** Agreed that no public original-card resonance sweep exists [C §2.2a], [X §3.1]. New corroboration of the mechanism: AN701 describes adding the filter's input to the resonance amplifier's input and footnotes it as used in the Juno-6, 60 and 106 [X-S13, R72, ✔ checked]; it gives no coefficient. Claude's MKS-30 trim window and per-chip readjustment note [C-S2] are single and ◇ not re-checked. Claude also found a simulation giving 240 pF (Juno-6/60) against 270 pF (clone) [C-S27, R7], consistent with the repo's settled OQ-18. Closing step, agreed: a dense resonance sweep at several cutoffs on an original card, with alternating bytes held out (X), using the existing `res_calibrate.mid` (C).

**b. Installed WAVE impedance and bias (OQ-15).** Agreed that nothing public gives the MC5534A's internal values or a WAVE-node reading. Both make it their top or second action (section 1, row 1). Codex adds a caution, single: a single resistor may not describe the node if its impedance depends on waveform state, so identify the terminal behaviour before forcing either root. Claude adds the DC readings that also fix the sub's mean and the diode onset.

**c. Tr20 quiet-current parameters (OQ-19).** Agreed that no BA662 data sheet or installed low-current curve is public, and that the closing step is a low-level gain sweep on an original card. Codex found a weak measurement: a blog correcting the VCA control maximum to 0.3 mA [X-S12, R45, ✔ checked: "Max current is not 0.5mA but 0.3mA"], with no unit or uncertainty, plus Open Music Labs' BA662 reverse-engineering account [X-S14, R46]. Claude's lead, an external transistor on the MKS-30 drawing [C-S1, R18], is unreadable at the scan's resolution (single, ◇).

**d. BBD bias and noise split (OQ-03, OQ-04).**
- Agreed: no public measurement of MN3009 noise against bias.
- Single (C): Roland's chorus bias procedure sets waveform symmetry, not noise. The service-note OCR asks for positive and negative halves "symmetrical with respect to the center horizontal line" [C-S19, R6, ✔ checked in substance; Claude's word "clipping" isn't in the OCR text].
- Single (C): BBD noise theory with measurements, Buss, Bailey and Eversole 1975 [C-S23, R43, R17] (abstract only).
- Single (X): the MN3009 noise maximum conflicts between two Panasonic sheets, 0.20 versus 0.15 mVrms. This is already recorded in the repo, and Codex could not resolve it (one scan is image-only, the other is too large to retrieve) [X-S36, X-S37].
- Options:
  - C: infer the bias point from even against odd harmonics of a sine through the chorus, without touching the trimmer.
  - X: measure harmonics and noise separately at the installed bias, bin long idle captures by clock phase, and measure the input-to-wet transfer before the summer.
  - Both are compatible.
  - Alpes Machines' clone kit shows a practical test-point layout [X-S34, R50].

**e. Recording-derived Mode-I chorus fit (OQ-01).** Agreed: no public Juno-106 clock or delay measurement exists, and the fix is to read the clock directly (section 1, row 3). Sibling data, single (C): a Juno-60 analysis reads 0.513 Hz and 0.863 Hz with a 1.66 to 5.35 ms sweep [C-S16, R26, ✔ checked], close to the product's 3.38 ms ± 1.978 ms. Claude's guess: clock residue might be visible in a 192 kHz idle capture. It labels this low confidence.

### 2.3 DCO master reference: resonator drift, no inter-voice detune

| Claim | Status | Detail |
| --- | --- | --- |
| No public data sheet or drift trace exists for the installed KMFC1034T1 | agreed | [C §2.3], [X §4] |
| A generic curve can't stand in for the installed part | agreed | Murata: oscillation depends on the IC and loading [X-S24, R36]. Kyocera: ceramic resonators manage about ±2500 ppm overall at best, against under ±200 ppm for quartz [C-S14, R30, ✔ checked]. |
| Don't add per-voice detune as a fidelity fix | agreed | C option D ("keep none"); X: "no evidence-based reason here to add six independent DCO pitch walks". Kiwi-106's detune is an upgrade feature [X-S25, R62]. Electric Druid: "All oscillators remain in tune between voices, even if the master pitch drifts" [C-S11, R20, ✔ checked]. |
| Three secondary articles call the reference a crystal | single (C), contradicted | Roland's parts list says ceramic resonator (repo `juno-106-fidelity-research.md:622`; Codex quotes the Kiwi-hosted service manual [X-S9, R63]). Electric Druid's "crystal" wording is ✔ checked; treat those articles as architecture only [C-S11, C-S12, C-S13]. |
| Closing step: a cold-start capture against a stable reference | agreed | C: an audio-only take, one held note every 30 s for an hour, with the interface as the reference, plus a bench counter option. X: the buffered clock against a calibrated timebase over repeated cold starts, recording per-voice self-oscillation in parallel to separate the analog drift. |
| Extract warm drift from the existing long captures first | single (C) | S effort, no hardware. |

### 2.4 Mains ripple and shared-rail transients

| Claim | Status | Detail |
| --- | --- | --- |
| No installed rail-ripple, current or transient measurement is public | agreed | [C §2.4], [X §5] |
| The 68 dB M5230L figure is a typical value at one test condition, not an installed bound | agreed | [X-S26, R67]; README says the same. |
| Hardware idle hum rises with the chorus: +9, +18 and +10 dB at 60, 120 and 240 Hz | single (C) | KR-106's noise header gives a dry and chorus-on band table [C-S4, R39, ✔ checked]; its current source models wet ripple "measured +18 dB above dry at 120 Hz" [C-S5, R40, ✔ checked]. The unit isn't named. |
| The reference unit's owner doesn't hear that hum on the Juno | **disputed** | Issue #55: "an unexpected hum in the KR output that I don't hear on the Juno", traced to KR-106's MAINS noise [C-S7, R24, ✔ checked]. This conflicts with the band table in spirit. **Settles it:** Claude's idle-line analysis by chorus state and coherence, plus a chain-floor take (Session 0 and A). |
| Degraded supply capacitors and regulator pass transistors let ripple and dynamic loads reach the supply lines | single (X) | Analogue Renaissance's AR80017A page, under "Drifting along with the supply lines" [X-S27, R2, ✔ checked via the archive copy]. Qualitative, from a maker with a commercial interest. |
| A 60 Hz line can't be full-wave rectifier ripple, so it points to pickup | single (C, inference) | Fits the repo's note that hum is present with sources off. |
| Options | agreed in direction | C: test the shipped 0.003-cent magnitude with ±120 Hz sidebands (about −103 dBc at 1 kHz predicted), sort the idle lines, take a chain-floor recording, fit the wet 120 Hz component, then bench the rails. X: synchronized rail, current, ground and audio capture, then controlled small perturbations. Claude's steps are cheaper and come first. |

### 2.5 Explicit priors and omitted mechanisms

| Item | Status | Detail |
| --- | --- | --- |
| Wet-mute JFET capacitances: Ciss 8.2 pF, Crss 2.6 pF typical | agreed | Claude from a reprint [C-S17, R68, ✔ checked in the PDF text]; Codex from a 2007 Toshiba sheet [X-S33, R77, ◇ refuses automated checks]. Both treat them as typicals, not installed values. |
| The gate-capacitance click is far below audibility (about 4 µV, rough bound) | single (C, inference) | Uses an assumed 15 V swing and a 10 kΩ node. Codex instead proposes a sensitivity study, then a switching capture. Compatible: state the bound, capture if it matters. |
| No charge-injection figure exists for the HPF multiplexer or the converter | agreed | Codex cites ADI's measurement method, Q = C_L·ΔV [X-S29, R51, ◇ analog.com refused the connection]. Claude proposes bounding hold droop plus injection from the ±238 Hz sidebands on existing takes. |
| µPC1252H2 even-order distortion: no public untrimmed measurement | agreed | Both propose a sweep of the common VCA across level and VCA LEVEL. Codex: NEC's THD test minimizes distortion with a trim [X-S30, R33, ◇ 403], and Akai's GX-747 uses a symmetry trim for H2 [X-S31, R42]. |
| Noise distribution | agreed on the test, single on the evidence | Both propose measuring amplitude quantiles. Claude adds weak evidence against a Gaussian assumption (a patent background says zener noise "is not Gaussian") [C-S24, R37], and that junction noise can move about ±50 % over 0 to 80 °C [C-S25, R8]. |
| Pulse-off residual, mixer coordinates, sub DC | agreed | All three are folded into the WAVE session (section 1, row 1). |
| Upper cutoff knee, resonance onset | agreed | They need an original card (2.2a, 2.6). Codex notes the 64 kHz/1.7 pair in `YouKnowEngine.h:1638–1675` is coupled and voiced [X-S8]. |
| How others do it | single (C) | KR-106 exposes these as user-adjustable variance and noise amounts [C-S6, R27]. |

### 2.6 Hardware reference scope

| Claim | Status | Detail |
| --- | --- | --- |
| No public labelled, calibrated original-card dataset exists | agreed | [C §2.6], [X §7] |
| The existing four-set comparison is useful listening evidence but randomized | agreed | Already in the repo [X-S16, R32]. |
| All KR-106 hardware takes come from the same Borish-card unit, and more of its takes exist than the repo cites | single (C) | Issues #23, #24 and #44 [C-S8, C-S9, C-S28, R21–R23]. Gemini adds `osc_test_bip.aif` [G, R31]. |
| Roland expects the hybrid to be re-trimmed per chip, so a "population match" means behaviour that survives the service trims | single (C) | Rests on [C-S2], ◇ not re-checked. |
| Replacement-card revisions differ, so record card type and revision with every capture | single (X) | [X-S27, R2] |
| Plan: a mixed-card unit first (C), or borrow one original unit and keep a second one held out (X) | agreed in substance | Both say one unit establishes that unit, not a population. |

### 2.7 The other open questions in the README

Codex covered all 21 OQ rows (`youknow-known-gaps-2026-11-16-codex.md` §8, with a coverage check). Claude covered them in one table (§2.7). They don't conflict. Codex adds detail on OQ-05 (loaded TA75558S clipping and slew), OQ-07 (hold acquisition), OQ-08 (serial, PIT and mux timing; MAME's driver is a skeleton [X-S35, R25]) and OQ-13 (minimum LFO rate, verifiable with an S-effort capture). Neither researcher found new evidence to revive anything on the "Removed on review" list.

## 3. Agreement table

✓ found it, ✗ contradicts it, – silent.

| Claim or recommendation | Claude | Codex | Gemini | Status |
| --- | --- | --- | --- | --- |
| WAVE-node test is a top priority | ✓ | ✓ | – | agreed |
| One documented original-card session | ✓ | ✓ | – | agreed |
| Measure the chorus clock and wet path directly | ✓ | ✓ | – | agreed |
| Reuse existing captures before new hardware time | ✓ | – | – | single |
| Publish an accuracy envelope; fill the support cache | ✓ | ✓ | – | agreed |
| Split BBD-event integration | – | ✓ | – | single |
| Output and load sweeps | ✓ (minor) | ✓ (top five) | – | agreed |
| MKS-30 and JX-3P documents as new evidence | ✓ | – | – | single, ◇ |
| No public KMFC1034T1 drift data | ✓ | ✓ | – | agreed |
| Don't add inter-voice detune | ✓ | ✓ | – | agreed |
| No public rail or ripple measurement | ✓ | ✓ | – | agreed |
| Hardware hum rises with the chorus | ✓ (KR-106 table) | – | – | disputed by [C-S7] |
| 2SK30A typical capacitances 8.2 / 2.6 pF | ✓ | ✓ | – | agreed |
| No charge-injection or untrimmed VCA data | ✓ | ✓ | – | agreed |
| Noise distribution needs measuring | ✓ | ✓ | – | agreed |
| No public original-card dataset | ✓ | ✓ | – | agreed |
| Analogue Renaissance as a source | – | ✓ | ✓ (searched it) | single, ✔ |
| Juno-60 chorus timing as sibling data | ✓ (pendragon) | ✓ (Holters/Parker) | ✓ (searched DAFx) | agreed, different sources |

## 4. Merged measurement and capture plan

The cheapest order. Sessions 0 and A need no cover off.

**Session 0: analysis only (existing captures)** (Claude)
1. ±120 Hz and ±238 Hz sidebands on the highest self-oscillation tone, for rail ripple and hold droop plus charge injection.
2. Idle lines at 60, 120, 180, 240 and 360 Hz by chorus state, with left/right coherence, to find where the hum enters.
3. Noise-take histogram, skewness and kurtosis, against the engine's render of the same take.
4. Common DCO pitch residual against elapsed time, for warm drift.
5. Add `osc_test_bip.aif` [G, R31] to the evidence index, after checking its provenance.

**Session A: audio only (reference unit #439522)** (Claude A, plus Codex's dry source matrix)
1. Chain floor: input shorted; synth off; synth on with VOLUME at zero.
2. Dry source matrix with exact SysEx: pulse-only with SUB off, saw-only, SUB-only, partial-SUB mixes, pulse-off transitions, at several pitches (X).
3. Cold start: one held DCO note and one noise note every 30 s for an hour, logging the temperature.
4. Idle chorus at 192 kHz in Off, I and II.
5. A self-oscillation sine with a VCA LEVEL sweep, and the same sine at three levels through chorus I.
6. Noise-only takes at zero, low, mid and full NOISE bytes (X).
7. Switching takes: chorus Off/I/II and the HPF positions under a steady tone. Repeated self-oscillation starts (X).

**Session B: bench, cover off, qualified technician (mains voltages are exposed)** (both)
1. WAVE node: DC with each source alone and all off; saw level open and with the 100 kΩ shunt; then a small controlled load across signal states (X).
2. Chorus: MN3101 clock min/max in I and II, the TP4 triangle, chorus input and both wet returns together (X), and the bias trimmers left as found unless a service adjustment is needed. If one is, document the before and after.
3. Rails: AC ripple at the card connector and at the MN3009 pins, idle, six voices held, and on note on/off; actual current if a probe is available (X).
4. The 8 MHz reference on a counter from cold.
5. Trimmer resistance readings (VR28, VR32, VR27) with power off.
6. A hold pedestal on one cutoff follower.
7. Common VCA and IC6 into known loads, across the selector and volume positions (X).

**Session C: original cards** (both)
- The same MIDI suite on a unit with original cards, ideally mixed original and replacement, with card history and time since power-on noted.
- Then a second unit, held out for validation.

**Analysis rules** (X): one fixed capture-chain gain, no per-patch EQ; fit terminal behaviour first; separate harmonics, broadband noise, hum and clock components; require improvement on held-out takes.

## 5. Source register

All 77 unique URLs from the three files are in `youknow-known-gaps-2026-11-16-sources.md` (and `youknow-known-gaps-2026-11-16-sources.json`), each with who cited it, today's link status and the closest Internet Archive snapshot. Summary:

- **Cited by:** Claude 30 URLs (its 28 sources; two have a second URL), Codex 31, Gemini 18 candidates. Only two URLs appear in more than one file: the 2SK30A scan (Claude and Codex, R1) and Analogue Renaissance's AR80017A page (Codex and Gemini, R2). The researchers mostly found different sources for the same conclusions.
- **Link check:**
  - 64 of 77 URLs resolve today.
  - Five refuse automated checks (ManualsLib ×2, manuals.plus, Toshiba, TME) and should be opened by hand.
  - Analogue Renaissance's HTTPS certificate is self-signed; the archive copy works.
  - analog.com dropped the connection.
- **Gemini's candidates:**
  - Five return 404: `dangelo.audio/bbd-blep`, three DAFx paths (2010, 2018, 2022) and a Softube page. These look like wrong paths taken from search snippets, so treat them as unverified.
  - The lewisfrancis.com `.aif` links answer HEAD with an HTML type but serve real AIFF audio on GET.
- **Archive:** 43 URLs have an Internet Archive snapshot. No new snapshots were requested: asking the archive to save the other 34 would publish their URLs, so I'd do that only on your say-so.
- **Repository sources:** Codex's S1–S8 and Claude's inline references are files in the pinned snapshot (README, research docs, decisions log, `YouKnowChorusRateIsland.h`, `YouKnowEngine.h`). Codex's links point to the temporary snapshot path; the same files are at commit `6b92c51` in the repo.
- **Sources marked weak by their own researcher** are kept in the files, with the reason: KR-106's architecture note, the three "crystal" articles, the Plasma Music vendor page, the patent background statement, and the sinneb blog.

## 6. Method and run statistics

| | Claude | Codex | Gemini |
| --- | --- | --- | --- |
| Route | Claude Code subagent | `codex exec` 0.167.0, read-only, live web search | Antigravity CLI `agy` 1.5.9, `--sandbox` |
| Effort | default | `ultra` (3 delegated agents) | high |
| Wall time | 26 min 7 s | 17 min 8 s | 10 min 54 s, then quota stop |
| Tokens | 376,869 | 10.15 M (9.60 M cached input), 54 K output | 401 K (4.35 M cache reads) |
| Searches / page reads | 48 / 62 (13 failed) | 10 searches and 17 opens in the main agent; the delegated agents' counts aren't reported | 15 searches |
| Sources | 28 | 37 (8 local) | 18 candidates |
| Plan cost | Claude plan usage | ChatGPT Pro weekly Codex limit 43 % → 46 % | Individual quota exhausted; resets around Sat 21 Nov 07:51 UTC |
| Status | completed | completed | stopped, partial record |

Merge checks: I opened 16 sources and the snapshot README to confirm the claims this report relies on (the ✔ marks). Five are marked ◇: four would not open for an automated check (ManualsLib, TME, analog.com, manuals.plus), and the MKS-30 drawing is an image scan I did not read.
