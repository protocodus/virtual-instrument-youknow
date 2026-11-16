<!-- Run header added by the coordinator. The report below it is Codex's final message, unedited. -->

| Field | Value |
| --- | --- |
| Date | 2026-11-16 |
| Question | How can each of YouKnow's documented known gaps be addressed? (brief: `youknow-known-gaps-2026-11-16-prompt.md`) |
| Product | YouKnow 1.4.9, commit 6b92c518355ee6c22ee4fc66936835544909555e |
| Researcher | OpenAI Codex CLI 0.167.0, `codex exec`, read-only sandbox, live web search |
| Model | Codex, effort `ultra`; ultra delegated to 3 extra agents. |
| Run | 2026-11-16 14:13:22Z to 14:30:30Z UTC, 17 min 8 s |
| Tokens | 10,098,353 input (9,601,024 cached), 54,475 output (11,123 reasoning), 10,152,828 total, across the main agent and 3 delegated agents |
| Plan use | ChatGPT Pro, weekly Codex limit 43 % before, 46 % after (main thread's reading) |
| Status | Completed |
| Note | Its local sources S1–S8 link to the temporary snapshot folder used for the run; the same files are in the repo at commit 6b92c51. |

---

# Closing YouKnow’s known fidelity gaps

**Date:** 2026-11-16  
**Researcher:** OpenAI Codex, using local read-only inspection and web research with parallel research agents. **Exact runtime model ID was not exposed; I cannot verify it.**  
**Target:** YouKnow 1.4.9, commit `6b92c518355ee6c22ee4fc66936835544909555e`.  
**Question:** How can every documented fidelity gap be closed or narrowed using public evidence, other implementations, or the cheapest credible hardware measurement?

No files were written. No new hardware measurements, audio analyses, or numerical benchmarks were performed.

**Reading convention:** Statements about published evidence and the implementation carry source citations. Recommendations, effort, expected audible impact, and confidence are **researcher judgments**, not measured results. “No public source found” describes the searches logged below, not proof that a source does not exist.

Effort: **S** = a small analysis or addition to an existing capture; **M** = an instrumented session plus analysis; **L** = a custom fixture, firmware investigation, or multi-unit study. Confidence concerns whether the proposed experiment can answer its question.

## 1. Summary: five highest-value next actions

| Rank | Action | Why this ranks highly | Effort; expected benefit |
|---|---|---|---|
| **1** | **Test the WAVE/mixer prior with dry source combinations, then measure WAVE DC and differential impedance.** | The selected approximately 95.7 kΩ and alternative approximately 5.0 kΩ priors satisfy the same SUB-only calibration but predict substantially different mixed-source behavior. The selected prior predicts a −15.5 dBc sub-octave component on isolated 50% pulse with SUB off. That is a particularly testable prediction, not a hardware observation. `Docs/decisions.md:2163–2221`. [S6] | **S** for an audio falsification test; **M** for identification. Potentially **high audible and correctness benefit**. |
| **2** | **Obtain one documented original-card unit and run a combined resonance, source-level, noise-onset, and quiet-VCA capture.** | Current validation identifies replacement voice cards, while resonance-versus-byte, original-card drive, and low-current behavior remain open. A single controlled session addresses several consequential uncertainties together. `README.md:1941–1947,1953`; `hardware-validation.md:153–164`. [S1][S5] | **M**. **High correctness**, potentially high audible benefit. One unit establishes that unit, not population statistics. |
| **3** | **Capture chorus clocks, input, both wet returns, and final stereo output together.** | Later bank evidence already narrows timing and hiss; what remains confounded is absolute delay, wet gain/support transfer, installed bias, and the harmonic/noise split. `README.md:1938–1939,1949`; `Docs/decisions.md:2019–2160`. [S1][S6] | **M**. Potentially **high benefit** for stereo movement, brightness, and hiss. |
| **4** | **Close the software-only numerical boundaries: cache availability and integration across BBD events.** | The fallback is an explicit prepared-grid limitation; the README separately identifies nonlinear-current integration across held-source derivative discontinuities. Neither needs another hardware recording to establish numerical convergence. `YouKnowChorusRateIsland.h:31–43`; `README.md:1949`. [S7][S1] | **M**. **High correctness** for affected configurations; audible benefit probably concentrated in difficult material and low host rates. |
| **5** | **Add calibrated output/load sweeps to that bench session.** | Installed output swing, knee, load-dependent slew, volume tracking, common-VCA tolerance, and physical output reference remain open despite settled nominal circuitry. `README.md:1940,1942,1948,1958`. [S1] | **S–M incremental effort**. Useful level accuracy and potentially audible improvements under heavy drive or realistic loading. |

**Inference:** Warm-up, rail, clock, and switching traces should run alongside these sessions. Their incremental capture cost is attractive, but their audible importance is less established than the mixer, voice, and chorus questions.

The rejected mechanisms listed in `README.md:1917–1927` remain rejected. This research found no new evidence sufficient to revive them. [S1]

## 2. Gap: numerical accuracy remains rate-dependent

### Current state

At ordinary rates, Normal halves the local circuit grids relative to Max; global Quality still affects other processing. At unusual rates, the chorus can retain the older path when the required support grid is unavailable. `README.md:1863–1873`; `YouKnowChorusRateIsland.h:31–43`. [S1][S7]

The owner heard no difference in the documented Normal/Max audition and approved Normal as the Rack default, while Max remained the engine/plugin default; this was a listening decision, not a numerical-equivalence finding. `Docs/decisions.md:1842–1884`. [S6]

### Evidence and how others handle it

- **Already known:** BBD simulation must distinguish physical clock-induced aliasing from extra aliasing introduced by the simulation. Gabrielli, D’Angelo, and Squartini’s BLEP paper addresses the latter and is already cited and reflected in YouKnow. “Add BLEP” therefore is not a new solution. [S20][S2]
- **Already known:** Zheleznov and Bilbao found improved memoryless antialiasing with cubic interpolation, but their stateful results favored linear interpolation. This does not justify inserting generic ADAA into YouKnow’s feedback filter without requalification. [S21]
- **New usable artifact:** Holters and Parker’s companion page supplies matched inputs and measured wet-only Juno-60 outputs. It provides an independent reconstruction test case, although it cannot establish Juno-106 parameters. The dataset index was inspected; its audio was not analyzed here. [S18][S19]
- **Commercial precedent:** Diva exposes separate real-time accuracy and offline-render accuracy settings. This demonstrates an explicit quality/cost policy, not proof that any setting matches a particular circuit. [S22]

### Options to close or narrow it

| Proposed action | Data or validation needed | Effort | Expected audible impact | Confidence |
|---|---|---:|---|---|
| Prepare the support grid actually required by the selected configuration outside the audio callback. Preserve IC6 antialiasing and control/latency alignment. | Exhaustive host-rate/Quality/CircuitRate coverage; memory and callback-cost measurements; existing circuit oracles. The current code explicitly avoids two additional 57 KiB caches. [S7] | M | Low at unaffected rates; potentially meaningful in fallback cases | High |
| Give supported low host rates a declared minimum internal circuit bandwidth, extending bounded grids where necessary. | Rate-convergence sweeps, high-cutoff/high-resonance cases, oscillator reconstruction, moving controls, noise PSD, and transition tests. [S1][S2] | M–L | Most relevant to unusually low host rates | Medium |
| Split nonlinear-current integration at actual BBD events instead of spanning a derivative discontinuity with one smooth approximation. | Existing physical-node oracle; full LFO cycles; nonlinear-minus-linear error as well as total-output error. The README already identifies this residual. [S1] | M | Probably low; useful numerical improvement | Medium |
| Publish an accuracy envelope for Normal and Max rather than claiming complete rate independence. | Converged references, reproducible stress cases, and blind listening where differences are material. [S6][S21] | S–M | Improves correctness claims; no automatic sound change | High |

**Open questions:** What numerical error is acceptable at each supported rate? Which errors survive blind listening? Can additional cache storage and event splitting meet the Rack’s resource limits?

**No hardware capture is required to answer those numerical questions.** Hardware is still required to establish that the equations and parameters being solved are the right ones. [S2]

## 3. Gap: selected estimates remain estimates

### 3.1 Original-card resonance-versus-byte calibration

**Current state:** The product uses nominal original-hybrid/service coordinates and a conditional resonance-control law; it does not possess an original-card resonance response family. `README.md:1945`; `Docs/decisions.md:45–50`. [S1][S6]

**Evidence found:** Sound Doctorin publishes first-hand original-module component readings, but no calibrated byte-to-resonance sweep. The repository already explains why its in-circuit resistance readings must be interpreted with parallel paths. [S10][S1]

**New corroboration:** Sound Semiconductor’s AN701 explains input-side resonance compensation and explicitly references the Juno-106. It supports the mechanism, not the installed compensation coefficient, junction onset, or byte law. [S13]

**How others handle it:** Clone designers use original-part measurements and circuit reconstruction; Roman Sowa expressly claims measurement-based development but publishes no numerical curves on the inspected product page. That is a promising data-holder lead, not validation. [S15]

**Proposed closure:** Capture each original voice at several cutoff settings, stepping resonance densely around onset and self-oscillation. Measure passband attenuation, resonant peak height/width, and autonomous oscillation amplitude separately. Where practical, record TP19 and TP8 simultaneously to separate filter and VCA behavior. Reserve alternating bytes and an entire cutoff setting for validation.

- **Data:** Exact bytes, card identities, service settings, input/reference level, repeated captures.
- **Effort:** M.
- **Expected audible impact:** High for resonant patches.
- **Confidence:** High for identifying the measured unit; low for population generalization.

**No public source found** for an identified original-card resonance-versus-byte family with sufficient calibration metadata. Searches Q2 and Q8 below.

### 3.2 Installed WAVE impedance, bias, and source-mixer coordinates

**Current state:** The November 14 upper-root selection changed the product’s mixer prior without changing its evidence class; both resistance roots preserve the isolated SUB calibration. `Docs/decisions.md:2163–2221`. [S6]

**Evidence found:** Roland supplies the external topology and approximate waveform description, but not installed MC5534A output impedance and bias. No public calibrated WAVE-node measurement was found. [S9]

**How others handle it:** Existing sibling-service and emulation comparisons narrow source balance, but YouKnow already documents their dependence and disagreement. Reusing those comparisons as independent corroboration would double-count evidence. `README.md:1941`. [S1]

| Proposed action | Data needed | Effort | Expected audible impact | Confidence |
|---|---|---:|---|---|
| First test the large output predictions with pulse-only/SUB=0, saw-only, SUB-only, and mixed sources at several partial-SUB settings. | Dry audio, exact SysEx, fixed gain, known PWM, several pitches and voices. | S | High if the selected prior is wrong | High as a falsification test |
| Measure WAVE before C56, the coupled downstream node, and Tr19 collector with DC-coupled, characterized probes. | Absolute DC, waveform levels, pulse-off transitions, settling after SUB changes. | M | High | High |
| Apply a small documented test current or reversible load and identify differential impedance across signal states. | Voltage/current changes, probe/load correction, frequency dependence. | M | High correctness; potentially high sound impact | High |

**Inference:** A single fitted resistor may remain inadequate if source impedance depends on waveform state or frequency. Identify the measurable terminal behavior before forcing it into either existing root.

**Open questions:** Absolute DC; conducting/nonconducting source impedance; diode onset under mixed sources; sub mean; pulse-off residual. These share one measurement problem and should not be fitted independently from unrelated recordings.

### 3.3 Tr20 quiet-current parameters and BA662 behavior

**Current state:** The selected Tr20 model uses a named 2SA1015 prior and an estimated temperature coefficient; installed low-current behavior remains unidentified. `README.md:1947`; `Docs/decisions.md:32–43`. [S1][S6]

**Evidence found:**

- Toshiba provides typical transistor characteristics, not the installed Tr20’s near-zero-current curve. [S11]
- **New, weak measurement:** Sinneb corrected an assumed VCA-control maximum from 0.5 mA to a reported measured 0.3 mA. The post provides no serial number, uncertainty, or quiet-current trace. It can screen high-current plausibility, not replace the current model. [S12]
- Open Music Labs describes electrical reverse engineering of original BA662s and limitations associated with clone component selection and thermal layout. This supports a measurement approach, not an original Juno population distribution. [S14]

**Proposed closure:** Hold a stable small signal at TP19 and measure TP8 gain while stepping densely through low VCA-control settings. Record TP7, C58, and voltage across the relevant control resistor to infer current without inserting a series ammeter. Repeat cold and warm; separately sweep audio level to distinguish control response from signal saturation.

- **Data:** Input/output amplitude and harmonics, control-node voltages, temperature, actual stored controls.
- **Effort:** M.
- **Expected audible impact:** Medium, concentrated in quiet tails and onset.
- **Confidence:** High for an effective installed current-to-gain law; medium for identifying individual internal parameters.

**Open questions:** Junction scale current, low-current gain, reference drift, BA662 loading, and thermal coefficient. **No public source found** closing these installed parameters; searches Q2–Q3.

### 3.4 Recording-derived chorus timing

**Current state:** The product has progressed beyond the original A11-only fit: the October bank comparison selected approximately ±1.978 ms depth while retaining the effective centre/rates. This remains recording-derived behavior of one original chorus board. `README.md:1938`; `Docs/decisions.md:2019–2058`. [S1][S6]

**Evidence and other approaches:** Holters/Parker’s Juno-60 study estimated its clock trajectory and reported a time-varying delay discrepancy reaching 0.13 ms. Its companion wet-only recordings are useful new test material, but the study illustrates why inferred clock trajectories need direct verification. [S18][S19]

**Proposed closure:** Simultaneously capture both BBD clocks, LFO voltages, chorus input, and wet outputs over at least ten cycles in I and II. Include the physical both-button state as a separate observation. Derive delay from the clock history and validate it against wet-path excitation.

- **Data:** Original-board identity, bias settings, synchronized clock/audio, calibrated channel response.
- **Effort:** M.
- **Expected audible impact:** Medium–high.
- **Confidence:** High for that board.

**Open questions:** Absolute delay, clock-reset dead time, asymmetry, and how much of the effective fit reflects support filters or the recording path. **No public source found** providing the required synchronized Juno-106 capture; search Q4.

### 3.5 BBD bias, noise split, and loaded support chain — including OQ-04

**Current state:** The accepted BBD transfer estimate depends on a declared subtraction of noise from a datasheet distortion reading. The loaded support model also retains provisional output-source impedance and a Juno-60 insertion-gain prior. `Docs/decisions.md:32–43`; `README.md:1949`. [S6][S1]

Later work already changed the product hiss scalar to ×3.11, removed the historical Mode-II noise lift for the measured-board profile, and selected an effective clock-dependent variance law; these are not new findings from this review. `Docs/decisions.md:2019–2084`. [S6]

**Evidence found:**

- The repository documents conflicting MN3009 maximum-noise specifications—0.20 versus 0.15 mVrms under nominally matching stated conditions. This review did not resolve their revision history: the standalone scan was image-only and the larger collection exceeded the retrieval limit. Neither maximum identifies typical installed PSD. [S1][S36][S37]
- Holters/Parker measured approximately +2.3 dB insertion gain in a Juno-60, not the Juno-106 under study. [S18]
- Alpes Machines’ clone instructions provide wet outputs, test points, and BBD calibration procedures. They demonstrate a practical measurement arrangement, while documented clone modifications prevent treating the clone as an original-unit reference. [S34]

| Proposed action | Data needed | Effort | Expected audible impact | Confidence |
|---|---|---:|---|---|
| Measure coherent sine harmonics and broadband noise separately at the installed bias, then at documented service-adjusted bias. | Input/output voltages, bias DC, several drive levels, clock phase, calibrated bandwidth. | M | Medium–high | High for output separation |
| Bin long idle captures by instantaneous clock/LFO phase and fit noise PSD and power jointly with support response. | Both wet lines, clocks, repeated long captures. | M | High for hiss behavior | High for an effective law |
| Measure input-to-wet transfer directly before the summer. | Low-level multisine/sweep, both channels, DC bias and clock traces. | M | Medium–high for wet level and brightness | High |
| Identify BBD output admittance with small known added loads, resolving clock-active/off phases. | Technician-controlled load fixture and both output-pin voltages. | M–L | Medium | Medium |

**Open questions:** Microscopic storage/transfer-noise strengths may remain non-identifiable even after the audible output law is measured. An effective PSD model is a legitimate endpoint; a unique microscopic interpretation is not required.

**No public source found** for calibrated original-Juno-106 bias/harmonic/noise separation or phase-resolved installed MN3009 output admittance. Searches Q4–Q5.

## 4. Gap: common DCO master-reference drift; no programmed inter-voice detune

### Current state

The product uses one shared ceramic-resonator temperature proxy. Equal settled divider counts retain equal frequencies, while phase and write timing remain distinct. `README.md:1882–1890`; `unison-hardware.md:87–141`. [S1][S4]

The three-second thermal time constant is explicitly accelerated software behavior, not measured hardware warm-up. `juno-106-fidelity-research.md:668–681`. [S2]

### Evidence and other implementations

Roland identifies the installed 8 MHz resonator as KMFC1034T1. Murata’s application guidance shows that oscillator behavior depends on the surrounding IC and loading, so another resonator’s curve cannot establish this installed part’s drift. [S9][S24]

KiwiTechnics describes programmable dividers sharing one master and discusses phase-related unison behavior. Its added detune capability is an upgrade feature, not evidence of original programmed detune. [S25]

### Options

| Proposed action | Data needed | Effort | Expected audible impact | Confidence |
|---|---|---:|---|---|
| Record the buffered common clock against an independently calibrated timebase through repeated cold starts. | Clock frequency, local temperature, supply voltage, fixed divider/count provenance. | M | Uncertain; probably modest | High |
| Record dry DCO periods and VCF self-oscillation separately during the same warm-up. | Per-voice audio plus clock/reference trace. | S increment | Separates common tuning from analog filter movement | High |
| Replace the proxy only with a measured common-reference law; retain existing independent phase/write behavior. | Repeated installed-part traces and validation on another unit. | S after capture | Correctness benefit | High |

**No public source found** for an installed KMFC1034T1 temperature/supply/warm-up trace. Search Q6.

**Open questions:** Unit spread, supply pulling, and long-term aging. A warm-up capture does not measure historical aging. **There is no evidence-based reason here to add six independent DCO pitch walks.**

## 5. Gap: mains ripple and shared-rail transients

### Current state

The present ripple term is a rated-load/typical-rejection scenario, while the audio-energy load proxy is not a measured rail-current model. `README.md:1891–1900`. [S1]

### Evidence and how others handle it

Mitsubishi’s 68 dB ripple-rejection figure is typical at a specified 120 Hz test condition. It does not establish the installed regulator/pass-transistor response, harmonic rejection, ground impedance, or load transient. [S26]

**New qualitative evidence:** Analogue Renaissance says replacement-card revisions differed in thermal transients and that later work addressed response to supply movement. No numerical transfer functions accompany that account. It makes card revision an important measurement confounder; it does not supply a rail model. [S27]

### Options

| Proposed action | Data needed | Effort | Expected audible impact | Confidence |
|---|---|---:|---|---|
| Capture reservoir ripple, destination-board rails, ground differences, actual current, and audio during idle, chords, resonant jumps, and chorus switching. | Synchronized voltage/current/audio; actual mains frequency; known card type and service condition. | M | Unknown | High for characterization |
| If reproducible coupling exists, identify rail-to-audio and rail-to-clock sensitivity separately using small controlled low-voltage perturbations. | Measured perturbation and response, at several frequencies and operating points. | M–L | Conditional | Medium |
| Fit measured current demand and supply/ground impedance before introducing dynamic sag. | Current trajectories and impedance response, not note count or audio RMS alone. | M | Conditional | High correctness |

**Inference:** Natural-event correlation alone cannot distinguish rail coupling from simultaneous control or signal changes. Controlled perturbation is needed when those causes remain confounded.

**No public source found** for installed multi-harmonic rejection, rail-current/audio transients, quantitative ground impedance, or master-clock supply pulling. Search Q7.

**Open questions:** Which nodes couple audibly, at what level, and on which card revisions? The typical 120 Hz calculation must not become an installed upper bound or universal inaudibility claim. [S1][S26]

## 6. Gap: explicit priors and deliberately omitted mechanisms

### Current state

The README distinguishes shipped estimates from omitted parasitics; nominal wet-switch channel behavior, pulse-off topology, and HPF stored-charge tails are already implemented. The remaining questions concern installed magnitudes and unmodelled parasitics. `README.md:1901–1916,1946,1955–1956`. [S1]

### Shipped priors

| Mechanism | Evidence and current boundary | Proposed closure: action and data | Effort; impact; confidence |
|---|---|---|---|
| **Mixer levels and sub half-wave DC** | These share the unresolved WAVE impedance/bias problem. The chosen mixer coordinate is not an installed measurement. [S6] | Use the source matrix and DC-coupled node measurements in §3.2. Estimate mean and AC components separately. | **M; high; high** |
| **Resonance onset and compensation** | Original-module readings and AN701 support topology, not the complete byte law. [S10][S13] | Measure passband compensation and resonance response separately across bytes, cutoff and drive. | **M; high; high** |
| **Noise onset, level and distribution** | The product already adopted the 2.9σ interpretation and revised source level; the older large nominal-model deficits should not be reported as unchanged product errors. `README.md:1953`. [S1] | Record long noise-only segments at zero, low, intermediate and full bytes. Report RMS, PSD, amplitude quantiles and bandwidth. Probe source and downstream nodes if separating source statistics from filtering. | **S–M; medium–high; high** |
| **Self-oscillation startup** | Original-card startup and dry-floor measurements remain open. `README.md:1953`. [S1] | Repeat source-off resonance starts, recording onset-time distribution and residual noise; do not fit from one startup. | **S increment; medium; high** |
| **Upper cutoff knee** | The 270 pF original-card alternative is retired. Separately, the code labels the 64 kHz saturation coordinate and exponent 1.7 as a coupled, voiced pair requiring refitting. This is distinct from the product’s 50 kHz cutoff cap. `YouKnowEngine.h:1638–1675`. [S8][S10] | Measure actual cutoff CV/code against original-card transfer or self-oscillation into the upper range, using adequate analog bandwidth. Jointly fit knee and slope while preserving service anchors. | **M; medium audible/high correctness; high** |

**No public source found** for an identified original unit’s low-byte noise law, noise-amplitude distribution, or calibrated original-card upper-knee family. Searches Q2–Q3.

### Omitted parasitics and residuals

| Mechanism | Public evidence and other approaches | Proposed closure: action and data | Effort; impact; confidence |
|---|---|---|---|
| **Wet-mute JFET capacitance, click and leakage** | The original Toshiba sheet has capacitance information. A cleaner later 2SK30ATM sheet gives typical 8.2 pF input and 2.6 pF reverse-transfer capacitances under different specified bias conditions. These are sensitivity-study inputs, not installed click measurements. [S32][S33] | First run a nominal capacitance sensitivity study. Then capture gate, wet input/output and final output during repeated silent and sine-driven switching at several LFO phases. Measure off-state residual separately. | **S study/M capture; low–medium; medium/high** |
| **HPF multiplexer injection** | Toshiba supplies Ron, leakage and terminal capacitance, but those do not identify injected charge. Existing passive-state tails must remain separate. [S28][S1] | Trigger on the command; capture switch node and output with silence and phase-controlled signals. Compare with the validated passive-state prediction before fitting an additional residual. | **M; unknown; high** |
| **Converter injection/acquisition** | ADI’s application guidance gives the measurement relation \(Q=C_L\Delta V\), its source-voltage dependence, and a high-impedance oscilloscope method. Its example switch values are not transferable to the Juno. [S29] | Capture equal-level rewrites and large code changes. Separate acquisition, opening-edge step and droop; include probe capacitance in the effective load. | **M; control correctness, audible impact unknown; high** |
| **Common µPC1252H2 even-order distortion** | NEC’s published THD measurement explicitly minimizes distortion with VR2. Akai’s use of symmetry adjustment illustrates how another application controls H2; neither establishes untrimmed Juno distortion. [S30][S31] | Measure common-VCA input and output simultaneously across signal level, frequency and VCA LEVEL. Resolve H2/H3, harmonic phase, gain and measurement floor. Fit only a reproducible residual. | **M; unknown, possibly relevant at high drive; high** |
| **Pulse-off residual, node level and loading** | The comparator/C56 topology is settled; installed residual and waveform details remain open. `README.md:1946`. [S1] | Include pulse on/off and source-disabled cases in the WAVE session, with absolute DC and downstream audio. | **S increment; low–medium; high** |

**Inference:** The cheapest useful endpoint may be a measured upper bound. A parasitic mechanism need not be added if its output effect remains below a characterized measurement floor.

**No public source found** for calibrated original-Juno wet-mute injection, HPF injection, converter opening steps, or untrimmed common-VCA harmonic sweeps. Searches Q5 and Q9–Q10.

**Open questions:** Which residuals exceed repeatability and instrument noise? Which depend on bias, temperature, or signal phase? A generic capacitance or leakage limit cannot answer those questions by itself. [S28][S29][S32][S33]

## 7. Gap: hardware-reference scope and original-card variation

### Current state

The principal identified validation unit, #439522, has Borish replacement voice chips and original oscillator chips; the repository separately identifies its chorus board as original. Its recordings remain useful, but their filter/VCA response does not establish original-card fidelity. `hardware-validation.md:153–164`; `Docs/decisions.md:2019–2028`. [S5][S6]

Post-calibration spreads and thermal trajectories remain priors, and the current product no longer selects the six replacement-card frequency fits. `README.md:1957`; `unison-hardware.md:122–140`. [S1][S4]

### Evidence and how others handle it

- **Already in repo:** Maciek Polak’s comparison covers original, treated-original, Analogue Renaissance and Roman Sowa card sets across 13 presets, but randomizes the four examples. It is useful listening evidence, not a labeled numerical calibration set. [S16]
- **Already in repo:** SynthMania supplies independent factory-patch audio, without card provenance or calibrated output level. [S17]
- **New contact lead:** Roman Sowa states that original-part measurements informed his replacement. The inspected page does not publish the measurements. [S15]
- **New qualitative caution:** Analogue Renaissance’s account identifies revision-dependent thermal and supply behavior, reinforcing the need to record replacement type and revision. [S27]

### Options

| Proposed action | Data needed | Effort | Expected benefit | Confidence |
|---|---|---:|---|---|
| Ask existing recordists and replacement makers for labeled raw captures, calibration notes and measurement curves. No messages were sent during this research. | Existing files with usable provenance and permission to use them. | S | Potentially high benefit per effort | Medium |
| Borrow one known-good original-card unit; photograph all six card markings and run one versioned capture suite. | Original/treated status, service history, exact MIDI/SysEx, raw PCM, calibrated gain and loads. | M | High correctness | High |
| Repeat selected tests cold-to-warm and on another day. | Temperatures, rail/reference voltages, identical test conditions. | M | Identifies within-unit variation | High |
| Keep a second original-card unit entirely held out; expand further before claiming population distributions. | Independent units and card histories. | M for second unit; L for population study | Prevents overgeneralization | High for detecting unit-specific fits |

**Open questions:** How much variation survives service adjustment? How much comes from aging, card treatment, component revision, or calibration? A second unit tests transferability; it does not by itself establish a population distribution.

**No public source found** providing a labeled, calibrated original-card dataset sufficient to close this scope gap. Search Q8.

## 8. Additional README gaps

The following covers the remaining open questions rather than treating the six requested groups as exhaustive. All current-state descriptions below refer to the pinned README. [S1]

### Output, loading, and absolute reference

| Gap and current state | Evidence and other handling | Proposed closure and required data | Effort; impact; confidence |
|---|---|---|---|
| **OQ-02: common-VCA installed tolerance.** Nominal law established; installed spread open. `README.md:1948`. [S1] | NEC specifies test conditions, including symmetry adjustment; another installation cannot establish Juno spread. [S30][S31] | Combine gain-law sweep with surrounding resistor measurements and common-VCA harmonic capture. | **S increment; modest; high** |
| **OQ-05: loaded TA75558S clipping/slew.** The ±13.5 V asymptote and knee remain provisional; the repository’s datasheet comparison is not an installed bound. `README.md:1940`. [S1] | Existing manufacturer data narrow plausibility, but no calibrated installed Juno load/slew sweep was found in Q10. | Capture IC6 pins and jacks simultaneously over level/frequency, with high-impedance and known loads; include stereo and mono normalling. | **M; potentially appreciable under drive; high** |
| **OQ-06: physical output reference.** Digital convention settled; physical reference open. `README.md:1942`. [S1] | Roland’s selector specification does not resolve every historical reference-impedance question. [S1][S9] | Measure volts RMS/peak for a fully specified service signal, selector and load. Preserve the agreed digital convention. | **S; correctness; high** |
| **OQ-17: volume tracking, selector loading, headphones.** Nominal networks exist; installed variation and driven behavior remain open. `README.md:1958`. [S1] | No public installed sweep found; nominal topology comes from the service circuit. [S9] | Extend the output session across volume positions, both channels, known receiver/cable loads, and separate headphone dummy loads with line jacks unplugged. | **M; potentially audible; high** |

**Open questions:** Does measured clipping belong to IC6, the common VCA, the voice path, or the recorder? Simultaneous internal/output capture is needed to distinguish them.

### Physical control timing, holds, and waveform reset

| Gap and current state | Evidence and other handling | Proposed closure and required data | Effort; impact; confidence |
|---|---|---|---|
| **OQ-07: hold acquisition and smoothing.** Topology is settled; acquisition, latch edges and installed C61 response remain open. `README.md:1943`. [S1] | Manufacturer measurement methods can separate acquisition, injection and droop. [S29] | Capture mux commands, hold node and C61; record actual WIDTH trim and voltage-dependent settling. | **M; control correctness; high** |
| **OQ-08: UART/PIT/mux physical timing.** Nominal execution is substantially reconstructed, but physical edge relationships and internal serial boundaries remain unresolved. `README.md:1944,2061–2067`. [S1] | MAME’s Juno driver is explicitly a skeleton, useful for architecture/ROM context rather than independent physical timing validation. [S35] | Synchronized logic/scope capture of serial, mux controls, PIT writes, TP5 and range pins during notes, unison, range changes and dense MIDI. | **M–L; timing/transient correctness; medium** |
| **OQ-08, analog portion: MC5534A reset, swing and recovery.** Reset duration, clamp and discharge parameters remain uncalibrated. `unison-hardware.md:135–138`. [S4] | Roland supplies the functional mechanism, not an installed reset trace. [S9] | Trigger on PIT output and capture C54/saw across notes/ranges with a low-loading probe. Measure recovery and saturation before choosing an approximation. | **M, shared setup; low–medium audible; high** |
| **OQ-12: envelope timing/revision scope.** Digital order is resolved for the named ROM; existing bank timing already agrees within its reported resolution. `README.md:1950`. [S1] | No independent physical timestamp dataset found in Q11. | Record firmware identity, command/mux events and isolated envelopes; use explicit threshold conventions. | **S increment; modest; high** |
| **OQ-13: minimum LFO rate.** Byte 0 predicts approximately 0.0363 Hz; direct installed verification remains open. `README.md:1951`. [S1] | No independent minimum-rate measurement found in Q11. | Record several cycles at panel minimum and known bytes 0 and 1; retain byte provenance. | **S; high correctness per effort; high** |
| **OQ-14: portamento pot/ADC mapping.** Nominal loaded-pot law and digital store order are established. `README.md:1952`. [S1] | Physical mapping/timestamps still require observation. | Record pot position, voltage, decoded value and repeated fixed-interval glides during the timing session. | **S increment; modest; high** |

**Important limit:** External logic traces cannot directly reveal internal RXB contents. Exact stop-bit-to-latch subcycles and overrun retention may require a controlled processor harness or instrumented firmware: **L effort, probably low normal-use audible benefit, medium confidence**.

**Coverage check:** OQ-01/03/04 are in §3; OQ-09/15/19 in §3; OQ-10 in §§4/7; OQ-11/16/18/20/21 in §6; OQ-02/05/06/07/08/12/13/14/17 above. All twenty-one README questions are covered.

## 9. Smallest credible measurement and capture plan

The following is a proposed consolidated plan, not work performed during this review. It follows the repository’s requirement to separate physical identification from numerical accuracy and to reserve independent validation material. [S2][S5]

### Equipment

Borrow or use existing equipment before buying:

- One documented, serviced Juno-106 with original voice cards and original chorus.
- A calibrated audio interface, fixed gain, and loopback/reference-level measurement.
- A DC-coupled oscilloscope with characterized high-impedance probes; low-capacitance probing for sensitive nodes.
- DMM, known load resistors, temperature sensors, and a suitable current probe or characterized small shunt.
- A referenced frequency counter and logic analyzer for clock/timing work.
- A low-distortion test source for internal transfer measurements.

**Inference:** The audio-only first pass is cheapest. Internal probing is justified where audio alone cannot distinguish source impedance, bias, gain, and downstream filtering.

### Session A: documented dry voice capture

1. Photograph serial, boards and card markings; record original/treated/replacement status, service history, firmware identity where available, warm-up, rails, volume, selector and receiver load.
2. Calibrate recording voltage and characterize interface response/noise.
3. Run exact MIDI/SysEx for isolated saw, pulse, sub and noise; mixed sources with partial SUB; pulse-off transitions; resonance steps at several cutoffs; low VCA/NOISE controls; upper cutoff; repeated self-oscillation starts.
4. Reserve controls and complete takes before fitting.

**Closes or narrows:** mixer predictions, source ratios, resonance law, low-level behavior, noise statistics, upper knee, and reference scope.

### Session B: one internal-node visit

Use the same warmed instrument and recording chain:

| Probe group | Procedure | Main gaps addressed |
|---|---|---|
| **WAVE/C56/Tr19; PIT/C54** | DC and AC captures; source combinations; pulse-off; controlled small loading; reset/range transitions. | WAVE impedance/bias, sub mean, pulse residual, MC5534A reset/recovery. |
| **TP19/TP8; TP7/C58/control resistor** | Low-control gain sweep, level sweep, resonance/cutoff matrix. | Original filter versus VCA separation, Tr20, drive and compensation. |
| **Chorus input, wet returns, clocks/LFO, final output** | I/II and physical both-button state; long idle plus sine/multisine; documented bias and mute cases. | Absolute timing, gain/support, noise law, bias split, switching. |
| **Common VCA and IC6/jacks** | Harmonic/level/frequency sweeps; selector, volume and known loads. | Common-VCA asymmetry/tolerance, clipping, slew and physical output reference. |
| **Mux controls and hold/HPF nodes** | Equal-level rewrites, code jumps, repeated phase-controlled switching. | Acquisition, injection, droop, smoothing and HPF residuals. |

**Proposal:** Start with unchanged bias and trims. If adjustment is needed to distinguish hypotheses, document the original position, perform the service procedure, and retain before/after captures.

### Session C: background warm-up, rail and timing record

Run this alongside A/B where possible:

- Repeated cold starts with buffered 8 MHz and separate CPU-clock measurements, temperature and rail logging.
- Actual current and ground differences during the same note/chorus/output stimuli.
- Per-voice self-oscillation captures to separate analog drift from common DCO drift.
- Minimum LFO and glide/envelope captures.
- Synchronized external serial/PIT/mux traces.

**Closes or narrows:** common clock drift, analog thermal variation, rail/current response and most external timing questions.

### Analysis and acceptance

**Proposed rules:**

- Keep one documented capture-chain gain; do not use per-patch EQ to absorb model error.
- Fit identifiable terminal behavior first. Use bounded circuit-parameter fitting; differentiable fitting is available when complexity warrants it, but it does not create missing information. [S23]
- Separate harmonic lines, broadband noise, hum and clock-related components.
- Compare stochastic and free-running signals statistically as well as spectrally; do not use unrestricted waveform subtraction as the sole criterion. [S2]
- Require improvement on held-out controls and repeated takes.
- Repeat the most discriminating subset on a second original-card unit before changing a nominal model intended to represent more than the first specimen.

## 10. Sources

**Access date for every entry: 2026-11-16.** Local sources were read from the supplied pinned snapshot; they have no visited web URL. Their citation links give the exact local paths. “Already in repo” refers to the underlying document, including when a different mirror was visited.

### Repository evidence

**S1. _YouKnow — README, Known gaps and Open questions_.** protocodus; undated document at the pinned snapshot. **Primary: implementation/evidence record; already in repo.** Local URL: not applicable; [exact file][S1]. Supports current gap definitions and all 21 OQs. Locate: lines 1859–2114. Quote: “Numerical accuracy is still rate-dependent.”

**S2. _Juno-106 fidelity research_.** YouKnow project; undated evolving document at the pinned snapshot. **Primary: research/validation record; already in repo.** Local URL: not applicable; [exact file][S2]. Supports model/evidence distinctions, clock assumptions, numerical qualifications and capture requirements. Quote: “Numerical agreement with a circuit equation proves that the implementation”.

**S3. _JUNO-106 recording references_.** YouKnow project; 2026-09-22 search record. **Primary: provenance audit; already in repo.** Local URL: not applicable; [exact file][S3]. Supports recording identities, scope and exclusions. Quote: “Descriptions below are the recordists’ attribution” — apostrophe normalized.

**S4. _Original six-voice Unison behavior_.** YouKnow project; audit dated 2026-10-08. **Primary: firmware/circuit audit; already in repo.** Local URL: not applicable; [exact file][S4]. Supports common-clock behavior and explicit dispersion/reset/thermal priors. Quote: “equal frequency does not imply equal phase.”

**S5. _Hardware validation — updated 2026-09-05_.** YouKnow project; dated 2026-09-05, retained snapshot record. **Primary: measurement analysis/provenance; already in repo.** Local URL: not applicable; [exact file][S5]. Supports #439522’s replacement-card identity and historical comparisons. Quote: “not a software reference”. Historical residuals are not automatically current-product residuals.

**S6. _YouKnow — decision log_.** YouKnow project; entries through 2026-11-14. **Primary: product decisions and measurement summaries; already in repo.** Local URL: not applicable; [exact file][S6]. Supports owner selections, latest mixer/chorus state and rejected mechanisms. Quote: “Acceptance by ear changes the product selection”.

**S7. _YouKnowChorusRateIsland.h_.** YouKnow project; undated source at pinned commit. **Primary: implementation; already in repo.** Local URL: not applicable; [exact file][S7]. Supports prepared-grid fallback and storage rationale, lines 31–43. Quote: “This avoids two extra 57 KiB support caches per Rack instance.”

**S8. _YouKnowEngine.h_.** YouKnow project; undated source at pinned commit. **Primary: implementation/evidence comments; already in repo.** Local URL: not applicable; [exact file][S8]. Supports unresolved coupled upper-cutoff parameters, lines 1638–1675. Quote: “Refitting the pair belongs to OQ-18.”

### Hardware, measurements and component evidence

**S9. _JUNO-106 Service Notes, First Edition_.** Roland Corporation; 1984-07-31. **Primary: service document; already in repo.**  
URL: <https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf>  
Supports part identity, topology and service procedures. Locate: parts list p.4; circuit/adjustment sections. Quote: “KMFC1034T1 8MHz ceramic resonator”. Scan and indexed text inspected; it does not supply installed statistical distributions.

**S10. _Technical assistance — Roland JUNO-106/HS-60-1984_.** Sound Doctorin’; undated page, measurement paragraph dated `8/6/2017` as printed. **Primary: first-hand repair measurements; already in repo.**  
URL: <https://www.sounddoctorin.com/synthtec/roland/juno106.htm>  
Supports original-module resistor/capacitor observations. Quote: “All caps are 250pf.” Weakness: in-circuit readings, unspecified uncertainty; no calibrated response family.

**S11. _2SA1015 — Toshiba Transistor Silicon PNP Epitaxial Type_.** Toshiba; 2007-11-01. **Primary: datasheet; already in repo.**  
URL: <https://media.digikey.com/PDF/Data%20Sheets/Toshiba%20PDFs/2SA1015.pdf>  
Supports named transistor curves and test conditions. Quote: “Electrical Characteristics (Ta = 25°C)”. Typical characteristics do not identify installed Tr20.

**S12. _80017a measurements_.** sinneb; 2011-03-24. **Primary: first-hand measurement blog; new, weak evidence.**  
URL: <https://sinneb.github.io/p299/>  
Supports corrected maximum VCA-control-current report. Quote: “Max current is not 0.5mA but 0.3mA”. Missing serial, calibration conditions, uncertainty and low-current curve.

**S13. _AN701: Designing Voltage Controlled Filters for Synthesizers with the SSI2164_.** Jeroen Allaert / Sound Semiconductor; revision 1.1, July 2019. **Primary: application note; new.**  
URL: <https://www.soundsemiconductor.com/downloads/AN701.pdf>  
Supports input-side resonance-compensation architecture; Juno-106 reference on p.14. Quote: “adding the filter’s input signal to the input of the resonance amplifier”. Does not calibrate original component values.

**S14. _Open Music Labs BA662 OTA Clone_.** Open Music Labs, reproduced by synthCube; undated. **Primary: reproduced reverse-engineering account; already in repo.**  
URL: <https://synthcube.com/open-music-labs-ba662-ota-clone/>  
Supports electrical probing methodology and clone thermal limitations. Quote: “The internal wiring of the BA662 was reverse engineered from an original part”. Small specimen count; not Juno population data.

**S15. _VCF-VCA hybrid clone of Roland 80017_.** Roman Sowa / MIDI Hardware; undated. **Primary: manufacturer account; new, unverified quantitative claims.**  
URL: <https://midi-hardware.com/index.php?product=80017&section=prod_info>  
Supports identification of a potential holder of original measurements. Quote: “just pure measurement of the original”. No published curves or raw captures; direct retrieval was inconsistent.

**S16. _Welcome to the analogia.pl Substack post number two._** Maciek Polak; 2026-06-06. **Primary: recording account; already in repo.**  
URL: <https://maciekpolak.substack.com/p/welcome-to-the-analogiapl-substack-b6b>  
Supports four-card-set, 13-preset randomized comparison. Quote: “I then randomized the order of each group of four recordings”. Unlabeled/non-calibrated comparison; useful but insufficient for fitting.

**S17. _Juno-106_.** SynthMania; undated. **Primary: recording index; already in repo.**  
URL: <https://www.synthmania.com/juno-106.htm>  
Supports independent factory-patch listening references. Quote: “Roland Juno-106 audio demos”. Card provenance and calibrated output gain are absent.

### Numerical and chorus research

**S18. _A Combined Model for a Bucket Brigade Device and its Input and Output Filters_.** Martin Holters and Julian D. Parker; DAFx-18, September 4–8, 2018. **Primary: paper and measurements; already in repo, author mirror visited.**  
URL: <https://www.hsu-hh.de/ant/wp-content/uploads/sites/699/2018/09/Holters-Parker-2018-A-Combined-Model-for-a-Bucket-Brigade-Device-and-its-Input-and-Output-Filters.pdf>  
Supports asynchronous BBD/filter modeling and Juno-60 experiment. Quote: “the BBD amplifies the signal by approximately 2.3 dB”. Sibling hardware and estimated clock trajectory limit transferability.

**S19. _A Combined Model for a Bucket Brigade Device and its Input and Output Filters — Listening examples_.** Martin Holters / Helmut Schmidt University; last changed 2018-06-18. **Primary: measurement dataset index; new artifact.**  
URL: <https://www.hsu-hh.de/ant/en/team/martin-holters/dafx2018-bbd>  
Supports availability of matched input/wet-only recordings and model examples. Quote: “this is prior to mixing with the original signal”. Index inspected; audio not analyzed here.

**S20. _Antialiasing in BBD Chips Using BLEP_.** Leonardo Gabrielli, Stefano D’Angelo and Stefano Squartini; DAFx25, September 2–5, 2025. **Primary: paper; already in repo.**  
URL: <https://dafx25.dii.univpm.it/wp-content/uploads/2025/09/DAFx25_paper_29.pdf>  
Supports physical versus simulation-generated aliasing and BLEP treatment. Quote: “additional aliasing components occur that need to be suppressed”. Its ideal-storage experiment does not calibrate noise or nonlinearities.

**S21. _Interpolation Filters for Antiderivative Antialiasing_.** Victor Zheleznov and Stefan Bilbao; DAFx24, September 3–7, 2024. **Primary: paper; already in repo.**  
URL: <https://dafx.de/paper-archive/2024/papers/DAFx24_paper_33.pdf>  
Supports different outcomes for memoryless and stateful interpolation. Quote: “remain in favour of linear interpolation”. Not a Juno-specific filter validation.

**S22. _Diva User Guide, version 1.4.8_.** u-he / Heckmann Audio; 2025-08-07. **Secondary: emulation documentation; new.**  
URL: <https://u-he.com/downloads/manuals/plugins/diva/Diva-user-guide.pdf>  
Supports separate real-time/offline accuracy controls, p.6. Quote: “OfflineAcc selects the resolution”. Product policy, not hardware metrology.

**S23. _Differentiable White-Box Virtual Analog Modeling_.** Fabián Esqueda, Boris Kuznetsov and Julian D. Parker; DAFx20in21, September 8–10, 2021. **Primary: paper; already in repo.**  
URL: <https://dafx.de/paper-archive/2021/proceedings/papers/DAFx20in21_paper_39.pdf>  
Supports parameter identification from measured input/output while retaining circuit structure. Quote: “learned from raw input–output audio measured from a real device”. Does not solve identifiability without suitable captures.

### Clock, rails, switches and common VCA

**S24. _Ceramic Resonator (CERALOCK) Application Manual_, P17E-16.** Murata Manufacturing; undated, printed file code `10.8.3`. **Primary: application manual; already in repo.**  
URL: <https://mm.digikey.com/Volume0/opasdata/d220001/medias/docus/2531/Ceramic%20Resonator%20(CERALOCK)%20Application%20Manual.pdf>  
Supports circuit/load/supply dependence. Quote: “although it varies with the characteristics of the IC.” Not KMFC1034T1 calibration.

**S25. _Kiwi-106 Upgrade User Manual v206_.** KiwiTechnics; undated. **Primary: upgrade-maker documentation; already in repo.**  
URL: <https://www.kiwitechnics.com/downloads/Kiwi-106/KiwiTechnics_Kiwi106_Manual_v206.pdf>  
Supports shared-divider architecture and distinguishes upgrade detune. Quote: “programmable dividers from a single master oscillator”. Not original-firmware drift metrology.

**S26. _M5230L Variable Output Voltage Regulator (Dual Tracking Type)_.** Mitsubishi Electric; mirrored sheet undated; corresponding data book 1984. **Primary: datasheet; underlying source already in repo, mirror new.**  
URL: <https://www.pilianidis.gr/images/sync_products/03-M5230L/03-m5230l.pdf>  
Also visited: <https://www.bitsavers.org/components/mitsubishi/_dataBooks/1984_Mitsubishi_General_Purpose_ICs.pdf>  
Supports conditional typical ripple rejection. Quote: “Ripple rejection”. Direct book retrieval failed; indexed book text and mirrored sheet were available.

**S27. _AR80017A VCF VCA JUNO-106 Voice Chip Filter IC Upgrade_.** Analogue Renaissance; undated. **Primary: maker design account; new, qualitative/commercially interested.**  
URL: <https://www.analoguerenaissance.com/AR80017A/>  
Supports reported revision-dependent thermal/supply behavior. Quote: “Drifting along with the supply lines”. Indexed text inspected; direct fetch returned 502. Promised graphs were absent; exact-match claims are not accepted as measured validation.

**S28. _TC4051,4052,4053BP/BF/BFT_.** Toshiba; served revision dated 2026-04-21. **Primary: datasheet; already in repo URL.**  
URL: <https://toshiba.semicon-storage.com/info/TC4052BP_datasheet_en_20160115.pdf?did=18603&prodName=TC4052BP>  
Supports conditional Ron/leakage/capacitance specifications. Quote: “Switch input capacitance”. The filename date differs from the served revision; terminal capacitance is not a charge-injection measurement.

**S29. _Ask The Applications Engineer—26: Switches and Multiplexers_.** Mary McCarthy and Anthony Collins / Analog Devices; 1997, *Analog Dialogue* 31(3). **Primary: application guidance; new.**  
URL: <https://www.analog.com/en/resources/analog-dialogue/articles/ask-the-applications-engineer-26.html>  
Also visited: <https://www.analog.com/media/en/analog-dialogue/volume-31/number-3/articles/volume31-number3.pdf>  
Supports \(Q=C_L\Delta V\), source-voltage dependence and measurement procedure. Quote: “The amount of charge injected into the load is given by”. Example component magnitudes are not transferable.

**S30. _Integrated Circuits for Consumer Use 1983/1984_, µPC1252H2 section.** NEC Corporation; 1983. **Primary: data book; already in repo, accessible mirror new.**  
URL: <https://manuals.plus/m/11071bf39c022bd3655b437c3be4f5bcffb8387af8df8c4ec49ce5b82e7eadc1>  
Supports symmetry-adjusted THD conditions, printed pp.257–259. Quote: “Make T.H.D. minimum with VR 2”. Original Archive retrieval failed; mirror OCR does not justify independently interpreting the ambiguous untrimmed inequality.

**S31. _Akai GX-747 dbx Service Manual_.** Akai; undated. **Primary: service document; new.**  
URL: <https://retronik.silicium.org/DOCUMENTS/Audiovideo/Akai/Akai-GX-747-DBX-Service-Manual.pdf>  
Supports another application’s symmetry adjustment for H2. Quote: “control the second order harmonic distortion.” Indexed text inspected; direct PDF exceeded retrieval capacity. No page number established; no Juno magnitude inferred.

**S32. _2SK30A — Silicon N Channel Junction Field Effect Transistor_.** Toshiba; undated catalogue scan. **Primary: datasheet; already in repo.**  
URL: <https://amptone.pl/templates/images/files/4686/1710237661-2sk30a-toshiba-6465.pdf>  
Supports family limits and capacitance curves, printed pp.562–564. Quote: “According to the value of IDSS”. Poor OCR; typical curves do not identify installed click/leakage.

**S33. _2SK30ATM_.** Toshiba; 2007-11-01. **Primary: datasheet; new.**  
URL: <https://www.tme.com/Document/45d943f10ed9aea181b6891d1cca0ac1/2SK30A.pdf>  
Supports clearly specified typical capacitance test coordinates. Quote: “Input capacitance Ciss”. Later variant; not proof of an installed 1980s part’s parameters.

**S34. _One-O-Six kit assembly_.** Alpes Machines; undated. **Primary: clone construction/calibration documentation; new page, other clone pages already cited.**  
URL: <https://www.alpesmachines.net/index.php/analogue-chorus/one-o-six-chorus/one-o-six-kit-assembly>  
Supports practical BBD test/calibration access and documented modifications. Quote: “You will calibrate the LEVEL meter and the two BBD delay”. Clone behavior is not original-unit calibration.

**S35. _juno106.cpp_.** AJR/MAME contributors; undated rolling source. **Primary: emulation implementation; already in repo.**  
URL: <https://github.com/mamedev/mame/blob/master/src/mame/roland/juno106.cpp>  
Supports scope of the public CPU/PIT implementation. Quote: “Skeleton driver for Roland Juno-106 synthesizer.” No physical-timing validation inferred.

**S36. _MN3009_.** Panasonic/Matsushita; undated scan. **Primary: datasheet; already in repo; conflicting specification retained.**  
URL: <https://www.experimentalistsanonymous.com/diy/Datasheets/MN3009.pdf>  
Supports the repository-reported standalone specification provenance. Opened as image-only; no readable locating quote. The 0.20 mVrms reading is reported from the repository audit, not newly extracted here. [S1]

**S37. _Panasonic BBD collection_ — repository description; cover title not independently verified.** Panasonic/Matsushita; undated. **Primary: manufacturer-sheet collection; already in repo; conflicting specification retained.**  
URL: <https://www.ka-electronics.com/images/pdf/Panasonic_BBD.pdf>  
Supports provenance of the repository-reported alternative noise specification. Retrieval failed at 36.6 MB; no quote independently verified. The 0.15 mVrms reading remains a repository-reported conflict, not a newly resolved revision. [S1]

## 11. Search log

All searches were performed on 2026-11-16. Search results without measurements or adequate provenance were not promoted into calibration evidence.

| ID | Main queries and sites | Outcome |
|---|---|---|
| **Q1 — numerical methods** | `site.dafx.de virtual analog multirate nonlinear filter oversampling aliasing`; `site.dafx.de BBD BLEP`; adaptive circuit simulation; HSU BBD examples; u-he accuracy documentation. | Confirmed existing BLEP/ADAA literature; found the new companion dataset and Diva policy comparison. The adaptive-QP paper was already cited in `Docs/research/modeling-research-2026-09.md`; it was not treated as a new discovery. |
| **Q2 — original filter and mixer** | `Juno 106 80017A resonance measurements filter original input impedance`; `"80017A" "resonance" "measurements"`; `"MC5534A" "output impedance"`; `"MC5534A" "impedance"`. Sound Doctorin, Sinneb, Sound Semiconductor, replacement makers. | Original component observations and compensation guidance found. **No public source found** for installed WAVE impedance/DC or a calibrated original-card resonance family. |
| **Q3 — quiet VCA and noise** | `"Juno 106" "Tr20"`; `Juno 106 BA662 Tr20 2SA1015 VCA measurement`; `"Juno 106" "noise" "distribution"`. Toshiba, ALFA, Open Music Labs, Sinneb. | Found weak maximum-current measurement and existing component references. **No public source found** for installed quiet-current/temperature or noise-distribution/onset captures. |
| **Q4 — chorus timing/noise** | `Juno 106 chorus MN3009 bias noise measured wet signal delay measurements`; `Juno 106 chorus clock oscilloscope 0.5 0.8 delay`; `MN3009 input bias distortion noise measurement datasheet`. HSU, DAFx, Alpes Machines, Panasonic mirrors. | Useful sibling dataset and clone procedures. **No public source found** for synchronized original-106 clocks/input/wet/bias measurements. |
| **Q5 — chorus parasitics** | `2SK30A gate capacitance switching charge injection Toshiba datasheet`; `Juno 106 MN3009 wet mute 2SK30A click measured`; Bell Tone repair searches. | Cleaner later Toshiba sheet found. **No public source found** for calibrated installed click/injection or phase-resolved BBD output impedance. |
| **Q6 — master clock** | `"KMFC1034T1"`; `"KMFC1034T1" resonator temperature`; `"Juno-106" "resonator" measurement temperature`; `"Juno-106" "drift" "MHz"`. Roland, Murata, KiwiTechnics. | Part identity and generic dependencies confirmed. **No public source found** for an installed calibrated drift/supply trace. |
| **Q7 — rails** | `"Juno 106" "M5230" ripple`; `"Juno-106" "power supply" oscilloscope ripple`; `"Juno-106" "100 Hz" ripple`; `M5230L datasheet ripple rejection 68 54`; Analogue Renaissance supply account. | Manufacturer specification and qualitative maker evidence found. **No public source found** for current/ground/transient datasets or multi-harmonic installed rejection. |
| **Q8 — original recordings/population** | `Juno 106 original voice chips recordings resonance calibration dataset`; `"Juno-106" "original" "calibration" "wav"`; `"Juno-106" "warm-up" measured original filter`; `"Juno-106" "thesis" measurements`; AES/DAFx Juno searches. | Existing Polak/SynthMania references and maker leads. **No public source found** closing labeled original-card calibration or population/thermal statistics. No directly useful Juno-106 thesis/AES measurement dataset was identified. |
| **Q9 — injection** | `"Juno-106" "charge injection"`; `"HD14051" "charge" injection`; `"HD14051BP" datasheet`; ADI/TI charge-measurement queries. | ADI method found. **No public source found** for installed HPF or hold-opening charge. |
| **Q10 — output/common VCA** | `"TA75558" "slew" datasheet`; `"Juno-106" "loaded" "TA75558"`; `"uPC1252" distortion`; `"UPC1252H2" datasheet NEC 1983`. | Existing Toshiba references, NEC trimmed-test conditions and Akai example. **No public source found** for loaded Juno output sweeps or untrimmed common-VCA harmonic measurements. |
| **Q11 — physical timing/reset** | `"Juno-106" "minimum" "LFO" "0.036"`; `"Juno-106" "LFO" "0.0363"`; `"Juno-106" "UART" "RXB"`; `"MC5534A" "reset" measurement`; MAME inspection. | **No public source found** for minimum-rate hardware verification, internal serial-boundary measurements, or calibrated installed reset/recovery traces. |

**Access limitations and excluded leads:**

- Synfo’s service PDF exceeded the retrieval limit; the KiwiTechnics scan and indexed service text were used.
- The larger Panasonic collection exceeded the retrieval limit; the standalone MN3009 scan was image-only. Their documented conflict remains unresolved.
- Gearspace’s Juno-106 chorus-noise thread returned 403. It was not used as new calibration; the repository already records the limited scope of the historical Mode-II lift.
- Bell Tone repair content and the D’Angelo companion-code page were inaccessible. No numerical claims were inferred from them.
- Some Analogue Renaissance and MIDI Hardware direct requests failed; the report labels indexed maker statements and their limitations.
- Forum hits on Syntaur, Vintage Synth Explorer and GroupDIY mainly concerned faults, substitutions or subjective comparisons. None supplied the controlled measurements required here, and none was used as calibration evidence.

[S1]: https://github.com/protocodus/virtual-instrument-youknow/blob/6b92c518355ee6c22ee4fc66936835544909555e/README.md#L1859
[S2]: https://github.com/protocodus/virtual-instrument-youknow/blob/6b92c518355ee6c22ee4fc66936835544909555e/Docs/research/juno-106-fidelity-research.md#L1090
[S3]: https://github.com/protocodus/virtual-instrument-youknow/blob/6b92c518355ee6c22ee4fc66936835544909555e/Docs/research/hardware-recordings.md#L1
[S4]: https://github.com/protocodus/virtual-instrument-youknow/blob/6b92c518355ee6c22ee4fc66936835544909555e/Docs/research/unison-hardware.md#L87
[S5]: https://github.com/protocodus/virtual-instrument-youknow/blob/6b92c518355ee6c22ee4fc66936835544909555e/Docs/validation/hardware-validation.md#L151
[S6]: https://github.com/protocodus/virtual-instrument-youknow/blob/6b92c518355ee6c22ee4fc66936835544909555e/Docs/decisions.md#L2163
[S7]: https://github.com/protocodus/virtual-instrument-youknow/blob/6b92c518355ee6c22ee4fc66936835544909555e/Source/DSP/YouKnowChorusRateIsland.h#L31
[S8]: https://github.com/protocodus/virtual-instrument-youknow/blob/6b92c518355ee6c22ee4fc66936835544909555e/Source/DSP/YouKnowEngine.h#L1638
[S9]: https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf
[S10]: https://www.sounddoctorin.com/synthtec/roland/juno106.htm
[S11]: https://media.digikey.com/PDF/Data%20Sheets/Toshiba%20PDFs/2SA1015.pdf
[S12]: https://sinneb.github.io/p299/
[S13]: https://www.soundsemiconductor.com/downloads/AN701.pdf
[S14]: https://synthcube.com/open-music-labs-ba662-ota-clone/
[S15]: https://midi-hardware.com/index.php?product=80017&section=prod_info
[S16]: https://maciekpolak.substack.com/p/welcome-to-the-analogiapl-substack-b6b
[S17]: https://www.synthmania.com/juno-106.htm
[S18]: https://www.hsu-hh.de/ant/wp-content/uploads/sites/699/2018/09/Holters-Parker-2018-A-Combined-Model-for-a-Bucket-Brigade-Device-and-its-Input-and-Output-Filters.pdf
[S19]: https://www.hsu-hh.de/ant/en/team/martin-holters/dafx2018-bbd
[S20]: https://dafx25.dii.univpm.it/wp-content/uploads/2025/09/DAFx25_paper_29.pdf
[S21]: https://dafx.de/paper-archive/2024/papers/DAFx24_paper_33.pdf
[S22]: https://u-he.com/downloads/manuals/plugins/diva/Diva-user-guide.pdf
[S23]: https://dafx.de/paper-archive/2021/proceedings/papers/DAFx20in21_paper_39.pdf
[S24]: https://mm.digikey.com/Volume0/opasdata/d220001/medias/docus/2531/Ceramic%20Resonator%20(CERALOCK)%20Application%20Manual.pdf
[S25]: https://www.kiwitechnics.com/downloads/Kiwi-106/KiwiTechnics_Kiwi106_Manual_v206.pdf
[S26]: https://www.pilianidis.gr/images/sync_products/03-M5230L/03-m5230l.pdf
[S27]: https://www.analoguerenaissance.com/AR80017A/
[S28]: https://toshiba.semicon-storage.com/info/TC4052BP_datasheet_en_20160115.pdf?did=18603&prodName=TC4052BP
[S29]: https://www.analog.com/en/resources/analog-dialogue/articles/ask-the-applications-engineer-26.html
[S30]: https://manuals.plus/m/11071bf39c022bd3655b437c3be4f5bcffb8387af8df8c4ec49ce5b82e7eadc1
[S31]: https://retronik.silicium.org/DOCUMENTS/Audiovideo/Akai/Akai-GX-747-DBX-Service-Manual.pdf
[S32]: https://amptone.pl/templates/images/files/4686/1710237661-2sk30a-toshiba-6465.pdf
[S33]: https://www.tme.com/Document/45d943f10ed9aea181b6891d1cca0ac1/2SK30A.pdf
[S34]: https://www.alpesmachines.net/index.php/analogue-chorus/one-o-six-chorus/one-o-six-kit-assembly
[S35]: https://github.com/mamedev/mame/blob/master/src/mame/roland/juno106.cpp
[S36]: https://www.experimentalistsanonymous.com/diy/Datasheets/MN3009.pdf
[S37]: https://www.ka-electronics.com/images/pdf/Panasonic_BBD.pdf