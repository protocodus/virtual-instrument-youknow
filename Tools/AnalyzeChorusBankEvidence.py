#!/usr/bin/env python3
"""The chorus evidence behind YouKnow 1.4.3-1.4.5, from all sixteen factory-bank captures of #439522.

Protocol. Lewis Francis's Juno-106 #439522 (original DCO chips and chorus board, Borish replacement
VCF/VCA cards) played each 106_calibration.zip bank sequence at one recording gain: per patch a chord,
five one-second notes C1-C5 one second apart, then a gap before the next patch. The engine renders the
same MIDI (`YouKnowRenderCalibrationEvents events-<bank>.txt <bank>.wav 1 product`, 48 kHz, the shipping
profile), and every estimator below runs on the capture and on that render alike, so estimator bias
cancels pair by pair. Hardware and render are aligned on the first chord's onset (10 ms RMS envelope,
first frame 20 dB over the preceding floor). The recording gain is the median note-peak difference over
all 487 single notes, 4.55 dB; only the absolute comparisons (`thump`) use it.

  sweep  Effective chorus timing per patch: AnalyzeChorusCapture's channel-ratio estimator (source
         cancels in L/R) on 0.12 s Hann frames every 0.1 s from 0.2 s into each single note, harmonics
         to 900 Hz above 3 % of the strongest, through the exported support response
         (`YouKnowMeasureChorusSupport a11-spectral nominal-2sa1015-nonlinear`). Centre, depth, rate,
         phase, two wet gains and a balance are fitted on C1/C3/C5 (soft-L1, 56 starts, rate free in
         0.35-1.2 Hz); C2/C4 are held out. A fit within 0.5 % of a bound on either side is dropped.
         Result: hardware minus engine, mean and standard error over the converged pairs.
  hiss   Hiss swing: 2-16 kHz level in 50 ms frames over the four gaps between single notes (0.25 s
         after note-off to 0.05 s before the next note), of short-release (RELEASE <= 20), noise-free
         chorus-on patches. L-R in dB is fitted to a + b*tri(rate*t + phase) at the mode's rate; b is
         the half-swing. Also each patch's idle floor, A-weighted, minus its own C4 note (Hann
         periodogram, AnalyzeChorusIdleFloors.levels), which cancels the recording gain, for
         RELEASE <= 40, noise-free patches.
  thump  Turnaround thumps: in each idle window of those patches, 2 kHz-lowpassed 4 ms frame means;
         an event is a first lobe beyond 500 uV (capture scale) in either channel. Reports its peak,
         sign, time to peak and the other channel's ratio. Also the idle floor's Welch PSD (1 s
         segments) integrated over 30-100 Hz without 55-65 Hz, where neither the board's coupling
         nor the recording chain's high-pass shapes it.

Limits. One serviced unit with replacement voice cards; only the chorus board and DCOs are original,
so only chorus quantities are read here. The capture's own recording chain high-passes below about
20 Hz and adds its own floor and 120 Hz hum; the thump lobe's shape and the chorus-off floor are its,
not the instrument's. Short idle windows hold under one sweep, so single-patch figures scatter and only
means over patches are reported.

On 1.4.5: sweep hardware minus engine -0.025 ms depth (s.e. 0.060, 17 pairs), centre -0.023 ms,
rate -0.002 Hz; hiss half-swing 12.41/12.33 dB (hardware, modes I/II) against 12.56/12.53, idle minus
C4 -0.15/-0.08 dB; thump floor at 30-100 Hz -0.03/+0.54 dB hardware minus engine.

Sources: https://www.lewisfrancis.com/nwio/Juno-10-test-audio-96K.zip and
https://kayrock.org/kr106/106_calibration.zip; every file is checked by hash.

  python3 Tools/AnalyzeChorusBankEvidence.py export --midi-zip 106_calibration.zip --out out/bank
  for b in A1x ... B8x; do build/YouKnowRenderCalibrationEvents out/bank/events-$b.txt out/bank/$b.wav 1 product; done
  ./build/YouKnowMeasureChorusSupport a11-spectral nominal-2sa1015-nonlinear > out/bank/support.json
  python3 Tools/AnalyzeChorusBankEvidence.py sweep|hiss|thump --midi-zip 106_calibration.zip \\
      --captures CAPTURE_DIR --models out/bank [--support out/bank/support.json] [--jobs 4]
"""
import argparse
import concurrent.futures
import hashlib
import json
import math
import sys
import zipfile
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy.optimize import least_squares
from scipy.signal import butter, sosfilt, sosfiltfilt, welch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import AnalyzeChorusCapture as capture_estimator  # noqa: E402
import AnalyzeChorusIdleFloors as floors  # noqa: E402

# bank: (capture AIFF sha256, MIDI sha256)
BANKS = {
    "A1x": ("b235ba2236c1a509627ce1e84fa35004b0d7c3e99eb36899c4c5de63cc668662", "ef7ff68b6059a913a30a70acbea54d1ef982884ed229f25d8684642295a5e511"),
    "A2x": ("1e97d34ed535fc6de4c678adad05634b89ac758a3badb0671c0c7bf057f3f0e3", "c327d966858736269eaaac5786e2c83172ade45050933abdaa722a6905d67b24"),
    "A3x": ("80c44da879dd4671ad0ee3cdd01eed2d08e5e7a416659df9e154187a24b0d1e5", "3a09889cbfcf2fa4a041053fe8c153c67aef6d2e6ada9038993377a93f9417bc"),
    "A4x": ("3ac2e2918343b163b65910fbdb56be00119de074fd32bd745684b18168452484", "4ccd071d33b455977f13d82af7726b0732dcadee0027e18f29d966c0b6a7b9ee"),
    "A5x": ("7e5af30d1b5424415bfcd77f4b5c3c654c75d9bcd5e96042a02db6fdb76332e1", "9499414743846545dd276cd6e7628a2799ce4dd574b0211b2414b89cdfc21364"),
    "A6x": ("f20d4474c53b0ffe79cf634e1f21981b00bcc573a59591f3127da466a75c9b6a", "241fe20aaa85d6e0f52cf5a7154c48308995623c9aa1ed23e3c31509b3c8f42b"),
    "A7x": ("b3fb0acf6606ace94262ce459194c8ce57d29c635713ff01def7c4a8be882856", "418410470f2f412b7595fd25c272e2638ade6bad3e678a238b30f8d3c79815e9"),
    "A8x": ("31f756e07dfe7b06d316862c85b6d6e982ab277d94df27a745aba34e90a110ed", "f9af90589d82bdd65a92f36f8ffea78f00c8a4ec485b18e159444e1970d5f141"),
    "B1x": ("fa4bc6c4a00eeb002f4ed8e2746665485acd3782b3868ee158b8c107b345ed70", "3004e24f245782cd135a02305002fd489abd3f8f739c63b72d93b61ac2b2af0a"),
    "B2x": ("29b9d5683ad14595b69f2890a06c2a5e854691f446eb41716e29b240edca85e6", "ba50907caa6f8e035dfe004e169e4ee9e2371ec4e070726eb72137bcff02265b"),
    "B3x": ("cfc6e32244b78ac409787e934501e3ff398bfe9793f8d85202763c79388fe621", "7a821f38a08316c2d4c111a45659b407a69b054e3c1002e922d91fbd30213775"),
    "B4x": ("d0f7953ea437eec31c5151e4090842f9010282f1b91c07f12c86601a07a10bde", "f30671297e229885176faf2d7e43136cdd8d982a865292160cbf7dae6356ccaf"),
    "B5x": ("bff504d4abf24004a0bf36717674d0e090d6049cc1c5f140a70b46bd57ee1f41", "69016975eeb56ac0e06b245b0f54493964c3de3505c95ada7d75b9fd11ad8611"),
    "B6x": ("250e7c18245c8ba3b2765ea757e8c2be3773f11015cb6967396354cdd53eec31", "20f1d4754a538c42ae754f06f0d083c317845fdd75d3895ee88e58efe5e24591"),
    "B7x": ("758fcb7dc2cd914e38cb2baa361cfe93b4fcafcf2bdbcdfa6bdb62db06d2a9f5", "89e55414ea72cf31e3f20e4fc672a62ac7e9dc3817880d772b030a304f92915e"),
    "B8x": ("364b000834d56e6aec98d3142b0245e7e57add2249ba68c0ee42b898c895d84a", "65e290450dc023217d20724ebc473e862759621d5b184f68a2571813afa2f06f"),
}
RECORDING_GAIN_DB = 4.55
MODE_ONE_RATE_HZ = 0.5159334275
RATES = {"I": MODE_ONE_RATE_HZ, "II": MODE_ONE_RATE_HZ * 1.6234799}
# The patches tried for sweep fits; those that converge on both sides are kept (17 pairs on 1.4.5).
SWEEP_PATCHES = ("A12 A16 A22 A34 A35 A43 B26 B43 B55 B78 B25 A37 A15 A36 A38 B11 B14 B65 B23 A68 "
                 "B47 B81 B84 A57 A71 A46 A78 B56 A75").split()


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def midi(zip_path, bank):
    data = zipfile.ZipFile(zip_path).read(f"106_calibration/bank_midi_106/bank_{bank}.mid")
    if sha256(data) != BANKS[bank][1]:
        raise SystemExit(f"bank {bank}: unknown MIDI")
    return data


def events(zip_path, bank):
    """(seconds, bytes) per event, the bank's 0x31 dumps restated as 0x30 (see AnalyzeChorusIdleFloors)."""
    out = []
    for seconds, kind, payload in floors.timeline(midi(zip_path, bank)):
        if kind == "sysex":
            data = bytearray(b"\xf0" + payload)
            if data[2] == 0x31:
                data[2] = 0x30
            out.append((seconds, bytes(data)))
        else:
            out.append((seconds, bytes([kind]) + payload))
    return out


def segments(event_list):
    """Per patch: its program dump and its (key, on, off) notes, chord first."""
    patches, current = [], None
    for seconds, data in event_list:
        if data[0] == 0xF0:
            current = dict(t=seconds, sysex=data, ons={}, notes=[])
            patches.append(current)
        elif data[0] & 0xF0 == 0x90 and data[2]:
            current["ons"].setdefault(data[1], []).append(seconds)
        elif data[0] & 0xF0 == 0x80 or data[0] & 0xF0 == 0x90:
            current["notes"].append((data[1], current["ons"][data[1]][-1], seconds))
    return patches


def chorus_of(dump):
    switch = dump[21]
    return "off" if switch & 32 else ("I" if switch & 64 else "II")


def envelope_db(audio, rate, hop=0.005, window=0.010):
    mono = sosfiltfilt(butter(2, 20, "hp", fs=rate, output="sos"), audio.mean(1))
    n, h = int(window * rate), int(hop * rate)
    frames = len(mono) // h - 2
    rms = np.array([np.sqrt(np.mean(mono[i * h:i * h + n] ** 2) + 1e-20) for i in range(frames)])
    return 20 * np.log10(rms + 1e-12), hop


def onset(env, hop, t0, search=0.5):
    first, last = int(max(0.0, t0 - 0.15) / hop), int((t0 + search) / hop)
    floor = np.median(env[max(0, first - 40):first]) if first > 40 else env[first:last].min()
    above = np.nonzero(env[first:last] > floor + 20)[0]
    return (first + above[0]) * hop if len(above) else None


def load(args, bank):
    """Capture and render, each with its shift from MIDI time to audio time."""
    path = args.captures / f"bank_{bank}_bip.aif"
    if sha256(path.read_bytes()) != BANKS[bank][0]:
        raise SystemExit(f"bank {bank}: unknown capture")
    rate, hardware = floors.read_aiff(path)
    model, model_rate = sf.read(args.models / f"{bank}.wav", dtype="float64")
    event_list = events(args.midi_zip, bank)
    patches = segments(event_list)
    first = patches[0]["notes"][0][1]
    out = []
    for audio, audio_rate in ((hardware, rate), (model, model_rate)):
        env, hop = envelope_db(audio, audio_rate)
        out.append((audio, audio_rate, onset(env, hop, first) - first, env, hop))
    return event_list, patches, out


def triangle(x):
    return 1 - 4 * np.abs((x % 1) - 0.5)


# ---------------------------------------------------------------- sweep
def sweep_fit(job):
    args, bank, index, which = job
    _, patches, sources = load(args, bank)
    audio, rate, shift, env, hop = sources[which]
    support = json.loads(Path(args.support).read_text())
    singles = patches[index]["notes"][-5:]

    def pitch(segment, f0):
        best, window, t = None, np.hanning(len(segment)), np.arange(len(segment)) / rate
        for cents in np.arange(-40, 40.1, 1.0):
            f = f0 * 2 ** (cents / 1200)
            h = f * np.arange(1, max(2, int(3000 / f)) + 1)
            v = np.abs(np.exp(-2j * np.pi * h[:, None] * t) @ (segment * window)).sum()
            if best is None or v > best[0]:
                best = (v, cents)
        return best[1]

    frames = []
    for note, (key, on, off) in enumerate(singles):
        start = onset(env, hop, on + shift) or (on + shift)
        f0 = 440 * 2 ** ((key - 69) / 12)
        fundamental = f0 * 2 ** (pitch(audio[int((start + 0.3) * rate):int((start + 0.6) * rate)].mean(1), f0) / 1200)
        for centre in start + np.arange(0.2, min(0.86, off - on - 0.05), 0.1):
            count = int(0.12 * rate)
            time, window = np.arange(count) / rate - 0.06, np.hanning(count)
            samples = audio[int((centre - 0.06) * rate):int((centre - 0.06) * rate) + count]
            freqs = fundamental * np.arange(1, int(900 / fundamental) + 1)
            if len(freqs) == 0:
                continue
            coef = (np.exp(-2j * np.pi * freqs[:, None] * time) @ (samples * window[:, None])) / window.sum()
            keep = np.linalg.norm(coef, axis=1) > np.linalg.norm(coef, axis=1).max() * 0.03
            freqs, coef = freqs[keep], coef[keep]
            frames.append(dict(time=centre, note=note, f=freqs, c=coef,
                               s=capture_estimator.support_response(support, freqs),
                               w=np.linalg.norm(coef, axis=1) + 0.05 * np.linalg.norm(coef)))
    quad = np.linspace(-0.06, 0.06, 25)
    weights = np.hanning(len(quad))
    weights /= weights.sum()
    train = [i for i, f in enumerate(frames) if f["note"] % 2 == 0]
    held = [i for i in range(len(frames)) if i not in train]

    def residual(q, indices):
        centre, depth, freq, phase, left_gain, right_gain, balance = q
        out = []
        for i in indices:
            fr = frames[i]
            tri = triangle(freq * (fr["time"] + quad) + phase)
            omega = -2j * np.pi * fr["f"][:, None]
            left = 1 + left_gain * fr["s"] * (np.exp(omega * (centre + depth * tri)) @ weights)
            right = 1 + right_gain * fr["s"] * (np.exp(omega * (centre - depth * tri)) @ weights)
            e = (fr["c"][:, 1] * left - balance * fr["c"][:, 0] * right) / fr["w"]
            out.extend(e.real)
            out.extend(e.imag)
        return np.asarray(out)

    lo = np.array([0.0015, 0.0003, 0.35, -2, 0.3, 0.3, 0.7])
    hi = np.array([0.006, 0.0035, 1.2, 2, 3, 3, 1.4])
    best = None
    for freq in np.linspace(0.37, 1.18, 7):
        for phase in (0, 0.25, 0.5, 0.75):
            for c0, d0 in ((0.0035, 0.002), (0.0028, 0.0012)):
                r = least_squares(lambda q: residual(q, train), [c0, d0, freq, phase, 1.2, 1.2, 1],
                                  bounds=(lo, hi), loss="soft_l1", f_scale=0.08, diff_step=1e-4, max_nfev=150)
                err = float(np.mean(residual(r.x, train) ** 2))
                if best is None or err < best[0]:
                    best = (err, r.x)
    q = best[1]
    return dict(bank=bank, patch=index + 1, source=("hardware", "model")[which],
                centre_ms=q[0] * 1e3, depth_ms=q[1] * 1e3, rate_hz=q[2],
                train_rmse=math.sqrt(best[0]),
                held_rmse=float(np.sqrt(np.mean(residual(q, held) ** 2))) if held else float("nan"),
                boundary=bool(np.any(np.minimum(q - lo, hi - q) / (hi - lo) < 0.005)))


def run_sweep(args):
    jobs = [(args, f"{p[:2]}x", int(p[2]) - 1, which) for p in SWEEP_PATCHES for which in (0, 1)]
    with concurrent.futures.ProcessPoolExecutor(args.jobs) as pool:
        fits = list(pool.map(sweep_fit, jobs))
    pairs = {}
    for f in fits:
        pairs.setdefault((f["bank"], f["patch"]), {})[f["source"]] = f
    diffs = {k: [] for k in ("depth_ms", "centre_ms", "rate_hz")}
    for pair in pairs.values():
        if any(pair[s]["boundary"] for s in ("hardware", "model")):
            continue
        for k in diffs:
            diffs[k].append(pair["hardware"][k] - pair["model"][k])
    summary = {k: dict(pairs=len(v), mean=float(np.mean(v)), standard_error=float(np.std(v, ddof=1) / math.sqrt(len(v))))
               for k, v in diffs.items() if len(v) > 1}
    return dict(fits=fits, hardware_minus_model=summary)


# ---------------------------------------------------------------- hiss
def run_hiss(args):
    swing, idle = {}, {}
    band = None
    for bank in BANKS:
        event_list, patches, sources = load(args, bank)
        rows = floors.patches(floors.timeline(midi(args.midi_zip, bank)),
                              len(sources[0][0]) / sources[0][1] - sources[0][2])
        for k, p in enumerate(patches):
            dump = p["sysex"]
            tone, mode = dump[5:21], chorus_of(dump)
            if mode == "off" or tone[4] or tone[14] > 20:
                continue
            notes = p["notes"][-5:]
            gaps = [(notes[i][2] + 0.25, notes[i + 1][1] - 0.05) for i in range(4)]
            fits = []
            for audio, rate, shift, _, _ in sources:
                band = butter(4, [2000, 16000], "bp", fs=rate, output="sos")
                t, level = [], []
                for a, b in gaps:
                    a, b = a + shift, b + shift
                    seg = sosfilt(band, audio[int((a - 0.05) * rate):int(b * rate)], axis=0)[int(0.05 * rate):]
                    h = int(0.05 * rate)
                    for i in range(len(seg) // h):
                        t.append(a + (i + 0.5) * 0.05)
                        level.append(10 * np.log10(np.mean(seg[i * h:(i + 1) * h] ** 2, 0) + 1e-30))
                t, level = np.array(t), np.array(level)
                d, best = level[:, 0] - level[:, 1], None
                for phase in np.linspace(0, 1, 200, endpoint=False):
                    x = np.c_[np.ones_like(t), triangle(RATES[mode] * t + phase)]
                    c, *_ = np.linalg.lstsq(x, d, rcond=None)
                    r = float(np.sum((x @ c - d) ** 2))
                    if best is None or r < best[0]:
                        best = (r, abs(c[1]))
                fits.append(best[1])
            swing.setdefault(mode, []).append(fits)
        for row in rows:
            if not row["idle_seconds"] or not row["note_seconds"] or row["release"] > 40 or row["noise"] \
                    or row["chorus"] == "off":
                continue
            w, n = row["idle_seconds"], row["note_seconds"]
            d = []
            for audio, rate, shift, _, _ in sources:
                i = np.array(floors.levels((rate, audio), [w[0] + shift, w[1] + shift], True))
                c4 = np.array(floors.levels((rate, audio), [n[0] + shift, n[1] + shift], True))
                d.append(10 * np.log10(np.mean(10 ** (i / 10))) - 10 * np.log10(np.mean(10 ** (c4 / 10))))
            idle.setdefault(row["chorus"], []).append(d[1] - d[0])
    return dict(
        swing_half_db={m: dict(patches=len(v), hardware_median=float(np.median([x[0] for x in v])),
                               model_median=float(np.median([x[1] for x in v]))) for m, v in swing.items()},
        idle_minus_c4_model_minus_hardware_db={m: dict(patches=len(v), mean=float(np.mean(v)),
                                                       sd=float(np.std(v, ddof=1))) for m, v in idle.items()})


# ---------------------------------------------------------------- thump
def run_thump(args):
    lobes, psd = {}, {}
    freqs = np.arange(1, 400)
    for bank in BANKS:
        _, _, sources = load(args, bank)
        rows = floors.patches(floors.timeline(midi(args.midi_zip, bank)),
                              len(sources[0][0]) / sources[0][1] - sources[0][2])
        for row in rows:
            if not row["idle_seconds"] or row["chorus"] == "off" or row["release"] > 40 or row["noise"]:
                continue
            w = row["idle_seconds"]
            for name, (audio, rate, shift, _, _) in zip(("hardware", "model"), sources):
                seg = audio[int((w[0] + shift) * rate):int((w[1] + shift) * rate)]
                scale = 1.0 if name == "hardware" else 10 ** (RECORDING_GAIN_DB / 20)
                f, p = welch(seg, rate, nperseg=min(len(seg), int(rate)), axis=0)
                psd.setdefault((row["chorus"], name), []).append(np.interp(freqs, f, p.mean(1)) * scale ** 2)
                y = sosfiltfilt(butter(4, 2000, "lp", fs=rate, output="sos"), seg, axis=0)
                q = int(rate * 0.004)
                lf = y[:len(y) // q * q].reshape(-1, q, 2).mean(1) * scale
                hit = np.nonzero(np.abs(lf).max(1) > 500e-6)[0]
                entry = lobes.setdefault((row["chorus"], name), dict(windows=0, events=[]))
                entry["windows"] += 1
                if len(hit) and hit[0] > 2:
                    first = hit[0]
                    channel = int(np.argmax(np.abs(lf[first])))
                    run = lf[first:first + 15, channel]
                    k = int(np.argmax(np.abs(run)))
                    entry["events"].append((run[k] * 1e3, (k + 1) * 4, lf[first + k, 1 - channel] / run[k]))
    keep = (freqs >= 30) & (freqs <= 100) & (np.abs(freqs - 60) > 4)
    out = {}
    for (mode, name), entry in sorted(lobes.items()):
        e = np.array(entry["events"]).reshape(-1, 3)
        out[f"{mode}/{name}"] = dict(
            windows=entry["windows"], events=len(e),
            first_lobe_mv_median=float(np.median(e[:, 0])) if len(e) else None,
            negative=int((e[:, 0] < 0).sum()), ms_to_peak_median=float(np.median(e[:, 1])) if len(e) else None,
            other_channel_ratio_median=float(np.median(e[:, 2])) if len(e) else None,
            floor_30_100_hz_db=float(10 * np.log10(np.mean(psd[(mode, name)], 0)[keep].sum())))
    for mode in ("I", "II"):
        out[f"{mode}/hardware_minus_model_30_100_hz_db"] = \
            out[f"{mode}/hardware"]["floor_30_100_hz_db"] - out[f"{mode}/model"]["floor_30_100_hz_db"]
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("export", "sweep", "hiss", "thump"))
    parser.add_argument("--midi-zip", type=Path, required=True)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--captures", type=Path)
    parser.add_argument("--models", type=Path)
    parser.add_argument("--support", type=Path)
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if args.command == "export":
        args.out.mkdir(parents=True, exist_ok=True)
        for bank in BANKS:
            text = "".join(f"{s:.9f}\t{d.hex()}\n" for s, d in events(args.midi_zip, bank))
            (args.out / f"events-{bank}.txt").write_text(text)
        return
    if args.command == "sweep" and not args.support:
        parser.error("sweep needs --support")
    result = dict(sweep=run_sweep, hiss=run_hiss, thump=run_thump)[args.command](args)
    print(json.dumps(result if args.command != "sweep" else result["hardware_minus_model"], indent=1))
    if args.out:
        args.out.write_text(json.dumps(result, indent=1))


if __name__ == "__main__":
    main()
