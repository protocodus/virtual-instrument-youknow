#!/usr/bin/env python3
"""Write the committed per-preset demo MIDI sources in Docs/audio/midi/.

One Standard MIDI File per factory preset, in host program order, named
<NNN>-<slug of "<slot> <preset name>">.mid as the shared preset demo renderer
expects (Tools/PresetDemoRenderer.h): the 128
archival programs A11..B88, then the 16 original product presets YB1..YP8
(standard G3a). Each phrase is written for the patch's role on the instrument
(bass lines, brass stabs, string and choir voicings, organ comping, clav and
funk riffs, mallets and bells, leads and reeds, drums, effects) rather than a
shared score. The product presets reuse the phrases of their committed
session library (Docs/audio/youknow-originals/midi), repeated as often as
fits the demo length.

Scores are written as the keys a player presses, on the hardware's 61-key
range; each preset's DCO range and saved transpose then place the sound where
its designers meant it, as they would for a player (A26 Celeste, 4' with +1
octave, sounds two octaves above the keys). Gates respect the preset's voice
count, and Unison presets get single lines.

The files are sources, not build outputs: rerun this only to change a phrase
on purpose, and commit the result in its own commit. --check verifies that the
committed files match this script byte for byte.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'Docs' / 'audio' / 'midi'
ORIGINALS = ROOT / 'Docs' / 'audio' / 'youknow-originals' / 'midi'
PPQ = 960
# The hardware's 61 keys, C1..C6.
LOWEST_KEY, HIGHEST_KEY = 36, 96

# slot, name, voice limit, role, tonic, mode, tempo, variant. The voice limits
# are the shipping bank's (Source/DSP/YouKnowPresets.cpp); a Unison patch has
# limit 1, since every voice sounds the one key.
BANK = [
    ('A11', 'Brass Set 1', 6, 'brass_stab', 'Bb', 'major', 112, 0),
    ('A12', 'Brass Swell', 6, 'brass_swell', 'Eb', 'major', 84, 0),
    ('A13', 'Trumpet', 6, 'trumpet', 'Bb', 'major', 104, 0),
    ('A14', 'Flutes', 6, 'flute', 'G', 'major', 92, 0),
    ('A15', 'Moving Strings', 6, 'strings', 'D', 'minor', 80, 0),
    ('A16', 'Brass & Strings', 6, 'brass_swell', 'F', 'major', 88, 1),
    ('A17', 'Choir', 6, 'choir', 'A', 'minor', 80, 0),
    ('A18', 'Piano I', 6, 'piano', 'C', 'major', 96, 0),
    ('A21', 'Organ I', 6, 'organ', 'F', 'major', 104, 0),
    ('A22', 'Organ II', 6, 'organ', 'G', 'mixolydian', 112, 1),
    ('A23', 'Combo Organ', 6, 'combo_organ', 'A', 'minor', 132, 0),
    ('A24', 'Calliope', 6, 'calliope', 'C', 'major', 138, 0),
    ('A25', 'Donald Pluck', 6, 'quack', 'F', 'major', 120, 0),
    ('A26', 'Celeste* (1 oct.up)', 6, 'celeste', 'C', 'major', 92, 0),
    ('A27', 'Elect. Piano I', 6, 'epiano', 'F', 'major', 88, 0),
    ('A28', 'Elect. Piano II', 6, 'epiano', 'D', 'dorian', 96, 1),
    ('A31', 'Clock Chimes* (1 oct. up)', 6, 'chimes', 'E', 'major', 72, 0),
    ('A32', 'Steel Drums', 6, 'steel_drum', 'C', 'major', 120, 0),
    ('A33', 'Xylophone', 6, 'xylophone', 'F', 'major', 132, 0),
    ('A34', 'Brass III', 6, 'brass_swell', 'C', 'minor', 80, 2),
    ('A35', 'Fanfare', 6, 'fanfare', 'D', 'major', 100, 0),
    ('A36', 'String III', 6, 'strings', 'G', 'major', 72, 1),
    ('A37', 'Pizzicato', 6, 'pizzicato', 'D', 'major', 116, 0),
    ('A38', 'High Strings', 6, 'high_strings', 'A', 'major', 76, 0),
    ('A41', 'Bass clarinet', 6, 'low_reed', 'D', 'minor', 88, 0),
    ('A42', 'English Horn', 6, 'double_reed', 'E', 'minor', 72, 0),
    ('A43', 'Brass Ensemble', 6, 'brass_stab', 'F', 'major', 120, 1),
    ('A44', 'Guitar', 6, 'guitar', 'E', 'minor', 104, 0),
    ('A45', 'Koto', 6, 'koto', 'A', 'in', 84, 0),
    ('A46', 'Dark Pluck', 6, 'pluck', 'C', 'minor', 108, 0),
    ('A47', 'Funky I', 6, 'funk', 'E', 'dorian', 100, 0),
    ('A48', 'Synth Bass I (unison)', 1, 'bass', 'D', 'minor', 116, 0),
    ('A51', 'Lead I', 6, 'lead', 'A', 'minor', 108, 0),
    ('A52', 'Lead II', 6, 'lead', 'E', 'minor', 96, 1),
    ('A53', 'Lead III', 6, 'lead', 'D', 'dorian', 104, 2),
    ('A54', 'Funky II', 6, 'funk', 'A', 'dorian', 104, 1),
    ('A55', 'Synth Bass II', 6, 'bass', 'F', 'minor', 104, 1),
    ('A56', 'Funky III', 6, 'funk', 'G', 'mixolydian', 96, 2),
    ('A57', 'Thud Wah', 6, 'wah', 'D', 'minor', 92, 0),
    ('A58', 'Going Up', 6, 'riser', 'C', 'major', 90, 0),
    ('A61', 'Piano II', 6, 'piano', 'G', 'major', 104, 1),
    ('A62', 'Clav', 6, 'clav', 'D', 'dorian', 100, 0),
    ('A63', 'Frontier Organ', 6, 'organ', 'D', 'major', 120, 2),
    ('A64', 'Snare Drum (unison)', 1, 'snare', 'D', 'major', 112, 0),
    ('A65', 'Tom Toms (unison)', 1, 'toms', 'A', 'major', 104, 0),
    ('A66', 'Timpani (unison)', 1, 'timpani', 'D', 'major', 84, 0),
    ('A67', 'Shaker', 6, 'shaker', 'C', 'major', 116, 0),
    ('A68', 'Synth Pad', 6, 'pad', 'F', 'major', 84, 0),
    ('A71', 'Sweep I', 6, 'sweep', 'C', 'minor', 80, 0),
    ('A72', 'Pluck Sweep', 6, 'pluck', 'G', 'minor', 96, 1),
    ('A73', 'Repeater', 6, 'repeater', 'E', 'minor', 100, 0),
    ('A74', 'Sweep II', 6, 'sweep', 'A', 'minor', 76, 1),
    ('A75', 'Pluck Bell', 6, 'bell', 'D', 'major', 100, 0),
    ('A76', 'Dark Synth Piano', 6, 'piano', 'C', 'minor', 88, 2),
    ('A77', 'Sustainer', 6, 'pad', 'Bb', 'major', 80, 1),
    ('A78', 'Wah Release', 6, 'wah', 'G', 'minor', 100, 1),
    ('A81', 'Gong (play low chords)', 6, 'gong', 'C', 'minor', 60, 0),
    ('A82', 'Resonance Funk', 6, 'res_funk', 'E', 'minor', 108, 0),
    ('A83', 'Drum Booms* (1 oct. down)', 6, 'booms', 'A', 'minor', 96, 0),
    ('A84', 'Dust Storm', 6, 'storm', 'D', 'minor', 70, 0),
    ('A85', 'Rocket Men', 6, 'rocket', 'C', 'major', 80, 0),
    ('A86', 'Hand Claps', 6, 'claps', 'C', 'major', 108, 0),
    ('A87', 'FX Sweep', 6, 'fx_sweep', 'E', 'minor', 80, 0),
    ('A88', 'Caverns', 6, 'caverns', 'D', 'minor', 70, 0),
    ('B11', 'Strings', 6, 'strings', 'C', 'major', 76, 2),
    ('B12', 'Violin', 6, 'violin', 'D', 'minor', 84, 0),
    ('B13', 'Chorus Vibes', 6, 'vibes', 'F', 'major', 96, 0),
    ('B14', 'Organ 1', 6, 'organ', 'C', 'major', 108, 3),
    ('B15', 'Harpsichord 1', 6, 'harpsichord', 'D', 'minor', 84, 0),
    ('B16', 'Recorder', 6, 'recorder', 'G', 'major', 96, 0),
    ('B17', 'Perc. Pluck', 6, 'pluck', 'A', 'minor', 112, 2),
    ('B18', 'Noise Sweep', 6, 'noise_sweep', 'C', 'major', 72, 0),
    ('B21', 'Space Chimes', 6, 'space_chimes', 'E', 'lydian', 84, 0),
    ('B22', 'Nylon Guitar', 6, 'nylon', 'A', 'minor', 92, 0),
    ('B23', 'Orchestral Pad', 6, 'pad', 'D', 'major', 72, 2),
    ('B24', 'Bright Pluck', 6, 'pluck', 'E', 'major', 120, 3),
    ('B25', 'Organ Bell', 6, 'organ_bell', 'G', 'major', 96, 0),
    ('B26', 'Accordion', 6, 'accordion', 'D', 'minor', 120, 0),
    ('B27', 'FX Rise 1', 6, 'riser', 'D', 'minor', 84, 1),
    ('B28', 'FX Rise 2', 6, 'riser', 'F', 'major', 84, 2),
    ('B31', 'Brass', 6, 'brass_stab', 'D', 'major', 116, 2),
    ('B32', 'Helicopter', 6, 'helicopter', 'C', 'major', 90, 0),
    ('B33', 'Lute', 6, 'lute', 'D', 'minor', 88, 0),
    ('B34', 'Chorus Funk', 6, 'funk', 'C', 'dorian', 100, 3),
    ('B35', 'Tomita', 6, 'tomita', 'C', 'major', 80, 0),
    ('B36', 'FX Sweep 1', 6, 'fx_sweep', 'G', 'minor', 76, 1),
    ('B37', 'Sharp Reed', 6, 'reed', 'D', 'minor', 104, 0),
    ('B38', 'Bass Pluck', 6, 'bass', 'G', 'minor', 112, 2),
    ('B41', 'Resonant Rise', 6, 'res_funk', 'A', 'minor', 100, 1),
    ('B42', 'Harpsichord 2', 6, 'harpsichord', 'G', 'major', 88, 1),
    ('B43', 'Dark Ensemble', 6, 'pad', 'C', 'minor', 72, 3),
    ('B44', 'Contact Wah', 6, 'contact_wah', 'E', 'dorian', 100, 0),
    ('B45', 'Noise Sweep 2', 6, 'noise_sweep', 'C', 'major', 60, 1),
    ('B46', 'Glassy Wah', 6, 'wah', 'A', 'dorian', 104, 2),
    ('B47', 'Phase Ensemble', 6, 'pad', 'E', 'minor', 76, 4),
    ('B48', 'Chorused Bell', 6, 'bell', 'A', 'major', 84, 1),
    ('B51', 'Clav', 6, 'clav', 'G', 'mixolydian', 104, 1),
    ('B52', 'Organ 2', 6, 'organ', 'Bb', 'major', 100, 4),
    ('B53', 'Bassoon', 6, 'bassoon', 'Bb', 'major', 92, 0),
    ('B54', 'Auto Release Noise Sweep', 6, 'noise_sweep', 'C', 'major', 84, 2),
    ('B55', 'Brass Ensemble', 6, 'brass_stab', 'Eb', 'major', 108, 3),
    ('B56', 'Ethereal', 6, 'ethereal', 'E', 'lydian', 60, 0),
    ('B57', 'Chorus Bell 2', 6, 'bell', 'F', 'major', 76, 2),
    ('B58', 'Blizzard', 6, 'storm', 'G', 'minor', 66, 1),
    ('B61', 'E. Piano with Tremolo', 6, 'epiano', 'Bb', 'major', 84, 2),
    ('B62', 'Clarinet', 6, 'clarinet', 'F', 'major', 96, 0),
    ('B63', 'Thunder', 6, 'thunder', 'C', 'minor', 60, 0),
    ('B64', 'Reedy Organ', 6, 'organ', 'E', 'minor', 96, 5),
    ('B65', 'Flute / Horn', 6, 'horn', 'F', 'major', 80, 0),
    ('B66', 'Toy Rhodes', 6, 'toy_rhodes', 'C', 'major', 100, 0),
    ('B67', "Surf's Up", 6, 'surf', 'G', 'major', 72, 0),
    ('B68', 'OW Bass', 6, 'ow_bass', 'E', 'minor', 92, 0),
    ('B71', 'Piccolo', 6, 'piccolo', 'D', 'major', 112, 0),
    ('B72', 'Melodic Taps', 6, 'taps', 'A', 'minor', 104, 0),
    ('B73', 'Meow Brass', 6, 'meow', 'C', 'blues', 96, 0),
    ('B74', 'Violin (high)', 6, 'violin', 'A', 'minor', 88, 1),
    ('B75', 'High Bells', 6, 'bell', 'C', 'major', 108, 3),
    ('B76', 'Rolling Wah', 6, 'rolling_wah', 'D', 'minor', 84, 0),
    ('B77', 'Ping Bell', 6, 'bell', 'Eb', 'major', 96, 4),
    ('B78', 'Brassy Organ', 6, 'organ', 'F', 'mixolydian', 116, 6),
    ('B81', 'Low Dark Strings', 6, 'low_strings', 'C', 'minor', 76, 0),
    ('B82', 'Piccolo Trumpet', 6, 'piccolo_trumpet', 'D', 'major', 100, 0),
    ('B83', 'Cello', 6, 'cello', 'G', 'minor', 72, 0),
    ('B84', 'High Strings', 6, 'high_strings', 'E', 'major', 80, 1),
    ('B85', 'Rocket Men', 6, 'rocket', 'G', 'major', 84, 1),
    ('B86', 'Forbidden Planet', 6, 'forbidden', 'C', 'whole', 72, 0),
    ('B87', 'Froggy', 6, 'froggy', 'F', 'major', 112, 0),
    ('B88', 'Owgan', 6, 'owgan', 'D', 'mixolydian', 100, 0),
]

PRODUCT = [  # slot, name, file in the session library, voice limit
    ('YB1', 'Round Sub', '01-YB1.mid', 1), ('YB2', 'Pulse Pluck', '02-YB2.mid', 1),
    ('YB3', 'Solid Saw', '03-YB3.mid', 1), ('YB4', 'Rubber Bass', '04-YB4.mid', 1),
    ('YB5', 'Octave Weight', '05-YB5.mid', 2), ('YB6', 'Short PWM', '06-YB6.mid', 1),
    ('YB7', 'Glide Mono', '07-YB7.mid', 1), ('YB8', 'Hollow Square', '08-YB8.mid', 1),
    ('YP1', 'Warm Ensemble', '09-YP1.mid', 6), ('YP2', 'Slow Horizon', '10-YP2.mid', 6),
    ('YP3', 'Glass Halo', '11-YP3.mid', 6), ('YP4', 'Velvet PWM', '12-YP4.mid', 6),
    ('YP5', 'Hollow Choir', '13-YP5.mid', 6), ('YP6', 'Brass Cloud', '14-YP6.mid', 6),
    ('YP7', 'Resonant Mist', '15-YP7.mid', 6), ('YP8', 'Dark Motion', '16-YP8.mid', 6),
]

PITCH_CLASS = {'C': 0, 'Db': 1, 'D': 2, 'Eb': 3, 'E': 4, 'F': 5, 'Gb': 6, 'G': 7,
               'Ab': 8, 'A': 9, 'Bb': 10, 'B': 11}
MODES = {
    'major': [0, 2, 4, 5, 7, 9, 11], 'minor': [0, 2, 3, 5, 7, 8, 10],
    'dorian': [0, 2, 3, 5, 7, 9, 10], 'mixolydian': [0, 2, 4, 5, 7, 9, 10],
    'lydian': [0, 2, 4, 6, 7, 9, 11], 'in': [0, 1, 5, 7, 8, 12, 13],
    'blues': [0, 3, 5, 6, 7, 10, 12], 'whole': [0, 2, 4, 6, 8, 10, 12],
}


class Key:
    """Scale-degree arithmetic around a tonic in a chosen octave."""

    def __init__(self, tonic, mode, octave):
        self.root = 12 * (octave + 1) + PITCH_CLASS[tonic]
        self.steps = MODES[mode]

    def __call__(self, degree, octave=0):
        """Degree 0 is the tonic; negative and >6 degrees wrap by octave."""
        o, d = divmod(degree, 7)
        return self.root + 12 * (o + octave) + self.steps[d]

    def triad(self, degree, octave=0, top=3):
        return [self(degree + 2 * i, octave) for i in range(top)]


class Score:
    def __init__(self, bpm):
        self.bpm = bpm
        self.notes = []  # (beat, length, sounding pitch, velocity)

    def add(self, beat, length, pitches, velocity=100):
        if isinstance(pitches, int):
            pitches = [pitches]
        for pitch in pitches:
            self.notes.append((beat, length, pitch, velocity))
        return self

    def line(self, start, steps, velocity=100, legato=0.92):
        """steps: (beats, pitch or None); returns the beat after the line."""
        beat = start
        for length, pitch in steps:
            if pitch is not None:
                self.add(beat, length * legato, pitch, velocity)
            beat += length
        return beat

    @property
    def end(self):
        return max(b + l for b, l, _, _ in self.notes)


# Each role writes a phrase of about 9-16 seconds at its tempo. Variants give
# presets that share a role a different progression, rhythm or contour.

def bass(k, v, s):
    lines = [
        [(.75, 0), (.25, 0), (.5, 7), (.5, 4), (.5, 2), (.5, 0), (.5, -1), (.5, 0)],
        [(.5, 0), (.25, None), (.25, 0), (.5, 7), (.25, 0), (.25, 6), (.5, 4), (.5, 2), (.5, 3), (.5, 4)],
        [(.25, 0), (.25, 0), (.25, 7), (.25, 0), (.5, 2), (.25, 0), (.25, 4), (.5, -1), (.5, -2), (1, 0)],
    ][v]
    beat = 0
    for shift in ([0, 0, 5, 4] if v != 1 else [0, 2, 5, 4]):
        start = beat
        for length, degree in lines:
            if degree is not None:
                s.add(beat, length * (.55 if v == 2 else .85), k(degree + shift),
                      110 if beat % 1 == 0 else 92)
            beat += length
        beat = start + 4
    s.add(beat, 1.5, k(0), 112)


def brass_stab(k, v, s):
    chords = [[0, 3, 4, 0], [0, 5, 3, 4], [0, -2, 3, 4], [0, 1, 3, 4]][v]
    rhythm = [[(0, .4), (.75, .4), (1.5, 1.2)], [(0, .3), (.5, .3), (1.5, .3), (2, 1.5)],
              [(0, .6), (1, .25), (1.5, 1.4)], [(0, .35), (.75, .35), (1.25, .35), (2, 1.6)]][v]
    for bar, degree in enumerate(chords):
        chord = k.triad(degree) + [k(degree, 1)]
        for at, length in rhythm:
            s.add(bar * 4 + at, length, chord, 112 if at == 0 else 100)
    s.add(16, 3, k.triad(0) + [k(0, 1), k(0, -1)], 116)


def brass_swell(k, v, s):
    progression = [[0, 3, 5, 4], [0, 5, 3, 4], [0, -2, 3, 4]][v]
    for bar, degree in enumerate(progression):
        chord = [k(degree, -1)] + k.triad(degree) + [k(degree + 4)]
        s.add(bar * 4, 3.7, chord, 104)
    s.add(16, 6, [k(0, -1)] + k.triad(0) + [k(4)], 108)


def trumpet(k, v, s):
    phrase = [(1, 4), (.5, 4), (.5, 5), (1, 6), (1, 4), (1.5, 7), (.5, 6), (2, 4),
              (.5, 2), (.5, 4), (1, 5), (1, 3), (1, 6), (1, 5), (3, 4)]
    beat = 0
    for length, degree in phrase:
        s.add(beat, length * .9, k(degree), 108 if length >= 1 else 96)
        beat += length


def flute(k, v, s):
    phrase = [(1.5, 4), (.5, 5), (1, 6), (1, 7), (2, 6), (1, 4), (1, 2),
              (1.5, 3), (.5, 4), (1, 5), (1, 3), (3, 4)]
    s.line(0, [(l, k(d)) for l, d in phrase], 92)


def strings(k, v, s):
    progressions = [[[0, 2, 4, 7], [-2, 3, 5, 7], [-3, 2, 4, 7], [-3, 1, 4, 6]],
                    [[0, 4, 7, 9], [-3, 4, 7, 9], [-2, 3, 5, 8], [-3, 4, 6, 8]],
                    [[0, 4, 7, 9], [-4, 3, 5, 9], [-2, 3, 5, 8], [-3, 1, 4, 6]]][v]
    for bar, voicing in enumerate(progressions):
        s.add(bar * 4, 3.85, [k(d, -1) for d in voicing], 100)
    s.add(16, 5, [k(d, -1) for d in [0, 4, 7, 9]], 100)
    # A slow top line moves across the held chords.
    s.line(0, [(2, k(9)), (2, k(8)), (2, k(8)), (2, k(7)), (2, k(6)), (2, k(7)), (4, k(7))], 92, legato=.97)


def choir(k, v, s):
    for bar, voicing in enumerate([[0, 4, 7, 9], [-2, 3, 5, 7], [-4, 2, 5, 7], [-3, 1, 4, 6]]):
        s.add(bar * 4, 3.9, [k(d, -1) for d in voicing], 96)
    s.add(16, 6, [k(d, -1) for d in [0, 4, 7, 9]], 96)


def piano(k, v, s):
    progression = [[0, 5, 3, 4], [0, 3, 5, 4], [0, 5, 3, 4]][v]
    for bar, degree in enumerate(progression):
        left = k(degree, -1)
        chord = k.triad(degree)
        s.add(bar * 4, 1.8, left, 96)
        s.add(bar * 4 + 2, 1.8, k(degree + 4, -1), 88)
        for at in [0.5, 1.5, 2.5, 3.5] if v != 2 else [0, 2]:
            s.add(bar * 4 + at, .45 if v != 2 else 1.8, chord, 84)
    s.add(16, 3, [k(0, -1)] + k.triad(0) + [k(0, 1)], 100)


def organ(k, v, s):
    progressions = [[0, 3, 4, 0], [0, -1, 3, 0], [0, 3, 0, 4], [0, 5, 1, 4], [0, 3, -1, 4], [0, -2, 3, 4], [0, -1, 3, 4]]
    progression = progressions[v]
    for bar, degree in enumerate(progression):
        chord = [k(degree, -1)] + k.triad(degree)
        if v in (1, 6):  # gospel/rock comping
            for at, length in [(0, .9), (1.5, .4), (2, .9), (3.5, .4)]:
                s.add(bar * 4 + at, length, chord, 100)
        elif v == 2:  # country two-step
            for at in range(4):
                s.add(bar * 4 + at, .45, chord if at % 2 else [k(degree, -2)], 100)
        else:
            s.add(bar * 4, 3.9, chord, 100)
    s.add(16, 3, [k(0, -1)] + k.triad(0), 100)
    if v in (0, 3, 4, 5):
        s.line(0, [(1, k(4)), (1, k(5)), (2, k(4)), (1, k(3)), (1, k(2)), (2, k(3)),
                   (1, k(4)), (1, k(6)), (2, k(5)), (2, k(4)), (2, k(2))], 96)


def combo_organ(k, v, s):
    riff = [(.5, 0), (.5, 2), (.5, 4), (.5, 2), (.5, 5), (.5, 4), (1, 2)]
    for bar, shift in enumerate([0, 0, -2, -3]):
        s.line(bar * 4, [(l, k(d + shift)) for l, d in riff], 100, .8)
    s.add(16, 2, k.triad(0), 104)


def calliope(k, v, s):
    melody = [(1, 0), (.5, 2), (.5, 4), (1, 7), (1, 4), (1, 5), (.5, 4), (.5, 3), (2, 2),
              (1, 1), (.5, 3), (.5, 5), (1, 8), (1, 7), (1, 6), (1, 5), (2, 4)]
    s.line(0, [(l, k(d)) for l, d in melody], 104, .85)
    for bar, degree in enumerate([0, 4, 4, 0, 0]):
        for beat in (1, 2):
            s.add(bar * 4 + beat, .4, k.triad(degree, -1), 80)


def quack(k, v, s):
    riff = [(.5, 0), (.25, None), (.25, 2), (.5, 4), (.5, 3), (.75, 2), (.25, None), (1, 0)]
    for bar, shift in enumerate([0, 3, 4, 0]):
        s.line(bar * 4, [(l, None if d is None else k(d + shift)) for l, d in riff], 100, .7)
    s.add(16, 1, k(0), 104)


def celeste(k, v, s):
    melody = [(1, 4), (1, 2), (1, 0), (1, 2), (2, 4), (1, 5), (1, 4), (1, 2), (1, 4), (2, 7),
              (1, 5), (1, 4), (1, 2), (1, 1), (3, 0)]
    s.line(0, [(l, k(d)) for l, d in melody], 92)
    for bar, degree in enumerate([0, 0, 3, 4]):
        s.add(bar * 4, 1.8, k.triad(degree, -1), 76)


def epiano(k, v, s):
    progression = [[[1, 3, 5, 7], [4, 6, 8, 10], [0, 2, 4, 6], [5, 7, 9, 11]],
                   [[0, 2, 4, 6], [3, 5, 7, 9], [0, 2, 4, 6], [3, 5, 7, 9]],
                   [[0, 2, 4, 6], [5, 7, 9, 11], [1, 3, 5, 7], [4, 6, 8, 10]]][v]
    for bar, chord in enumerate(progression):
        s.add(bar * 4, .9, k(chord[0], -1), 96)
        s.add(bar * 4 + .5, 1.4, [k(d) for d in chord[1:]], 88)
        s.add(bar * 4 + 2.5, 1.3, [k(d) for d in chord[1:]], 80)
    s.add(16, 3, [k(0, -1)] + [k(d) for d in [2, 4, 6, 8]], 92)


def chimes(k, v, s):
    # Westminster quarters, then the hour.
    for i, phrase in enumerate([[2, 0, 1, -3], [-3, 1, 2, 0], [2, 1, 0, -3]]):
        s.line(i * 4, [(1, k(d)) for d in phrase], 100, .95)
    for i in range(3):
        s.add(12 + i * 2, 1.9, k(0, -1), 104)


def steel_drum(k, v, s):
    melody = [(.5, 4), (.5, 4), (.5, 5), (.5, 4), (1, 2), (1, 0), (.5, 2), (.5, 4), (1, 5), (1, 4),
              (.5, 4), (.5, 5), (.5, 7), (.5, 5), (1, 4), (1, 2), (.5, 1), (.5, 2), (1, 0), (1, None)]
    s.line(0, [(l, None if d is None else k(d)) for l, d in melody], 104, .5)
    for bar, degree in enumerate([0, 4, 0, 4]):
        for at in (.5, 1.5, 2.5, 3.5):
            s.add(bar * 4 + at, .3, k.triad(degree, -1)[1:], 76)


def xylophone(k, v, s):
    run = [0, 2, 4, 7, 9, 7, 4, 2]
    beat = 0
    for bar, shift in enumerate([0, 3, 4, 0]):
        for d in run:
            s.add(beat, .2, k(d + shift), 100 if beat % 1 == 0 else 86)
            beat += .5
    s.add(beat, 1, k(7), 104)


def fanfare(k, v, s):
    calls = [(.5, 0), (.25, 0), (.25, 0), (1, 4), (.5, 2), (.5, 4), (2, 7),
             (.5, 4), (.25, 4), (.25, 4), (1, 5), (.5, 3), (.5, 1), (2, 4)]
    beat = s.line(0, [(l, k(d)) for l, d in calls], 112, .85)
    s.add(beat, 3, [k(0, -1)] + k.triad(0), 116)
    for bar, degree in enumerate([0, 0, 3, 4]):
        s.add(bar * 4, 3.8, [k(degree, -1), k(degree + 4, -1)], 96)


def pizzicato(k, v, s):
    pattern = [0, 2, 4, 2, 7, 4, 2, 4]
    for bar, shift in enumerate([0, 3, 4, 0]):
        for i, d in enumerate(pattern):
            s.add(bar * 4 + i * .5, .2, k(d + shift), 100 if i % 2 == 0 else 84)
        s.add(bar * 4, .25, k(shift, -1), 96)
    s.add(16, .3, [k(0, -1), k(0), k(4), k(0, 1)], 108)


def high_strings(k, v, s):
    melody = [(3, 4), (1, 5), (2, 6), (2, 4), (3, 3), (1, 2), (4, 4)] if v == 0 else \
             [(2, 2), (2, 4), (3, 6), (1, 5), (2, 4), (2, 2), (4, 1)]
    s.line(0, [(l, k(d)) for l, d in melody], 96, .97)
    for bar, voicing in enumerate([[0, 2, 4], [-2, 0, 3], [-4, -1, 2], [-3, 1, 4]]):
        s.add(bar * 4, 3.9, [k(d) for d in voicing], 88)


def low_reed(k, v, s):
    melody = [(1, 0), (1, 2), (1, 4), (.5, 3), (.5, 2), (2, 0), (1, -1), (1, 1), (1, 3),
              (.5, 2), (.5, 1), (2, -1), (1, 0), (1, 4), (2, 7), (4, 0)]
    s.line(0, [(l, k(d)) for l, d in melody], 96, .9)


def double_reed(k, v, s):
    melody = [(2, 4), (1, 3), (1, 2), (2, 1), (1, 2), (1, 4), (3, 3), (1, 2), (4, 0)]
    s.line(0, [(l, k(d)) for l, d in melody], 92, .95)


def guitar(k, v, s):
    shapes = [[0, 4, 7, 9, 11], [-2, 3, 5, 9, 10], [-4, 2, 5, 7, 9], [-3, 1, 4, 7, 8]]
    for bar, shape in enumerate(shapes):
        notes = [k(d, -1) for d in shape]
        for i, pitch in enumerate(notes + notes[-2:0:-1][:3]):
            s.add(bar * 4 + i * .5, 1.4 if i < 4 else .9, pitch, 96)
    s.add(16, 3, [k(d, -1) for d in [0, 4, 7, 9, 11]], 100)


def koto(k, v, s):
    melody = [(1, 4), (.5, 3), (.5, 2), (1, 1), (1, 2), (1.5, 4), (.5, 5), (2, 4),
              (1, 2), (.5, 1), (.5, 0), (1, 1), (1, 2), (3, 0)]
    beat = 0
    for length, d in melody:
        s.add(beat, min(length, 1.5), k(d), 100)
        beat += length
    s.add(0, 4, [k(0, -1), k(3, -1)], 80)
    s.add(8, 4, [k(-2, -1), k(1, -1)], 80)


def pluck(k, v, s):
    arps = [[0, 2, 4, 7, 4, 2], [0, 4, 7, 9, 7, 4], [0, 2, 4, 6, 4, 2], [0, 4, 7, 11, 9, 7]][v]
    for bar, shift in enumerate([0, -2, -4, -3] if v in (0, 2) else [0, 3, 4, 0]):
        for i in range(8):
            s.add(bar * 4 + i * .5, .35, k(arps[i % len(arps)] + shift), 100 if i % 2 == 0 else 86)
    s.add(16, 2, k.triad(0) + [k(0, 1)], 100)


def funk(k, v, s):
    riffs = [
        [(.25, 0), (.25, None), (.25, 0), (.25, 2), (.5, None), (.25, 3), (.25, 0), (.5, 4), (.25, None), (.25, 3), (.5, 0), (.5, None)],
        [(.5, [2, 4, 6]), (.25, None), (.25, [2, 4, 6]), (.5, None), (.25, [2, 4, 6]), (.25, [3, 5, 7]), (.5, None), (.5, [2, 4, 6]), (.25, None), (.25, [2, 4, 6]), (.5, None)],
        [(.25, 0), (.25, 0), (.25, None), (.25, 7), (.25, 6), (.25, 4), (.5, 0), (.25, None), (.25, 3), (.5, 4), (.5, 0), (.5, None)],
        [(.25, [0, 2, 4]), (.25, None), (.5, [0, 2, 4]), (.25, None), (.25, [1, 3, 5]), (.5, [0, 2, 4]), (.5, None), (.25, [0, 2, 4]), (.25, [0, 2, 4]), (.5, None), (.5, [-1, 1, 3])],
    ][v]
    beat = 0
    for bar, shift in enumerate([0, 0, 3, 0]):
        start = beat
        for length, d in riffs:
            if d is not None:
                pitches = [k(x + shift) for x in (d if isinstance(d, list) else [d])]
                s.add(beat, length * .6, pitches, 104 if (beat - start) % 1 == 0 else 90)
            beat += length
        beat = start + 4
    s.add(16, .5, [k(0)] if v in (0, 2) else [k(x) for x in (0, 2, 4)], 108)


def lead(k, v, s):
    melodies = [
        [(1, 4), (.5, 5), (.5, 4), (1, 2), (1, 0), (1.5, 2), (.5, 4), (2, 5), (1, 7), (.5, 6), (.5, 5), (1, 4), (1, 2), (4, 4)],
        [(2, 0), (1, 2), (1, 3), (1.5, 4), (.5, 3), (1, 2), (1, 0), (2, -1), (2, 1), (4, 0)],
        [(.5, 0), (.5, 2), (.5, 3), (.5, 4), (1, 6), (1, 4), (.5, 3), (.5, 4), (1, 2), (1, 0),
         (.5, 7), (.5, 6), (.5, 4), (.5, 3), (1, 4), (1, 2), (3, 0)],
    ][v]
    s.line(0, [(l, k(d)) for l, d in melodies], 104, .95)


def wah(k, v, s):
    rhythm = [[(0, .5), (1.5, .5), (2.5, 1)], [(0, 1.5), (2, 1.5)], [(0, .25), (.75, .25), (1.5, .75), (3, .5)]][v]
    for bar, degree in enumerate([0, 0, 3, 4]):
        for at, length in rhythm:
            s.add(bar * 4 + at, length, [k(degree, -1)] + k.triad(degree)[1:], 104)
    s.add(16, 2, [k(0, -1)] + k.triad(0)[1:], 108)


def riser(k, v, s):
    for i, degree in enumerate([0, 2, 4, 7]):
        s.add(i * 2, 9 - i * 2, k(degree + 7 * (v == 1)), 100)
    s.add(10, 6, [k(0, 1), k(4, 1)], 104)


def clav(k, v, s):
    riff = [(.25, 0), (.25, None), (.25, 0), (.25, 3), (.25, None), (.25, 4), (.5, 0), (.25, 2), (.25, None), (.25, 0), (.25, -1), (.5, 0), (.5, None)]
    if v == 1:
        riff = [(.5, [0, 2]), (.25, None), (.25, [0, 2]), (.25, 4), (.25, 3), (.5, [0, 2]), (.5, None), (.25, 6), (.25, 7), (.5, [4, 6]), (.5, None)]
    for bar, shift in enumerate([0, 0, 3, 0]):
        beat = bar * 4
        for length, d in riff:
            if d is not None:
                s.add(beat, length * .6, [k(x + shift) for x in (d if isinstance(d, list) else [d])], 104)
            beat += length
    s.add(16, .5, [k(0), k(2), k(4)], 108)


def snare(k, v, s):
    pattern = [(0, 96), (1, 116), (2, 96), (3, 116), (3.75, 80)]
    for bar in range(3):
        for at, vel in pattern:
            s.add(bar * 4 + at, .2, k(0), vel)
    for i in range(16):  # closing roll
        s.add(12 + i * .25, .12, k(0), 70 + i * 3)
    s.add(16, .4, k(0), 124)


def toms(k, v, s):
    fills = [0, 2, 4, 2, 0, -3]
    for bar in range(3):
        for i, d in enumerate([4, 4, 2, 0]):
            s.add(bar * 4 + i, .4, k(d), 104)
    for i, d in enumerate(fills + [-5, -5]):
        s.add(12 + i * .5, .3, k(d), 112)
    s.add(16, .6, k(-7), 120)


def timpani(k, v, s):
    s.add(0, 1.5, k(0), 110)
    s.add(2, 1.5, k(-3), 100)
    s.add(4, 1.5, k(0), 110)
    for i in range(16):  # roll on the dominant into the tonic
        s.add(8 + i * .25, .2, k(-3), 70 + i * 3)
    s.add(12, 1, k(0), 124)
    s.add(14, 1, k(-3), 104)
    s.add(15, 2, k(0), 120)


def shaker(k, v, s):
    for i in range(32):
        s.add(i * .5, .2, k(0), 108 if i % 2 else 80)


def pad(k, v, s):
    progressions = [[[0, 2, 4, 6], [3, 5, 7, 9], [5, 7, 9, 11], [4, 6, 8, 10]],
                    [[0, 4, 7, 9], [3, 7, 9, 12], [-2, 2, 4, 7], [-3, 1, 4, 6]],
                    [[0, 4, 7, 9, 11], [-2, 3, 5, 7, 9], [-4, 2, 5, 7, 9], [-3, 1, 4, 6, 8]],
                    [[0, 2, 4, 7], [-2, 2, 4, 7], [-3, 1, 4, 6], [-1, 1, 3, 6]],
                    [[0, 4, 6, 9], [-3, 2, 4, 7], [-2, 3, 5, 7], [0, 4, 6, 9]]][v]
    for bar, voicing in enumerate(progressions):
        s.add(bar * 4, 3.9, [k(d, -1) for d in voicing], 96)
    s.add(16, 5, [k(d, -1) for d in progressions[0]], 96)


def sweep(k, v, s):
    s.add(0, 7.8, [k(0, -1), k(4, -1), k(7, -1)], 100)
    s.add(8, 7.8, [k(-2, -1), k(3, -1), k(5, -1)], 100)
    s.add(16, 6, [k(0, -1), k(4, -1), k(7, -1), k(9, -1)], 100)


def repeater(k, v, s):
    for bar, voicing in enumerate([[0, 4, 7], [-2, 2, 5], [-3, 2, 4], [-1, 1, 4]]):
        s.add(bar * 4, 3.8, [k(d, -1) for d in voicing], 100)
    s.add(16, 4, [k(d, -1) for d in [0, 4, 7, 9]], 100)


def bell(k, v, s):
    melodies = [
        [(1, 7), (1, 4), (1, 5), (1, 2), (2, 4), (2, 0), (1, 2), (1, 4), (1, 5), (1, 7), (4, 4)],
        [(2, 4), (1, 7), (1, 6), (2, 4), (2, 2), (2, 1), (1, 2), (1, 4), (4, 0)],
        [(1.5, 0), (.5, 4), (2, 7), (1.5, 5), (.5, 4), (2, 2), (1, 4), (1, 2), (2, 0)],
        [(.5, 0), (.5, 4), (.5, 7), (.5, 9), (1, 7), (1, 4), (.5, 2), (.5, 4), (.5, 5), (.5, 7), (1, 4), (1, 2),
         (.5, 0), (.5, 2), (.5, 4), (.5, 5), (2, 4), (2, 0)],
        [(1, 4), (1, 2), (2, 0), (1, 5), (1, 4), (2, 2), (1, 4), (1, 7), (2, 6), (4, 7)],
    ][v]
    s.line(0, [(l, k(d)) for l, d in melodies], 100, .9)


def gong(k, v, s):
    # The patch notes ask for low chords.
    for at, voicing in [(0, [0, 4, 7]), (4, [-2, 2, 5]), (8, [-3, 1, 4]), (12, [0, 4, 7])]:
        s.add(at, 3.5, [k(d, -1) for d in voicing], 112)


def res_funk(k, v, s):
    riff = [(.5, 0), (.25, 0), (.25, 7), (.5, 3), (.5, 0), (.25, 5), (.25, 4), (.5, 3), (1, 0)] if v == 0 else \
           [(1, 0), (.5, 7), (.5, 0), (1, 3), (1, 4)]
    for bar, shift in enumerate([0, 0, 3, 4]):
        beat = bar * 4
        for length, d in riff:
            s.add(beat, length * .7, k(d + shift), 108)
            beat += length
    s.add(16, 1.5, k(0), 112)


def booms(k, v, s):
    for bar in range(4):
        for at, d in [(0, 0), (1.5, 0), (2, 4), (3, -3)]:
            s.add(bar * 4 + at, .6, k(d), 116 if at == 0 else 100)
    s.add(16, 1, k(0), 124)


def storm(k, v, s):
    s.add(0, 7.8, [k(0, -1), k(4, -1)], 100)
    s.add(4, 11.8, [k(7, -1), k(9, -1)], 96)
    s.add(8, 8, [k(2, -1)], 90)


def rocket(k, v, s):
    for i in range(3):
        s.add(i * 5, 4.5, k(0 + 2 * i), 104)
    s.add(15, 4, [k(0), k(4)], 108)


def claps(k, v, s):
    pattern = [1, 3, 3.5] if v == 0 else [1, 2.75, 3]
    for bar in range(4):
        for at in pattern:
            s.add(bar * 4 + at, .2, [k(0), k(4)], 110)
    s.add(16, .3, [k(0), k(4)], 120)


def fx_sweep(k, v, s):
    s.add(0, 5.8, [k(0, -1), k(7, -1)], 104)
    s.add(6, 5.8, [k(-2, -1), k(5, -1)], 104)
    s.add(12, 6, [k(0, -1), k(4, -1), k(7, -1)], 104)


def caverns(k, v, s):
    for at, d in [(0, 0), (2, 4), (4, 2), (6, -1), (8, 0), (10, 3), (12, -2), (14, 0)]:
        s.add(at, 1.5, k(d, -1), 100)
    s.add(0, 15.5, k(0, -2), 90)


def violin(k, v, s):
    melody = [(1, 4), (1, 5), (2, 7), (.5, 6), (.5, 5), (1, 4), (2, 2), (1, 3), (1, 4), (2, 5),
              (1, 4), (1, 2), (4, 0)] if v == 0 else \
             [(2, 7), (1, 6), (1, 7), (2, 9), (2, 7), (1, 6), (1, 4), (2, 5), (4, 4)]
    s.line(0, [(l, k(d)) for l, d in melody], 100, .97)


def vibes(k, v, s):
    for bar, chord in enumerate([[0, 2, 4, 6], [3, 5, 7, 9], [1, 3, 5, 7], [4, 6, 8, 10]]):
        s.add(bar * 4, 1.5, [k(d, -1) for d in chord], 88)
    melody = [(1.5, 6), (.5, 4), (1, 2), (1, 4), (2, 5), (1, 4), (1, 2), (1.5, 1), (.5, 2),
              (1, 3), (1, 4), (4, 4)]
    s.line(0, [(l, k(d)) for l, d in melody], 100, .8)


def harpsichord(k, v, s):
    figure = [0, 4, 7, 4, 9, 4, 7, 4] if v == 0 else [0, 2, 4, 5, 7, 5, 4, 2]
    beat = 0
    for bar, shift in enumerate([0, 3, -3, 4]):
        for d in figure:
            s.add(beat, .22, k(d + shift), 100)
            beat += .25
        s.add(bar * 2, 1.9, k(shift, -1), 92)
    for d in [0, 4, 7, 11, 7, 4, 0]:
        s.add(beat, .22, k(d), 100)
        beat += .25
    s.add(beat, 2, [k(0, -1), k(0), k(4), k(7)], 104)


def recorder(k, v, s):
    melody = [(1, 0), (1, 2), (1, 4), (1, 5), (2, 4), (1, 2), (1, 0), (1, 1), (1, 2), (2, 3), (1, 2), (1, 1), (4, 0)]
    s.line(0, [(l, k(d)) for l, d in melody], 92, .85)


def noise_sweep(k, v, s):
    s.add(0, [12, 16, 8][v], [k(0), k(4)], 100)
    if v == 2:
        s.add(9, 3, [k(2), k(5)], 100)
        s.add(13, 3, [k(0), k(4)], 100)


def space_chimes(k, v, s):
    for i, d in enumerate([0, 4, 7, 11, 9, 4, 6, 2, 11, 7, 4, 0]):
        s.add(i * 1.25, 1.2, k(d), 92 + (i % 3) * 8)


def nylon(k, v, s):
    pattern = [0, 4, 7, 9, 11, 9, 7, 4]
    for bar, shift in enumerate([0, -2, -4, -3]):
        for i, d in enumerate(pattern):
            s.add(bar * 4 + i * .5, 1.2 if i == 0 else .7, k(d + shift, -1), 96 if i else 104)
    s.add(16, 3, [k(d, -1) for d in [0, 4, 7, 9]], 100)


def organ_bell(k, v, s):
    melody = [(1, 4), (1, 2), (1, 0), (1, 4), (2, 5), (2, 7), (1, 9), (1, 7), (1, 5), (1, 4), (4, 2)]
    s.line(0, [(l, k(d)) for l, d in melody], 100, .9)
    for bar, degree in enumerate([0, 3, 5, 4]):
        s.add(bar * 4, 3.9, k.triad(degree, -1), 84)


def accordion(k, v, s):
    melody = [(1, 4), (.5, 3), (.5, 4), (1, 6), (1, 4), (1, 3), (1, 1), (2, 2),
              (1, 2), (.5, 1), (.5, 2), (1, 4), (1, 3), (4, 0)]
    s.line(0, [(l, k(d)) for l, d in melody], 100, .9)
    for bar, degree in enumerate([0, 4, 4, 0]):
        for at in (0, 2):
            s.add(bar * 4 + at, .5, k(degree, -1), 96)
        for at in (1, 3):
            s.add(bar * 4 + at, .5, k.triad(degree, -1)[1:], 84)


def helicopter(k, v, s):
    s.add(0, 15, k(0), 110)


def lute(k, v, s):
    for bar, shift in enumerate([0, -2, -3, -4]):
        for i, d in enumerate([0, 2, 4, 7, 4, 2]):
            s.add(bar * 3 + i * .5, 1.0, k(d + shift), 96)
    s.add(12, 3, [k(d) for d in [0, 4, 7]], 100)


def tomita(k, v, s):
    melody = [(2, 4), (1, 2), (1, 1), (2, 0), (2, -1), (1, 0), (1, 2), (2, 4), (4, 5), (4, 4)]
    s.line(0, [(l, k(d)) for l, d in melody], 100, .97)


def reed(k, v, s):
    melody = [(.5, 0), (.5, 2), (1, 3), (.5, 4), (.5, 3), (1, 2), (1, 0), (1, 4), (.5, 5), (.5, 4),
              (1, 3), (1, 2), (2, 1), (.5, 2), (.5, 3), (1, 4), (4, 0)]
    s.line(0, [(l, k(d)) for l, d in melody], 104, .9)


def contact_wah(k, v, s):
    for bar in range(4):
        for at, length, d in [(0, .5, 0), (.75, .25, 0), (1.5, .5, 3), (2.5, .5, 4), (3.25, .5, 2)]:
            s.add(bar * 4 + at, length * .8, [k(d), k(d + 2)], 104)
    s.add(16, 1, [k(0), k(2), k(4)], 108)


def bassoon(k, v, s):
    melody = [(1, 0), (.5, 1), (.5, 2), (1, 4), (1, 2), (1, 3), (1, 1), (2, -1), (1, 0), (1, 4),
              (1, 3), (1, 1), (4, 0)]
    s.line(0, [(l, k(d)) for l, d in melody], 96, .8)


def ethereal(k, v, s):
    s.add(0, 15.5, [k(0, -1), k(4, -1)], 92)
    s.add(4, 11.5, [k(1), k(6, -1)], 88)
    s.add(8, 7.5, [k(2)], 88)


def clarinet(k, v, s):
    melody = [(1, 4), (.5, 5), (.5, 4), (1, 2), (1, 4), (2, 7), (1, 6), (1, 5), (1, 4), (1, 3),
              (1, 2), (.5, 1), (.5, 2), (4, 0)]
    s.line(0, [(l, k(d)) for l, d in melody], 96, .95)


def thunder(k, v, s):
    s.add(0, 3, [k(0, -1), k(1, -1)], 120)
    s.add(6, 2, [k(-2, -1)], 110)
    s.add(9, 4, [k(0, -1), k(2, -1)], 124)


def horn(k, v, s):
    melody = [(2, 0), (1, 4), (1, 2), (2, 4), (2, 7), (1, 6), (1, 4), (1, 2), (1, 3), (4, 4)]
    s.line(0, [(l, k(d)) for l, d in melody], 96, .97)


def toy_rhodes(k, v, s):
    melody = [(.5, 0), (.5, 2), (.5, 4), (.5, 7), (1, 6), (1, 4), (.5, 5), (.5, 4), (.5, 2), (.5, 0), (2, 1),
              (.5, 0), (.5, 2), (.5, 4), (.5, 5), (1, 7), (1, 9), (4, 7)]
    s.line(0, [(l, k(d)) for l, d in melody], 100, .8)
    for bar, degree in enumerate([0, 4, 3, 4]):
        s.add(bar * 4, 1.8, k.triad(degree, -1), 80)


def surf(k, v, s):
    s.add(0, 8, [k(0, -1), k(4, -1), k(7, -1)], 104)
    s.add(8, 8, [k(3, -1), k(5, -1), k(7, -1)], 104)


def ow_bass(k, v, s):
    # Its attack is 3.3 s: long notes, each one given time to open.
    for at, d in [(0, 0), (6, 3), (12, 4)]:
        s.add(at, 5.6, k(d), 108)


def piccolo(k, v, s):
    melody = [(.5, 4), (.5, 5), (.5, 4), (.5, 2), (1, 0), (1, 4), (.5, 7), (.5, 6), (.5, 5), (.5, 4),
              (1, 2), (1, 1), (.5, 2), (.5, 4), (.5, 5), (.5, 6), (2, 7), (4, 0)]
    s.line(0, [(l, k(d)) for l, d in melody], 100, .8)


def taps(k, v, s):
    pattern = [0, 4, 2, 7, 0, 4, 9, 7]
    for bar, shift in enumerate([0, 3, 5, 4]):
        for i, d in enumerate(pattern):
            s.add(bar * 4 + i * .5, .2, k(d + shift), 100)
    s.add(16, 1, k(0), 104)


def meow(k, v, s):
    phrase = [(1, 0), (1, 1), (.5, 2), (.5, 3), (1, 4), (2, 5), (1, 4), (1, 2), (2, 1), (1, 0), (1, 1), (4, 0)]
    s.line(0, [(l, k(d)) for l, d in phrase], 104, .9)


def rolling_wah(k, v, s):
    s.add(0, 7.8, [k(0, -1), k(4, -1), k(7, -1)], 100)
    s.add(8, 7.8, [k(-2, -1), k(3, -1), k(5, -1)], 100)


def low_strings(k, v, s):
    for bar, voicing in enumerate([[0, 4, 7], [-2, 3, 5], [-3, 2, 4], [-4, 1, 4]]):
        s.add(bar * 4, 3.9, [k(d, -1) for d in voicing], 100)
    s.add(16, 6, [k(d, -1) for d in [0, 4, 7]], 100)


def piccolo_trumpet(k, v, s):
    melody = [(.5, 0), (.5, 4), (.5, 7), (.5, 4), (1, 9), (1, 7), (.5, 5), (.5, 4), (.5, 2), (.5, 4), (2, 0),
              (.5, 4), (.5, 5), (.5, 7), (.5, 9), (1, 8), (1, 7), (4, 7)]
    s.line(0, [(l, k(d)) for l, d in melody], 108, .85)


def cello(k, v, s):
    melody = [(2, 0), (1, 2), (1, 3), (2, 4), (1, 3), (1, 2), (2, 1), (2, -1), (1, 0), (1, 2), (4, 0)]
    s.line(0, [(l, k(d)) for l, d in melody], 100, .97)


def forbidden(k, v, s):
    for i, d in enumerate([0, 3, 1, 5, 2, 4, 6, 0]):
        s.add(i * 2, 1.8, k(d), 100)


def froggy(k, v, s):
    for bar in range(4):
        for at, d in [(0, 0), (.75, 0), (1.5, 2), (2.5, 4), (3, 2)]:
            s.add(bar * 4 + at, .3, k(d + (3 if bar == 2 else 0)), 104)
    s.add(16, .5, k(0), 108)


def owgan(k, v, s):
    for bar, degree in enumerate([0, 6, 3, 0]):
        for at, length in [(0, 1.5), (2, .75), (3, .75)]:
            s.add(bar * 4 + at, length, [k(degree), k(degree + 2), k(degree + 4)], 104)
    s.add(16, 2, [k(0), k(2), k(4)], 108)


# Register per role: the octave of the tonic on the keyboard. The patch's
# DCO range and saved transpose move it from there, as they would for a player.
ROLES = {
    'bass': (bass, 2), 'brass_stab': (brass_stab, 4), 'brass_swell': (brass_swell, 4),
    'trumpet': (trumpet, 4), 'flute': (flute, 5), 'strings': (strings, 4), 'choir': (choir, 4),
    'piano': (piano, 4), 'organ': (organ, 4), 'combo_organ': (combo_organ, 4), 'calliope': (calliope, 5),
    'quack': (quack, 4), 'celeste': (celeste, 4), 'epiano': (epiano, 4), 'chimes': (chimes, 4),
    'steel_drum': (steel_drum, 5), 'xylophone': (xylophone, 4), 'fanfare': (fanfare, 4),
    'pizzicato': (pizzicato, 4), 'high_strings': (high_strings, 4), 'low_reed': (low_reed, 3),
    'double_reed': (double_reed, 4), 'guitar': (guitar, 4), 'koto': (koto, 4), 'pluck': (pluck, 4),
    'funk': (funk, 3), 'lead': (lead, 4), 'wah': (wah, 3), 'riser': (riser, 4), 'clav': (clav, 4),
    'snare': (snare, 3), 'toms': (toms, 3), 'timpani': (timpani, 3), 'shaker': (shaker, 5),
    'pad': (pad, 4), 'sweep': (sweep, 4), 'repeater': (repeater, 4), 'bell': (bell, 4),
    'gong': (gong, 4), 'res_funk': (res_funk, 2), 'booms': (booms, 2), 'storm': (storm, 3),
    'rocket': (rocket, 4), 'claps': (claps, 3), 'fx_sweep': (fx_sweep, 3), 'caverns': (caverns, 4),
    'violin': (violin, 4), 'vibes': (vibes, 4), 'harpsichord': (harpsichord, 4), 'recorder': (recorder, 5),
    'noise_sweep': (noise_sweep, 4), 'space_chimes': (space_chimes, 4), 'nylon': (nylon, 4),
    'organ_bell': (organ_bell, 5), 'accordion': (accordion, 4), 'helicopter': (helicopter, 3),
    'lute': (lute, 4), 'tomita': (tomita, 4), 'reed': (reed, 4), 'contact_wah': (contact_wah, 3),
    'bassoon': (bassoon, 2), 'ethereal': (ethereal, 4), 'clarinet': (clarinet, 4),
    'thunder': (thunder, 4), 'horn': (horn, 3), 'toy_rhodes': (toy_rhodes, 4), 'surf': (surf, 4),
    'ow_bass': (ow_bass, 2), 'piccolo': (piccolo, 5), 'taps': (taps, 4), 'meow': (meow, 4),
    'rolling_wah': (rolling_wah, 4), 'low_strings': (low_strings, 4),
    'piccolo_trumpet': (piccolo_trumpet, 5), 'cello': (cello, 3), 'forbidden': (forbidden, 4),
    'froggy': (froggy, 3), 'owgan': (owgan, 3),
}


def slug(text):
    return re.sub(r'[^a-z0-9]+', '-', text.lower()).strip('-')


def vlq(value):
    data = [value & 127]
    value >>= 7
    while value:
        data.insert(0, 128 | (value & 127))
        value >>= 7
    return bytes(data)


def smf(title, bpm, notes, end_beats):
    """notes: (beat, length, key, velocity) at pressed keys."""
    tempo = round(60000000 / bpm)
    name = title.encode('utf-8')
    events = [(0, -2, b'\xff\x03' + vlq(len(name)) + name),
              (0, -1, b'\xff\x51\x03' + tempo.to_bytes(3, 'big')),
              (0, -1, b'\xff\x58\x04\x04\x02\x18\x08')]
    for beat, length, key, velocity in notes:
        on = round(beat * PPQ)
        events.append((on, 1, bytes((0x90, key, velocity))))
        events.append((on + max(1, round(length * PPQ)), 0, bytes((0x80, key, 0))))
    track, previous = bytearray(), 0
    for tick, _, payload in sorted(events):
        track += vlq(tick - previous) + payload
        previous = tick
    track += vlq(max(previous, round(end_beats * PPQ)) - previous) + b'\xff\x2f\x00'
    return b'MThd' + struct.pack('>IHHH', 6, 0, 1, PPQ) + b'MTrk' + struct.pack('>I', len(track)) + track


def read_smf(data):
    """Note gates of a format-0 file: (tempo bpm, [(beat, length, key, velocity)], end beat)."""
    ppq = struct.unpack('>H', data[12:14])[0]
    track = data[22:]
    pos, tick, bpm, held, notes, status = 0, 0, 120.0, {}, [], 0

    def read_vlq():
        nonlocal pos
        value = 0
        while True:
            byte = track[pos]
            pos += 1
            value = (value << 7) | (byte & 127)
            if byte < 128:
                return value
    while pos < len(track):
        tick += read_vlq()
        if track[pos] & 0x80:
            status = track[pos]
            pos += 1
        if status == 0xff:
            kind = track[pos]
            pos += 1
            length = read_vlq()
            if kind == 0x51:
                bpm = 60000000 / int.from_bytes(track[pos:pos + 3], 'big')
            pos += length
            if kind == 0x2f:
                break
            continue
        key, velocity = track[pos], track[pos + 1]
        pos += 2
        if status & 0xf0 == 0x90 and velocity:
            held[key] = (tick, velocity)
        elif status & 0xf0 in (0x80, 0x90):
            start, vel = held.pop(key)
            notes.append((start / ppq, (tick - start) / ppq, key, vel))
    return bpm, sorted(notes), tick / ppq


def restrike(notes):
    """A key struck again while held ends its previous gate just before."""
    by_key, result = {}, []
    for note in sorted(notes):
        by_key.setdefault(note[2], []).append(note)
    for key_notes in by_key.values():
        for (b, l, k, v), following in zip(key_notes, key_notes[1:] + [None]):
            if following is not None and b + l > following[0] - 1 / 32:
                l = following[0] - 1 / 32 - b
            if l >= 1 / 16:
                result.append((b, l, k, v))
    return sorted(result)


def check_gates(slot, notes, limit):
    gates = sorted([(round(b * PPQ), 1, k) for b, l, k, _ in notes]
                   + [(round((b + l) * PPQ), 0, k) for b, l, k, _ in notes])
    held = set()
    for _, on, key in gates:
        if (key in held) == bool(on):
            raise ValueError(f'{slot}: overlapping gates on key {key}')
        held.add(key) if on else held.discard(key)
        if len(held) > limit:
            raise ValueError(f'{slot}: {len(held)} keys held, the patch has {limit} voices')


def tail_beats(bpm):
    # Room for the release, chorus and filter tails after the last gate.
    return 2.5 * bpm / 60.0


def demo_seconds(bpm, notes):
    # The shared renderer plays to the last key-up plus a 2.5 s tail and
    # requires 10-20 s.
    return max(b + length for b, length, _, _ in notes) * 60 / bpm + 2.5


def file_name(index, slot, name):
    return f'{index:03d}-{slug(f"{slot} {name}")}.mid'


def build():
    files, problems = {}, []
    for index, (slot, name, limit, role, tonic, mode, bpm, variant) in enumerate(BANK, start=1):
        writer, octave = ROLES[role]
        score = Score(bpm)
        writer(Key(tonic, mode, octave), variant, score)
        notes = []
        for beat, length, key, velocity in sorted(set(score.notes)):
            if not LOWEST_KEY <= key <= HIGHEST_KEY:
                problems.append(f'{slot}: key {key} is off the keyboard')
            notes.append((beat, length, key, velocity))
        if limit == 1:  # Unison: one line; a later note ends the previous one.
            notes = [(b, min(l, nb - b - 1 / 32) if nb is not None else l, k, v)
                     for (b, l, k, v), nb in zip(notes, [n[0] for n in notes[1:]] + [None])]
        notes = restrike(notes)
        try:
            check_gates(slot, notes, limit)
        except ValueError as error:
            problems.append(str(error))
        seconds = demo_seconds(bpm, notes)
        if not 10 <= seconds <= 20:
            problems.append(f'{slot}: {seconds:.1f} s, not 10-20 s')
        title = f'{slot} {name}'
        files[file_name(index, slot, name)] = smf(title, bpm, notes, score.end + tail_beats(bpm))
    for index, (slot, name, source, limit) in enumerate(PRODUCT, start=len(BANK) + 1):
        bpm, notes, end = read_smf((ORIGINALS / source).read_bytes())
        passes = 1
        while demo_seconds(bpm, [(b + passes * end, l, k, v) for b, l, k, v in notes]) <= 20:
            passes += 1
        played = [(b + p * end, l, k, v) for p in range(passes) for b, l, k, v in notes]
        check_gates(slot, played, 128 if limit == 1 else limit)
        seconds = demo_seconds(bpm, played)
        if not 10 <= seconds <= 20:
            problems.append(f'{slot}: {seconds:.1f} s, not 10-20 s')
        files[file_name(index, slot, name)] = smf(
            f'{slot} {name}', bpm, played, passes * end + tail_beats(bpm))
    if problems:
        raise ValueError('; '.join(problems))
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--check', action='store_true', help='fail if the committed files differ')
    args = parser.parse_args()
    files = build()
    if args.check:
        existing = {p.name for p in OUT.glob('*.mid')}
        stale = sorted(existing ^ set(files)) + sorted(
            n for n, d in files.items() if (OUT / n).exists() and (OUT / n).read_bytes() != d)
        if stale:
            sys.exit('Docs/audio/midi differs from Tools/WriteDemoMidi.py: ' + ', '.join(stale))
        print(f'{len(files)} MIDI sources match')
        return
    OUT.mkdir(parents=True, exist_ok=True)
    for stale in OUT.glob('*.mid'):
        if stale.name not in files:
            stale.unlink()
    for name, data in files.items():
        (OUT / name).write_bytes(data)
    print(f'wrote {len(files)} MIDI sources to {OUT.relative_to(ROOT)}')


if __name__ == '__main__':
    main()
