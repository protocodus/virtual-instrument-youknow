#!/usr/bin/env python3
"""Reproduce the serviced #439522 six-card VCF comparison (NumPy/SciPy).

Primary source: https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/44
comments 4359218903 (192 kHz six-card full-sweep request) and 4359681843
(capture); issue16 identifies Borish replacement cards, serviced in 2022.
Audio: https://www.lewisfrancis.com/nwio/vcf_fullsweep_bip.aif
MIDI: https://kayrock.org/kr106/vcf_fullsweep.mid
Export the exact MIDI with AnalyzeHardwareCalibration.py, then render using
YouKnowRenderCalibrationEvents EVENTS MODEL.wav 0 shipping 1 nominal
fixed-service serviced439522 192000 4 rotary6. The engine caps requested 4x
to actual 1x at 192 kHz; this is a native 192 kHz shipping-kernel comparison.

extract measures each steady code in the middle 25..90% of its dwell, excluding
initial/final ENV-depth127 patches. Physical slots follow the six note spans
of the instructed rotary test; there is ONE sweep per card, alternating
direction. Thus direction/time are confounded with card identity. Odd sorted
code indices are held out BEFORE fitting; this tests interpolation within the
same take, not independent repeated hardware or original-card population.

fit estimates only FREQ intercept, WIDTH slope and effective current ceiling.
The existing saturation exponent1.7 and three measured DAC carry increments
are FIXED; carry converts cents to nominal DAC counts BEFORE the fitted slope,
exactly as in Engine.cpp. The 4..8 Hz /120..145 cents-byte /64.8..72.9 kHz
pole-ceiling bounds are explicit model plausibility constraints. The latter
spans historical 240/270 pF model alternatives, not measured Borish tolerances
(270 pF is clone evidence, not original80017A evidence). No part value is
identified individually. --trim must be the shipping full-RES frequencyTrim.

compare evaluates ACTUAL rendered waveforms on complementary codes, including
byte127 above50 kHz. The 192 kHz source preserves those fundamentals. Verify
numerical frequency convergence separately at192/384 kHz before attributing
high-code error solely to the physical model. No resampling above code87.

resonance compares each noise spectrum with its own RES0 spectrum to cancel
fixed recording gain/input coloration, then compares these ratios to hardware.
Welch8 Hz bins,100..3000 Hz; alternating steps are reported separately without
fitting any parameter. Peak position is a secondary noisy spectral statistic.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

import numpy as np
from scipy.io import wavfile
from scipy.optimize import least_squares
from scipy.signal import periodogram, resample_poly, welch


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


class Audio:
    """Selective PCM AIFF access keeps the 640 MiB capture out of RAM."""
    def __init__(self, path):
        self.path = str(path)
        self.file = open(path, "rb")
        header = self.file.read(12)
        self.aiff = header[:4] == b"FORM" and header[8:] == b"AIFF"
        if self.aiff:
            self.offset = None
            self.rate = None
            while chunk := self.file.read(8):
                if len(chunk) != 8:
                    raise ValueError("truncated AIFF chunk")
                kind, size = struct.unpack(">4sI", chunk)
                start = self.file.tell()
                if kind == b"COMM":
                    data = self.file.read(18)
                    self.channels, self.frames, bits = struct.unpack(">hIh", data[:8])
                    exponent, mantissa = struct.unpack(">HQ", data[8:])
                    self.rate = int(round(math.ldexp(mantissa, (exponent & 32767) - 16383 - 63)))
                    if exponent & 32768 or bits != 24:
                        raise ValueError("requires positive-rate uncompressed24-bit AIFF")
                elif kind == b"SSND":
                    offset, _ = struct.unpack(">II", self.file.read(8))
                    self.offset = self.file.tell() + offset
                self.file.seek(start + size + (size & 1))
            if self.offset is None or self.rate is None:
                raise ValueError("missing AIFF audio or format")
        else:
            self.file.close()
            self.rate, self.data = wavfile.read(path, mmap=True)
            self.frames = len(self.data)
            self.channels = self.data.shape[1] if self.data.ndim == 2 else 1

    def read(self, start, end):
        first, last = round(start * self.rate), round(end * self.rate)
        if not 0 <= first < last <= self.frames:
            raise ValueError(f"invalid window {start}..{end} for {self.path}")
        if self.aiff:
            self.file.seek(self.offset + first * self.channels * 3)
            data = np.frombuffer(self.file.read((last-first)*self.channels*3), np.uint8)
            data = data.reshape(-1, self.channels, 3).astype(np.int32)
            value = (data[:, :, 0] << 16) | (data[:, :, 1] << 8) | data[:, :, 2]
            return ((value ^ 8388608) - 8388608).astype(float) / 8388608
        data = self.data[first:last]
        if data.ndim == 1:
            data = data[:, None]
        scale = 1 if data.dtype.kind == "f" else 2 ** (data.dtype.itemsize * 8 - 1)
        return data.astype(float) / scale


def events(path):
    return [(float(t), bytes.fromhex(h)) for t, h in
            (line.split() for line in Path(path).read_text().splitlines() if line.strip())]


def windows(path):
    midi = events(path)
    notes, active = [], None
    for time, data in midi:
        if data[0] & 240 == 144 and data[2]:
            if active is not None:
                raise ValueError("rotary fixture requires non-overlapping notes")
            active = time
        elif data[0] & 240 == 128 or (data[0] & 240 == 144 and not data[2]):
            if active is not None:
                notes.append((active, time))
                active = None
    if len(notes) != 6:
        raise ValueError("fullsweep must contain exactly six rotary notes")
    patches = [(time, data) for time, data in midi if len(data) == 24]
    rows = []
    for (time, data), (next_time, _) in zip(patches, patches[1:]):
        if data[12] != 0:
            continue
        code = data[10]
        duration = min(next_time-time, .25 if code > 95 else (8 if code < 16 else 4))
        start, end = time + duration*.25, time + duration*.9
        slots = [i for i, (on, off) in enumerate(notes) if on <= start < end <= off]
        if len(slots) != 1:
            raise ValueError(f"steady window does not belong to one card: {start}..{end}")
        rows.append(dict(voice=slots[0], code=code, time=time, start=start, end=end))
    if [sum(row["voice"] == slot for row in rows) for slot in range(6)] != [92]*6:
        raise ValueError("incomplete six-card,92-code capture")
    return rows


def frequency(signal, rate):
    nfft = 1 << (len(signal)*4-1).bit_length()
    frequencies, power = periodogram(signal, rate, window="hann", nfft=nfft)
    allowed = (frequencies >= 2) & (frequencies < rate*.45)
    index = np.flatnonzero(allowed)[np.argmax(power[allowed])]
    q = np.log(np.maximum(power[index-1:index+2], 1e-300))
    delta = .5*(q[0]-q[2])/(q[0]-2*q[1]+q[2])
    peak = (index+delta)*rate/nfft
    band = (frequencies > peak*.95) & (frequencies < peak*1.05)
    return float(peak), float(power[band].sum()/power.sum())


def extract(path, event_path):
    audio = Audio(path)
    if audio.rate < 192000:
        raise ValueError("fullsweep extraction requires native>=192 kHz to retain ultrasonic fundamentals")
    rows = windows(event_path)
    for row in rows:
        data = audio.read(row["start"], row["end"])[:, 0]
        factor = 64 if row["code"] < 64 else (8 if row["code"] < 88 else 1)
        signal = resample_poly(data, 1, factor) if factor > 1 else data
        hz, fraction = frequency(signal, audio.rate/factor)
        row.update(frequency_hz=hz, peak_fraction=fraction, rms=float(np.std(data)))
    return dict(audio=str(Path(path).resolve()), audio_sha256=sha256(path),
                events_sha256=sha256(event_path), sample_rate=audio.rate, rows=rows)


def model(parameters, codes):
    base, slope, ceiling = parameters
    carry = (codes >= 32)*(-4.64) + (codes >= 64)*23.31 + (codes >= 96)*(-4.48)
    counts = codes*128 + carry*(1143/1200)
    raw = base * np.exp2(counts*slope/(128*1200))
    return raw / np.power(1 + np.power(raw/ceiling, 1.7), 1/1.7)


def split(rows, voice):
    ordered = sorted((r for r in rows if r["voice"] == voice), key=lambda r: r["code"])
    return ordered, np.arange(len(ordered)) % 2 == 0


def error_stats(error):
    return dict(rms_cents=float(np.sqrt(np.mean(error**2))), max_abs_cents=float(np.max(np.abs(error))))


def fit(measurement, trim):
    if not np.isfinite(trim) or trim <= 0:
        raise ValueError("trim must be finite and positive")
    result = []
    for voice in range(6):
        rows, training = split(measurement["rows"], voice)
        codes = np.array([r["code"] for r in rows])
        measured = np.array([r["frequency_hz"] for r in rows])
        optimum = least_squares(lambda p: 1200*np.log2(model(p, codes[training])/measured[training]),
                                [5.8, 133., 70000/trim],
                                bounds=([4,120,64800/trim], [8,145,72900/trim]),
                                xtol=1e-12, ftol=1e-12, gtol=1e-12)
        error = 1200*np.log2(model(optimum.x, codes)/measured)
        result.append(dict(voice=voice, parameters=optimum.x.tolist(),
                           training=error_stats(error[training]), heldout=error_stats(error[~training]),
                           rows=[dict(r, training=bool(t), error_cents=float(e))
                                 for r,t,e in zip(rows,training,error)]))
    return dict(source_sha256=measurement["audio_sha256"], trim=trim,
                validation="complementary-code interpolation within one take; no repeated cards", cards=result)


def compare(reference, candidate):
    if reference["events_sha256"] != candidate["events_sha256"]:
        raise ValueError("source and model must follow identical MIDI")
    result = []
    for voice in range(6):
        rows, training = split(reference["rows"], voice)
        rendered, _ = split(candidate["rows"], voice)
        if [r["code"] for r in rows] != [r["code"] for r in rendered]:
            raise ValueError("source and model code sets differ")
        error = 1200*np.log2(np.array([r["frequency_hz"] for r in rendered]) /
                            np.array([r["frequency_hz"] for r in rows]))
        result.append(dict(voice=voice, training=error_stats(error[training]),
                           heldout=error_stats(error[~training]),
                           rows=[dict(r, training=bool(t), model_hz=m["frequency_hz"], error_cents=float(e))
                                 for r,m,t,e in zip(rows,rendered,training,error)]))
    return dict(reference_sha256=reference["audio_sha256"], candidate_sha256=candidate["audio_sha256"],
                sample_rate=candidate["sample_rate"], cards=result)


def resonance(reference_path, model_path, event_path):
    steps = [(t, data[11]) for t,data in events(event_path) if len(data) == 24]
    spectra = []
    for path in (reference_path, model_path):
        audio, power = Audio(path), []
        for time, _ in steps:
            signal = audio.read(time+.35, time+1.85).mean(axis=1)
            f,p = welch(signal, audio.rate, nperseg=audio.rate//8, noverlap=audio.rate//16)
            power.append(p)
        power = np.array(power)
        band = (f >= 100) & (f <= 3000)
        peak_band = (f >= 400) & (f <= 1200)
        spectra.append((10*np.log10(power[:,band]/power[0,band]),
                        f[peak_band][np.argmax(power[:,peak_band], axis=1)]))
    error = spectra[1][0]-spectra[0][0]
    peak_error = 1200*np.log2(spectra[1][1]/spectra[0][1])
    heldout = np.arange(len(steps)) % 2 == 1
    strong = np.array([code >= 48 for _,code in steps])
    return dict(reference_sha256=sha256(reference_path), candidate_sha256=sha256(model_path),
                training_ratio_rms_db=float(np.sqrt(np.mean(error[(~heldout)&(np.arange(len(steps))>0)]**2))),
                heldout_ratio_rms_db=float(np.sqrt(np.mean(error[heldout]**2))),
                heldout_peak_rms_cents_res48up=float(np.sqrt(np.mean(peak_error[heldout&strong]**2))),
                rows=[dict(code=code, training=not bool(h), ratio_rms_db=float(np.sqrt(np.mean(e**2))),
                           peak_error_cents=float(p)) for (_,code),h,e,p in zip(steps,heldout,error,peak_error)])


def self_test():
    for rate in (192000,384000):
        for hz in (5.12,249.8,51054.0):
            seconds = 5 if hz < 10 else .2
            signal = np.sin(2*np.pi*hz*np.arange(round(rate*seconds))/rate)
            measured, _ = frequency(signal,rate)
            assert abs(1200*np.log2(measured/hz)) < .15, (rate,hz,measured)
    codes = np.arange(128)
    values = model([5.8,133,63000],codes)
    assert np.all(np.diff(values) > 0)
    assert values[-1] > 50000
    # Parameter recovery uses the training grid only; held-out observations
    # are deliberately corrupted so accidentally fitting them fails loudly.
    measured = dict(audio_sha256="synthetic", rows=[])
    truth = np.array([5.8,133.,63000.])
    for voice in range(6):
        for code in range(128):
            hz = model(truth,np.array([code]))[0]
            measured["rows"].append(dict(voice=voice,code=code,
                frequency_hz=float(hz*(1.25 if code % 2 else 1))))
    recovered = fit(measured,1.1227132081985474)
    assert all(np.max(np.abs(np.array(card["parameters"])/truth-1)) < 1e-7
               for card in recovered["cards"])
    print("voice-card analyzer: sample-rate/ultrasonic-frequency and monotonic-law checks passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("extract"); p.add_argument("audio"); p.add_argument("events"); p.add_argument("output")
    p = sub.add_parser("fit"); p.add_argument("measurement"); p.add_argument("output"); p.add_argument("--trim",type=float,required=True)
    p = sub.add_parser("compare"); p.add_argument("reference"); p.add_argument("candidate"); p.add_argument("output")
    p = sub.add_parser("resonance"); p.add_argument("reference"); p.add_argument("candidate"); p.add_argument("events"); p.add_argument("output")
    sub.add_parser("self-test")
    args = parser.parse_args()
    read = lambda path: json.loads(Path(path).read_text())
    if args.command == "self-test":
        self_test(); return
    if args.command == "extract": result = extract(args.audio,args.events)
    elif args.command == "fit": result = fit(read(args.measurement),args.trim)
    elif args.command == "compare": result = compare(read(args.reference),read(args.candidate))
    else: result = resonance(args.reference,args.candidate,args.events)
    Path(args.output).write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps({k:v for k,v in result.items() if k not in ("rows","cards")}))
    for card in result.get("cards",[]):
        print(json.dumps({k:v for k,v in card.items() if k != "rows"}))


if __name__ == "__main__":
    main()
