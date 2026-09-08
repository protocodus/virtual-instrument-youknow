#!/usr/bin/env python3
"""Measure original-DCO PWM duty and modulation without fitting DSP constants.

Source: Lewis Francis's April 22 2026 Juno-106 #439522 recording, original
MC5534A DCOs with Borish replacement VCF/VCA cards. Exact input MIDI is
https://kayrock.org/kr106/pwm_test.mid; capture is
https://lewisfrancis.com/nwio/pwm_test_bip.aif, supplied at
https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/44#issuecomment-4296833083

The bank contains manual PWM0..105 every .25s from 7s, then LFO byte30 at
depth0/21/42/63/84/105 every4s from33.5s, MIDI55 at16', chorusOff. Use the
original192kHz PCM and actual 48kHz/4x shipping-engine before/after renders.
Derivative edge peaks, quadratically interpolated, estimate pulse high time
relative to adjacent reset edges. Common capture gain/DC offset cancel;
unequal edge filtering and custom-IC asymmetry do not necessarily cancel.

Manual codes0/21/63/105 characterize each recording's DC slope independently.
No DSP value is fitted. The remaining manual codes check that affine summary.
Dynamic depths21/63 are diagnostics;42/84/105 are held-out controls. Each
modulation span is divided by that recording's manual DC slope times depth,
separating static service calibration from dynamic attenuation. Both total
range and1..99percentile range are reported, plus H3/H1 and independent LFO
frequency estimates. The latter are descriptive, not time warps. Early/late
subwindows report repeatability; one capture cannot identify population
tolerances or uniquely fit both control poles behind a ~98Hz pulse carrier.

python3 Tools/AnalyzeHardwarePwm.py --reference pwm-test.wav \
    --before pwm-before.wav --after pwm-after.wav --output report.json
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from scipy.optimize import minimize_scalar
from scipy.signal import find_peaks

from AnalyzeHardwareCalibration import load_audio, sha256

PCM_SHA256 = "3171057c4310544400e5f843fd661270af8ae08c858d9800a33c5c1ea30f825e"
AIFF_SHA256 = "90dc997ca41ca66552c8bc4a73b1b2e63af4099432e068a54d73a6700b23d006"
MIDI_SHA256 = "affcd71c233c4e03616f2037ef2fc66a0c93c0eb957a4413cf09bde42e78fff2"
MANUAL_TRAIN = {0, 21, 63, 105}
DYNAMIC_HELD = {42, 84, 105}


def duty(rate, audio, begin, end):
    if begin < 0 or end*rate > len(audio):
        raise ValueError("pulse window outside recording")
    x = audio[round(begin*rate):round(end*rate), 0]
    delta = np.diff(x)
    def edges(sign):
        q = sign*delta
        peaks, _ = find_peaks(q, prominence=max(q)*.35, distance=rate/350)
        peaks = peaks[(peaks > 0) & (peaks < len(delta)-1)]
        curvature = q[peaks-1]-2*q[peaks]+q[peaks+1]
        correction = .5*(q[peaks-1]-q[peaks+1])/curvature
        return (peaks+correction)/rate+begin
    rising, falling = edges(1), edges(-1)
    j = np.searchsorted(falling, rising)
    good = (j > 0) & (j < len(falling))
    rising, j = rising[good], j[good]
    period = falling[j]-falling[j-1]
    width = (falling[j]-rising)/period
    good = (period > .009) & (period < .012) & (width > .45) & (width < .995)
    if good.sum() < .75*(end-begin)*98-3:
        raise ValueError("pulse edges are not identifiable at the expected ~98Hz carrier")
    return rising[good], width[good]


def modulation_harmonics(t, d):
    def fit(frequency):
        harmonics = np.arange(1, 8)
        phase = 2*np.pi*(t-t[0])[:, None]*frequency*harmonics
        design = np.c_[np.ones(len(t)), np.cos(phase), np.sin(phase)]
        coeff = np.linalg.lstsq(design, d, rcond=None)[0]
        return np.mean((d-design@coeff)**2), coeff
    grid = np.linspace(1.9, 2.6, 72)
    coarse = min(grid, key=lambda f: fit(f)[0])
    fitted = minimize_scalar(lambda f: fit(f)[0], bounds=(coarse-.02, coarse+.02),
                             method="bounded", options={"xatol": 1e-8})
    if not fitted.success or not 1.91 < fitted.x < 2.59:
        raise ValueError("LFO30 frequency not identifiable in the documented bracket")
    error, coeff = fit(fitted.x)
    magnitude = np.hypot(coeff[1:8], coeff[8:])
    return {"frequency_hz": fitted.x, "harmonic_amplitude": magnitude.tolist(),
            "h3_h1_db": float(20*np.log10(magnitude[2]/magnitude[0])),
            "projection_rms_residual_duty": float(np.sqrt(error))}


def span(d, normalization):
    return {"range_normalized": float(np.ptp(d)/normalization),
            "percentile_range_normalized": float(np.diff(np.quantile(d,[.01,.99]))[0]/normalization)}


def measure(rate, audio):
    manual = []
    for code in range(106):
        start = 7+.25*code
        _, d = duty(rate, audio, start+.09, start+.24)
        manual.append({"code": code, "median_duty": float(np.median(d)),
                       "cycle_standard_deviation": float(np.std(d)), "cycles": len(d)})
    training = [r for r in manual if r["code"] in MANUAL_TRAIN]
    matrix = np.array([[1, r["code"]] for r in training])
    intercept, slope = np.linalg.lstsq(matrix, [r["median_duty"] for r in training], rcond=None)[0]
    static_errors = [r["median_duty"]-intercept-slope*r["code"] for r in manual if r["code"] not in MANUAL_TRAIN]
    dynamic = []
    for i, code in enumerate([21,42,63,84,105], 1):
        start = 33.5+4*i
        t, d = duty(rate, audio, start+.2, start+3.9)
        row = {"code": code, "role": "held_out" if code in DYNAMIC_HELD else "diagnostic",
               "window_seconds": [start+.2,start+3.9], "cycles": len(d),
               **span(d,slope*code), **modulation_harmonics(t,d)}
        row["early"] = span(d[t<start+2],slope*code)
        row["late"] = span(d[t>=start+2],slope*code)
        dynamic.append(row)
    return {"manual": manual, "manual_intercept": float(intercept), "manual_slope_per_byte": float(slope),
            "manual_affine_heldout_rms_duty": float(np.sqrt(np.mean(np.square(static_errors)))),
            "dynamic": dynamic}


def comparison(reference, model):
    rows = []
    metrics = ["range_normalized", "percentile_range_normalized", "h3_h1_db", "frequency_hz"]
    for real, candidate in zip(reference["dynamic"],model["dynamic"]):
        row = {"code": real["code"], "role": real["role"]}
        row.update({key: candidate[key]-real[key] for key in metrics})
        rows.append(row)
    held = [r for r in rows if r["role"] == "held_out"]
    return {"model_minus_hardware": rows,
            "heldout_rms": {key: float(np.sqrt(np.mean([r[key]**2 for r in held]))) for key in metrics}}


def self_test():
    from scipy.signal import butter, sosfilt
    for rate in [48000,192000]:
        t = np.arange(rate)/rate
        for width in [.5,.7,.95]:
            x = np.where(np.mod(t*98.137+.12,1)<width,1.,-1.)
            x = sosfilt(butter(2,6000,fs=rate,output="sos"),x)
            a = np.c_[x,x]
            _, measured = duty(rate,a,.1,.9)
            _, changed = duty(rate,3.7*a+.24,.1,.9)
            assert abs(np.median(measured)-width)<.0012
            assert np.max(abs(measured-changed))<1e-12
    t = np.arange(400)/98.137
    d = .7+.15*np.cos(2*np.pi*2.234*t+.13)+.018*np.cos(6*np.pi*2.234*t+.42)
    r = modulation_harmonics(t,d)
    assert abs(r["frequency_hz"]-2.234)<2e-6
    assert abs(r["h3_h1_db"]-20*np.log10(.018/.15))<2e-4
    print("PASS: pulse duty under filtering/rate/gain/DC changes; independent modulation pitch and harmonic ratio")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test",action="store_true")
    for name in ["reference","before","after","output"]:
        parser.add_argument("--"+name,type=Path)
    args = parser.parse_args()
    if args.self_test:
        self_test(); return
    if not all([args.reference,args.before,args.after,args.output]):
        parser.error("reference,before,after,output are required")
    rate, audio = load_audio(args.reference)
    pcm = hashlib.sha256(np.ascontiguousarray(audio,dtype="<f8")).hexdigest()
    if pcm != PCM_SHA256 or rate != 192000:
        raise ValueError("unknown or resampled hardware PWM reference")
    result = {"reference": measure(rate,audio), "before": measure(*load_audio(args.before)),
              "after": measure(*load_audio(args.after))}
    result["before_comparison"] = comparison(result["reference"],result["before"])
    result["after_comparison"] = comparison(result["reference"],result["after"])
    result["provenance"] = {"unit": "Juno106 #439522 original DCOs; replacement Borish VCF/VCA cards",
        "reference_aiff_sha256": AIFF_SHA256, "reference_pcm_sha256": pcm, "midi_sha256": MIDI_SHA256,
        "before_sha256": sha256(args.before), "after_sha256": sha256(args.after), "analyzer_sha256": sha256(__file__),
        "manual_nuisance_fit_codes": sorted(MANUAL_TRAIN), "dynamic_heldout_codes": sorted(DYNAMIC_HELD),
        "fitted_dsp_parameters": [], "limitations": [
            "One unit; downstream unequal edge response can bias inferred duty.",
            "~98Hz carrier samples a ~2.23Hz PWM trajectory; finite cycle sampling limits extrema accuracy.",
            "Independent frequency estimation describes rate differences; no waveform time warp or phase alignment.",
            "Manual DC slope/intercept characterize static service differences only; they do not change either engine render.",
            "These recordings cannot uniquely identify both RC poles or the installed VR31 position."]}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(result,indent=2,allow_nan=False)+"\n")
    print(json.dumps({key:result[key]["heldout_rms"] for key in ["before_comparison","after_comparison"]},indent=2))


if __name__ == "__main__":
    main()
