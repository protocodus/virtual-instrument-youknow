#!/usr/bin/env python3
"""Four questions put to the #439522 captures already in hand, against the engine's render of each take.

The 2026-11-16 known-gaps research (Docs/research/known-gaps-research-2026-11-16/, combined report
section 4, "Session 0") named four shipped assumptions that existing recordings can test without new
hardware time. Each subcommand measures the capture and the engine's render of the same MIDI by one
protocol, so estimator bias is shared, and reports both.

  sidebands  Mains ripple and converter-pass modulation of the cutoff. vcf_fullsweep (192 kHz, six
             cards, RES 127) self-oscillates at each cutoff code's tone. On each steady window of
             codes 16-95 (AnalyzeVoiceCardCalibration.windows: 25-90 % of the dwell) the tone is
             demodulated (demodulate): its frequency in cents and its envelope as a fraction, each
             as a modulation spectrum averaged over the windows of one tone band (2-6 kHz, above
             6 kHz; lower tones beat with the DCO's feedthrough). Lines are read at 60, 120 and 180 Hz, at the firmware pass
             (225-245 Hz: 4.2 ms nominal; 4.231-4.304 ms from KR-106's LFO rates on this unit,
             README OQ-08) and at twice it (modulation_line). Frequency modulation of the cutoff by
             a fixed fraction reads the same in cents in every band; a line whose cents fall with
             the tone is additive. The shipped rail ripple (enableRailRipple) is 0.003 cents peak at
             120 Hz under Unit Character and nothing at character 0.
  hum        Where the idle mains lines enter. In every idle window of the sixteen bank captures
             (AnalyzeChorusIdleFloors.patches; RELEASE <= 40, NOISE 0), each line k x f_mains for
             k = 1..6, f_mains fitted per window from the 60/120 Hz lines within 59.7-60.3 Hz: its
             power from a least-squares sine at that frequency per channel, and its L/R cross power
             accumulated over all windows of a chorus state, giving magnitude-squared coherence and
             phase. The same fit 5 and 7 Hz either side measures the broadband floor under each line;
             a line counts only where its pooled power stands 6 dB over that floor's, since the chorus
             raises the idle floor and a least-squares sine always finds some power. Levels are dBFS of the capture and dB under each patch's own C4 note (Hann
             periodogram, AnalyzeChorusIdleFloors.levels), which cancels the recording gain.
  noise      The noise source's amplitude distribution. NOISE-only windows: osc_test (96 kHz, NOISE
             bytes 102/114/127, 0.5-1.8 s after each patch) and the isolator take's noise windows
             (192 kHz, 7.0-8.3 s). Capture and render are band-passed alike (4th-order Butterworth,
             200 Hz-12 kHz, zero phase) and the capture resampled to the render's rate; skewness,
             excess kurtosis and the 0.1/1/99/99.9 % quantiles in units of the standard deviation,
             each with a 20-block standard error. A Gaussian source gives 0 and -/+3.09/2.33.
  pitch      Common DCO pitch against elapsed time. In every chorus-off, DCO-LFO-0 patch of the sixteen
             banks, each C1-C5 note 0.2 s after its onset to 0.05 s before its release: the
             fundamental by Hann periodogram and log-parabolic interpolation, searched within 60 cents
             of the render's. The render uses the fixed nominal 8 MHz clock, settled, so hardware minus
             render is the unit's own clock offset (MASTER TUNE included) and its drift. Elapsed time
             is each AIFF's archive timestamp minus its duration plus the time in the file; the
             timestamps are the recorder's file times, a proxy for the recording's end, not a log.

Limits. One serviced unit: original DCO chips, chorus board and converters, Borish replacement VCF/VCA
cards. Its recording chain has its own floor, hum and high-pass; nothing here separates the chain from
the instrument except what the chorus-state contrast does. None of these takes was made for these
questions, and a capture that does not show an effect bounds it only to its own floor.

Sources (hash-checked): https://www.lewisfrancis.com/nwio/vcf_fullsweep_bip.aif with
https://kayrock.org/kr106/vcf_fullsweep.mid; https://www.lewisfrancis.com/nwio/Juno-10-test-audio-96K.zip
with https://kayrock.org/kr106/106_calibration.zip; https://lewisfrancis.com/nwio/osc_test_bip.aif with
https://kayrock.org/kr106/osc_test.mid; https://lewisfrancis.com/nwio/osc_calibrate_new_bip.aif with
https://kayrock.org/kr106/osc_calibrate_new.mid.

  python3 Tools/AnalyzeExistingCaptures.py sidebands --capture vcf_fullsweep_bip.aif \\
      --events fullsweep-events.txt [--model fullsweep.wav] --out sidebands.json
  (fullsweep.wav: YouKnowRenderCalibrationEvents EVENTS fullsweep.wav 1 product 1 nominal
   fixed-service serviced439522 192000 4 rotary6, Unit Character so the rail ripple is on)
  python3 Tools/AnalyzeExistingCaptures.py hum --midi-zip 106_calibration.zip --captures BANKS --models RENDERS
  python3 Tools/AnalyzeExistingCaptures.py noise --osc-test osc_test_bip.aif --osc-test-events E1 \\
      --osc-test-model M1.wav --isolators osc_calibrate_new_bip.aif --isolators-model M2.wav
  python3 Tools/AnalyzeExistingCaptures.py pitch --midi-zip 106_calibration.zip --captures BANKS --models RENDERS
"""
import argparse
import datetime
import hashlib
import json
import math
import sys
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy.signal import butter, resample_poly, sosfiltfilt

sys.path.insert(0, str(Path(__file__).resolve().parent))
import AnalyzeChorusBankEvidence as bank_evidence  # noqa: E402
import AnalyzeChorusIdleFloors as floors  # noqa: E402
import AnalyzeVoiceCardCalibration as voice_cards  # noqa: E402
from AnalyzeHardwareCalibration import peak_measurement  # noqa: E402

OSC_TEST_AIFF_SHA256 = "b0309cd365e3025fce993d409be05b3d39cbc51daac114dbfe0428d33e39093f"
RIPPLE_CENTS = 0.003
# The firmware pass: 4.2 ms nominal (238 Hz), 4.231-4.304 ms from KR-106's LFO rates on this unit.
MODULATION_LINES = {"60 Hz": (59.5, 60.5), "120 Hz": (119.5, 120.5), "180 Hz": (179.5, 180.5),
                    "pass": (225.0, 245.0), "pass x2": (450.0, 490.0)}
DEMOD_RATE = 4000
DEMOD_FRAMES = 1152
LINE_HARMONICS = (1, 2, 3, 4, 5, 6)
NEIGHBOUR_OFFSETS_HZ = (-7.0, -5.0, 5.0, 7.0)


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def db10(x):
    return 10 * math.log10(max(float(x), 1e-300))


# ---------------------------------------------------------------- sidebands
def spectrum(segment, rate):
    segment = segment - segment.mean()
    nfft = 1 << (len(segment) - 1).bit_length()
    power = np.abs(np.fft.rfft(segment * np.hanning(len(segment)), nfft)) ** 2
    return power, rate / nfft


def demodulate(segment, rate):
    """The tone's frequency (cents) and envelope (fractional) modulation spectra, or None.

    The tone is the strongest bin 20 Hz-0.45 rate, used from 2 kHz: the analytic signal keeps
    f0 -/+ 700 Hz, and below 2 kHz that band holds enough of the switched-off 16' C4 DCO and
    sub's feedthrough harmonics (65.4 Hz apart) for their beats with the tone to read as lines in
    the capture and the render alike. Both traces are block
    averaged to DEMOD_RATE and cut into DEMOD_FRAMES Hann segments; each returned spectrum is the
    mean power over segments, scaled so a sinusoid of peak A reads A squared on its bin.
    """
    power, df = spectrum(segment, rate)
    usable = slice(int(20 / df), int(0.45 * rate / df))
    k0 = usable.start + int(np.argmax(power[usable]))
    f0 = k0 * df
    carrier_dbfs = db10(power[k0 - 3:k0 + 4].sum() / (np.hanning(len(segment)).sum() ** 2 / 2))
    if not (2000 <= f0 <= 20000) or carrier_dbfs < -60:
        return None
    n = len(segment)
    frequencies = np.fft.fftfreq(n, 1 / rate)
    keep = (frequencies > f0 - 700) & (frequencies < f0 + 700)
    analytic = np.fft.ifft(np.fft.fft(segment - segment.mean()) * keep * 2)
    trim = int(0.05 * n)
    instantaneous = np.diff(np.unwrap(np.angle(analytic)))[trim:-trim] * rate / (2 * np.pi)
    magnitude = np.abs(analytic)[trim:-trim - 1]
    step = int(round(rate / DEMOD_RATE))
    m = len(instantaneous) // step
    instantaneous = instantaneous[:m * step].reshape(m, step).mean(1)
    magnitude = magnitude[:m * step].reshape(m, step).mean(1)
    cents = 1200 * np.log2(instantaneous / instantaneous.mean())
    depth = magnitude / magnitude.mean() - 1
    if m < DEMOD_FRAMES:
        return None
    window = np.hanning(DEMOD_FRAMES)
    fm, am, parts = 0, 0, m // DEMOD_FRAMES
    for j in range(parts):
        for trace, name in ((cents, "fm"), (depth, "am")):
            part = trace[j * DEMOD_FRAMES:(j + 1) * DEMOD_FRAMES]
            line = (2 * np.abs(np.fft.rfft((part - part.mean()) * window)) / window.sum()) ** 2
            if name == "fm":
                fm = fm + line
            else:
                am = am + line
    return f0, fm / parts, am / parts, rate / step / DEMOD_FRAMES


def modulation_line(power, df, low, high):
    """Peak amplitude of the strongest line in [low, high] Hz over the floor 3-12 bins beside it.

    A Hann line spreads over three bins holding 1.5 times its squared peak; the floor is the
    median there over ln 2 (the mean of a chi-squared-2 floor). Checked on synthetic FM tones
    (0.003-0.3 cents, 120-232 Hz) to within 2 % of the imposed deviation.
    """
    centre = (low + high) / 2
    grid = np.arange(len(power)) * df
    span = (grid >= min(low, centre - df)) & (grid <= max(high, centre + df))
    k = np.flatnonzero(span)[int(np.argmax(power[span]))]
    beside = (np.abs(grid - grid[k]) > 3 * df) & (np.abs(grid - grid[k]) < 12 * df)
    floor = float(np.median(power[beside])) / math.log(2)
    lobe = float(power[k - 1:k + 2].sum())
    hz = grid[k]
    if 0 < k < len(power) - 1 and min(power[k - 1:k + 2]) > 0:
        y0, y1, y2 = np.log(power[k - 1:k + 2])
        hz += df * 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2)
    return dict(hz=round(float(hz), 2), peak=float(math.sqrt(max(lobe - 3 * floor, 0) / 1.5)),
                floor=float(math.sqrt(3 * floor / 1.5)), over_floor_db=round(db10(lobe / (3 * floor)), 1),
                detected=bool(lobe > 4 * 3 * floor))


def ripple_sideband_dbc(f0, cents=RIPPLE_CENTS, modulation_hz=120.0):
    """Each first FM sideband of a tone at f0 modulated by `cents` peak at `modulation_hz`."""
    deviation = f0 * (2 ** (cents / 1200) - 1)
    return 20 * math.log10(deviation / modulation_hz / 2)


def run_sidebands(args):
    sources = [("hardware", args.capture)] + ([("model", args.model)] if args.model else [])
    rows = [r for r in voice_cards.windows(args.events) if 16 <= r["code"] <= 95]
    result = dict(protocol="sidebands", demod_rate=DEMOD_RATE, segment_frames=DEMOD_FRAMES,
                  ripple_prediction_cents=RIPPLE_CENTS,
                  ripple_prediction_sideband_dbc_at_1khz=round(ripple_sideband_dbc(1000.0), 1))
    for name, path in sources:
        audio = voice_cards.Audio(path)
        bands = {}
        for r in rows:
            data = audio.read(r["start"], r["end"])
            found = demodulate(np.asarray(data[:, 0] if data.ndim == 2 else data, np.float64), audio.rate)
            if found is None:
                continue
            f0, fm, am, df = found
            key = "2-6 kHz" if f0 < 6000 else "above 6 kHz"
            band = bands.setdefault(key, dict(windows=0, f0=[], fm=0, am=0, df=df))
            band["windows"] += 1
            band["f0"].append(f0)
            band["fm"] = band["fm"] + fm
            band["am"] = band["am"] + am
        summary = {}
        for key, band in bands.items():
            fm, am, df = band["fm"] / band["windows"], band["am"] / band["windows"], band["df"]
            summary[key] = dict(windows=band["windows"], f0_median_hz=round(float(np.median(band["f0"]))),
                                fm_cents={k: modulation_line(fm, df, *v) for k, v in MODULATION_LINES.items()},
                                am_depth={k: modulation_line(am, df, *v) for k, v in MODULATION_LINES.items()})
        result[name] = dict(source=str(Path(path).name), sha256=file_sha256(path), rate=audio.rate, bands=summary)
    return result


# ---------------------------------------------------------------- hum
def mains_frequency(segment, rate):
    t = np.arange(len(segment)) / rate
    best = None
    for f in np.arange(59.70, 60.30001, 0.005):
        power = 0.0
        for k in (1, 2):
            c, s = np.cos(2 * np.pi * k * f * t), np.sin(2 * np.pi * k * f * t)
            power += abs(segment @ c) ** 2 + abs(segment @ s) ** 2
        if best is None or power > best[0]:
            best = (power, f)
    return best[1]


def line_phasors(segment, rate, frequency):
    """Complex amplitude per channel of a sine at `frequency`, least squares with an offset."""
    t = np.arange(len(segment)) / rate
    basis = np.c_[np.cos(2 * np.pi * frequency * t), np.sin(2 * np.pi * frequency * t), np.ones_like(t)]
    coefficients, *_ = np.linalg.lstsq(basis, segment, rcond=None)
    return coefficients[0] - 1j * coefficients[1]


def run_hum(args):
    acc = {}
    for bank in bank_evidence.BANKS:
        _, _, sources = bank_evidence.load(args, bank)
        rows = floors.patches(floors.timeline(bank_evidence.midi(args.midi_zip, bank)),
                              len(sources[0][0]) / sources[0][1] - sources[0][2])
        for row in rows:
            if not row["idle_seconds"] or not row["note_seconds"] or row["release"] > 40 or row["noise"]:
                continue
            w, n = row["idle_seconds"], row["note_seconds"]
            if w[1] - w[0] < 0.5:
                continue
            for name, (audio, rate, shift, _, _) in zip(("hardware", "model"), sources):
                seg = audio[int((w[0] + shift) * rate):int((w[1] + shift) * rate)]
                seg = seg - seg.mean(0)
                mains = mains_frequency(seg.mean(1), rate) if name == "hardware" else 60.0
                c4 = np.array(floors.levels((rate, audio), [n[0] + shift, n[1] + shift], True))
                c4_db = 10 * np.log10(np.mean(10 ** (c4 / 10)))
                entry = acc.setdefault((row["chorus"], name), dict(windows=0, seconds=0.0, mains=[], lines={}))
                entry["windows"] += 1
                entry["seconds"] += len(seg) / rate
                entry["mains"].append(mains)
                for k in LINE_HARMONICS:
                    p = line_phasors(seg, rate, k * mains)
                    beside = np.mean([np.abs(line_phasors(seg, rate, k * mains + d)) ** 2
                                      for d in NEIGHBOUR_OFFSETS_HZ], 0)
                    line = entry["lines"].setdefault(k, dict(level_dbfs=[], re_c4_db=[], cross=0j, auto=np.zeros(2),
                                                             beside=np.zeros(2)))
                    rms = np.abs(p) / math.sqrt(2)
                    line["level_dbfs"].append((20 * np.log10(rms + 1e-15)).tolist())
                    line["re_c4_db"].append((20 * np.log10(rms + 1e-15) - c4_db).tolist())
                    weight = len(seg) / rate
                    line["cross"] += weight * p[0] * np.conj(p[1])
                    line["auto"] += weight * np.abs(p) ** 2
                    line["beside"] += weight * beside
    out = {}
    for (mode, name), entry in sorted(acc.items()):
        lines = {}
        for k, line in sorted(entry["lines"].items()):
            levels = np.array(line["level_dbfs"])
            relative = np.array(line["re_c4_db"])
            coherence = abs(line["cross"]) ** 2 / (line["auto"][0] * line["auto"][1])
            over = 10 * np.log10(line["auto"] / line["beside"])
            lines[str(k)] = dict(level_dbfs_median_lr=np.median(levels, 0).round(2).tolist(),
                                 re_c4_db_median_lr=np.median(relative, 0).round(2).tolist(),
                                 over_floor_db_lr=over.round(1).tolist(),
                                 line_lr=[bool(o > 6) for o in over],
                                 lr_coherence=round(float(coherence), 3),
                                 lr_phase_deg=round(float(np.degrees(np.angle(line["cross"]))), 1))
        out[f"{mode}/{name}"] = dict(windows=entry["windows"], seconds=round(entry["seconds"], 1),
                                     mains_hz_median=float(np.median(entry["mains"])), lines=lines)
    for name in ("hardware", "model"):
        for mode in ("I", "II"):
            if f"{mode}/{name}" in out and f"off/{name}" in out:
                on, off = out[f"{mode}/{name}"]["lines"], out[f"off/{name}"]["lines"]
                out[f"{mode}_minus_off/{name}_re_c4_db"] = {
                    k: (np.array(on[k]["re_c4_db_median_lr"]) - np.array(off[k]["re_c4_db_median_lr"])).round(2).tolist()
                    for k in on}
    return dict(protocol="hum", states=out)


# ---------------------------------------------------------------- noise
def distribution(x, blocks=20):
    def stats(v):
        v = v - v.mean()
        s = v.std()
        z = v / s
        return [float(np.mean(z ** 3)), float(np.mean(z ** 4) - 3)] + \
            [float(q) for q in np.quantile(z, [0.001, 0.01, 0.99, 0.999])]
    whole = stats(x)
    parts = np.array([stats(p) for p in np.array_split(x, blocks)])
    se = parts.std(0, ddof=1) / math.sqrt(blocks)
    names = ("skewness", "excess_kurtosis", "q0.1%", "q1%", "q99%", "q99.9%")
    return {n: dict(value=round(v, 4), se=round(float(e), 4)) for n, v, e in zip(names, whole, se)}


def band(x, rate):
    return sosfiltfilt(butter(4, [200, 12000], "bp", fs=rate, output="sos"), x)


def noise_windows_osc_test(events):
    selected = []
    for line in Path(events).read_text().splitlines():
        t, text = line.split()
        raw = bytes.fromhex(text)
        if raw[0] == 0xF0 and len(raw) == 24 and 30 <= float(t) <= 52 and raw[9] in (102, 114, 127):
            selected.append((raw[9], float(t) + 0.5, float(t) + 1.8))
    if [s[0] for s in selected] != [102, 114, 127]:
        raise SystemExit("osc_test events: expected the NOISE sweep's 102/114/127 patches at 30-52 s")
    return selected


def run_noise(args):
    result = dict(protocol="noise", band_hz=[200, 12000], takes={})
    takes = []
    if args.osc_test:
        if file_sha256(args.osc_test) != OSC_TEST_AIFF_SHA256:
            raise SystemExit("osc_test: unknown capture")
        takes += [(f"osc_test NOISE {b}", args.osc_test, args.osc_test_model, a, e)
                  for b, a, e in noise_windows_osc_test(args.osc_test_events)]
    if args.isolators:
        takes += [("isolators noise", args.isolators, args.isolators_model, 7.0, 8.3)]
    for label, capture, model, a, e in takes:
        rate, hardware = floors.read_aiff(capture) if str(capture).endswith(".aif") else floors.read_wav(capture)
        rendered, model_rate = sf.read(model, dtype="float64")
        entry = {}
        for name, audio, r in (("hardware", hardware, rate), ("model", rendered, model_rate)):
            seg = band(audio[int(a * r):int(e * r), 0], r)
            if name == "hardware" and r != model_rate:
                g = math.gcd(int(r), int(model_rate))
                seg = resample_poly(seg, int(model_rate) // g, int(r) // g)
            trim = int(0.05 * model_rate)
            entry[name] = distribution(seg[trim:-trim])
            entry[name]["rms_dbfs"] = round(20 * math.log10(float(np.std(seg[trim:-trim])) + 1e-15), 2)
        result["takes"][label] = entry
    return result


# ---------------------------------------------------------------- pitch
def zip_times(zip_path):
    import zipfile
    times = {}
    for info in zipfile.ZipFile(zip_path).infolist():
        name = Path(info.filename).name
        if name.startswith("bank_") and name.endswith("_bip.aif") and "__MACOSX" not in info.filename:
            times[name[5:8]] = datetime.datetime(*info.date_time).timestamp()
    return times


def run_pitch(args):
    if args.captures_zip:
        stamps = zip_times(args.captures_zip)
    else:
        stamps = {b: (args.captures / f"bank_{b}_bip.aif").stat().st_mtime for b in bank_evidence.BANKS}
    start = None
    rows = []
    for bank in sorted(bank_evidence.BANKS, key=lambda b: stamps[b]):
        event_list, patches, sources = bank_evidence.load(args, bank)
        (hw, hw_rate, hw_shift, _, _), (model, model_rate, model_shift, _, _) = sources
        file_start = stamps[bank] - len(hw) / hw_rate
        start = file_start if start is None else start
        for p in patches:
            dump = p["sysex"]
            tone = dump[5:21]
            if bank_evidence.chorus_of(dump) != "off" or tone[2] or tone[4]:
                continue
            for key, on, off in p["notes"][-5:]:
                if off - on < 0.5:
                    continue
                a, b = on + 0.2, off - 0.05
                m = model[int((a + model_shift) * model_rate):int((b + model_shift) * model_rate)].mean(1)
                nominal = 440 * 2 ** ((key - 69) / 12)
                mf = peak_measurement(m, model_rate, low=nominal / 2.2, high=nominal * 4.4)["peak_hz"]
                h = hw[int((a + hw_shift) * hw_rate):int((b + hw_shift) * hw_rate)].mean(1)
                hf = peak_measurement(h, hw_rate, low=mf * 2 ** (-60 / 1200), high=mf * 2 ** (60 / 1200))["peak_hz"]
                rows.append(dict(bank=bank, patch=p["t"], key=key, model_hz=mf, hardware_hz=hf,
                                 cents=1200 * math.log2(hf / mf),
                                 minutes=(file_start - start + a + hw_shift) / 60))
    c = np.array([r["cents"] for r in rows])
    t = np.array([r["minutes"] for r in rows])
    keep = np.abs(c - np.median(c)) < 15
    slope, intercept = np.polyfit(t[keep], c[keep], 1)
    residual = c[keep] - (slope * t[keep] + intercept)
    se = float(np.std(residual, ddof=2) / math.sqrt(np.sum((t[keep] - t[keep].mean()) ** 2)))
    by_bank = {}
    for r in rows:
        by_bank.setdefault(r["bank"], []).append(r)
    return dict(protocol="pitch", notes=len(rows), kept_within_15_cents_of_median=int(keep.sum()),
                offset_cents_median=float(np.median(c)), span_minutes=float(t.max() - t.min()),
                drift_cents_per_minute=float(slope), drift_se=se,
                residual_rms_cents=float(np.sqrt(np.mean(residual ** 2))),
                banks=[dict(bank=b, minutes=round(float(np.mean([r["minutes"] for r in v])), 2),
                            notes=len(v), cents_median=round(float(np.median([r["cents"] for r in v])), 3))
                       for b, v in by_bank.items()],
                rows=rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("sidebands", "hum", "noise", "pitch"))
    parser.add_argument("--capture", type=Path)
    parser.add_argument("--events", type=Path)
    parser.add_argument("--model", type=Path)
    parser.add_argument("--midi-zip", type=Path)
    parser.add_argument("--captures", type=Path)
    parser.add_argument("--captures-zip", type=Path)
    parser.add_argument("--models", type=Path)
    parser.add_argument("--osc-test", type=Path)
    parser.add_argument("--osc-test-events", type=Path)
    parser.add_argument("--osc-test-model", type=Path)
    parser.add_argument("--isolators", type=Path)
    parser.add_argument("--isolators-model", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    result = dict(sidebands=run_sidebands, hum=run_hum, noise=run_noise, pitch=run_pitch)[args.command](args)
    text = json.dumps(result, indent=1)
    if args.out:
        args.out.write_text(text)
    summary = {k: v for k, v in result.items() if k != "rows"}
    for name in ("hardware", "model"):
        if isinstance(summary.get(name), dict):
            summary[name] = {k: v for k, v in summary[name].items() if k != "rows"}
    print(json.dumps(summary, indent=1)[:20000])


if __name__ == "__main__":
    main()
