#!/usr/bin/env python3
"""Write the committed demo MIDI sources in Docs/audio/midi/.

Eight archival presets, each showing a different side of the instrument
(README G3, G3a): brass stabs, a string pad under a top line, electric piano
comping, a funk riff, a unison bass line, a lead melody, a synth pad and an
effect. Each file is named <NNN>-<slug of "<slot> <preset name>">.mid as the
shared preset demo renderer expects (Tools/PresetDemoRenderer.h), where NNN is
the program number in the host's list: A11..A88, B11..B88, then the original
product presets YB1..YP8. Each slot and name must match the shipping bank in
Source/DSP/YouKnowPresets.cpp, so a renamed preset fails here. The four More
demos (README G3d) are written to Docs/audio/midi/more/ and played through the
panels in Tools/DemoPanels.h.

Scores are written as the keys a player presses, on the hardware's 61-key
range; each preset's DCO range and saved transpose then place the sound where
its designers meant it, as they would for a player. Gates respect the preset's
voice count, and the Unison preset gets a single line. Every phrase's last
event falls at 5.5-9.5 s, so the renderer's 2.5 s tail keeps each demo within
8-12 s (G3c).

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
PPQ = 960
# The hardware's 61 keys, C1..C6.
LOWEST_KEY, HIGHEST_KEY = 36, 96

PITCH_CLASS = {'C': 0, 'Db': 1, 'D': 2, 'Eb': 3, 'E': 4, 'F': 5, 'Gb': 6, 'G': 7,
               'Ab': 8, 'A': 9, 'Bb': 10, 'B': 11}
MODES = {
    'major': [0, 2, 4, 5, 7, 9, 11], 'minor': [0, 2, 3, 5, 7, 8, 10],
    'dorian': [0, 2, 3, 5, 7, 9, 10],
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
        self.notes = []  # (beat, length, key, velocity)
        self.bends = []  # (beat, 14-bit pitch-wheel value)

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


# The preset phrases, one per demo.

def brass_stab(k, s):
    # Stabs on I, IV and V, then the tonic held over its low octave.
    for bar, degree in enumerate([0, 3, 4]):
        chord = k.triad(degree) + [k(degree, 1)]
        for at, length in [(0, .4), (.75, .4), (1.5, 1.2)]:
            s.add(bar * 4 + at, length, chord, 112 if at == 0 else 100)
    s.add(12, 3, k.triad(0) + [k(0, 1), k(0, -1)], 116)


def strings(k, s):
    # i and v held, then i, while a slow top line moves across them.
    for bar, voicing in enumerate([[0, 2, 4, 7], [-3, 1, 4, 6]]):
        s.add(bar * 4, 3.85, [k(d, -1) for d in voicing], 100)
    s.add(8, 4, [k(d, -1) for d in [0, 4, 7, 9]], 100)
    s.line(0, [(2, k(9)), (2, k(8)), (2, k(7)), (2, k(6)), (4, k(7))], 92, legato=.97)


def epiano(k, s):
    # ii7 and V7 comped over a bass note, then Imaj9.
    for bar, chord in enumerate([[1, 3, 5, 7], [4, 6, 8, 10]]):
        s.add(bar * 4, .9, k(chord[0], -1), 96)
        s.add(bar * 4 + .5, 1.4, [k(d) for d in chord[1:]], 88)
        s.add(bar * 4 + 2.5, 1.3, [k(d) for d in chord[1:]], 80)
    s.add(8, 3, [k(0, -1)] + [k(d) for d in [2, 4, 6, 8]], 92)


def funk(k, s):
    riff = [(.25, 0), (.25, None), (.25, 0), (.25, 2), (.5, None), (.25, 3), (.25, 0),
            (.5, 4), (.25, None), (.25, 3), (.5, 0), (.5, None)]
    for bar, shift in enumerate([0, 0, 3]):
        beat = bar * 4
        for length, degree in riff:
            if degree is not None:
                s.add(beat, length * .6, k(degree + shift), 104 if beat % 1 == 0 else 90)
            beat += length
    s.add(12, .5, k(0), 108)


def bass(k, s):
    line = [(.75, 0), (.25, 0), (.5, 7), (.5, 4), (.5, 2), (.5, 0), (.5, -1), (.5, 0)]
    for bar, shift in enumerate([0, 0, 5, 4]):
        beat = bar * 4
        for length, degree in line:
            s.add(beat, length * .85, k(degree + shift), 110 if beat % 1 == 0 else 92)
            beat += length
    s.add(16, 1.5, k(0), 112)


def lead(k, s):
    melody = [(1, 4), (.5, 5), (.5, 4), (1, 2), (1, 0), (1.5, 2), (.5, 4), (2, 5), (1, 7),
              (.5, 6), (.5, 5), (1, 4), (1, 2), (4, 4)]
    s.line(0, [(l, k(d)) for l, d in melody], 104, .95)


def pad(k, s):
    for bar, voicing in enumerate([[0, 2, 4, 6], [3, 5, 7, 9]]):
        s.add(bar * 4, 3.9, [k(d, -1) for d in voicing], 96)
    s.add(8, 4, [k(d, -1) for d in [0, 2, 4, 6]], 96)


def rocket(k, s):
    # Each launch needs its own long key for the sweep to run.
    for i in range(2):
        s.add(i * 4.5, 4, k(2 * i), 104)
    s.add(9, 3, [k(0), k(4)], 108)


# program, slot, name, voice limit, phrase, octave of the tonic, tonic, mode,
# tempo. The voice limits are the shipping bank's
# (Source/DSP/YouKnowPresets.cpp); a Unison patch has limit 1, since every
# voice sounds the one key.
DEMOS = [
    (1, 'A11', 'Brass Set 1', 6, brass_stab, 4, 'Bb', 'major', 112),
    (5, 'A15', 'Moving Strings', 6, strings, 4, 'D', 'minor', 80),
    (15, 'A27', 'Elect. Piano I', 6, epiano, 4, 'F', 'major', 88),
    (31, 'A47', 'Funky I', 6, funk, 3, 'E', 'dorian', 100),
    (32, 'A48', 'Synth Bass I (unison)', 1, bass, 2, 'D', 'minor', 116),
    (33, 'A51', 'Lead I', 6, lead, 4, 'A', 'minor', 108),
    (48, 'A68', 'Synth Pad', 6, pad, 4, 'F', 'major', 84),
    (61, 'A85', 'Rocket Men', 6, rocket, 4, 'C', 'major', 80),
]


# The More demos, written in seconds at 120 bpm (one beat is half a second),
# where each gesture comes from.

def seconds(value):
    return value * 2


def filter_brass(s):
    # Stabs working the resonant filter envelope, then a held chord pushed
    # up on the bender for 0.9 s.
    at = 0.0
    for _ in range(2):
        for chord, velocity, hold, gap in [
                ((50, 57, 62, 66), 121, .28, .16), ((50, 57, 62, 66), 108, .20, .24),
                ((48, 55, 60, 64), 121, .28, .16), ((48, 55, 60, 64), 108, .20, .24),
                ((46, 53, 58, 62), 121, .55, .35)]:
            s.add(seconds(at), seconds(hold), list(chord), velocity)
            at += hold + gap
    s.add(seconds(at), seconds(2.7), [50, 57, 62, 66], 127)
    s.bends += [(seconds(at + .8), 16383), (seconds(at + 1.7), 8192)]


def self_oscillation(s):
    # One voice: the limit cycle grows on the first long key, then the same
    # ringing filter is played up an arpeggio and back.
    at = 0.0
    for key, hold in [(48, 2.0)] + [(key, .55) for key in (55, 60, 64, 67, 72, 67, 64, 60)] + [(48, 1.8)]:
        s.add(seconds(at), seconds(hold), key, 114)
        at += hold + .05


def unison_glide(s):
    at = 0.0
    for key, hold in [(45, .5), (52, .5), (57, .5), (60, .9), (57, .45), (55, .45), (57, 1.4),
                      (69, 1.6), (45, 2.2)]:
        s.add(seconds(at), seconds(hold), key, 121)
        at += hold + .06


def delayed_vibrato(s):
    s.add(0, seconds(7.5), [53, 60, 65, 69], 114)


# name, voice limit, phrase; the names match the showcases in
# Tools/RenderPresetDemos.cpp, which sets each panel.
MORE = [
    ('Filter Brass', 6, filter_brass),
    ('Self-Oscillation', 1, self_oscillation),
    ('Unison Glide', 1, unison_glide),
    ('Delayed Vibrato', 6, delayed_vibrato),
]


def slug(text):
    return re.sub(r'[^a-z0-9]+', '-', text.lower()).strip('-')


def vlq(value):
    data = [value & 127]
    value >>= 7
    while value:
        data.insert(0, 128 | (value & 127))
        value >>= 7
    return bytes(data)


def smf(title, bpm, notes, bends, end_beats):
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
    for beat, value in bends:
        events.append((round(beat * PPQ), 1, bytes((0xe0, value & 127, value >> 7))))
    track, previous = bytearray(), 0
    for tick, _, payload in sorted(events):
        track += vlq(tick - previous) + payload
        previous = tick
    track += vlq(max(previous, round(end_beats * PPQ)) - previous) + b'\xff\x2f\x00'
    return b'MThd' + struct.pack('>IHHH', 6, 0, 1, PPQ) + b'MTrk' + struct.pack('>I', len(track)) + track


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


def check_gates(title, notes, limit):
    gates = sorted([(round(b * PPQ), 1, k) for b, l, k, _ in notes]
                   + [(round((b + l) * PPQ), 0, k) for b, l, k, _ in notes])
    held = set()
    for _, on, key in gates:
        if (key in held) == bool(on):
            raise ValueError(f'{title}: overlapping gates on key {key}')
        held.add(key) if on else held.discard(key)
        if len(held) > limit:
            raise ValueError(f'{title}: {len(held)} keys held, the patch has {limit} voices')


def tail_beats(bpm):
    # The shared renderer's tail after the last event.
    return 2.5 * bpm / 60.0


def gated(title, score, limit, problems):
    notes = []
    for beat, length, key, velocity in sorted(set(score.notes)):
        if not LOWEST_KEY <= key <= HIGHEST_KEY:
            problems.append(f'{title}: key {key} is off the keyboard')
        notes.append((beat, length, key, velocity))
    if limit == 1:  # Unison: one line; a later note ends the previous one.
        notes = [(b, min(l, nb - b - 1 / 32) if nb is not None else l, k, v)
                 for (b, l, k, v), nb in zip(notes, [n[0] for n in notes[1:]] + [None])]
    notes = restrike(notes)
    try:
        check_gates(title, notes, limit)
    except ValueError as error:
        problems.append(str(error))
    last = max([b + l for b, l, _, _ in notes] + [b for b, _ in score.bends]) * 60 / score.bpm
    if not 5.5 <= last <= 9.5:
        problems.append(f'{title}: its last event at {last:.2f} s is outside 5.5-9.5 s')
    return smf(title, score.bpm, notes, score.bends, score.end + tail_beats(score.bpm))


def factory_names():
    """The shipping factory bank's names, in program order."""
    source = (ROOT / 'Source' / 'DSP' / 'YouKnowPresets.cpp').read_text()
    return re.findall(r'"([^"]+)"', re.search(r'factoryNames \{\{(.*?)\}\};', source, re.S).group(1))


def build():
    files, problems = {}, []
    names = factory_names()
    for program, slot, name, limit, writer, octave, tonic, mode, bpm in DEMOS:
        # Program n is slot <bank><group><patch>, 64 to a bank (Patch Selection).
        index = program - 1
        bank_slot = f"{'AB'[index // 64]}{index % 64 // 8 + 1}{index % 8 + 1}"
        if (slot, name) != (bank_slot, names[index]):
            problems.append(f'program {program} is {bank_slot} {names[index]!r} in the bank, not {slot} {name!r}')
        score = Score(bpm)
        writer(Key(tonic, mode, octave), score)
        title = f'{slot} {name}'
        files[f'{program:03d}-{slug(title)}.mid'] = gated(title, score, limit, problems)
    for number, (name, limit, writer) in enumerate(MORE, start=1):
        score = Score(120)
        writer(score)
        files[f'more/{number:02d}-{slug(name)}.mid'] = gated(name, score, limit, problems)
    if problems:
        raise ValueError('; '.join(problems))
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--check', action='store_true', help='fail if the committed files differ')
    args = parser.parse_args()
    files = build()
    existing = {p.relative_to(OUT).as_posix() for p in [*OUT.glob('*.mid'), *OUT.glob('more/*.mid')]}
    if args.check:
        stale = sorted(existing ^ set(files)) + sorted(
            n for n, d in files.items() if (OUT / n).exists() and (OUT / n).read_bytes() != d)
        if stale:
            sys.exit('Docs/audio/midi differs from Tools/write_preset_midi.py: ' + ', '.join(stale))
        print(f'{len(files)} MIDI sources match')
        return
    (OUT / 'more').mkdir(parents=True, exist_ok=True)
    for stale in existing - set(files):
        (OUT / stale).unlink()
    for name, data in files.items():
        (OUT / name).write_bytes(data)
    print(f'wrote {len(files)} MIDI sources to {OUT.relative_to(ROOT)}')


if __name__ == '__main__':
    main()
