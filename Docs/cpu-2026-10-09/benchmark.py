#!/usr/bin/env python3
"""Serial baseline/candidate/baseline CPU comparison using the product audit."""
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
    parser.add_argument('--rate', type=int, default=48000)
    parser.add_argument('--factor', type=int, choices=[1, 2, 4], default=1)
    parser.add_argument('--cpu', type=int, help='Optional Linux taskset CPU affinity')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    binaries = {name: path.resolve() for name, path in
                [('baseline', args.baseline), ('candidate', args.candidate)]}
    hashes = {name: digest(path) for name, path in binaries.items()}
    runs = []
    summary = []
    for mode in ['original', 'direct']:
        records = []
        protocols = []
        for label in ['baseline-a', 'candidate', 'baseline-b']:
            binary = binaries['candidate' if label == 'candidate' else 'baseline']
            flag = '--original-cpu-benchmark' if mode == 'original' else '--cpu-benchmark'
            command = [str(binary), flag, str(args.rate), str(args.factor)]
            if args.cpu is not None:
                command = ['taskset', '-c', str(args.cpu)] + command
            result = subprocess.run(command, capture_output=True, text=True, timeout=600)
            (args.output / f'{mode}-{label}.txt').write_text(result.stdout + result.stderr)
            result.check_returncode()
            protocol = [line for line in result.stdout.splitlines() if line.startswith('protocol ')]
            if len(protocol) != 1:
                raise RuntimeError('missing or ambiguous benchmark protocol')
            protocols.append(protocol[0])
            rows = {}
            for line in result.stdout.splitlines():
                if not line.startswith('cpu_timing '):
                    continue
                _, scenario, quality, fingerprint, median, minimum, mad, realtime = line.split()
                values = list(map(float, [median, minimum, mad, realtime]))
                if not all(math.isfinite(value) and value >= 0 for value in values) or values[0] <= 0:
                    raise RuntimeError('invalid benchmark timing')
                rows[scenario] = dict(quality=quality, fingerprint=fingerprint,
                                      median_cpu_ms=values[0], minimum_cpu_ms=values[1],
                                      mad_cpu_ms=values[2], cpu_realtime_ratio=values[3])
            if len(rows) != (7 if mode == 'original' else 8):
                raise RuntimeError('incomplete benchmark scenario matrix')
            records.append(rows)
            runs.append(dict(mode=mode, label=label, command=command, scenarios=rows))
            print(mode, label, 'complete', flush=True)
        if not records[0].keys() == records[1].keys() == records[2].keys():
            raise RuntimeError('scenario sets changed between builds')
        if not protocols[0] == protocols[1] == protocols[2]:
            raise RuntimeError('benchmark protocols changed between builds')
        for scenario, candidate in records[1].items():
            a, b = records[0][scenario], records[2][scenario]
            if not a['quality'] == b['quality'] == candidate['quality']:
                raise RuntimeError('applied quality changed between builds')
            baseline = min(a['median_cpu_ms'], b['median_cpu_ms'])
            row = dict(mode=mode, scenario=scenario, baseline_cpu_ms=baseline,
                       candidate_cpu_ms=candidate['median_cpu_ms'],
                       reduction_percent=100 * (1 - candidate['median_cpu_ms'] / baseline),
                       fingerprints_match=a['fingerprint'] == b['fingerprint'] == candidate['fingerprint'])
            summary.append(row)
            print(mode, scenario, f"{row['reduction_percent']:.2f}%", flush=True)
    if hashes != {name: digest(path) for name, path in binaries.items()}:
        raise RuntimeError('a benchmark binary changed during measurement')
    report = dict(rate=args.rate, factor=args.factor, affinity_cpu=args.cpu,
                  statistic='1 - candidate median / faster bracketing baseline median',
                  binary_sha256=hashes, runs=runs, summary=summary,
                  median_reduction_by_mode={mode: statistics.median(
                      row['reduction_percent'] for row in summary if row['mode'] == mode)
                      for mode in ['original', 'direct']})
    (args.output / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
