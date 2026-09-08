#!/usr/bin/env python3
"""Fit one bounded shared-noise source scale using actual engine renders.

Protocol: fit equally weighted 20 Hz--20 kHz noise/saw and noise/true-selfosc
ratios on the first 0.55 s of three stable May-2026 isolator windows. Test the
later, disjoint 0.55 s without refitting. The 29-point resonance sweep and the
11-point NOISE-control sweep are independent recording/patch holdouts. Their
normalizations are within each file; the absent first hardware saw note is
never used. A scalar may improve absolute source ratios and resonance growth
while leaving a source-reference inconsistency: report all residuals.

No transfer shape, capture EQ, per-source gain or time warp is fitted. The
[1, 4] search interval is the engine comparison guard, not a Roland tolerance.
The source is Juno-106 #439522 with original DCOs and replacement Borish
VCF/VCA cards. Its VR32 trim, TP8 crest convention and recording gain are not
known. A fit to these outputs cannot establish an original 80017A's universal
noise level or replace Roland's 4 Vp-p TP8 service procedure.

Sources: https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/44
https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/16
The canonical PCM hashes below preserve correspondence after AIFF-to-WAV
conversion. Unknown PCM is rejected, rather than silently attributed.

Renderer contract: executable EVENTS OUTPUT CHARACTER shipping NOISE_SCALE,
with the existing full engine signal path at 48 kHz/4x. Its executable hash,
arguments, output hashes, source/event hashes and all windows are archived.
Audio A/B preserve source ratios: identical gain, deliberately no loudness
matching. Use --self-test without third-party captures or renderer access.
"""
import argparse
import hashlib
import json
import math
import platform
import shutil
import subprocess
from pathlib import Path

import numpy as np
import scipy
from scipy.optimize import brentq

from AnalyzeHardwareCalibration import common_band_rms, db, load_audio, sha256

PCM = {
    "isolators": "05d3817d991ba5178a41d9c063b6eefb0bb87cf15aff0a081835aa46a656ccee",
    "resonance": "d9ea17efd7112f56b7faa017cc04a1752b951152e9abeaa70f8973d4da50e68b",
    "noise_sweep": "e756ef938fb8b17f77357281b64cbf115bac58f5e13cb7318d2e2d562ffe57f3",
}
FIT_WINDOWS = {"saw": (1.0, 1.55), "noise": (7.0, 7.55), "selfosc": (9.0, 9.55)}
HELDOUT_WINDOWS = {"saw": (1.75, 2.30), "noise": (7.75, 8.30), "selfosc": (9.75, 10.30)}


def pcm_hash(audio):
    return hashlib.sha256(np.ascontiguousarray(audio, dtype="<f8")).hexdigest()


def checked_reference(path, kind):
    rate, audio = load_audio(path)
    digest = pcm_hash(audio)
    if digest != PCM[kind]:
        raise ValueError(f"{kind}: unknown reference PCM; cannot assert capture identity")
    return (rate, audio), {"path": str(path.resolve()), "sha256": sha256(path),
                          "pcm_float64le_sha256": digest, "sample_rate": rate,
                          "channels": audio.shape[1], "seconds": len(audio) / rate}


def band_level(recording, window):
    rate, audio = recording
    begin, end = (round(t * rate) for t in window)
    if begin < 0 or end > len(audio) or begin >= end:
        raise ValueError("measurement window is outside recording")
    return np.array([db(common_band_rms(audio[begin:end, c], rate))
                     for c in range(audio.shape[1])])


def ratios(recording, windows):
    levels = {name: band_level(recording, window) for name, window in windows.items()}
    return np.array([levels["noise"] - levels["saw"],
                     levels["noise"] - levels["selfosc"]])


def ratio_report(reference, model, windows):
    r, m = ratios(reference, windows), ratios(model, windows)
    if r.shape != m.shape:
        raise ValueError("reference/model channel counts differ")
    error = m - r
    return {"reference_db": r.tolist(), "model_db": m.tolist(),
            "error_db": error.tolist(), "rms_error_db": float(np.sqrt(np.mean(error ** 2))),
            "rows": ["noise/saw", "noise/true_selfosc"], "windows_seconds": windows}


def patches(events):
    # This protocol consumes the frozen 24-byte Manual dumps. A malformed or
    # different codec is not silently interpreted as the expected patch bank.
    result = {}
    previous = -math.inf
    for line in Path(events).read_text().splitlines():
        time, text = line.split()
        time = float(time)
        if not math.isfinite(time) or time < previous:
            raise ValueError("unordered/non-finite event time")
        previous = time
        raw = bytes.fromhex(text)
        if raw[0] == 0xf0:
            if len(raw) != 24 or raw[:3] != bytes.fromhex("f04131") or raw[-1] != 0xf7:
                raise ValueError("expected a 24-byte Roland Manual dump")
            result[time] = {"noise": raw[9], "cutoff": raw[10], "resonance": raw[11]}
    return result


def sweep_report(reference, model, selected, control, anchor):
    rows = []
    reference_anchor = band_level(reference, (anchor + .50, anchor + 1.80))
    model_anchor = band_level(model, (anchor + .50, anchor + 1.80))
    for time, patch in selected:
        window = (time + .50, time + 1.80)
        r = band_level(reference, window) - reference_anchor
        m = band_level(model, window) - model_anchor
        if r.shape != m.shape:
            raise ValueError("reference/model sweep channels differ")
        rows.append({"byte": patch[control], "window_seconds": window,
                     "reference_normalized_db": r.tolist(), "model_normalized_db": m.tolist(),
                     "error_db": (m - r).tolist()})
    # Exclude the noise-off floor when assessing a gain law; retain it as a row.
    usable = [row for row in rows if control != "noise" or row["byte"] > 0]
    values = np.array([row["error_db"] for row in usable])
    return {"rows": rows, "rms_error_db": float(np.sqrt(np.mean(values ** 2))),
            "anchor_window_seconds": [anchor + .5, anchor + 1.8],
            "excluded_from_rms": [0] if control == "noise" else []}


class Renderer:
    def __init__(self, executable, output, character):
        self.executable = executable.resolve()
        self.output = output
        self.character = character
        self.runs = []
        self.cache = {}

    def render(self, kind, events, scale):
        scale = float(np.float32(scale))
        key = (kind, scale)
        if key not in self.cache:
            path = self.output / f"{kind}-scale-{scale:.8g}.wav"
            if path.exists():
                raise ValueError(f"refusing to replace evidence: {path}")
            command = [str(self.executable), str(events.resolve()), str(path),
                       str(self.character), "shipping", f"{scale:.9g}"]
            done = subprocess.run(command, check=True, capture_output=True, text=True)
            self.runs.append({"command": command, "scale": scale, "output_sha256": sha256(path),
                              "stdout": done.stdout.strip()})
            self.cache[key] = (path, load_audio(path))
            print(f"rendered {kind} at {scale:.7g}", flush=True)
        return self.cache[key]


def run(args):
    if args.output.exists():
        raise ValueError("output directory must be new, preserving previous evidence")
    args.output.mkdir(parents=True)
    references = {}
    provenance = {}
    for kind in PCM:
        references[kind], provenance[kind] = checked_reference(getattr(args, f"reference_{kind}"), kind)
    event_paths = {name: getattr(args, f"events_{name}") for name in PCM}
    p_res = patches(event_paths["resonance"])
    selected_res = [(t, p) for t, p in p_res.items() if p["noise"] == 127 and p["cutoff"] == 64]
    if len(selected_res) != 29 or [p["resonance"] for _, p in selected_res][::28] != [0, 127]:
        raise ValueError("expected the dedicated 29-point resonance bank")
    p_noise = patches(event_paths["noise_sweep"])
    selected_noise = [(t, p) for t, p in p_noise.items() if 30 <= t <= 50]
    if [p["noise"] for _, p in selected_noise] != [0, 13, 25, 38, 51, 64, 76, 89, 102, 114, 127]:
        raise ValueError("expected the documented NOISE sweep at 30..52s")
    # Preserve all previous events, including silent hardware sections, so the
    # model's card allocation and warm-up are not reset around measurement.
    prefix = args.output / "noise-sweep-prefix-events.txt"
    prefix.write_text("".join(line + "\n" for line in event_paths["noise_sweep"].read_text().splitlines()
                              if float(line.split()[0]) <= 52))
    render = Renderer(args.renderer, args.output, args.character)
    history = []
    def objective(scale):
        _, model = render.render("isolators", event_paths["isolators"], scale)
        report = ratio_report(references["isolators"], model, FIT_WINDOWS)
        mean = float(np.mean(report["error_db"]))
        history.append({"scale": float(np.float32(scale)), "mean_error_db": mean,
                        "rms_error_db": report["rms_error_db"]})
        return mean
    # Balance the two log-ratio residuals with equal weight. Both share the
    # noise numerator; the complete render may also move selfosc startup phase
    # slightly. Report each actual residual instead of assuming perfectly
    # independent denominators or forcing inconsistent references to agree.
    lower, upper = objective(1), objective(4)
    if lower * upper >= 0:
        raise ValueError("optimum is not bracketed inside [1,4]; no qualified profile")
    fitted = float(np.float32(brentq(objective, 1, 4, xtol=1e-4)))
    a_path, a = render.render("isolators", event_paths["isolators"], 1)
    b_path, b = render.render("isolators", event_paths["isolators"], fitted)
    report = {"schema": 1, "source": "Juno-106 #439522, original DCOs; Borish replacement VCF/VCA cards",
              "renderer": {"path": str(args.renderer.resolve()), "sha256": sha256(args.renderer),
                           "character": args.character, "quality": "48 kHz/4x; shipping Poly/Cubic/RK4 x1"},
              "analyzer_sha256": sha256(__file__), "references": provenance,
              "events": {kind: {"path": str(path.resolve()), "sha256": sha256(path)} for kind, path in event_paths.items()},
              "versions": {"python": platform.python_version(), "numpy": np.__version__, "scipy": scipy.__version__},
              "fit": {"scale": fitted, "gain_db": db(fitted), "search_bounds": [1, 4], "history": history,
                      "objective": "zero mean dB noise/saw and noise/true-selfosc error, equal weight across ratios/channels",
                      "baseline": ratio_report(references["isolators"], a, FIT_WINDOWS),
                      "candidate": ratio_report(references["isolators"], b, FIT_WINDOWS)},
              "heldout_isolators": {"baseline": ratio_report(references["isolators"], a, HELDOUT_WINDOWS),
                                     "candidate": ratio_report(references["isolators"], b, HELDOUT_WINDOWS)}}
    for kind, selected, control, anchor in [("resonance", selected_res, "resonance", 0),
                                             ("noise_sweep", selected_noise, "noise", 50)]:
        events = prefix if kind == "noise_sweep" else event_paths[kind]
        _, before = render.render(kind, events, 1)
        _, after = render.render(kind, events, fitted)
        report[f"heldout_{kind}"] = {"baseline": sweep_report(references[kind], before, selected, control, anchor),
                                    "candidate": sweep_report(references[kind], after, selected, control, anchor)}
    report["render_runs"] = render.runs
    report["limitations"] = ["A source-level calibration candidate for one serviced replacement-card unit, not universal original hardware calibration.",
        "Disjoint windows test repeatability within one take; separate sweeps are independent captures, not independent units.",
        "The optimum and residual errors depend on the complete engine revision, character setting and calibration; refit after a material VCF/VCA change.",
        "No calibrated recording gain, VR32 position or TP8 crest-factor measurement is available; the 4 Vp-p service anchor is not replaced.",
        "Noise/control-curve shape and PSD remain unchanged; normalized-noise sweep is a held-out check, not a fitted curve.",
        "Noise-off measurements describe the floor and are excluded from noise-law RMS. Original osc_test's first saw note is absent and unused."]
    (args.output / "report.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    audition = args.output / "audition"
    audition.mkdir()
    shutil.copyfile(a_path, audition / "A.wav")
    shutil.copyfile(b_path, audition / "B.wav")
    (audition / "key.md").write_text(f"A: baseline mainNoiseLevelScale 1.\n\nB: fitted reference source scalar {fitted:.9g}.\n\n"
        "Same exact MIDI, seeds, sample rate, block size, character and shipping kernels. Source level is the variable under judgment: no level matching or EQ. "
        "Both preserve the relative saw/pulse/sub/noise/selfosc levels in the entire same-gain take. Full conditions, hashes and fit/holdout residuals are in report.json. "
        "A fitted scalar describes this serviced replacement-card reference, not a universal original Juno-106 noise trim.\n")
    print(json.dumps({"fitted_scale": fitted, "gain_db": db(fitted), "heldout_ratio_rms_before_db": report["heldout_isolators"]["baseline"]["rms_error_db"],
                      "heldout_ratio_rms_after_db": report["heldout_isolators"]["candidate"]["rms_error_db"],
                      "heldout_resonance_rms_before_db": report["heldout_resonance"]["baseline"]["rms_error_db"],
                      "heldout_resonance_rms_after_db": report["heldout_resonance"]["candidate"]["rms_error_db"]}, indent=2))


def self_test():
    rate = 48000
    t = np.arange(rate * 11) / rate
    audio = np.zeros((len(t), 2))
    for windows in (FIT_WINDOWS, HELDOUT_WINDOWS):
        for name, (begin, end) in windows.items():
            mask = (t >= begin) & (t < end)
            gain = {"saw": 1.0, "noise": 2.7, "selfosc": 2.0}[name]
            audio[mask, 0] = gain * np.sin(2 * np.pi * 400 * t[mask])
            audio[mask, 1] = .37 * audio[mask, 0]
    measured = ratios((rate, audio), FIT_WINDOWS)
    assert np.max(abs(measured - np.array([[db(2.7)], [db(1.35)]]))) < 1e-10
    assert ratio_report((rate, audio), (rate, audio * 9), HELDOUT_WINDOWS)["rms_error_db"] < 1e-10
    try:
        band_level((rate, audio), (10, 12))
    except ValueError:
        pass
    else:
        raise AssertionError("out-of-range window was accepted")
    print("PASS: gain-invariant source ratios, independent fit/heldout windows, channel gain cancellation, window bounds")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--renderer", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--character", type=float, default=1.0)
    for name in PCM:
        parser.add_argument(f"--reference-{name.replace('_','-')}", dest=f"reference_{name}", type=Path)
        parser.add_argument(f"--events-{name.replace('_','-')}", dest=f"events_{name}", type=Path)
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if any(getattr(args, name) is None for name in ["renderer", "output"] +
           [prefix + kind for kind in PCM for prefix in ("reference_", "events_")]):
        parser.error("renderer, output, and all three reference/events pairs are required")
    if not math.isfinite(args.character) or not 0 <= args.character <= 2:
        parser.error("character must be finite in 0..2")
    run(args)


if __name__ == "__main__":
    main()
