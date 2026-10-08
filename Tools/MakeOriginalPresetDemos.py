#!/usr/bin/env python3
"""Compose and render the sixteen original YouKnow presets, without tone edits.

Requires Python 3, numpy, ffmpeg/ffprobe and YouKnowRenderOriginalPresets.
Run from the repository root after building that CMake target:
  python3 Tools/MakeOriginalPresetDemos.py
Outputs 24-bit/96 kHz WAV, matching MIDI, a listening page and measured manifest.
The raw float render is cached with the renderer/score SHA-256, never reused
across a changed engine or performance. No EQ, compression or added effects.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import html
import json
import math
from pathlib import Path
import re
import struct
import subprocess
import wave
import zipfile

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
RATE = 96000
PPQ = 960
LEAD_IN = 0.08


def bass(slot, title, key, bpm, description, events):
    return dict(slot=slot, title=title, key=key, bpm=bpm, category="Bass",
                description=description, end=16.0,
                notes=[(beat, length, pitch, 0.86) for beat, length, pitch in events])


def pad(slot, title, key, bpm, description, harmony):
    """Keep common tones held across chords; roll only newly entering voices.

    Written pitches are sounding pitches. MIDI is compensated for the actual
    preset's DCO range later, without changing the preset itself. Each chord
    is explicitly voiced, with at most five held keys and one spare release
    voice. Note velocities are stored for portability; this bank has velocity
    response at zero, so articulation comes from rhythm and note duration.
    """
    notes, active, beat = [], {}, 0.0
    changes = []
    for length, name, pitches in harmony:
        changes.append(dict(beat=beat, length=length, chord=name))
        for pitch in list(active):
            if pitch not in pitches:
                start = active.pop(pitch)
                notes.append((start, beat - 0.018 - start, pitch, 0.84))
        entering = [pitch for pitch in pitches if pitch not in active]
        for index, pitch in enumerate(entering):
            active[pitch] = beat + index * 0.018
        beat += length
    for pitch, start in active.items():
        notes.append((start, beat - 0.05 - start, pitch, 0.84))
    return dict(slot=slot, title=title, key=key, bpm=bpm, category="Pad",
                description=description, end=beat, notes=sorted(notes), harmony=changes)


def compositions():
    # Each bass is a four-bar miniature: statement, answer, development,
    # and a tonic landing. Rests and gate lengths are part of the score.
    return [
        bass("YB1", "After Hours", "D minor", 94,
             "A spacious sub-bass motif, an upper-octave answer and a grounded D-minor landing.", [
            (0, .82, 38), (1.5, .32, 38), (2.25, .42, 45), (3, .62, 36),
            (4, .9, 34), (5.5, .32, 41), (6.25, .42, 45), (7, .72, 41),
            (8, .82, 38), (9.5, .32, 50), (10.25, .40, 48), (11, .65, 45),
            (12, .42, 36), (12.75, .42, 33), (13.5, 2.4, 38)]),
        bass("YB2", "Side Street", "F-sharp minor", 116,
             "Clipped, lightly swung sixteenths with a recurring pickup and a longer final answer.", [
            (0, .30, 42), (.78, .17, 42), (1.25, .35, 49), (2, .30, 42),
            (2.78, .18, 52), (3.25, .42, 49), (3.78, .15, 44),
            (4, .34, 45), (4.78, .17, 45), (5.25, .35, 52), (6, .34, 49),
            (6.78, .18, 47), (7.25, .38, 45), (7.78, .15, 40),
            (8, .34, 42), (8.78, .18, 49), (9.25, .34, 54), (10, .34, 52),
            (10.78, .18, 49), (11.25, .38, 47), (11.78, .15, 45),
            (12, .34, 44), (12.75, .36, 49), (13.5, .3, 40), (14, 1.9, 42)]),
        bass("YB3", "Night Drive", "E minor", 108,
             "A steady saw-bass hook opens into octave accents, then falls through a compact cadence.", [
            (0, .70, 40), (1, .28, 40), (1.5, .28, 47), (2.25, .55, 40),
            (3, .32, 43), (3.5, .30, 47),
            (4, .70, 36), (5, .28, 36), (5.5, .30, 43), (6.25, .52, 48),
            (7, .32, 47), (7.5, .30, 43),
            (8, .68, 45), (9, .30, 45), (9.5, .32, 52), (10.25, .55, 50),
            (11, .32, 47), (11.5, .30, 45),
            (12, .62, 35), (13, .32, 42), (13.5, .30, 38), (14, 1.9, 40)]),
        bass("YB4", "Soft Bounce", "C Dorian", 104,
             "Elastic offbeat phrasing lets the resonant envelope speak; the major sixth brightens the answer.", [
            (0, .42, 36), (.75, .22, 43), (1.5, .64, 39), (2.5, .36, 36),
            (3.25, .54, 46), (4, .42, 41), (4.75, .22, 48), (5.5, .60, 45),
            (6.5, .34, 43), (7.25, .55, 39),
            (8, .42, 36), (8.75, .22, 48), (9.5, .60, 46), (10.5, .34, 43),
            (11.25, .24, 41), (11.75, .20, 39), (12.25, .45, 38),
            (13, .36, 43), (13.75, 2.15, 36)]),
        bass("YB5", "Heavy Velvet", "A minor", 92,
             "Wide spaces around a heavy two-voice bass; upper notes answer the low A without crowding it.", [
            (0, 1.25, 33), (1.75, .50, 40), (2.75, .88, 45),
            (4, 1.10, 36), (5.5, .50, 43), (6.5, 1.0, 40),
            (8, 1.10, 38), (9.5, .45, 45), (10.5, .60, 43), (11.5, .32, 40),
            (12, .65, 35), (13, .40, 40), (13.75, 2.15, 33)]),
        bass("YB6", "Crosswalk", "G Dorian", 122,
             "A nimble PWM ostinato with displaced accents, melodic thirds and a concise turnaround.", [
            (0, .22, 43), (.5, .20, 50), (1.25, .22, 43), (1.75, .20, 53),
            (2.5, .22, 50), (3.25, .20, 46), (3.75, .17, 43),
            (4, .22, 48), (4.5, .20, 55), (5.25, .22, 48), (5.75, .20, 57),
            (6.5, .22, 55), (7.25, .20, 52), (7.75, .17, 50),
            (8, .22, 43), (8.5, .20, 50), (9.25, .22, 55), (9.75, .20, 53),
            (10.5, .22, 50), (11.25, .20, 46), (11.75, .17, 45),
            (12, .24, 48), (12.75, .24, 46), (13.5, .25, 50), (14.25, 1.60, 43)]),
        bass("YB7", "Blue Hour", "B minor", 88,
             "Sung legato pairs reveal the saved portamento, with a rising response and a low tonic return.", [
            (0, 1.56, 35), (1.5, .70, 42), (2.5, .81, 47), (3.25, .65, 45),
            (4, 1.56, 43), (5.5, .70, 47), (6.5, .81, 50), (7.25, .65, 47),
            (8, 1.06, 40), (9, .81, 47), (9.75, .81, 50), (10.5, .81, 54),
            (11.25, .65, 50), (12, .81, 42), (12.75, 1.06, 37),
            (13.75, 2.15, 35)]),
        bass("YB8", "Paper Lantern", "A-flat major", 100,
             "A rounded square-wave melody balances warm sixths, small breaths and an unhurried resolution.", [
            (0, .80, 44), (1.25, .38, 51), (2, .70, 48), (3, .65, 46),
            (4, .85, 41), (5.25, .38, 48), (6, .65, 51), (7, .65, 53),
            (8, .75, 49), (9.25, .38, 53), (10, .70, 51), (11, .65, 48),
            (12, .55, 46), (12.75, .45, 43), (13.5, 2.4, 44)]),
        pad("YP1", "Golden Hour", "D major", 78,
            "Close inner movement beneath a shared F-sharp; open ninths settle into a luminous D6/9.", [
            (4, "Dmaj9", [50, 57, 61, 64, 66]),
            (4, "Bm9", [47, 57, 61, 62, 66]),
            (4, "Gmaj9", [43, 57, 59, 62, 66]),
            (4, "D6/9", [50, 57, 59, 64, 66])]),
        pad("YP2", "First Light", "A major", 72,
            "Three patient blooms move from F-sharp minor through Dmaj9 to an open A6/9.", [
            (6, "F#m9", [54, 61, 64, 68, 73]),
            (6, "Dmaj9", [50, 61, 64, 66, 69]),
            (6, "A6/9", [45, 59, 61, 64, 66])]),
        pad("YP3", "Prism", "E major", 86,
            "An upper voice catches the light over changing major-seventh colours and a suspended cadence.", [
            (4, "Emaj7", [52, 59, 63, 68]),
            (2, "C#m7", [49, 59, 64, 68]),
            (2, "C#m7", [49, 59, 64, 71]),
            (4, "Amaj13", [45, 56, 61, 66]),
            (2, "B9sus", [47, 57, 61, 66]),
            (2, "Eadd9", [52, 59, 66, 68, 71])]),
        pad("YP4", "Satin Room", "B-flat major", 82,
            "A silk-soft major-nine progression with a held F and gently falling inner voices.", [
            (4, "Bbmaj9", [46, 57, 60, 62, 65]),
            (4, "Gm9", [43, 57, 58, 62, 65]),
            (4, "Ebmaj9", [39, 55, 58, 62, 65]),
            (4, "Bb6/9", [46, 55, 60, 62, 65])]),
        pad("YP5", "Quiet Cathedral", "D minor", 74,
            "Widely spaced choral voices descend into a suspended chord before returning to D minor.", [
            (4, "Dm(add9)", [50, 57, 65, 76]),
            (4, "Bbmaj7", [46, 57, 65, 74]),
            (4, "Gsus2", [43, 57, 62, 69]),
            (4, "Dm9", [50, 60, 65, 69, 76])]),
        pad("YP6", "Skyline", "E-flat major", 92,
            "Breathing brass chords anticipate the bar line, then lift through a suspended dominant to E-flat.", [
            (3.5, "Ebmaj9", [51, 58, 62, 65, 67]),
            (4, "Cm9", [48, 58, 62, 63, 67]),
            (4, "Abmaj9", [44, 58, 60, 63, 67]),
            (2, "Bb9sus", [46, 56, 60, 63, 65]),
            (2.5, "Ebmaj9", [51, 58, 62, 65, 67])]),
        pad("YP7", "Silver Rain", "F Lydian", 70,
            "An airy raised fourth dissolves through Cmaj9/E and resolves into a warmer Fmaj9.", [
            (6, "Fmaj9(#11, no3)", [53, 60, 64, 67, 71]),
            (6, "Cmaj9/E", [52, 59, 62, 67, 72]),
            (6, "Fmaj9", [53, 60, 64, 67, 69])]),
        pad("YP8", "Last Train", "C minor", 80,
            "A low, moving C-minor wash opens toward A-flat and F minor before closing on an added ninth.", [
            (4, "Cm9", [48, 55, 58, 62, 63]),
            (4, "Abmaj7", [44, 55, 60, 63, 67]),
            (4, "Fm9", [41, 56, 60, 63, 67]),
            (4, "Cm(add9)", [48, 55, 60, 62, 63])]),
    ]


def run(*args):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True, text=True)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def variable_length(number):
    output = [number & 127]
    while number >> 7:
        number >>= 7
        output.insert(0, (number & 127) | 128)
    return bytes(output)


def write_midi(path, demo):
    # No Program Change: the product bank follows the archival 128 MIDI slots.
    # Select the named original preset, and use the exact saved DCO range.
    title = f'{demo["slot"]} {demo["preset"]} - {demo["title"]}'.encode()
    comment = (f'Select {demo["slot"]} {demo["preset"]}; saved preset, Max quality. '
               'MIDI pitches compensate the preset DCO octave.').encode()
    tempo = round(60000000 / demo['bpm'])
    events = [(0, -3, b'\xff\x03' + variable_length(len(title)) + title),
              (0, -2, b'\xff\x01' + variable_length(len(comment)) + comment),
              (0, -1, b'\xff\x51\x03' + tempo.to_bytes(3, 'big')),
              (0, -1, b'\xff\x58\x04\x04\x02\x18\x08')]
    for beat, length, note, velocity in demo['midi_notes']:
        events += [(round(beat * PPQ), 1, bytes([0x90, note, round(velocity * 127)])),
                   (round((beat + length) * PPQ), 0, bytes([0x80, note, 0]))]
    data, previous = bytearray(), 0
    for tick, _, message in sorted(events):
        data.extend(variable_length(tick - previous) + message)
        previous = tick
    end = max(previous, round(demo['end'] * PPQ))
    data.extend(variable_length(end - previous) + b'\xff\x2f\x00')
    path.write_bytes(b'MThd' + struct.pack('>IHHH', 6, 0, 1, PPQ)
                     + b'MTrk' + struct.pack('>I', len(data)) + data)


def loudness(path, raw=False):
    inputs = ['-f', 'f32le', '-ar', str(RATE), '-ac', '2'] if raw else []
    result = run('ffmpeg', '-hide_banner', '-nostats', *inputs, '-i', path,
                 '-af', 'loudnorm=I=-18:TP=-2:LRA=11:print_format=json', '-f', 'null', '-')
    # Only input metrics are used. The filter's processed output is discarded.
    blocks = re.findall(r'\{\s*"input_i".*?\}', result.stderr, re.DOTALL)
    if not blocks:
        raise RuntimeError(f'No loudness result for {path}: {result.stderr[-1000:]}')
    metrics = json.loads(blocks[-1])
    return {key: float(metrics[key]) for key in ['input_i', 'input_tp', 'input_lra']}


def wav24(path, audio):
    rng = np.random.default_rng(20261003)
    # TPDF at one LSB. Rounding happens once, after all gain and boundary fades.
    dither = rng.random(audio.shape) - rng.random(audio.shape)
    values = np.rint(audio * 8388608.0 + dither).astype(np.int32)
    if np.max(np.abs(values)) >= 8388607:
        raise RuntimeError('24-bit PCM would clip')
    packed = values.astype('<i4').view(np.uint8).reshape(-1, 4)[:, :3].tobytes()
    with wave.open(str(path), 'wb') as output:
        output.setnchannels(2)
        output.setsampwidth(3)
        output.setframerate(RATE)
        output.writeframes(packed)


def render_one(demo, renderer, renderer_hash, destination, scratch):
    stem = demo['stem']
    score = scratch / f'{stem}.score'
    score_text = (f'# {demo["slot"]}: {demo["title"]}\ntempo {demo["bpm"]}\nend {demo["end"]}\n'
                  + ''.join(f'note {b:.6f} {d:.6f} {n} {v:.6f}\n'
                            for b, d, n, v in demo['midi_notes']))
    score.write_text(score_text)
    raw = scratch / f'{stem}.f32'
    log = scratch / f'{stem}.render.json'
    signature = hashlib.sha256((renderer_hash + score_text).encode()).hexdigest()
    cached = json.loads(log.read_text()) if log.exists() else {}
    if cached.get('signature') == signature and raw.exists() and sha256(raw) == cached.get('raw_sha256'):
        engine = cached['engine']
    else:
        print(f'Rendering {demo["slot"]} {demo["preset"]} — {demo["title"]}', flush=True)
        engine = json.loads(run(renderer, demo['slot'], score, raw).stdout)
        log.write_text(json.dumps(dict(signature=signature, engine=engine, raw_sha256=sha256(raw)), indent=2))
    audio = np.fromfile(raw, dtype='<f4').reshape(-1, 2).astype(np.float64)
    assert len(audio) == round(engine['seconds'] * RATE)
    assert np.isfinite(audio).all()
    # A quiet opening and a smooth stop after the complete envelope release.
    head, tail = round(.012 * RATE), round(.32 * RATE)
    audio[:head] *= (0.5 - 0.5 * np.cos(np.linspace(0, np.pi, head)))[:, None]
    audio[-tail:] *= (0.5 + 0.5 * np.cos(np.linspace(0, np.pi, tail)))[:, None]
    measured = scratch / f'{stem}.edges.f32'
    audio.astype('<f4').tofile(measured)
    before = loudness(measured, raw=True)
    gain_db = min(-18.0 - before['input_i'], -2.1 - before['input_tp'])
    assert math.isfinite(gain_db)
    audio *= 10 ** (gain_db / 20)
    output = destination / f'{stem}.wav'
    wav24(output, audio)
    after = loudness(output)
    assert after['input_tp'] <= -2.0, f'True-peak headroom failed: {output}'
    assert after['input_i'] <= -17.85
    assert np.max(np.abs(audio.mean(axis=0))) < .005
    assert np.max(np.abs(audio[[0, -1]])) < 1e-8
    # Data for the self-contained listening page; aggregate both channels.
    envelope = [float(np.sqrt(np.mean(block ** 2)))
                for block in np.array_split(audio, 100)]
    maximum = max(envelope)
    envelope = [round(value / maximum, 4) for value in envelope]
    midi = destination / 'MIDI' / f'{stem}.mid'
    write_midi(midi, demo)
    result = {key: value for key, value in demo.items() if key not in ('notes', 'midi_notes')}
    result.update(engine=engine, seconds=round(len(audio) / RATE, 3),
                  wav=output.name, midi=f'MIDI/{midi.name}', gain_db=round(gain_db, 3),
                  integrated_lufs=after['input_i'], true_peak_dbtp=after['input_tp'],
                  loudness_range_lu=after['input_lra'], note_count=len(demo['notes']),
                  max_dc=float(np.max(np.abs(audio.mean(axis=0)))), waveform=envelope,
                  wav_sha256=sha256(output), midi_sha256=sha256(midi))
    print(f'Finished {demo["slot"]}: {result["seconds"]:.1f}s, '
          f'{after["input_i"]:.1f} LUFS, {after["input_tp"]:.1f} dBTP', flush=True)
    return result


def make_page(destination, results):
    cards = []
    for index, demo in enumerate(results):
        bars = ''.join(f'<rect x="{i*5}" y="{24-21*v:.2f}" width="2" '
                       f'height="{max(1,42*v):.2f}" rx="1"/>'
                       for i, v in enumerate(demo['waveform']))
        escape = html.escape
        cards.append(f'''<article data-kind="{demo['category'].lower()}" data-index="{index}">
          <div class="cardtop"><span class="slot">{demo['slot']}</span><span>{demo['category']} · {demo['seconds']:.1f}s</span></div>
          <h2>{escape(demo['preset'])}</h2><div class="piece">{escape(demo['title'])}</div>
          <p>{escape(demo['description'])}</p>
          <div class="music">{escape(demo['key'])}<span>·</span>{demo['bpm']} BPM</div>
          <div class="wave"><svg viewBox="0 0 500 48" aria-hidden="true" preserveAspectRatio="none">{bars}</svg></div>
          <audio controls preload="none" src="{demo['wav']}" aria-label="{escape(demo['preset'])} demo"></audio>
          <div class="downloads"><a href="{demo['wav']}" download>WAV ↗</a><a href="{demo['midi']}" download>MIDI ↗</a></div>
        </article>''')
    page = '''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>YouKnow — Original Preset Sessions</title><style>
:root{color-scheme:dark;--bg:#111917;--card:#182320;--line:#33413b;--ink:#f0edde;--muted:#acb8aa;--accent:#d3b780}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.6 -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}
main{max-width:1210px;margin:auto;padding:46px 38px 60px}.brand{display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid var(--line);padding-bottom:22px;font-size:12px;letter-spacing:.16em;text-transform:uppercase}.brand strong{font-size:19px;letter-spacing:.08em}.tag{color:var(--accent)}
header{padding:64px 0 42px;max-width:860px}.eyebrow{font-size:11px;text-transform:uppercase;letter-spacing:.21em;color:var(--accent)}h1{font-family:Georgia,serif;font-size:clamp(44px,6vw,76px);font-weight:400;line-height:1.08;letter-spacing:-.045em;margin:20px 0 24px}header p{color:var(--muted);max-width:615px;font-size:17px}button,a{touch-action:manipulation}button{cursor:pointer;font:inherit}a{color:var(--ink);text-underline-offset:4px}button:focus-visible,a:focus-visible,audio:focus-visible{outline:2px solid var(--accent);outline-offset:5px}
.toolbar{display:flex;gap:12px;align-items:center;justify-content:space-between;flex-wrap:wrap;margin:4px 0 27px}.filters{display:flex;gap:7px}.filters button,.sequence{border:1px solid var(--line);border-radius:30px;padding:8px 18px;background:transparent;color:var(--muted)}.filters button[aria-pressed=true]{background:var(--accent);border-color:var(--accent);color:#18201c}.sequence{color:var(--ink)}.status{color:var(--muted);font-size:12px;min-height:20px;margin:0 0 16px}.grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:17px}
article{min-width:0;display:flex;flex-direction:column;background:var(--card);border:1px solid var(--line);border-radius:5px;padding:23px 23px 18px;transition:border-color .2s}article.playing{border-color:var(--accent)}article[hidden]{display:none}.cardtop{display:flex;justify-content:space-between;color:var(--muted);font-size:11px;letter-spacing:.06em}.slot{color:var(--accent)}h2{font:400 28px/1.15 Georgia,serif;margin:20px 0 5px;letter-spacing:-.02em}.piece{font-size:11px;text-transform:uppercase;letter-spacing:.14em;color:var(--accent)}article p{font-size:13px;color:var(--muted);margin:19px 0 13px;flex:1}.music{font-size:11px;color:var(--muted)}.music span{padding:0 8px}.wave{height:54px;margin-top:17px;opacity:.75}.wave svg{width:100%;height:42px;fill:var(--accent)}audio{width:100%;height:34px;margin:6px 0 13px}.downloads{display:flex;gap:17px;font-size:10px;letter-spacing:.08em}.downloads a{color:var(--muted);text-decoration:none}.downloads a:hover{color:var(--accent)}
footer{margin-top:45px;padding-top:23px;border-top:1px solid var(--line);display:flex;gap:30px;justify-content:space-between;color:var(--muted);font-size:12px}footer p{max-width:630px;margin:0}footer a{white-space:nowrap}@media(max-width:960px){.grid{grid-template-columns:repeat(2,minmax(0,1fr))}}@media(max-width:620px){main{padding:27px 20px}.grid{grid-template-columns:1fr}header{padding:42px 0 25px}.brand{font-size:9px}.toolbar{align-items:flex-start}footer{display:block}footer p{margin-bottom:18px}}
</style><main><div class="brand"><strong>YouKnow</strong><span class="tag">Original Preset Sessions / 01</span></div>
<header><div class="eyebrow">Eight basses · Eight pads · Sixteen original pieces</div><h1>A little space.<br>A lot of character.</h1><p>Short compositions for the original YouKnow bank. Each sound gets its own rhythm, harmony and room to breathe.</p><div class="eyebrow">Maximum quality · 24-bit / 96 kHz stereo</div></header>
<div class="toolbar"><div class="filters" aria-label="Filter presets"><button aria-pressed="true" data-filter="all">All 16</button><button aria-pressed="false" data-filter="bass">Basses 8</button><button aria-pressed="false" data-filter="pad">Pads 8</button></div><button class="sequence" id="sequence">Play collection →</button></div><div class="status" id="status" aria-live="polite">Choose a preset to listen.</div><section class="grid" aria-label="Preset demos">''' + '\n'.join(cards) + '''</section>
<footer><p>Rendered from the saved presets through YouKnow’s shipping engine, with the instrument’s own chorus. Gain-only finishing toward −18 LUFS, with at least 2 dB of true-peak headroom. Natural releases, gentle file-edge fades and 24-bit TPDF dither. Select the named preset before playing its MIDI.</p><div><a href="manifest.json">Render details ↗</a><br><a href="YouKnow-Original-Presets.m3u">Playlist ↗</a></div></footer></main>
<script>
const cards=[...document.querySelectorAll('article')],players=cards.map(c=>c.querySelector('audio')),status=document.getElementById('status'),sequence=document.getElementById('sequence');let continuous=false;
function stopSequence(){continuous=false;sequence.textContent='Play collection →'}
players.forEach((audio,i)=>{audio.addEventListener('play',()=>{players.forEach((other,j)=>{if(i!==j)other.pause()});cards.forEach((c,j)=>c.classList.toggle('playing',i===j));status.textContent='Now playing: '+cards[i].querySelector('h2').textContent+' — '+cards[i].querySelector('.piece').textContent});audio.addEventListener('pause',()=>{cards[i].classList.remove('playing');if(!players.some(p=>!p.paused))status.textContent='Playback paused.'});audio.addEventListener('ended',()=>{cards[i].classList.remove('playing');if(continuous){const next=cards.findIndex((c,j)=>j>i&&!c.hidden);if(next>=0){players[next].currentTime=0;players[next].play().catch(()=>{stopSequence();status.textContent='Press play on the next preset to continue.'})}else{stopSequence();status.textContent='Collection finished.'}}else status.textContent='Choose a preset to listen.'});audio.addEventListener('error',()=>{stopSequence();status.textContent='Audio could not be loaded. Keep this page beside the WAV files.'})});
document.querySelectorAll('[data-filter]').forEach(button=>button.onclick=()=>{stopSequence();players.forEach(p=>p.pause());document.querySelectorAll('[data-filter]').forEach(b=>b.setAttribute('aria-pressed',String(b===button)));cards.forEach(c=>c.hidden=button.dataset.filter!=='all'&&c.dataset.kind!==button.dataset.filter);status.textContent='Choose a preset to listen.'});
sequence.onclick=()=>{if(continuous){stopSequence();players.forEach(p=>p.pause());return}continuous=true;sequence.textContent='Pause collection';const current=players.findIndex((p,i)=>!p.paused&&!cards[i].hidden),next=current>=0?current:cards.findIndex(c=>!c.hidden);if(next>=0){if(current<0)players[next].currentTime=0;players[next].play().catch(()=>{stopSequence();status.textContent='Press a preset’s play button to start.'})}};
</script></html>'''
    (destination / 'index.html').write_text(page)


def package_collection(destination, results, renderer):
    expected = [demo['slot'] for demo in compositions()]
    assert len(results) == len(expected) and {r['slot'] for r in results} == set(expected)
    results = sorted(results, key=lambda result: expected.index(result['slot']))
    for result in results:
        assert sha256(destination / result['wav']) == result['wav_sha256']
        assert sha256(destination / result['midi']) == result['midi_sha256']
        assert result['engine']['quality_selected'] == 4
        assert result['engine']['internal_rate'] == 192000
    commit = run('git', '-C', ROOT, 'rev-parse', 'HEAD').stdout.strip()
    manifest = dict(collection='YouKnow — Original Preset Sessions',
                    source_commit=commit, renderer_sha256=sha256(renderer),
                    composer_sha256=sha256(Path(__file__)),
                    format='24-bit PCM stereo WAV', sample_rate=RATE,
                    quality='Maximum (4x selected; 2x applied at 96 kHz, 192 kHz internal)',
                    filter='Exact tanh / MersonHalfSteps',
                    mastering='Static gain toward -18 LUFS, constrained to -2.1 dBTP; no dynamics or EQ',
                    external_effects=False, preset_parameters_modified=False,
                    dither='24-bit TPDF', lead_in_seconds=LEAD_IN,
                    midi_note='Select the named preset. MIDI starts at beat 0; WAV has 80 ms lead-in. No Program Change.',
                    demos=results)
    (destination / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
    (destination / 'YouKnow-Original-Presets.m3u').write_text('#EXTM3U\n' + ''.join(
        f'#EXTINF:{r["seconds"]},{r["slot"]} {r["preset"]} — {r["title"]}\n{r["wav"]}\n' for r in results))
    make_page(destination, results)
    package = destination.parent / 'YouKnow-Original-Preset-Sessions-24bit-96kHz.zip'
    files = [destination / r['wav'] for r in results] + [destination / r['midi'] for r in results]
    files += [destination / name for name in ['index.html', 'manifest.json', 'YouKnow-Original-Presets.m3u']]
    with zipfile.ZipFile(package, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for path in files:
            archive.write(path, 'YouKnow-Original-Preset-Sessions/' + str(path.relative_to(destination)))
    print(f'Complete: {len(results)} presets, {sum(r["seconds"] for r in results):.1f}s, {package}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path, default=ROOT / 'build-dsp/YouKnowRenderOriginalPresets')
    parser.add_argument('--output', type=Path, default=ROOT / 'Docs/audio/youknow-originals')
    parser.add_argument('--scratch', type=Path, default=ROOT / 'out/original-preset-demos')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--only', nargs='*', help='Render selected slots for audition before the full set')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 8:
        parser.error('--jobs must be between 1 and 8')
    args.renderer = args.renderer.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'MIDI').mkdir(exist_ok=True)
    args.scratch.mkdir(parents=True, exist_ok=True)
    bank = {}
    for line in run(args.renderer, '--list').stdout.splitlines():
        slot, name, offset, attack, release = line.split('\t')
        bank[slot] = dict(preset=name, octave_offset=int(offset),
                          attack_seconds=float(attack), release_seconds=float(release))
    demos = compositions()
    assert {d['slot'] for d in demos} == set(bank), 'Score set does not cover the original bank exactly'
    assert len(demos) == len(bank), 'Duplicate preset score'
    for index, demo in enumerate(demos, 1):
        demo.update(bank[demo['slot']])
        demo['stem'] = f'{index:02}-{demo["slot"]}-' + re.sub('[^a-z0-9]+', '-', demo['preset'].lower()).strip('-')
        demo['midi_notes'] = [(b, d, n - demo['octave_offset'], v) for b, d, n, v in demo['notes']]
    if args.only:
        if not set(args.only) <= set(bank):
            parser.error('Unknown slot in --only')
        demos = [d for d in demos if d['slot'] in args.only]
    renderer_hash = sha256(args.renderer)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(render_one, d, args.renderer, renderer_hash, args.output, args.scratch) for d in demos]
        results = [future.result() for future in futures]
    if args.only:
        (args.scratch / 'audition-results.json').write_text(json.dumps(results, indent=2) + '\n')
        return
    package_collection(args.output, results, args.renderer)


if __name__ == '__main__':
    main()
