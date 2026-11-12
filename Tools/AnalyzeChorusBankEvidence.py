#!/usr/bin/env python3
"""The chorus evidence behind YouKnow 1.4.3-1.4.7, from all sixteen factory-bank captures of #439522.

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
  balance  Left-right balance through the chorus: each single note's RMS level 0.15-0.95 s after its
         onset, L-R in dB, averaged per patch (notes under -70 dBFS on the capture dropped), then over
         the patches of each chorus mode. Chorus on minus chorus off cancels the recording chain's own
         imbalance and the dry path's; reported for hardware, engine and their difference.
  comb   Wet/dry ratio per channel: each channel is dry plus delayed wet, so a steady harmonic's
         envelope swings between |1 + r| and |1 - r| as the delay sweeps, and its depth
         (max - min) / (max + min), 98th and 2nd percentiles after removing a log-linear trend, is
         min(r, 1/r). Harmonics of mode I patches without LFO to pitch, cutoff or pulse width and
         without noise, fundamental from 120 Hz (refined +/-40 cents), demodulated through a 6th-order
         low-pass at min(0.4 f0, 150 Hz), kept where the comb's beat rate stays inside it and the
         harmonic stands 35 dB over the envelope midway to the next one on both sides. Median depth
         per channel in three bands from 0.8 to 3.15 kHz.
  timing The firmware's pass clock, which steps the envelopes and the LFO. Release: on ENV-mode
         patches with SUSTAIN >= 30, the time after key-up (carried from each note's own onset) at
         which the 8 ms RMS envelope, 1 ms hop, has fallen 20 and 30 dB below its last 50 ms, kept
         where both sides take 100 ms or more; hardware over engine per note. LFO: on noise-free
         patches with DCO LFO >= 8, the fundamental's instantaneous frequency (band-pass 0.75-1.33 f0,
         analytic phase, 1 kHz trace) over the note's last 0.5 s, fitted to a sinusoid with a linear
         amplitude and trend, rate free within 30 % of the periodogram's peak; kept where both sides
         swing 3 cents or more over at least 1.5 cycles, median per patch, hardware over engine.

Limits. One serviced unit with replacement voice cards; only the chorus board and DCOs are original,
so only chorus quantities are read here; `timing` reads the CPU board's clock through those cards,
whose own settling is why release falls under 100 ms are left out. The capture's own recording chain high-passes below about
20 Hz and adds its own floor and 120 Hz hum; the thump lobe's shape and the chorus-off floor are its,
not the instrument's. Short idle windows hold under one sweep, so single-patch figures scatter and only
means over patches are reported.

On 1.4.5: sweep hardware minus engine -0.025 ms depth (s.e. 0.060, 17 pairs), centre -0.023 ms,
rate -0.002 Hz; hiss half-swing 12.41/12.33 dB (hardware, modes I/II) against 12.56/12.53, idle minus
C4 -0.15/-0.08 dB; thump floor at 30-100 Hz -0.03/+0.54 dB hardware minus engine.
On 1.4.7: balance, chorus on minus off, -0.21/-0.35 dB (hardware, modes I/II) against -0.13/-0.30,
hardware minus engine -0.06 dB (s.e. 0.08; 1.4.6: -1.20); comb depth left over right +0.33/+0.21/+0.26
dB (hardware, three bands) against +0.16/+0.33/+0.43 (1.4.6: -1.45/-1.29/-1.07).
On 1.4.7: timing, hardware over engine, release fall x1.011 (median of 290 falls; mean x1.009, s.e.
1.7 %); LFO rate x0.998 and vibrato depth x1.038 (medians of 17 patches; means x1.038 and x1.028, s.e.
3.8 %, the slow rates holding under two cycles).

Sources: https://www.lewisfrancis.com/nwio/Juno-10-test-audio-96K.zip and
https://kayrock.org/kr106/106_calibration.zip; every file is checked by hash.

  python3 Tools/AnalyzeChorusBankEvidence.py export --midi-zip 106_calibration.zip --out out/bank
  for b in A1x ... B8x; do build/YouKnowRenderCalibrationEvents out/bank/events-$b.txt out/bank/$b.wav 1 product; done
  ./build/YouKnowMeasureChorusSupport a11-spectral nominal-2sa1015-nonlinear > out/bank/support.json
  python3 Tools/AnalyzeChorusBankEvidence.py sweep|hiss|thump|balance|comb|timing --midi-zip 106_calibration.zip \\
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
from scipy.signal import butter, hilbert, sosfilt, sosfiltfilt, welch

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


# ---------------------------------------------------------------- balance
def standard_error(values):
    return float(np.std(values, ddof=1) / math.sqrt(len(values)))


def run_balance(args):
    per_mode = {}
    for bank in BANKS:
        _, patches, sources = load(args, bank)
        for p in patches:
            notes = []
            for _, on, _ in p["notes"][-5:]:
                level = []
                for audio, rate, shift, env, hop in sources:
                    start = onset(env, hop, on + shift) or (on + shift)
                    seg = audio[int((start + 0.15) * rate):int((start + 0.95) * rate)]
                    level.append(10 * np.log10(np.mean(seg ** 2, 0) + 1e-30))
                if level[0].min() > -70:
                    notes.append([level[0][0] - level[0][1], level[1][0] - level[1][1]])
            if notes:
                per_mode.setdefault(chorus_of(p["sysex"]), []).append(np.mean(notes, 0))
    per_mode = {m: np.array(v) for m, v in per_mode.items()}
    off = per_mode["off"]
    out = {m: dict(patches=len(v), hardware_l_minus_r_db=float(v[:, 0].mean()), hardware_se=standard_error(v[:, 0]),
                   model_l_minus_r_db=float(v[:, 1].mean()), model_se=standard_error(v[:, 1]))
           for m, v in per_mode.items()}
    for name, on in (("I", per_mode["I"]), ("II", per_mode["II"]), ("both", np.r_[per_mode["I"], per_mode["II"]])):
        d_on, d_off = on[:, 0] - on[:, 1], off[:, 0] - off[:, 1]
        out[f"{name}_minus_off"] = dict(
            hardware_db=float(on[:, 0].mean() - off[:, 0].mean()), model_db=float(on[:, 1].mean() - off[:, 1].mean()),
            hardware_minus_model_db=float(d_on.mean() - d_off.mean()),
            standard_error=float(np.hypot(standard_error(d_on), standard_error(d_off))))
    return out


# ---------------------------------------------------------------- comb
COMB_BANDS = ((800, 1250), (1250, 2000), (2000, 3150))


def comb_pitch(segment, rate, nominal):
    best, window, t = None, np.hanning(len(segment)), np.arange(len(segment)) / rate
    for cents in np.arange(-40, 40.1, 1.0):
        f = nominal * 2 ** (cents / 1200)
        harmonics = f * np.arange(1, max(2, int(3000 / f)) + 1)
        v = np.abs(np.exp(-2j * np.pi * harmonics[:, None] * t) @ (segment * window)).sum()
        if best is None or v > best[0]:
            best = (v, f)
    return best[1]


def comb_depths(audio, rate, start, nominal, top_hz, slope):
    """[(frequency, channel, depth, snr_db)] for one note's steady part."""
    seg = audio[int((start + 0.15) * rate):int((start + 0.97) * rate)]
    if len(seg) < int(0.7 * rate):
        return []
    f0 = comb_pitch(audio[int((start + 0.3) * rate):int((start + 0.6) * rate)].mean(1), rate, nominal)
    t = np.arange(len(seg)) / rate
    width = min(0.4 * f0, 150.0)
    low = butter(6, width, "lp", fs=rate, output="sos")
    trim = int(0.1 * rate)

    def envelope(channel, frequency):
        z = seg[:, channel] * np.exp(-2j * np.pi * frequency * t)
        return np.abs(sosfiltfilt(low, z.real) + 1j * sosfiltfilt(low, z.imag))[trim:-trim]

    out = []
    # the comb beats at f * d(delay)/dt; keep that inside the envelope filter
    for k in range(int(np.ceil(COMB_BANDS[0][0] / f0)), int(min(top_hz, 0.8 * width / slope) / f0) + 1):
        for channel in (0, 1):
            env, floor = envelope(channel, k * f0), envelope(channel, (k + 0.5) * f0)
            snr = 20 * np.log10(np.sqrt(np.mean(env ** 2)) / (np.sqrt(np.mean(floor ** 2)) + 1e-20))
            x = np.arange(len(env))
            flat = env / np.exp(np.polyval(np.polyfit(x, np.log(env + 1e-12), 1), x))
            hi, lo = np.percentile(flat, 98), np.percentile(flat, 2)
            out.append((k * f0, channel, (hi - lo) / (hi + lo), snr))
    return out


def comb_bank(job):
    args, bank = job
    _, patches, sources = load(args, bank)
    rows = []
    for index, p in enumerate(patches):
        dump = p["sysex"]
        tone, mode = dump[5:21], chorus_of(dump)
        # steady harmonics only: no LFO to pitch or cutoff, no noise, no LFO-swept pulse width
        swept_pulse = (dump[21] & 8) and tone[3] and not (dump[22] & 1)
        if mode == "off" or tone[2] or tone[8] or tone[4] or swept_pulse:
            continue
        octave = {1: 0.5, 2: 1.0, 4: 2.0}.get(dump[21] & 7, 1.0) * (0.5 if tone[15] else 1.0)
        for key, on, _ in p["notes"][-5:]:
            fundamental = 440 * 2 ** ((key - 69) / 12) * octave
            if fundamental < 120:
                continue
            pair = []
            for audio, rate, shift, env, hop in sources:
                start = onset(env, hop, on + shift) or (on + shift)
                pair.append(comb_depths(audio, rate, start, fundamental, COMB_BANDS[-1][1],
                                        4 * 1.978e-3 * RATES[mode]))
            for (f, channel, hardware, snr), (f2, channel2, model, snr2) in zip(*pair):
                if channel == channel2 and abs(f / f2 - 1) < 0.03 and min(snr, snr2) > 35:
                    rows.append((bank, index, mode, f, channel, hardware, model))
    return rows


def run_comb(args):
    with concurrent.futures.ProcessPoolExecutor(args.jobs) as pool:
        rows = [r for bank_rows in pool.map(comb_bank, [(args, bank) for bank in BANKS]) for r in bank_rows]
    out = {}
    for lo, hi in COMB_BANDS:
        entry = {}
        for channel, name in ((0, "left"), (1, "right")):
            pick = [r for r in rows if r[2] == "I" and r[4] == channel and lo <= r[3] < hi]
            ratio = 20 * np.log10([r[5] / r[6] for r in pick])
            entry[name] = dict(harmonics=len(pick), patches=len({r[:2] for r in pick}),
                               hardware_depth_median=float(np.median([r[5] for r in pick])),
                               model_depth_median=float(np.median([r[6] for r in pick])),
                               hardware_over_model_db_median=float(np.median(ratio)),
                               standard_error=standard_error(ratio))
        for source in ("hardware", "model"):
            entry[f"{source}_left_over_right_db"] = 20 * math.log10(
                entry["left"][f"{source}_depth_median"] / entry["right"][f"{source}_depth_median"])
        out[f"{lo}-{hi} Hz"] = entry
    return out


# ---------------------------------------------------------------- timing
def vibrato(segment, rate, f0):
    """Rate (Hz), depth (cents) and cycles of the fundamental's vibrato over the segment's last 0.5 s."""
    band = sosfiltfilt(butter(4, [f0 * 0.75, f0 * 1.33], "bp", fs=rate, output="sos"), segment)
    step = int(rate / 1000)
    hz = np.diff(np.unwrap(np.angle(hilbert(band)))[::step]) * 1000 / (2 * np.pi)
    cents = 1200 * np.log2(np.maximum(hz[-520:-20], 1.0) / np.median(hz[-520:-20]))
    t = np.arange(len(cents)) / 1000
    spectrum = np.abs(np.fft.rfft((cents - cents.mean()) * np.hanning(len(cents)), 1 << 16))
    freqs = np.fft.rfftfreq(1 << 16, 1e-3)
    guess = freqs[np.argmax(spectrum * ((freqs > 1.5) & (freqs < 40)))]

    def residual(q):
        return (q[0] + q[1] * t) * np.sin(2 * np.pi * q[2] * t + q[3]) + q[4] + q[5] * t - cents

    fits = [least_squares(residual, [np.std(cents) * 1.4, 0, guess, phase, 0, 0],
                          bounds=([-np.inf, -np.inf, guess * 0.7, -np.inf, -np.inf, -np.inf],
                                  [np.inf, np.inf, guess * 1.3, np.inf, np.inf, np.inf]))
            for phase in (0, np.pi / 2, np.pi, 3 * np.pi / 2)]
    q = min(fits, key=lambda f: f.cost).x
    return float(q[2]), float(abs(q[0] + q[1] * t.mean())), float(q[2] * t[-1])


def timing_bank(job):
    args, bank = job
    _, patches, sources = load(args, bank)
    release, lfo = [], []
    for index, p in enumerate(patches):
        tone, switches = p["sysex"][5:21], p["sysex"][21:23]
        octave = {1: 0.5, 2: 1.0, 4: 2.0}.get(switches[0] & 7, 1.0)
        for key, on, off in p["notes"][-5:]:
            falls, swings = [], []
            for audio, rate, shift, _, _ in sources:
                env, hop = envelope_db(audio[int((on + shift - 0.3) * rate):int((off + shift + 2.5) * rate)], rate,
                                       hop=0.001, window=0.008)
                start = onset(env, hop, 0.3) or 0.3
                up = int((start + off - on) / hop)
                tail = env[up:] - env[up - 60:up - 10].mean()
                falls.append([np.argmax(tail <= -drop) * hop if (tail <= -drop).any() else 0.0 for drop in (20, 30)])
                if tone[2] >= 8 and not tone[4]:
                    begin = int((on + shift + start - 0.3) * rate)
                    swings.append(vibrato(audio[begin:begin + int((off - on) * rate)].mean(1), rate,
                                          440 * 2 ** ((key - 69) / 12) * octave))
            if not switches[1] & 4 and tone[13] >= 30:
                release += [math.log2(h / m) for h, m in zip(*falls) if min(h, m) >= 0.1]
            if swings and min(s[1] for s in swings) >= 3 and min(s[2] for s in swings) >= 1.5:
                lfo.append((bank, index, swings[0][0] / swings[1][0], swings[0][1] / swings[1][1]))
    return release, lfo


def run_timing(args):
    with concurrent.futures.ProcessPoolExecutor(args.jobs) as pool:
        results = list(pool.map(timing_bank, [(args, bank) for bank in BANKS]))
    release = np.array([r for rel, _ in results for r in rel])
    notes = [n for _, lfo in results for n in lfo]
    patches = sorted({n[:2] for n in notes})
    rate = np.log2([np.median([n[2] for n in notes if n[:2] == p]) for p in patches])
    depth = np.log2([np.median([n[3] for n in notes if n[:2] == p]) for p in patches])

    def ratio(logs):
        return dict(n=len(logs), hardware_over_model_median=float(2 ** np.median(logs)),
                    hardware_over_model_mean=float(2 ** logs.mean()),
                    standard_error=float(2 ** standard_error(logs) - 1))

    return dict(release_fall_20_30_db=ratio(release), lfo_rate_per_patch=ratio(rate), lfo_depth_per_patch=ratio(depth))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("export", "sweep", "hiss", "thump", "balance", "comb", "timing"))
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
    result = dict(sweep=run_sweep, hiss=run_hiss, thump=run_thump, balance=run_balance, comb=run_comb,
                  timing=run_timing)[args.command](args)
    print(json.dumps(result if args.command != "sweep" else result["hardware_minus_model"], indent=1))
    if args.out:
        args.out.write_text(json.dumps(result, indent=1))


if __name__ == "__main__":
    main()
