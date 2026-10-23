#!/usr/bin/env python3
"""Compare two nulltest output directories: residual (candidate - baseline) per preset.
Usage: compare_null.py BASE_DIR CAND_DIR [--csv out.csv]"""
import sys, os, math, glob
import numpy as np

def db(x):
    return 20.0 * math.log10(x) if x > 0 else -999.0

def main():
    base, cand = sys.argv[1], sys.argv[2]
    csv = None
    if '--csv' in sys.argv:
        csv = sys.argv[sys.argv.index('--csv') + 1]
    rows = []
    for path in sorted(glob.glob(os.path.join(base, '*.f32'))):
        name = os.path.basename(path)
        other = os.path.join(cand, name)
        if not os.path.exists(other):
            print(f'{name}: missing in candidate'); continue
        a = np.fromfile(path, dtype=np.float32).astype(np.float64)
        b = np.fromfile(other, dtype=np.float32).astype(np.float64)
        if a.shape != b.shape:
            print(f'{name}: length mismatch'); continue
        exact = np.array_equal(a.view(np.float64), b.view(np.float64)) and np.array_equal(
            np.fromfile(path, dtype=np.uint32), np.fromfile(other, dtype=np.uint32))
        r = b - a
        base_rms = math.sqrt(np.mean(a * a)) if a.size else 0.0
        res_rms = math.sqrt(np.mean(r * r)) if r.size else 0.0
        res_peak = float(np.max(np.abs(r))) if r.size else 0.0
        # worst 100 ms window (stereo interleaved: 2 * rate/10 samples; rate unknown, use 9600 values ~ 100 ms at 48k)
        w = 9600
        n = (r.size // w) * w
        if n > 0:
            rw = r[:n].reshape(-1, w); aw = a[:n].reshape(-1, w)
            rr = np.sqrt(np.mean(rw * rw, axis=1)); ar = np.sqrt(np.mean(aw * aw, axis=1))
            mask = ar > 1e-6
            worst_win = float(np.max(rr[mask] / ar[mask])) if mask.any() else 0.0
        else:
            worst_win = 0.0
        rows.append((name, exact, db(base_rms), db(res_rms), db(res_rms / base_rms) if base_rms > 0 else -999.0,
                     db(res_peak), db(worst_win)))
    rows.sort(key=lambda t: -t[4])
    print(f'{"preset":24s} {"exact":5s} {"base_rms_dBFS":>13s} {"res_rms_dBFS":>12s} {"res_rel_dB":>10s} {"res_peak_dBFS":>13s} {"worst100ms_rel_dB":>17s}')
    for name, exact, br, rr, rel, pk, ww in rows:
        print(f'{name:24s} {"yes" if exact else "no":5s} {br:13.2f} {rr:12.2f} {rel:10.2f} {pk:13.2f} {ww:17.2f}')
    n_exact = sum(1 for r in rows if r[1])
    if rows:
        print(f'\n{len(rows)} presets, {n_exact} bit-exact; worst relative residual {rows[0][4]:.2f} dB ({rows[0][0]}); '
              f'worst residual peak {max(r[5] for r in rows):.2f} dBFS; worst 100 ms window {max(r[6] for r in rows):.2f} dB')
    if csv:
        with open(csv, 'w') as f:
            f.write('preset,exact,baseline_rms_dbfs,residual_rms_dbfs,residual_relative_db,residual_peak_dbfs,worst_100ms_relative_db\n')
            for name, exact, br, rr, rel, pk, ww in sorted(rows):
                f.write(f'{name},{int(exact)},{br:.2f},{rr:.2f},{rel:.2f},{pk:.2f},{ww:.2f}\n')

if __name__ == '__main__':
    main()
