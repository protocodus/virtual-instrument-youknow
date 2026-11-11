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

## Left open

- The intrinsic noise of the M5218L and TA75558S stages around the chorus is
  not modelled. From their datasheet densities it would raise the idle
  floor by about 0.1 dB.
- Everything that depends on the original voice cards (VCF, VCA, envelopes)
  cannot be read from this unit, whose cards are replacements. The open
  questions in the README stand.
