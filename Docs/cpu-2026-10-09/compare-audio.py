#!/usr/bin/env python3
"""Render and compare the 20-case raw-float audio matrix (requires NumPy)."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import subprocess
import numpy as np


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    binaries = dict(baseline=args.baseline.resolve(), candidate=args.candidate.resolve())
    hashes = {name: digest(path) for name, path in binaries.items()}
    cases = []
    matrix = list(itertools.product(['original', 'direct'], [44100, 48000, 96000], [1, 2, 4], [173]))
    matrix += [(mode, 48000, 1, 64) for mode in ['original', 'direct']]
    for mode, rate, factor, block in matrix:
        captures, samples, metadata = {}, {}, {}
        for label, binary in binaries.items():
            name = f'{label}-{mode}-{rate}-{factor}-{block}'
            output = (args.output / f'{name}.f32').resolve()
            command = [str(binary), str(output), mode, str(rate), str(factor), str(block)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=120)
            (args.output / f'{name}.txt').write_text(result.stdout + result.stderr)
            result.check_returncode()
            metadata[label] = dict(item.split('=', 1) for item in result.stdout.split())
            if metadata[label]['complete'] != 'true' or output.stat().st_size != 4 * rate * 2 * 4:
                raise RuntimeError('incomplete audio fixture')
            captures[label] = dict(sha256=digest(output), bytes=output.stat().st_size,
                                   metadata=result.stdout.strip())
            samples[label] = np.fromfile(output, dtype=np.float32).astype(np.float64)
            if not np.isfinite(samples[label]).all():
                raise RuntimeError('non-finite audio')
        for key in ['protocol', 'profile', 'timing', 'rate', 'requested_quality',
                    'applied_quality', 'kernel', 'early', 'solver', 'block', 'frames',
                    'channels', 'format', 'endian', 'normalized']:
            if metadata['baseline'][key] != metadata['candidate'][key]:
                raise RuntimeError(f'fixture configuration differs: {key}')
        error = samples['candidate'] - samples['baseline']
        peak, rms = float(np.max(np.abs(error))), float(np.sqrt(np.mean(error * error)))
        row = dict(mode=mode, rate=rate, factor=factor, block=block, captures=captures,
                   peak_error=peak, rms_error=rms,
                   baseline_rms=float(np.sqrt(np.mean(samples['baseline'] ** 2))),
                   identical=captures['baseline']['sha256'] == captures['candidate']['sha256'],
                   passed=peak <= 1e-6 and rms <= 1e-8)
        cases.append(row)
        print(mode, rate, factor, block, 'identical' if row['identical'] else f'peak={peak} rms={rms}', flush=True)
        if not row['passed']:
            raise RuntimeError('audio difference exceeds fixture thresholds')
    if hashes != {name: digest(path) for name, path in binaries.items()}:
        raise RuntimeError('renderer changed during comparison')
    report = dict(cases=cases, renderer_sha256=hashes,
                  total_stereo_frames=sum(4 * row['rate'] for row in cases),
                  all_identical=all(row['identical'] for row in cases),
                  cpu_metadata_note='Diagnostic only: audio verification may run concurrently with other tests.')
    (args.output / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
