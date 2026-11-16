# JUNO-106 recording references

Located/checked 2026-09-22 for the owner's weak-resonance report. These are
hardware recording sources; KR-106 output is not the reference in this list.
Descriptions below are the recordists' attribution, not a claim that a new
matched-patch listening or spectral comparison has already been completed.
No further DSP change was made during this search.

## Independent factory-patch recordings: SynthMania

Primary recorder's index: https://www.synthmania.com/juno-106.htm

- A25 Donald Pluck: https://www.synthmania.com/Roland%20Juno-106/Audio/Juno-106%20Factory%20Preset%20Group%20A/25%20Donald%20Pluck.mp3
- A71 Sweep I: https://www.synthmania.com/Roland%20Juno-106/Audio/Juno-106%20Factory%20Preset%20Group%20A/71%20Sweep%20I.mp3
- B41 Resonant Rise: https://www.synthmania.com/Roland%20Juno-106/Audio/Juno-106%20Factory%20Preset%20Group%20B/41%20Resonant%20Rise.mp3
- B44 Contact Wah: https://www.synthmania.com/Roland%20Juno-106/Audio/Juno-106%20Factory%20Preset%20Group%20B/44%20Contact%20Wah.mp3

The recorder explicitly associates Donald Pluck with a duck/resonance sound,
Sweep I with a resonant sweep, and Resonant Rise with the VCF. These are useful
independent listening references. The page does not identify original versus
replacement filter/VCA cards or document a calibrated recording gain. Do not
infer maximum RES from a sound name, or derive absolute synth output gain from
these MP3s. Match the actual factory patch before any numerical comparison.
The linked audio endpoints were resolved by the web tool as audio/mpeg.

## Controlled resonance recording with corresponding MIDI

Lewis Francis's real JUNO-106 has Borish replacement VCF/VCA cards. The owner
identifies these in the primary discussion; this is real hardware, but cannot
alone establish an original 80017A population's response.

- Recording: https://www.lewisfrancis.com/nwio/res_calibrate_bip.aif
- Matching MIDI: https://kayrock.org/kr106/res_calibrate.mid
- Owner's upload: https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/44#issuecomment-4273881211
- Unit/card attribution: https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/44
- Additional VCF full sweep: https://lewisfrancis.com/nwio/vcf_fullsweep_bip.aif
- Full-sweep upload: https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/44#issuecomment-4359681843

HEAD checks with the system curl verified HTTP 200 for the resonance AIFF
(33,475,904 bytes; audio/x-aiff) and MIDI (1,074 bytes; audio/midi). The upstream
workspace already retains these calibration materials. This resonance scan is
distinct from the separate oscillator/self-oscillation isolator used in the f5
balance correction; see RESONANCE.md for that earlier comparison's scope.

## Oscillator and noise sweep of the same unit

Checked 2026-11-16, after the known-gaps research
([combined report](known-gaps-research-2026-11-16/youknow-known-gaps-2026-11-16-combined.md),
section 4) found this take uncited here. It is the same serviced unit, #439522
with Borish replacement cards, as the resonance recording above.

- Recording: https://lewisfrancis.com/nwio/osc_test_bip.aif, SHA-256
  `b0309cd365e3025fce993d409be05b3d39cbc51daac114dbfe0428d33e39093f`, 24-bit
  stereo AIFF at 96 kHz, 96.27 s
- Matching MIDI: https://kayrock.org/kr106/osc_test.mid, SHA-256
  `051ea38ee992b7fe49b8356be9395830706e7c81099d7ab4653ca0b470ad2d43`

It sits beside the unit's other takes on the same two hosts, under the same
`_bip` naming, and the repository already uses it under these hashes:
[`Tools/AnalyzeHardwareNoise.py`](../../Tools/AnalyzeHardwareNoise.py) reads
its NOISE sweep, [`Tools/AnalyzeSubMixerCalibration.py`](../../Tools/AnalyzeSubMixerCalibration.py)
fits the SUB diode law to its SUB sweep, and
[`Tools/AnalyzeExistingCaptures.py`](../../Tools/AnalyzeExistingCaptures.py)
`noise` measures its NOISE windows' amplitude distribution. The upload comment
in the KR-106 issue tracker was not re-read on 2026-11-16 (GitHub refused this
session), so its attribution rests on the shared host, naming and unit, as for
the unit's other takes cited from issue #44. Its first saw note is absent and
unused.

## Original and replacement cards in one listening comparison

Maciek Polak / analogia.pl, published 2026-06-06:
https://maciekpolak.substack.com/p/welcome-to-the-analogiapl-substack-b6b
Embedded video: https://www.youtube.com/watch?v=VAK_jqB3Pz0

The recorder describes 13 factory presets recorded with four card sets:
original 80017As, acetone-treated originals, Analogue Renaissance replacements,
and Roman Sowa replacements. The four examples per patch are randomized, so
this is useful for variation between sets, not a labeled original-chip
calibration target. The author also reports the untouched originals become
unreliable after roughly an hour. Video embedding was verified on the primary
article; the YouTube page itself was throttled by the browsing tool.

## Excluded as an original-filter target

https://www.analoguerenaissance.com/AR80017A-GS/
The manufacturer's Gravitational Slingshot demos intentionally demonstrate a
modified filter character. They should not determine stock JUNO-106 resonance.

## Next comparison

Use the independent factory examples for the reported quacking character and
the controlled MIDI/AIFF pair for resonance progression. Keep one reference
gain per complete recording, verify patch identity, and distinguish the height
and shape of the resonant peak from overall output loudness. F5 remains a
candidate correction, not proof of exact original-hardware matching.
