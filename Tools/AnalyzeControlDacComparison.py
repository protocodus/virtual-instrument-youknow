#!/usr/bin/env python3
"""Compare matching raw control-DAC renders and prepare controlled A/B WAVs.

Inputs are the three RenderControlDacProbe fixtures or the five factory-score
renders, with their original JSON sidecars. Require 96 kHz stereo float32,
identical frames, patch/pitch/quality metadata and score identity. Existing
factory sidecars name a score path, not a historical score hash: this tool
records its current hash but does not claim to reconstruct missing provenance.

Raw residual is RMS(B-A)/RMS(A); matched residual uses one whole-file RMS gain
on B, without alignment, filtering or spectral fitting. Centroid is Hann-
windowed stereo spectral-magnitude centroid over 20..8000 Hz. These describe a
build difference, not perceptual fidelity or agreement with hardware.

Delivery applies identical 5 ms linear head and 60 ms linear end guards, matches
whole-file stereo RMS AFTER those guards, then applies one shared gain giving
both files at least 3 dB peak headroom. Save 96 kHz 24-bit PCM with deterministic
TPDF dither; never modify input. A is before, B is corrected. Metrics retain
raw levels/residuals, delivery trims, source hashes and round-trip PCM levels.

python3 Tools/AnalyzeControlDacComparison.py --before BEFORE --after AFTER --output OUT
python3 Tools/AnalyzeControlDacComparison.py --self-test
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import tempfile
import wave

import numpy as np
from scipy.io import wavfile

RATE = 96000
PROBES = {'quiet-sustain', 'sustain-and-gate', 'noise-resonance'}
FACTORY = {'A11', 'A48', 'A53', 'B11', 'A68'}
FACTORY_CRITICAL = ('preset', 'sample_rate', 'quality_selected',
                    'oversampling_applied', 'internal_rate', 'vcf_tanh',
                    'vcf_solver', 'aging', 'unit_character', 'volume',
                    'key_mode', 'portamento', 'pitch_bend', 'mod_wheel',
                    'master_tune_cents', 'factory_tone_bytes', 'seconds',
                    'peak_held_keys', 'score')
PROBE_CRITICAL = ('sample_rate', 'internal_rate', 'quality_selected', 'tanh',
                  'solver', 'block_size', 'unit_character', 'aging',
                  'preroll_seconds')
LSB = 1.0 / 8388608.0


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def db(value):
    return 20.0 * math.log10(value) if value > 0 else None


def rms(audio):
    return float(np.sqrt(np.mean(np.square(audio, dtype=np.float64))))


def levels(audio):
    return {'rms': rms(audio), 'rms_dbfs': db(rms(audio)),
            'peak': float(np.max(np.abs(audio))),
            'peak_dbfs': db(float(np.max(np.abs(audio))))}


def wave_format(path):
    """Inspect actual RIFF encoding; dtype alone cannot distinguish source PCM."""
    with Path(path).open('rb') as stream:
        header = stream.read(12)
        if len(header) != 12:
            raise ValueError(f'{path}: truncated RIFF header')
        riff, size, form = struct.unpack('<4sI4s', header)
        if riff != b'RIFF' or form != b'WAVE' or size + 8 != Path(path).stat().st_size:
            raise ValueError(f'{path}: expected complete little-endian RIFF/WAVE')
        fmt, data_bytes = None, None
        while stream.tell() < size + 8:
            chunk = stream.read(8)
            if len(chunk) != 8:
                raise ValueError(f'{path}: truncated chunk')
            name, length = struct.unpack('<4sI', chunk)
            if stream.tell() + length + (length & 1) > size + 8:
                raise ValueError(f'{path}: chunk exceeds file')
            if name == b'fmt ':
                if fmt is not None or length < 16 or length > 1024:
                    raise ValueError(f'{path}: invalid format chunk')
                body = stream.read(length)
                tag, channels, rate, byte_rate, alignment, bits = struct.unpack('<HHIIHH', body[:16])
                if tag == 0xfffe:
                    if length < 40 or struct.unpack('<H', body[18:20])[0] != bits:
                        raise ValueError(f'{path}: unsupported extensible valid bits')
                    if body[28:40] != bytes.fromhex('00001000800000aa00389b71'):
                        raise ValueError(f'{path}: unsupported extensible subtype')
                    tag = struct.unpack('<I', body[24:28])[0]
                fmt = {'encoding_tag': tag, 'channels': channels, 'sample_rate': rate,
                       'byte_rate': byte_rate, 'block_align': alignment, 'bits': bits}
            elif name == b'data':
                if data_bytes is not None:
                    raise ValueError(f'{path}: multiple data chunks')
                data_bytes = length
                stream.seek(length, 1)
            else:
                stream.seek(length, 1)
            if length & 1:
                stream.seek(1, 1)
        if (fmt is None or fmt['block_align'] <= 0 or not data_bytes
                or data_bytes % fmt['block_align']):
            raise ValueError(f'{path}: missing or partial sample data')
        fmt['frames'] = data_bytes // fmt['block_align']
        return fmt


def load_float(path):
    fmt = wave_format(path)
    expected = {'encoding_tag': 3, 'channels': 2, 'sample_rate': RATE,
                'byte_rate': RATE * 8, 'block_align': 8, 'bits': 32}
    if any(fmt.get(key) != value for key, value in expected.items()):
        raise ValueError(f'{path}: expected 96 kHz stereo IEEE float32, got {fmt}')
    rate, audio = wavfile.read(path)
    if rate != RATE or audio.dtype != np.float32 or audio.shape != (fmt['frames'], 2):
        raise ValueError(f'{path}: decoded format/shape mismatch')
    if not np.all(np.isfinite(audio)) or rms(audio) <= 1e-12:
        raise ValueError(f'{path}: nonfinite or effectively silent audio')
    return audio.astype(np.float64), fmt


def discover(directory):
    directory = Path(directory).resolve()
    stems = {p.stem for p in directory.glob('*.wav')}
    if stems not in (PROBES, FACTORY):
        raise ValueError(f'{directory}: expected exactly three probe or five factory WAV stems')
    if stems == PROBES:
        manifest = directory / 'probe.json'
        meta = json.loads(manifest.read_text())
        fixtures = meta.get('fixtures', [])
        if len(fixtures) != 3 or {row.get('name') for row in fixtures} != PROBES:
            raise ValueError(f'{manifest}: fixture inventory mismatch')
        rows = {row['name']: {**{k: v for k, v in meta.items() if k != 'fixtures'}, **row}
                for row in fixtures}
        paths = {stem: manifest for stem in stems}
        return directory, 'probe', rows, paths
    paths = {stem: directory / (stem + '.wav.json') for stem in stems}
    return directory, 'factory', {stem: json.loads(path.read_text()) for stem, path in paths.items()}, paths


def score_path(value, directory):
    path = Path(value)
    possibilities = [path] if path.is_absolute() else [directory / path, directory / path.name, path]
    for candidate in possibilities:
        if candidate.is_file():
            return candidate.resolve()
    raise ValueError(f'cannot resolve recorded score {value!r}')


def check_metadata(kind, before, after, before_dir, after_dir):
    keys = FACTORY_CRITICAL if kind == 'factory' else PROBE_CRITICAL + ('name', 'seconds')
    for key in keys:
        if key not in before or key not in after:
            raise ValueError(f'missing critical metadata: {key}')
        if before[key] != after[key]:
            raise ValueError(f'critical metadata differs: {key}')
    for key in ('score_sha256', 'events', 'midi_events', 'renderer_source_sha256', 'seed'):
        if key in before or key in after:
            if key not in before or key not in after or before[key] != after[key]:
                raise ValueError(f'critical optional metadata differs: {key}')
    if before['sample_rate'] != RATE or before['internal_rate'] != 192000 or before['quality_selected'] != 4:
        raise ValueError('metadata does not describe maximum-quality 96 kHz rendering')
    tanh = before['vcf_tanh'] if kind == 'factory' else before['tanh']
    solver = before['vcf_solver'] if kind == 'factory' else before['solver']
    if tanh != 'Exact' or solver != 'MersonHalfSteps':
        raise ValueError('metadata does not select Exact/MersonHalfSteps')
    provenance = {}
    if kind == 'factory':
        if before['oversampling_applied'] != 2:
            raise ValueError('metadata does not select 192 kHz internal processing')
        tone = before['factory_tone_bytes']
        if len(tone) != 18 or any(type(v) is not int or not 0 <= v <= 255 for v in tone):
            raise ValueError('invalid original factory tone bytes')
        paths = [score_path(meta['score'], directory)
                 for meta, directory in ((before, before_dir), (after, after_dir))]
        hashes = [sha256(path) for path in paths]
        if hashes[0] != hashes[1]:
            raise ValueError('referenced scores have different contents')
        if 'score_sha256' in before and before['score_sha256'] != hashes[0]:
            raise ValueError('recorded score hash does not match current score')
        provenance = {'score_sha256_at_analysis': hashes[0],
                      'historical_score_hash_recorded': 'score_sha256' in before,
                      'limitation': None if 'score_sha256' in before else
                      'Sidecars record a path only; current score identity does not prove historical events.'}
    return provenance


def centroid(audio):
    window = np.hanning(len(audio))[:, None]
    spectrum = np.fft.rfft((audio - np.mean(audio, axis=0)) * window, axis=0)
    magnitude = np.sqrt(np.sum(np.abs(spectrum) ** 2, axis=1))
    frequency = np.fft.rfftfreq(len(audio), 1.0 / RATE)
    mask = (frequency >= 20.0) & (frequency <= 8000.0)
    weight = float(np.sum(magnitude[mask]))
    return float(np.dot(frequency[mask], magnitude[mask]) / weight) if weight > 0 else None


def pair_metrics(before, after):
    if before.shape != after.shape:
        raise ValueError('audio frame/channel shapes differ')
    a, b = levels(before), levels(after)
    if a['rms'] <= 1e-12 or b['rms'] <= 1e-12:
        raise ValueError('cannot RMS-match effectively silent material')
    gain = a['rms'] / b['rms']
    ca, cb = centroid(before), centroid(after)
    return {'before': a, 'after': b,
            'raw_rms_shift_db': db(b['rms'] / a['rms']),
            'raw_peak_shift_db': db(b['peak'] / a['peak']),
            'raw_residual_dbc': db(rms(after - before) / a['rms']),
            'rms_matched_after_gain': gain, 'rms_matched_after_trim_db': db(gain),
            'rms_matched_residual_dbc': db(rms(after * gain - before) / a['rms']),
            'spectral_centroid_before_hz': ca, 'spectral_centroid_after_hz': cb,
            'spectral_centroid_shift_hz': cb - ca if ca is not None and cb is not None else None}


def guards(frames):
    head, tail = round(.005 * RATE), round(.060 * RATE)
    if frames < head + tail:
        raise ValueError('audio too short for nonoverlapping 5 ms / 60 ms guards')
    envelope = np.ones(frames)
    envelope[:head] = np.linspace(0.0, 1.0, head)
    envelope[-tail:] = np.linspace(1.0, 0.0, tail)
    return envelope[:, None]


def listening_pair(before, after):
    envelope = guards(len(before))
    a, b = before * envelope, after * envelope
    match = rms(a) / rms(b)
    b *= match
    # Reserve two quantizer LSBs for the one-LSB triangular dither and rounding.
    target = 10.0 ** (-3.0 / 20.0)
    shared = (target - 2.0 * LSB) / max(float(np.max(np.abs(a))), float(np.max(np.abs(b))))
    return a * shared, b * shared, {'after_rms_match_gain': match,
        'after_rms_match_trim_db': db(match), 'shared_gain': shared,
        'shared_gain_db': db(shared), 'head_guard_seconds': .005,
        'end_guard_seconds': .060, 'guard_curve': 'linear, endpoint-inclusive',
        'rms_match_stage': 'whole-file stereo RMS after identical guards',
        'target_peak_dbfs': -3.0, 'dither': 'deterministic TPDF, one 24-bit LSB peak each direction',
        'identical_dither_seed_for_pair': True}


def write_pcm24(path, audio, seed):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(seed)
    dither = rng.random(audio.shape) - rng.random(audio.shape)
    quantized = np.rint(audio * 8388608.0 + dither).astype(np.int64)
    quantized[[0, -1], :] = 0  # preserve the guard's two exact silent endpoints
    if np.max(quantized) > 8388607 or np.min(quantized) < -8388608:
        raise ValueError('PCM conversion would clip')
    packed = np.stack((quantized & 255, (quantized >> 8) & 255,
                       (quantized >> 16) & 255), axis=-1).astype(np.uint8)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix='.dac-pcm-', suffix='.wav',
                                         dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
        with wave.open(str(temporary), 'wb') as output:
            output.setnchannels(2)
            output.setsampwidth(3)
            output.setframerate(RATE)
            output.writeframes(packed.tobytes())
        fmt = wave_format(temporary)
        if fmt['encoding_tag'] != 1 or fmt['bits'] != 24 or fmt['frames'] != len(audio):
            raise ValueError('written PCM format failed round-trip validation')
        rate, decoded = wavfile.read(temporary)
        if rate != RATE or decoded.dtype != np.int32 or decoded.shape != audio.shape:
            raise ValueError('written PCM decoding failed round-trip validation')
        restored = decoded.astype(np.float64) / 2147483648.0
        if not np.array_equal(decoded.astype(np.int64), quantized * 256):
            raise ValueError('packed 24-bit samples changed on round trip')
        if np.max(np.abs(restored)) > 10.0 ** (-3.0 / 20.0):
            raise ValueError('delivery exceeds -3 dBFS peak ceiling')
        os.replace(temporary, path)
        temporary = None
        return {'format': fmt, **levels(restored), 'sha256': sha256(path)}
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def atomic_text(path, text):
    path = Path(path)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8',
                                         prefix='.dac-text-', dir=path.parent,
                                         delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(text)
        os.replace(temporary, path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def compare(before_dir, after_dir, output_dir):
    before_dir, kind, before_meta, before_sidecars = discover(before_dir)
    after_dir, after_kind, after_meta, after_sidecars = discover(after_dir)
    output_dir = Path(output_dir).resolve()
    if kind != after_kind or before_meta.keys() != after_meta.keys():
        raise ValueError('before/after fixture sets differ')
    if before_dir == after_dir:
        raise ValueError('before and after must be separate render directories')
    for source_dir in (before_dir, after_dir):
        if output_dir == source_dir or output_dir.is_relative_to(source_dir) or source_dir.is_relative_to(output_dir):
            raise ValueError('output and input directories must be disjoint')
    # Validate the complete input corpus before creating any delivery files.
    checked = []
    for stem in sorted(before_meta):
        if not (output_dir / stem).resolve().is_relative_to(output_dir):
            raise ValueError(f'{stem}: output fixture directory escapes through a symlink')
        provenance = check_metadata(kind, before_meta[stem], after_meta[stem], before_dir, after_dir)
        paths = (before_dir / (stem + '.wav'), after_dir / (stem + '.wav'))
        before, fmt_a = load_float(paths[0])
        after, fmt_b = load_float(paths[1])
        if fmt_a != fmt_b or before.shape != after.shape:
            raise ValueError(f'{stem}: encoded format/frames differ')
        if abs(len(before) / RATE - before_meta[stem]['seconds']) > .5 / RATE:
            raise ValueError(f'{stem}: metadata duration disagrees with audio frames')
        if kind == 'factory' and before_meta[stem]['preset'] != stem:
            raise ValueError(f'{stem}: preset sidecar names another patch')
        if len(before) < round(.065 * RATE):
            raise ValueError(f'{stem}: too short for guards')
        if stem == 'quiet-sustain' and len(before) < round(3.35 * RATE):
            raise ValueError('quiet-sustain is missing its fourth body window')
        checked.append((stem, paths, before, after, fmt_a, provenance))
    output_dir.mkdir(parents=True, exist_ok=True)
    results = {'schema_version': 1, 'kind': kind, 'before_directory': str(before_dir),
               'after_directory': str(after_dir), 'sample_rate': RATE,
               'residual_reference': 'before whole-file stereo RMS; no temporal alignment',
               'zero_db_values': 'null denotes zero residual (negative infinity dB)',
               'centroid': 'stereo magnitude, Hann window, 20..8000 Hz', 'files': []}
    for stem, paths, before, after, fmt, provenance in checked:
        result = {'name': stem, 'source_format': fmt,
                  'source_sha256': {'before': sha256(paths[0]), 'after': sha256(paths[1])},
                  'metadata_sha256': {'before': sha256(before_sidecars[stem]),
                                      'after': sha256(after_sidecars[stem])},
                  'metadata': before_meta[stem], 'score_provenance': provenance,
                  'raw_comparison': pair_metrics(before, after)}
        if stem == 'quiet-sustain':
            result['quiet_body_windows'] = []
            for index in range(4):
                start, end = .05 + .9 * index + .25, .05 + .9 * index + .60
                sl = slice(round(start * RATE), round(end * RATE))
                result['quiet_body_windows'].append({'note_index': index,
                    'start_seconds': start, 'end_seconds': end,
                    **pair_metrics(before[sl], after[sl])})
        a, b, delivery = listening_pair(before, after)
        directory = output_dir / stem
        seed = int.from_bytes(hashlib.sha256(stem.encode()).digest()[:8], 'little')
        delivery['A'] = write_pcm24(directory / 'A.wav', a, seed)
        delivery['B'] = write_pcm24(directory / 'B.wav', b, seed)
        delivery['encoded_rms_difference_db'] = db(delivery['B']['rms'] / delivery['A']['rms'])
        result['delivery'] = delivery
        results['files'].append(result)
        atomic_text(directory / 'metrics.json', json.dumps(result, indent=2, allow_nan=False) + '\n')
        atomic_text(directory / 'key.md',
            f'# Listening key\n\nA = before. B = corrected. Fixture: {stem}.\n\n'
            'The two files share 5 ms head and 60 ms end linear guards. Whole-file stereo RMS '
            'is matched after those guards, followed by one shared peak-safe gain. '
            'Both are 96 kHz 24-bit PCM with deterministic TPDF dither.\n\n'
            f"B matching trim: {delivery['after_rms_match_trim_db']:.9f} dB; "
            f"shared gain: {delivery['shared_gain_db']:.9f} dB. "
            'Raw before/after levels, residuals, PCM measurements and hashes are in metrics.json. '
            'This compares engine revisions and does not establish hardware fidelity.\n')
    atomic_text(output_dir / 'metrics.json', json.dumps(results, indent=2, allow_nan=False) + '\n')
    revision_keys = ('git_revision', 'dsp_revision', 'source_revision', 'revision')
    first_stem = next(iter(before_meta))
    revisions = []
    for label, metadata in (('Before', before_meta[first_stem]), ('Corrected', after_meta[first_stem])):
        recorded = [f'{key}={metadata[key]}' for key in revision_keys if key in metadata]
        revisions.append(f"{label} revision: {', '.join(recorded) if recorded else 'not recorded in source metadata'}.\n")
    links = '\n'.join(f'- [{stem} listening key]({stem}/key.md)' for stem in sorted(before_meta))
    atomic_text(output_dir / 'key.md',
        '# Listening keys\n\nA = before. B = corrected in every pair.\n\n'
        f'Before source directory: `{before_dir}`.\n\nCorrected source directory: `{after_dir}`.\n\n'
        + '\n'.join(revisions) + '\n'
        'Both files receive the same 5 ms head and 60 ms end linear guards. '
        'Match whole-file stereo RMS after those guards, then apply one shared gain '
        'to keep both peaks at or below -3 dBFS. Delivery is 96 kHz 24-bit PCM with '
        'deterministic TPDF dither. Per-file keys report the exact trims; metrics.json '
        'retains raw levels, residuals, validated metadata and input/output SHA-256 hashes. '
        'These compare engine revisions, not measured hardware fidelity.\n\n' + links + '\n')
    return results


def self_test():
    frames = RATE // 4
    t = np.arange(frames) / RATE
    tone = .08 * np.sin(2 * np.pi * 400 * t)
    a = np.column_stack((tone, tone * .7))
    b = a * 2
    measured = pair_metrics(a, b)
    assert abs(measured['raw_rms_shift_db'] - 20 * math.log10(2)) < 1e-11
    assert abs(measured['raw_peak_shift_db'] - 20 * math.log10(2)) < 1e-11
    assert abs(measured['raw_residual_dbc']) < 1e-11
    assert measured['rms_matched_residual_dbc'] is None
    assert abs(measured['spectral_centroid_shift_hz']) < 1e-10
    transient = b.copy()
    transient[frames // 2, 0] += .05
    assert pair_metrics(a, transient)['rms_matched_residual_dbc'] > -90
    la, lb, _ = listening_pair(a, b)
    assert abs(rms(la) / rms(lb) - 1) < 1e-12
    assert np.array_equal(la, lb)
    assert np.all(la[[0, -1]] == 0)
    assert np.max(np.abs(la)) < 10 ** (-3 / 20)
    with tempfile.TemporaryDirectory(prefix='youknow-dac-comparison-test-') as name:
        root = Path(name)
        one = write_pcm24(root / 'A.wav', la, 42)
        two = write_pcm24(root / 'B.wav', lb, 42)
        assert one['sha256'] == two['sha256']
        assert one['format']['bits'] == 24 and one['format']['channels'] == 2
        assert abs(one['rms'] - rms(la)) < 2 * LSB
        for rate, samples in ((48000, a.astype(np.float32)),
                              (RATE, a[:, 0].astype(np.float32)),
                              (RATE, (a * 32767).astype(np.int16)),
                              (RATE, a.astype(np.float64))):
            wavfile.write(root / 'bad.wav', rate, samples)
            try:
                load_float(root / 'bad.wav')
            except ValueError:
                pass
            else:
                raise AssertionError('accepted wrong source format')
        wavfile.write(root / 'good.wav', RATE, a.astype(np.float32))
        decoded, fmt = load_float(root / 'good.wav')
        assert decoded.shape == a.shape and fmt['bits'] == 32
        meta = dict(zip(PROBE_CRITICAL, (RATE, 192000, 4, 'Exact', 'MersonHalfSteps', 256, 1, 0, .37)))
        meta.update(name='quiet-sustain', seconds=.25)
        check_metadata('probe', meta, dict(meta), root, root)
        for key, value in [('sample_rate', 48000), ('solver', 'Rk4Single'), ('unit_character', 0), ('seconds', .3)]:
            wrong = {**meta, key: value}
            try:
                check_metadata('probe', meta, wrong, root, root)
            except ValueError:
                pass
            else:
                raise AssertionError(f'accepted metadata mismatch: {key}')
        (root / 'score.txt').write_text('Original synthetic score: MIDI 60 for one quarter-second.\n')
        factory = {key: 0 for key in FACTORY_CRITICAL}
        factory.update(preset='A11', sample_rate=RATE, quality_selected=4,
                       oversampling_applied=2, internal_rate=192000,
                       vcf_tanh='Exact', vcf_solver='MersonHalfSteps',
                       factory_tone_bytes=[0] * 18, seconds=.25,
                       score=str(root / 'score.txt'))
        check_metadata('factory', factory, dict(factory), root, root)
        for key, value in [('factory_tone_bytes', [1] + [0] * 17),
                           ('master_tune_cents', 12), ('pitch_bend', .5)]:
            try:
                check_metadata('factory', factory, {**factory, key: value}, root, root)
            except ValueError:
                pass
            else:
                raise AssertionError(f'accepted factory pitch/tone mismatch: {key}')
        try:
            pair_metrics(a, b[:-1])
        except ValueError:
            pass
        else:
            raise AssertionError('accepted unequal frame counts')
        malformed = bytearray((root / 'good.wav').read_bytes())
        fmt_offset = malformed.index(b'fmt ') + 8
        malformed[fmt_offset + 12:fmt_offset + 14] = b'\x00\x00'
        (root / 'bad-alignment.wav').write_bytes(malformed)
        try:
            load_float(root / 'bad-alignment.wav')
        except ValueError:
            pass
        else:
            raise AssertionError('accepted zero block alignment')
        before_dir, after_dir = root / 'before', root / 'after'
        before_dir.mkdir()
        after_dir.mkdir()
        long_tone = np.tile(a, (17, 1))[:round(4.05 * RATE)].astype(np.float32)
        manifest = {key: meta[key] for key in PROBE_CRITICAL}
        manifest['fixtures'] = [{'name': stem, 'seconds': 4.05} for stem in sorted(PROBES)]
        for directory, gain in ((before_dir, 1), (after_dir, 2)):
            (directory / 'probe.json').write_text(json.dumps(manifest))
            for stem in PROBES:
                wavfile.write(directory / (stem + '.wav'), RATE, long_tone * gain)
        result = compare(before_dir, after_dir, root / 'listening')
        assert len(result['files']) == 3
        assert (root / 'listening' / 'key.md').is_file()
        for row in result['files']:
            assert abs(row['delivery']['encoded_rms_difference_db']) < 1e-12
            assert row['delivery']['A']['sha256'] == row['delivery']['B']['sha256']
            if row['name'] == 'quiet-sustain':
                assert len(row['quiet_body_windows']) == 4
                assert all(abs(window['raw_rms_shift_db'] - 20 * math.log10(2)) < 1e-10
                           for window in row['quiet_body_windows'])
        input_hash = sha256(before_dir / 'quiet-sustain.wav')
        try:
            compare(before_dir, after_dir, before_dir / 'output')
        except ValueError:
            pass
        else:
            raise AssertionError('accepted output nested in input')
        assert sha256(before_dir / 'quiet-sustain.wav') == input_hash
    print('Control-DAC comparison self-test passed: scalar/transient residuals, RMS match, guards, '
          '24-bit round trip, corpus export, body windows, and format/metadata/path rejection.')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--before', type=Path)
    parser.add_argument('--after', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        if any(value is not None for value in (args.before, args.after, args.output)):
            parser.error('--self-test takes no corpus paths')
        self_test()
        return
    if any(value is None for value in (args.before, args.after, args.output)):
        parser.error('--before, --after and --output are required')
    try:
        result = compare(args.before, args.after, args.output)
    except (ValueError, OSError, KeyError, TypeError, json.JSONDecodeError) as error:
        parser.exit(1, f'Comparison failed: {error}\n')
    print(f"Validated and prepared {len(result['files'])} matching {result['kind']} A/B pairs in {args.output}")


if __name__ == '__main__':
    main()
