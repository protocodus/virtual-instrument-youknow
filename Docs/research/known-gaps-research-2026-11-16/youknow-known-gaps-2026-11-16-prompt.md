# Research brief: closing YouKnow's known fidelity gaps

Date: 2026-11-16. Product: YouKnow 1.4.9, a software emulation of the Roland Juno-106 (repository protocodus/virtual-instrument-youknow), pinned at commit 6b92c518355ee6c22ee4fc66936835544909555e.

A read-only snapshot of that commit is in this folder:
[local snapshot path of commit 6b92c51, removed when this brief was committed]

Do not modify, create or delete any files in the snapshot or anywhere else, except the single output file if you are told to write one.

## Question

How can each of YouKnow's documented known gaps be addressed? For every gap, find what public evidence exists that could close or narrow it, how others have dealt with it, and, where no public evidence exists, the cheapest credible measurement or capture that would close it.

## Read first (local snapshot)

- `README.md`, the section from the heading `## Known gaps` to the heading `## Build`. Its bullet "Removed on review" lists mechanisms already rejected, with reasons. Don't propose them again unless you have new evidence, and say so if you do.
- `Docs/research/juno-106-fidelity-research.md`, `Docs/research/hardware-recordings.md`, `Docs/research/unison-hardware.md`, `Docs/validation/hardware-validation.md` and `Docs/decisions.md` (the owner's by-ear verdicts). These say what is already known and already cited, so you build on them instead of repeating them.
- `Source/`, when you need to see how a mechanism is implemented today.

## Gaps to cover

1. Numerical accuracy is still rate-dependent (`CircuitRate::Normal` versus Max, unusually low host rates, the chorus support-cache fallback).
2. Estimates still open: original-card resonance-versus-byte calibration, installed WAVE impedance and bias, Tr20 quiet-current parameters, the BBD bias/noise split, and the recording-derived Mode-I chorus fit.
3. DCO master reference: drift of the 8 MHz ceramic resonator (no calibrated installed-part trace), and no programmed inter-voice detune.
4. No calibrated mains-ripple or shared-rail transient model (regulator rejection, real current draw, ripple harmonics, load transients, ground impedance).
5. Explicit priors: mixer level coordinates, sub half-wave DC magnitude, resonance onset and compensation, noise control onset, upper cutoff knee, noise distribution. Left out until measured: the chorus wet-mute JFETs' gate capacitance, click and leakage; the HPF multiplexer's charge injection; converter charge injection; the µPC1252H2's untrimmed even-order distortion; the pulse-off switching residual, node level and loading.
6. Hardware reference scope: the validation recordings come from a unit with replacement voice cards, so nothing establishes a match to original voice cards.

If the README lists a gap that is not named here, cover it too.

## What to look for

Primary sources first: Roland service notes, schematics and parts lists; datasheets and application notes for the components the README names; peer-reviewed and conference papers (for example DAFx, AES, ICMC); theses; measurements published by repair technicians and by makers of replacement voice chips and clones; and published recordings or datasets of original Juno-106 units. Also look at how other emulations and hardware clones document the same mechanisms. Forum posts count as evidence only when they hold measurements or first-hand repair data, and must be labelled as forum sources.

## Rules for sources (the most important part)

- Every factual claim carries an inline citation such as [S3] to a numbered source.
- Never invent or guess a URL, title, author, page or figure. If you can't find a source for something, write "No public source found" and say what you searched.
- For each source, record: number (S1, S2, ...), title, author or publisher, date published (or "undated"), the full URL exactly as you visited it, the date you accessed it, its type (primary: service document, datasheet, measurement or paper; secondary: article or emulation documentation; forum), the claims it supports, and a short locating quote of at most 15 words when the page has text.
- Mark each source "already in repo" if the snapshot's docs already cite it, or "new" if not.
- Keep every source you relied on, including weak or contradicted ones, and say which they are.

## Output (Markdown only)

1. Header: title, date, researcher (tool and exact model id), and the question.
2. Summary: the five highest-value next actions, ranked by expected audible or correctness benefit per unit of effort.
3. One section per gap: the current state in YouKnow (one or two sentences, with file and line references), evidence found, how others handle it, options to close it (each with what to do, the data needed, effort S/M/L, expected audible impact, and confidence high/medium/low), and open questions.
4. Measurement and capture plan: the smallest set of measurements on real hardware that would close the most gaps, with the equipment and procedure.
5. Sources: the full numbered list in the format above.
6. Search log: the main queries and sites you tried, including those that found nothing.

Say clearly what is sourced fact, what is your inference, and what is a guess. Prefer fewer, well-sourced findings over many speculative ones.
