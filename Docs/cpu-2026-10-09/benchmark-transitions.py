#!/usr/bin/env python3
"""Time the four-second transition fixture in serial baseline/candidate brackets."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cpu', type=int)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    binaries = {'baseline': args.baseline.resolve(), 'candidate': args.candidate.resolve()}
    hashes = {name: digest(path) for name, path in binaries.items()}
    runs, comparisons = [], []
    for mode in ['original', 'direct']:
        for repeat in range(3):
            bracket = []
            for label in ['baseline-a', 'candidate', 'baseline-b']:
                name = f'{mode}-{repeat + 1}-{label}'
                output = (args.output / f'{name}.f32').resolve()
                binary = binaries['candidate' if label == 'candidate' else 'baseline']
                command = [str(binary), str(output), mode, '48000', '1', '173']
                if args.cpu is not None:
                    command = ['taskset', '-c', str(args.cpu)] + command
                result = subprocess.run(command, capture_output=True, text=True, timeout=120)
                (args.output / f'{name}.txt').write_text(result.stdout + result.stderr)
                result.check_returncode()
                metadata = dict(item.split('=', 1) for item in result.stdout.split())
                expected = dict(complete='true', protocol='youknow-cpu-2026-10-09-v1',
                                profile='active-product', timing=mode, rate='48000',
                                requested_quality='1', applied_quality='1', block='173',
                                frames='192000', channels='2', format='f32', normalized='false')
                if any(metadata.get(key) != value for key, value in expected.items()):
                    raise RuntimeError('unexpected fixture configuration')
                seconds = float(metadata['process_cpu_seconds'])
                if metadata['complete'] != 'true' or not math.isfinite(seconds) or seconds <= 0:
                    raise RuntimeError('invalid fixture timing')
                if output.stat().st_size != 4 * 48000 * 2 * 4:
                    raise RuntimeError('incomplete fixture audio')
                row = dict(mode=mode, repeat=repeat + 1, label=label, command=command,
                           metadata=metadata, process_cpu_seconds=seconds,
                           audio_sha256=digest(output))
                bracket.append(row)
                runs.append(row)
            expected = bracket[0]['metadata']
            for row in bracket[1:]:
                for key, value in expected.items():
                    if key not in ['process_cpu_seconds', 'max_callback_cpu_us']:
                        if row['metadata'][key] != value:
                            raise RuntimeError(f'fixture metadata differs: {key}')
                if row['audio_sha256'] != bracket[0]['audio_sha256']:
                    raise RuntimeError('transition fixture audio differs')
            baseline = min(bracket[0]['process_cpu_seconds'], bracket[2]['process_cpu_seconds'])
            candidate = bracket[1]['process_cpu_seconds']
            reduction = 100 * (1 - candidate / baseline)
            comparisons.append(dict(mode=mode, repeat=repeat + 1,
                                    baseline_cpu_seconds=baseline, candidate_cpu_seconds=candidate,
                                    reduction_percent=reduction, audio_identical=True))
            print(mode, repeat + 1, f'{reduction:.2f}%', flush=True)
    if hashes != {name: digest(path) for name, path in binaries.items()}:
        raise RuntimeError('a renderer changed during measurement')
    report = dict(rate=48000, factor=1, block=173, affinity_cpu=args.cpu,
                  statistic='median of three reductions against faster bracketing baseline',
                  scope='process CPU only; excludes preparation, synchronous event/control delivery and I/O',
                  renderer_sha256=hashes, runs=runs, comparisons=comparisons,
                  median_reduction_by_mode={mode: statistics.median(
                      row['reduction_percent'] for row in comparisons if row['mode'] == mode)
                      for mode in ['original', 'direct']})
    (args.output / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
