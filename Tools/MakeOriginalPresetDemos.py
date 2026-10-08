#!/usr/bin/env python3
"""Render the authored YouKnow bank: 24-bit/96 kHz WAV, MIDI and listening page.

Build YouKnowRenderOriginalPresets first. Rendering requires NumPy and FFmpeg;
composition, MIDI and packaging use only the Python standard library. The
renderer retains saved patch/performance controls and selects maximum quality
with the active product circuit profile, using Direct performance timing.
Finishing applies static gain, file-edge fades and TPDF dither only.
Use --only YB1 for audition; --package-only refreshes a completed collection
without rerendering after a commit when the recorded source hashes still match.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import html
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import wave
import zipfile

ROOT = Path(__file__).resolve().parents[1]
RATE = 96000
PPQ = 960
EXPECTED_SLOTS = [f'YB{i}' for i in range(1, 9)] + [f'YP{i}' for i in range(1, 9)]


def write_json(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(*arguments):
    result = subprocess.run([str(arg) for arg in arguments], capture_output=True,
                            text=True, encoding='utf-8', errors='replace')
    if result.returncode:
        raise RuntimeError(f'{arguments[0]} failed ({result.returncode}):\n{result.stderr[-4000:]}')
    return result


def source_identity():
    paths = command('git', '-C', ROOT, 'ls-files', 'Source/DSP', 'CMakeLists.txt').stdout.splitlines()
    paths += ['Tools/PresetScores.h', 'Tools/RenderOriginalPresets.cpp', 'Tools/MakeOriginalPresetDemos.py']
    digest = hashlib.sha256()
    for relative in sorted(set(paths)):
        digest.update(relative.encode('utf-8') + b'\0')
        digest.update((ROOT / relative).read_bytes())
        digest.update(b'\0')
    dirty = bool(command('git', '-C', ROOT, 'status', '--porcelain', '--', *paths).stdout.strip())
    return dict(sha256=digest.hexdigest(), git_commit=command('git', '-C', ROOT, 'rev-parse', 'HEAD').stdout.strip(),
                tracked_sources_modified=dirty)


def make_performances(bank):
    """Sounding-pitch scores; compensate the saved DCO range before making MIDI."""
    basses = [
        ('Tidal Step', 'D minor', 108, [(0,.6,38),(.75,.25,45),(1.25,.4,41),(2,.7,38),(3,.3,36),(3.5,.3,33),(4,.6,38),(5,.3,50),(5.5,.3,48),(6,.4,45),(6.75,1.1,38)]),
        ('Copper Wire', 'E minor', 128, [(0,.2,40),(.5,.2,47),(1,.3,43),(1.75,.2,40),(2.25,.3,42),(3,.3,47),(3.5,.3,50),(4,.2,40),(4.5,.2,52),(5,.3,50),(5.75,.2,47),(6.25,.3,43),(7,.8,40)]),
        ('Northbound', 'G minor', 116, [(0,.75,43),(1,.25,50),(1.5,.3,46),(2.25,.6,41),(3,.5,38),(4,.75,39),(5,.25,46),(5.5,.3,48),(6.25,.6,50),(7,.8,43)]),
        ('Pocket Spring', 'C Dorian', 120, [(0,.35,36),(.75,.35,43),(1.5,.6,39),(2.5,.35,45),(3.25,.4,43),(4,.35,36),(4.75,.35,48),(5.5,.6,46),(6.5,.35,38),(7.25,.6,36)]),
        ('Low Orbit', 'A minor', 102, [(0,1.2,33),(1.5,.45,40),(2.5,.75,45),(3.5,.3,43),(4,1.2,36),(5.5,.45,43),(6.25,.35,35),(7,.85,33)]),
        ('Window Light', 'F major', 134, [(0,.18,41),(.5,.18,48),(1,.18,45),(1.5,.3,53),(2.25,.2,50),(2.75,.2,48),(3.5,.25,45),(4,.18,46),(4.5,.18,53),(5,.18,50),(5.5,.3,55),(6.25,.2,48),(6.75,.2,45),(7.25,.6,41)]),
        ('Turn Back', 'B minor', 110, [(0,1.1,35),(1,1.1,42),(2,1.1,47),(3,.8,45),(4,1.1,43),(5,1.1,42),(6,1.1,37),(7,.85,35)]),
        ('Open Courtyard', 'A-flat major', 106, [(0,.8,44),(1.25,.5,48),(2,.7,51),(3,.6,53),(4,.8,49),(5.25,.5,48),(6,.7,46),(7,.85,44)]),
    ]
    pads = [
        ('Amber Rooms', 'D major', 94, [[50,57,61,66],[47,57,62,66],[50,57,64,66]]),
        ('Unfolding', 'A major', 82, [[54,61,64,68],[50,61,66,69],[45,59,61,66]]),
        ('High Windows', 'E major', 106, [[52,59,63,68],[45,56,61,68],[52,59,66,71]]),
        ('Soft Edges', 'B-flat major', 96, [[46,57,62,65],[43,58,62,65],[46,55,60,65]]),
        ('Still Water', 'D minor', 88, [[50,57,65,76],[46,57,65,74],[50,60,65,76]]),
        ('Late Arrival', 'E-flat major', 104, [[51,58,62,67],[44,58,63,67],[51,58,65,67]]),
        ('Cloud Map', 'F Lydian', 90, [[53,60,67,71],[52,59,67,72],[53,60,64,69]]),
        ('Afterimage', 'C minor', 92, [[48,55,58,63],[44,55,60,67],[48,55,62,63]]),
    ]
    by_slot = {entry['slot']: entry for entry in bank}
    if len(bank) != 16 or set(by_slot) != set(EXPECTED_SLOTS):
        raise ValueError('Renderer bank must contain exactly YB1–YB8 and YP1–YP8')
    performances = []
    for index, slot in enumerate(EXPECTED_SLOTS):
        entry = by_slot[slot]
        if index < 8:
            title, key, bpm, gates = basses[index]
            beats = 8.0
            description = 'A two-bar bass phrase with a low statement, upper answer and closing tonic.'
        else:
            title, key, bpm, chords = pads[index - 8]
            # Give the last entering voice at least a second beyond the saved
            # envelope attack. Slow pads need patient phrases, not short gates
            # that never reveal their intended sustain or filter bloom.
            attack = entry.get('attack_seconds', 0.0)
            chord_beats = math.ceil(max(2.0, (attack + 1.0) * bpm / 60.0 + .15625) * 2.0) / 2.0
            lengths = [chord_beats, chord_beats + (0.5 if index % 2 == 0 else 0.0), chord_beats + 1.0]
            beats = sum(lengths)
            description = 'Three closely voiced chords; shared tones stay held while the inner parts move.'
            gates, held = [], {}
            beat = 0.0
            for chord_index, pitches in enumerate(chords):
                for pitch in list(held):
                    if pitch not in pitches:
                        start = held.pop(pitch)
                        gates.append((start, beat - .0625 - start, pitch))
                for roll, pitch in enumerate(p for p in pitches if p not in held):
                    held[pitch] = beat + roll / 32.0
                beat += lengths[chord_index]
            gates += [(start, beats - .125 - start, pitch) for pitch, start in held.items()]
        performance = dict(slot=slot, name=entry['name'], title=title, key=key,
                           category=entry['category'], description=description, bpm=bpm, beats=beats,
                           notes=[dict(beat=b, duration=d, key=p - entry['offset'], velocity=104 if index < 8 else 100)
                                  for b, d, p in sorted(gates)])
        validate_performance(performance, entry)
        performances.append(performance)
    return performances


def validate_performance(performance, preset):
    def bounded(value, low, high):
        return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value) and low <= value <= high
    if not bounded(performance['bpm'], 30, 240) or not bounded(performance['beats'], .005, 64):
        raise ValueError('Invalid tempo or score length')
    notes = performance['notes']
    if not isinstance(notes, list) or not 1 <= len(notes) <= 4096:
        raise ValueError('A performance requires 1–4096 notes')
    gates = []
    for note in notes:
        if (not bounded(note['beat'], 0, 64) or not bounded(note['duration'], .005, 64)
                or not isinstance(note['key'], int) or isinstance(note['key'], bool) or not 0 <= note['key'] <= 127
                or not isinstance(note['velocity'], int) or isinstance(note['velocity'], bool) or not 1 <= note['velocity'] <= 127
                or note['beat'] + note['duration'] > performance['beats'] + 1e-8):
            raise ValueError('Invalid note gate')
        gates.extend([(round(note['beat'] * PPQ), 1, note['key']),
                      (round((note['beat'] + note['duration']) * PPQ), 0, note['key'])])
    held = set()
    limit = 128 if preset['key_mode'] == 'Unison' else preset['polyphony']
    for _, on, key in sorted(gates):
        if (key in held) == bool(on):
            raise ValueError('Overlapping or unmatched same-key note gates')
        held.add(key) if on else held.remove(key)
        if len(held) > limit:
            raise ValueError('Score exceeds preset polyphony')
    if held:
        raise ValueError('Unreleased note gates')


def vlq(value):
    if value < 0 or value > 0x0fffffff:
        raise ValueError('MIDI delta outside the SMF range')
    data = [value & 127]
    value >>= 7
    while value:
        data.insert(0, 128 | (value & 127))
        value >>= 7
    return bytes(data)


def write_midi(path, performance):
    name = f'{performance["slot"]} {performance["name"]} / {performance["title"]}'.encode('utf-8')
    instructions = f'Select {performance["slot"]} {performance["name"]}; no Program Change. MIDI begins at beat zero.'.encode('utf-8')
    tempo = round(60000000 / performance['bpm'])
    events = [(0, -2, b'\xff\x03' + vlq(len(name)) + name),
              (0, -1, b'\xff\x01' + vlq(len(instructions)) + instructions),
              (0, -1, b'\xff\x51\x03' + tempo.to_bytes(3, 'big')),
              (0, -1, b'\xff\x58\x04\x04\x02\x18\x08')]
    for note in performance['notes']:
        events += [(round(note['beat'] * PPQ), 1, bytes((0x90, note['key'], note['velocity']))),
                   (round((note['beat'] + note['duration']) * PPQ), 0, bytes((0x80, note['key'], 0)))]
    track, previous = bytearray(), 0
    for tick, _, payload in sorted(events):
        track.extend(vlq(tick - previous) + payload)
        previous = tick
    track.extend(vlq(max(previous, round(performance['beats'] * PPQ)) - previous) + b'\xff\x2f\x00')
    path.write_bytes(b'MThd' + struct.pack('>IHHH', 6, 0, 1, PPQ)
                     + b'MTrk' + struct.pack('>I', len(track)) + track)


def meter(path, raw=False):
    raw_input = ['-f', 'f32le', '-ar', str(RATE), '-ac', '2'] if raw else []
    result = command('ffmpeg', '-hide_banner', '-nostats', *raw_input, '-i', path,
                     '-af', 'loudnorm=I=-18:TP=-2:LRA=11:print_format=json', '-f', 'null', '-')
    match = re.findall(r'\{\s*"input_i".*?\}', result.stderr, flags=re.S)
    if not match:
        raise RuntimeError('FFmpeg did not report loudness metrics')
    values = json.loads(match[-1])
    return dict(lufs=float(values['input_i']), true_peak_dbtp=float(values['input_tp']))


def master_audio(raw, output, frames):
    import numpy as np
    audio = np.fromfile(raw, dtype='<f4').reshape(-1, 2).astype(np.float64)
    if len(audio) != frames or not np.isfinite(audio).all():
        raise ValueError('Invalid or incomplete raw audio')
    opening, closing = round(.01 * RATE), min(round(.25 * RATE), len(audio) // 2)
    audio[:opening] *= np.linspace(0.0, 1.0, opening)[:, None] ** 2
    audio[-closing:] *= np.linspace(1.0, 0.0, closing)[:, None] ** 2
    edges = raw.with_suffix('.edges.f32')
    audio.astype('<f4').tofile(edges)
    before = meter(edges, raw=True)
    if not all(math.isfinite(value) for value in before.values()):
        raise ValueError('Silent or invalid input loudness')
    gain = min(-18.0 - before['lufs'], -2.2 - before['true_peak_dbtp'])
    if not math.isfinite(gain):
        raise ValueError('Silent or invalid audio loudness')
    audio *= 10.0 ** (gain / 20.0)
    random = np.random.default_rng(106)
    samples = np.rint(audio * 8388608.0 + random.random(audio.shape) - random.random(audio.shape)).astype(np.int32)
    if np.max(np.abs(samples.astype(np.int64))) >= 8388608:
        raise ValueError('Master would clip 24-bit PCM')
    packed = samples.astype('<i4').view(np.uint8).reshape(-1, 4)[:, :3].tobytes()
    temporary = output.with_suffix('.partial.wav')
    with wave.open(str(temporary), 'wb') as wav:
        wav.setparams((2, 3, RATE, 0, 'NONE', 'not compressed'))
        wav.writeframes(packed)
    after = meter(temporary)
    if not all(math.isfinite(value) for value in after.values()) or after['true_peak_dbtp'] > -2.0:
        raise ValueError('Master failed the true-peak headroom check')
    temporary.replace(output)
    return dict(gain_db=round(gain, 3), **after)


def render_performance(performance, renderer, output, scratch, identity):
    slot = performance['slot']
    stem = f'{EXPECTED_SLOTS.index(slot) + 1:02d}-{slot}'
    score_text = f'tempo {performance["bpm"]}\nlength {performance["beats"]}\n' + ''.join(
        f'gate {n["beat"]:.9f} {n["duration"]:.9f} {n["key"]} {n["velocity"]}\n' for n in performance['notes'])
    score = scratch / f'{stem}.score'
    score.write_text(score_text, encoding='utf-8')
    signature = dict(renderer_sha256=sha256(renderer), source_sha256=identity['sha256'],
                     score_sha256=hashlib.sha256(score_text.encode('utf-8')).hexdigest())
    raw, cache = scratch / f'{stem}.f32', scratch / f'{stem}.raw.json'
    try:
        cached = json.loads(cache.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        cached = {}
    if cached.get('signature') == signature and raw.exists() and sha256(raw) == cached.get('raw_sha256'):
        engine = cached['engine']
    else:
        print(f'Rendering {slot} — {performance["title"]}', flush=True)
        temporary = raw.with_suffix('.partial.f32')
        engine = json.loads(command(renderer, slot, score, temporary).stdout)
        temporary.replace(raw)
        write_json(cache, dict(signature=signature, engine=engine, raw_sha256=sha256(raw)))
    wav, midi = output / f'{stem}.wav', output / 'MIDI' / f'{stem}.mid'
    finishing = master_audio(raw, wav, engine['frames'])
    write_midi(midi, performance)
    result = {key: value for key, value in performance.items() if key != 'notes'}
    result.update(seconds=round(engine['frames'] / RATE, 3), note_count=len(performance['notes']),
                  wav=wav.name, midi=f'MIDI/{midi.name}', engine=engine, provenance=signature,
                  wav_sha256=sha256(wav), midi_sha256=sha256(midi), **finishing)
    write_json(scratch / f'{stem}.finished.json', result)
    print(f'Finished {slot}: {result["seconds"]:.1f}s, {result["lufs"]:.1f} LUFS', flush=True)
    return result


def write_player(output, manifest):
    cards = []
    for take in manifest['performances']:
        escaped = {key: html.escape(str(take.get(key, '')), quote=True)
                   for key in ['slot', 'name', 'title', 'category', 'description', 'key', 'bpm', 'wav', 'midi']}
        cards.append('''<article data-category="{category}"><div class="label">{slot} · {category}</div>
<h2>{name}</h2><p class="title">{title}</p><p>{description}</p><div class="details">{key} · {bpm} BPM</div>
<audio controls preload="none" src="{wav}" aria-label="{name} demo"></audio>
<div class="downloads"><a href="{wav}" download>WAV ↗</a><a href="{midi}" download>MIDI ↗</a></div></article>'''.format(**escaped))
    page = '''<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>YouKnow / Original Sessions</title><style>
:root{color-scheme:dark;font-family:system-ui,sans-serif;background:#15201e;color:#f3ead9}*{box-sizing:border-box}
body{margin:0}main{max-width:1160px;margin:auto;padding:48px 24px}header{max-width:740px;margin-bottom:38px}
.label,.details,.title{color:#c1ad83;font-size:13px}h1{font:normal clamp(40px,6vw,72px)/1.1 Georgia,serif;margin:18px 0}h2{font:normal 28px Georgia,serif;margin:18px 0 8px}
p{color:#b4c0b9;line-height:1.6}nav{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:18px}button,a{color:inherit}button{border:1px solid #58645b;border-radius:24px;background:transparent;padding:9px 16px;cursor:pointer}
button[aria-pressed=true]{background:#c1ad83;color:#17221f}button:focus-visible,a:focus-visible{outline:2px solid #f3ead9;outline-offset:4px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,300px),1fr));gap:18px}article{padding:24px;background:#1e2c28;border:1px solid #3d4b43;border-radius:6px}article[hidden]{display:none}article.playing{border-color:#c1ad83}audio{display:block;width:100%;margin:22px 0 12px}.downloads{display:flex;gap:20px;font-size:12px}footer{border-top:1px solid #3d4b43;margin-top:36px;padding-top:18px;font-size:13px}
</style></head><body><main><header><div class="label">YOUKNOW / SIXTEEN ORIGINAL SESSIONS</div>
<h1>Small phrases.<br>Distinct voices.</h1><p>Eight basses and eight pads, played with their saved controls. Every preset gets an original short performance.</p>
<div class="label">24-bit / 96 kHz stereo · Maximum quality</div></header>
<nav aria-label="Preset filters"><button data-filter="all" aria-pressed="true">All 16</button><button data-filter="Bass" aria-pressed="false">Bass</button><button data-filter="Pad" aria-pressed="false">Pads</button><button id="collection">Play visible collection →</button></nav>
<p id="status" role="status">Choose a sound to listen.</p><section class="grid">''' + '\n'.join(cards) + '''</section>
<footer><p>The active product DSP circuit profile, with Direct performance timing and measured converter geometry. Static gain toward −18 LUFS with at least 2 dB true-peak headroom; file-edge fades and 24-bit TPDF dither. No added effects, EQ or dynamics. Choose the named preset before playing its MIDI; MIDI starts at beat zero and WAV has a 50 ms lead-in.</p>
<a href="manifest.json">Render details</a> · <a href="YouKnow-Original-Presets.m3u">Playlist</a></footer></main><script>
const cards=[...document.querySelectorAll('article')],players=cards.map(c=>c.querySelector('audio')),status=document.querySelector('#status'),collection=document.querySelector('#collection');let automatic=false;
function cancel(){automatic=false;collection.textContent='Play visible collection →'}
function play(i){players[i].play().catch(()=>{cancel();status.textContent='Press the audio player to begin.'})}
players.forEach((audio,i)=>{audio.addEventListener('play',()=>{players.forEach((p,j)=>{if(j!==i)p.pause()});cards.forEach((c,j)=>c.classList.toggle('playing',j===i));status.textContent='Playing '+cards[i].querySelector('h2').textContent});audio.addEventListener('pause',()=>{cards[i].classList.remove('playing');if(!players.some(p=>!p.paused)&&!audio.ended){cancel();status.textContent='Playback paused.'}});audio.addEventListener('ended',()=>{if(automatic){const next=cards.findIndex((c,j)=>j>i&&!c.hidden);if(next>=0){players[next].currentTime=0;play(next)}else{cancel();status.textContent='Collection finished.'}}else status.textContent='Choose a sound to listen.'});audio.addEventListener('error',()=>{cancel();status.textContent='Audio unavailable. Keep the page beside its WAV files.'})});
document.querySelectorAll('[data-filter]').forEach(button=>button.addEventListener('click',()=>{cancel();players.forEach(p=>p.pause());cards.forEach(c=>c.hidden=button.dataset.filter!=='all'&&c.dataset.category!==button.dataset.filter);document.querySelectorAll('[data-filter]').forEach(b=>b.setAttribute('aria-pressed',String(b===button)));status.textContent='Choose a sound to listen.'}));
collection.addEventListener('click',()=>{if(automatic){cancel();players.forEach(p=>p.pause());return}const current=players.findIndex((p,i)=>!p.paused&&!cards[i].hidden),first=current<0?cards.findIndex(c=>!c.hidden):current;if(first>=0){automatic=true;collection.textContent='Pause collection';if(current<0)players[first].currentTime=0;play(first)}});
</script></body></html>'''
    (output / 'index.html').write_text(page, encoding='utf-8')


def package(output, scratch):
    identity = source_identity()
    results = []
    for index, slot in enumerate(EXPECTED_SLOTS, 1):
        path = scratch / f'{index:02d}-{slot}.finished.json'
        result = json.loads(path.read_text(encoding='utf-8'))
        if result['slot'] != slot or result['provenance']['source_sha256'] != identity['sha256']:
            raise ValueError(f'{slot}: source changed since rendering; regenerate the collection')
        for kind in ['wav', 'midi']:
            if sha256(output / result[kind]) != result[f'{kind}_sha256']:
                raise ValueError(f'{slot}: changed or missing {kind} artifact')
        results.append(result)
    manifest = dict(collection='YouKnow / Original Sessions', source=identity, sample_rate=RATE,
                    format='24-bit PCM stereo WAV', mastering='Static gain toward -18 LUFS, capped at -2.2 dBTP; edge fades; 24-bit TPDF dither',
                    performances=results)
    write_json(output / 'manifest.json', manifest)
    write_player(output, manifest)
    playlist = '#EXTM3U\n' + ''.join(f'#EXTINF:{take["seconds"]},{take["slot"]} {take["name"]} — {take["title"]}\n{take["wav"]}\n' for take in results)
    (output / 'YouKnow-Original-Presets.m3u').write_text(playlist, encoding='utf-8')
    archive = output.parent / 'YouKnow-Original-Sessions.zip'
    files = ['index.html', 'manifest.json', 'YouKnow-Original-Presets.m3u']
    files += [take[kind] for take in results for kind in ['wav', 'midi']]
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as zipped:
        for relative in files:
            zipped.write(output / relative, f'YouKnow-Original-Sessions/{relative}')
    print(f'Packaged sixteen performances: {output}', flush=True)


def find_renderer(requested):
    executable = 'YouKnowRenderOriginalPresets.exe' if os.name == 'nt' else 'YouKnowRenderOriginalPresets'
    candidates = ([requested / executable, requested / 'Release' / executable] if requested and requested.is_dir()
                  else [requested] if requested else [ROOT / directory / executable
                        for directory in ['build-dsp', 'build-dsp/Release', 'build', 'build/Release', 'build-win/Release']])
    if requested and not requested.is_dir() and requested.suffix.lower() != '.exe':
        candidates.append(requested.with_suffix('.exe'))
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise ValueError('Renderer was not found; build YouKnowRenderOriginalPresets or pass --renderer PATH')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path)
    parser.add_argument('--output', type=Path, default=ROOT / 'Docs/audio/youknow-originals')
    parser.add_argument('--scratch', type=Path, default=ROOT / 'out/original-sessions')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--only', nargs='+', choices=EXPECTED_SLOTS)
    parser.add_argument('--package-only', action='store_true')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 8 or (args.only and args.package_only):
        parser.error('--jobs must be 1–8; --only cannot be combined with --package-only')
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'MIDI').mkdir(exist_ok=True)
    args.scratch.mkdir(parents=True, exist_ok=True)
    if args.package_only:
        package(args.output, args.scratch)
        return
    renderer = find_renderer(args.renderer)
    bank = json.loads(command(renderer, '--list').stdout)
    performances = make_performances(bank)
    selected = [take for take in performances if not args.only or take['slot'] in args.only]
    identity = source_identity()
    with ThreadPoolExecutor(max_workers=args.jobs) as workers:
        list(workers.map(lambda take: render_performance(take, renderer, args.output, args.scratch, identity), selected))
    if not args.only:
        package(args.output, args.scratch)


if __name__ == '__main__':
    main()
