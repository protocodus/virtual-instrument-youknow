# YouKnow known gaps: Gemini research pass (partial, quota stopped)

| Field | Value |
| --- | --- |
| Date | 2026-11-16 |
| Question | How can each of YouKnow's documented known gaps be addressed? (brief: `youknow-known-gaps-2026-11-16-prompt.md`) |
| Product | YouKnow 1.4.9, commit 6b92c518355ee6c22ee4fc66936835544909555e |
| Researcher | Gemini, through the Antigravity CLI `agy` 1.5.9 (the Gemini CLI is not installed) |
| Model | Gemini at high effort, signed in with the owner's Google account |
| Run | 2026-11-16 14:13:22Z to 14:24:16Z UTC, 10 min 54 s |
| Tokens | 380,553 input, 20,516 output (12,693 thinking), 4,353,209 cache reads, 401,069 total |
| Status | **Stopped, no report.** `RESOURCE_EXHAUSTED (429): Individual quota reached. Please upgrade your subscription to increase your limits. Resets in 113h27m33s` (around Sat 21 Nov 07:51 UTC). |

Gemini returned no report text. This file preserves what it had done when the quota stopped it, recovered from its saved conversation (`~/.gemini/antigravity-cli/conversations/f45d2d34-2bca-4be2-a701-6c8ff0c6ca00.db`). None of it has been checked yet, and none of it is a finding: it is only a record of where Gemini looked.

## What it read

It read the brief's local files first: `README.md`, `Docs/research/juno-106-fidelity-research.md`, `Docs/research/hardware-recordings.md`, `Docs/research/unison-hardware.md`, `Docs/validation/hardware-validation.md` and `Docs/decisions.md`.

## Web searches it ran (15, alphabetical, not in run order)

1. `site:analoguerenaissance.com D80017A OR AR80017A OR D5534A exact page title URL verbatim quote measurements` (domain: analoguerenaissance.com)
2. `site:dafx.de BBD OR "bucket brigade" OR "Juno" OR "IR3109" OR "BA662" OR "antiderivative antialiasing" exact title authors year URL` (domain: dafx.de)
3. `site:github.com/kayrockscreenprinting/ultramaster_kr106 README VCF chorus DCO noise calibration` (domain: github.com)
4. `"100222" OR "100229" Roland "Service Information" Juno-106`
5. `"1SS133" Rohm datasheet forward voltage capacitance AND "2SA1015" Toshiba low current Vbe hFE`
6. `"2SK30A" OR "2SK30ATM" Toshiba datasheet Ciss Crss capacitance pF`
7. `"80017A" "AR80017A" OR "Borish" OR "Open80017a" Juno-106 measurements resonanceoriginal vs clone`
8. `"Cherry Audio" "DCO-106" manual Juno-106 voice chorus filter`
9. `"HD14051B" OR "TC4052B" OR "CD4051B" charge injection picocoulombs datasheet application note`
10. `"KMFC1034T1" OR "12389728" Roland Juno-106 8MHz ceramic resonator drift temperature`
11. `"M5230L" Mitsubishi datasheet "ripple rejection" load regulation output impedance`
12. `"MC5534A" OR "5534A" Roland Juno-106 schematic teardown impedance reverse engineering`
13. `"Softube" "Model 84" Juno-106 manual voice calibration chorus`
14. `"TAL-U-NO-LX" manual oversampling calibration Juno chorus filter`
15. `"uPC1252H2" OR "UPC1252H2" NEC datasheet THD distortion symmetry trim maximum input`

## URLs it saw that the repo docs don't already cite (18)

These came from its search results and the pages it opened. They are candidates, not verified sources. Two Google page-script URLs (`csp.withgoogle.com`) were left out as noise.

- http://www.analoguerenaissance.com/AR80017A/
- http://www.analoguerenaissance.com/D5534A/
- http://www.analoguerenaissance.com/D80017/
- http://www.analoguerenaissance.com/D80017A/
- https://dangelo.audio/bbd-blep/
- https://kayrock.org/kr106/osc_test.mid
- https://kayrock.org/kr106/vcf_fullsweep.mid
- https://lewisfrancis.com/nwio/osc_test_bip.aif
- https://vtda.org/pubs/BSTJ/vol53-1974/articles/bstj53-7-1211.pdf
- https://www.dafx.de/paper-archive/2010/papers/pdf/P_11.pdf
- https://www.dafx.de/paper-archive/2016/dafxpapers/18-DAFx-16_paper_33-PN.pdf
- https://www.dafx.de/paper-archive/2018/DAFx2018_paper_30.pdf
- https://www.dafx.de/paper-archive/2022/papers/dafx22_paper_6.pdf
- https://www.lewisfrancis.com/nwio/vcf_fullsweep_bip.aif (correction at merge: the repo already cites this file under the bare `lewisfrancis.com` host, in `Docs/research/hardware-recordings.md:36`)
- https://www.softube.com/eu/plug-ins/model-84-polyphonic-synthesizer
- https://www.synfo.nl/servicemanuals/Roland/JUNO-6_SERVICE_NOTES.pdf
- https://www.ti.com/lit/ds/symlink/tl064.pdf
- https://www.ti.com/lit/ds/symlink/tl082.pdf

All 189 URLs found in its conversation, including the ones it read from the repo docs, are kept in the run folder (`runs/gemini-partial-urls.txt`), not delivered.
