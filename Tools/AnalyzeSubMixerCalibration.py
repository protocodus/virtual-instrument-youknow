#!/usr/bin/env python3
"""Identify the SUB diode law from the hash-pinned osc_test hardware sweep.

The original MC5534A DCOs are in Juno-106 #439522; the VCF/VCA cards are Borish
replacements. Fixed gain/patch within this recording makes a relative source
control sweep useful; it does not establish original-card absolute drive,
WAVE impedance or population tolerances. The source MIDI repeats MIDI60 at
SUB bytes 0,13,25,38,51,64,76,89,102,114,127 during seconds8..30.

Use the existing lossless AIFF->WAV conversion; no resampling/normalization.
Fit one physically derived aggregate S using bytes25/51/76/102 only, with the
nominal silicon slope26mV fixed. Normalize at127 (an endpoint anchor, NOT a
held-out test). Validate13/38/64/89/114 on actual separately rendered WAVs.
Windows start0.5s after patch and end1.8s after it. Their duration leaves both
edges out; frequency is fitted independently at each nonzero byte so the
hardware tuning difference is not mistaken for an amplitude error.

python3 Tools/AnalyzeSubMixerCalibration.py --reference osc-test.wav \
    --before baseline.wav --after candidate.wav --output comparison.json
"""
import argparse
import hashlib
import json
import math
from pathlib import Path

import numpy as np
from scipy.optimize import brentq, minimize_scalar

from AnalyzeHardwareCalibration import common_band_rms, db, load_audio, sha256

PCM_SHA256 = "e756ef938fb8b17f77357281b64cbf115bac58f5e13cb7318d2e2d562ffe57f3"
AIFF_SHA256 = "b0309cd365e3025fce993d409be05b3d39cbc51daac114dbfe0428d33e39093f"
MIDI_SHA256 = "051ea38ee992b7fe49b8356be9395830706e7c81099d7ab4653ca0b470ad2d43"
CODES = np.array([0, 13, 25, 38, 51, 64, 76, 89, 102, 114, 127])
TRAIN = {25, 51, 76, 102}
HELD = {13, 38, 64, 89, 114}
RAIL = 10 * 4064 / 4096
THERMAL = 0.026


def fundamental(samples, rate):
    weights = np.hanning(len(samples))
    weighted = (samples - np.mean(samples)) * weights
    times = np.arange(len(samples)) / rate

    def amplitude(frequency):
        return 2 * abs(np.dot(weighted, np.exp(-2j * np.pi * frequency * times))) / weights.sum()

    fit = minimize_scalar(lambda f: -amplitude(f), bounds=(64, 66),
                          method="bounded", options={"xatol": 1e-8})
    if not fit.success or not 64.05 < fit.x < 65.95:
        raise ValueError("sub fundamental not identifiable within the recorded note's bracket")
    return float(fit.x), float(amplitude(fit.x))


def measure(rate, audio):
    if len(audio) < 30 * rate:
        raise ValueError("recording does not contain the complete sub sweep")
    result = []
    for index, code in enumerate(CODES):
        start = 8 + 2 * index
        window = audio[round((start + .5) * rate):round((start + 1.8) * rate), 0]
        if code:
            frequency, amplitude = fundamental(window, rate)
            # Independent subwindows describe local repeatability only.
            parts = [fundamental(part, rate)[1] for part in np.array_split(window, 3)]
            spread = db(max(parts) / min(parts))
        else:
            frequency, amplitude, spread = None, None, None
        result.append({"code": int(code), "fundamental_hz": frequency,
                       "fundamental_peak": amplitude,
                       "band_rms": common_band_rms(window, rate),
                       "subwindow_amplitude_range_db": spread,
                       "role": "fit" if code in TRAIN else "held_out" if code in HELD
                       else "endpoint" if code == 127 else "floor"})
    endpoint = result[-1]["fundamental_peak"]
    for row in result:
        row["relative_fundamental"] = row["fundamental_peak"] / endpoint if row["code"] else None
    return result


def fit_span(rows):
    # Vfull - V = S(1-j) - Vt ln(j). Only S is unknown; no generic polynomial,
    # estimated diode ideality, hidden gain or additional knee is optimized.
    train = [r for r in rows if r["code"] in TRAIN]
    j = np.array([r["relative_fundamental"] for r in train])
    v = RAIL * (1 - np.array([r["code"] for r in train]) / 127)
    return float(np.linalg.lstsq((1 - j)[:, None], v + THERMAL * np.log(j), rcond=None)[0][0])


def diode_gain(code, span):
    if code == 0:
        return 0.0
    if code == 127:
        return 1.0
    v = RAIL * (code / 127 - 1)
    return brentq(lambda j: span * (j - 1) + THERMAL * math.log(j) - v,
                  1e-30, 1, xtol=1e-16)


def comparison(reference, candidate):
    errors = []
    for real, model in zip(reference, candidate):
        if real["code"]:
            errors.append({"code": real["code"], "role": real["role"],
                           "model_minus_hardware_db": db(model["relative_fundamental"]
                                                           / real["relative_fundamental"])})
    return {"rows": errors,
            "held_out_max_abs_db": max(abs(r["model_minus_hardware_db"])
                                       for r in errors if r["code"] in HELD)}


def self_test():
    span = 8.9
    rows = [{"code": int(code), "relative_fundamental": diode_gain(code, span)}
            for code in CODES if code]
    assert abs(fit_span(rows) - span) < 1e-12
    for rate in [48000, 96000]:
        t = np.arange(round(1.3 * rate)) / rate
        for phase in [.17, 2.4]:
            x = .03 + .37 * np.sin(2 * np.pi * 65.137 * t + phase)
            x += .11 * np.sin(2 * np.pi * 3 * 65.137 * t + .3)
            frequency, amplitude = fundamental(x, rate)
            assert abs(frequency - 65.137) < 2e-5
            assert abs(db(amplitude / .37)) < 1e-4
    print("SUB calibration analyzer self-test passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--before", type=Path)
    parser.add_argument("--after", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if not all([args.reference, args.before, args.after, args.output]):
        parser.error("reference, before, after and output are required")
    rate, samples = load_audio(args.reference)
    pcm_hash = hashlib.sha256(np.ascontiguousarray(samples, dtype="<f8").tobytes()).hexdigest()
    if pcm_hash != PCM_SHA256 or rate != 96000:
        raise ValueError("reference is not the hash-pinned, original-rate osc_test recording")
    hardware = measure(rate, samples)
    before_rate, before_samples = load_audio(args.before)
    after_rate, after_samples = load_audio(args.after)
    before = measure(before_rate, before_samples)
    after = measure(after_rate, after_samples)
    span = fit_span(hardware)
    result = {
        "source_audio": "https://lewisfrancis.com/nwio/osc_test_bip.aif",
        "source_midi": "https://kayrock.org/kr106/osc_test.mid",
        "unit": "Juno-106 #439522, original DCOs, Borish replacement VCF/VCA cards",
        "scope": "One fixed-gain recording; unknown physical WAVE impedances and recording gain; no population inference.",
        "reference_pcm_sha256": pcm_hash, "original_aiff_sha256": AIFF_SHA256,
        "midi_sha256": MIDI_SHA256, "analyzer_sha256": sha256(__file__),
        "before_sha256": sha256(args.before), "after_sha256": sha256(args.after),
        "fitted_series_span_volts": span, "fixed_diode_slope_volts": THERMAL,
        "fit_codes": sorted(TRAIN), "held_out_codes": sorted(HELD), "normalization_anchor": 127,
        "hardware": hardware,
        "analytic_held_out_db": [{"code": r["code"], "error_db": db(diode_gain(r["code"], span)
                                               / r["relative_fundamental"])}
                                 for r in hardware if r["code"] in HELD],
        "before": comparison(hardware, before), "after": comparison(hardware, after),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"series_span_volts": span,
                      "before_held_out_max_abs_db": result["before"]["held_out_max_abs_db"],
                      "after_held_out_max_abs_db": result["after"]["held_out_max_abs_db"]}, indent=2))


if __name__ == "__main__":
    main()
