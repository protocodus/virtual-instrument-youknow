# Stage review, 2026-11-11

A pass over each stage of the engine against the service-manual circuit and
the identified unit's captures, looking for anything left that a derivation
or a measurement could settle. Result: one chorus correction (1.4.7), and
nothing else that does not need a new capture.

## Derived constants re-derived

Each figure below was recomputed from the part values the source cites and
agrees with the code:

| Stage | Checked |
| --- | --- |
| DCO | the sub switch's storage skew as a second harmonic of π·Δt·f; the ramp droop figures (31.5 and 16.8 µV) |
| VCF and VCA control | C56's 3.969 kΩ source and 39.7 ms settling; C58's 687.5 µs; the VR28 bracket |
| LFO | the mode rates from the timing network |
| Supply | the 80 µV rail ripple |
| Chorus | the MN3009 delay law; R16‖C5's 219 kHz corner; the 2.2 kΩ Johnson noise of 6.02 nV/√Hz |
| Output | the summer's noise gain of 5.692 and its 527 kHz bandwidth; the headphone amplifier's 3.6× gain, 1.944 MHz and 13.44 Hz corners |
| Noise | the OTA input shot noise of 462 nV/√Hz |

The firmware timing needs no check of this kind: the engine executes the
ROM, so the pass period is the ROM's.

## What the captures add

The sixteen factory-bank captures of #439522 (original DCOs and chorus
board, replacement voice cards) were compared note by note with the engine's
render of the same MIDI.

- **Level through the chorus.** Hardware minus engine, summed over both
  channels, reads the same within 0.1 dB with the chorus off, in mode I and
  in mode II (the common figure is the recording gain). The chorus's
  insertion level needs no change.
- **Left-right balance through the chorus.** The engine leaned about 1 dB
  left where the board does not. Corrected in 1.4.7
  ([decision](../decisions.md)).
- **Comb depth against frequency.** After the correction each channel's
  depth agrees within 0.2 dB from 0.8 to 3.15 kHz. Above 5 kHz the few
  usable harmonics (four patches) read the capture's left channel about
  1 dB shallower than the engine's; too few to act on.
- **Envelope and LFO clock** (added 2026-11-12). The firmware steps both
  once per pass, and the pass's duration on a real unit was open (OQ-12).
  Release falls of 100 ms and longer take 1.011 times the engine's (290
  falls, s.e. 1.7 %); the LFO runs at 0.998 times the engine's rate with
  1.038 times its vibrato depth (medians of 17 patches, s.e. 3.8 %). The
  4.2 ms pass stands; nothing changes.
- **Pitch.** The capture sits 7 cents flat overall (its tune setting). Over
  C1 to C5 it rises 2.5 cents and the engine 2.8, and the notes of one
  patch spread 3.9 cents on the capture and 3.6 on the engine (45 patches
  without LFO or noise). Scratch measurement, not in a tool.

## Left open

- The intrinsic noise of the M5218L and TA75558S stages around the chorus is
  not modelled. From their datasheet densities it would raise the idle
  floor by about 0.1 dB.
- Everything that depends on the original voice cards (VCF, VCA, envelopes)
  cannot be read from this unit, whose cards are replacements. The open
  questions in the README stand.
