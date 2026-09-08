#!/usr/bin/env python3
"""Identify observable chorus properties in Lewis Francis's corrected A11 take.

Source: https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/16#issuecomment-4184997000
Audio: Juno-10-test-audio-96K.zip / bank_A1x_bip.aif (96 kHz stereo).
MIDI: https://kayrock.org/kr106/106_calibration.zip / bank_midi_106/bank_A1x.mid

Only A11 is qualified here. The downloaded MIDI uses nonzero Manual markers
for A12-A18, while the owner describes corrected banks; do not silently rewrite
those messages or assume their correspondence. Preserve the first 17 original
events and render with YouKnowRenderCalibrationEvents, character 1, shipping.

  python3 Tools/AnalyzeChorusCapture.py --midi bank_A1x.mid --events A11.txt
  YouKnowRenderCalibrationEvents A11.txt model.wav 1 shipping
  YouKnowMeasureChorusSupport > support.json
  python3 Tools/AnalyzeChorusCapture.py --reference hardware.wav --model model.wav \
      --support support.json --reference-pitch-cents -6.5 --output result.json

Convert the original AIFF to a PCM WAV without resampling. Its normalized PCM
hash is pinned, independent of WAV headers. The oscillator detune is a declared
capture nuisance coordinate, not a DCO calibration. Frequency-domain channel
ratios cancel the common source, while the exported shipping support response
avoids fitting capture EQ as delay. A window-averaged triangular trajectory is
only a diagnostic hypothesis: real loading, clock transfer, component spread,
finite note duration and oscillator/envelope evolution remain confounds.

Fit complete C1/C3/C5 note windows; withhold C2/C4 for validation. First recover
the known model parameters with the same estimator. Report noise at the capture
level and spectrum/correlation separately; no voltage reference or recording
gain is available. This tool never updates DSP defaults or claims population
calibration from one unit. Requires NumPy and SciPy.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path

import numpy as np
from scipy.optimize import least_squares
from scipy.signal import periodogram

from AnalyzeHardwareCalibration import export_events, load_audio, sha256


SOURCE = "https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/16#issuecomment-4184997000"
AUDIO_URL = "https://www.lewisfrancis.com/nwio/Juno-10-test-audio-96K.zip"
MIDI_URL = "https://kayrock.org/kr106/106_calibration.zip"
MIDI_HASH = "ef7ff68b6059a913a30a70acbea54d1ef982884ed229f25d8684642295a5e511"
AIFF_HASH = "b235ba2236c1a509627ce1e84fa35004b0d7c3e99eb36899c4c5de63cc668662"
PCM_HASH = "1ed7f92f6fe213ed9d0fdea13e133c8ee2e4e0b3f202b535eb7a9d7c5a703772"
NOTES = [(36, 1.594791667), (48, 3.594791667), (60, 5.594791667),
         (72, 7.594791667), (84, 9.594791667)]


def support_response(data, frequencies):
    z = np.exp(-2j * np.pi * frequencies / data["sample_rate"])
    response = np.ones(len(frequencies), complex)
    for index, network in enumerate(data["networks"]):
        state = np.asarray(network["state_by_column"]).T
        drive = np.asarray(network["drive_by_sample"])
        rhs = np.einsum("fk,ki->fi", z[:, None] ** np.arange(4), drive)
        solved = np.linalg.solve(np.eye(6)[None, :, :] - z[:, None, None] * state,
                                 rhs[:, :, None])[:, :, 0]
        response *= solved[:, 5] if index == 0 else solved[:, 4] - solved[:, 5]
    return response


def noise_observations(audio, rate):
    # The 200 ms pre-roll precedes the first note. Later gaps include release
    # tails and are deliberately not treated as noise-only measurements.
    x = audio[:int(0.2 * rate)]
    frequencies, power = periodogram(x, rate, axis=0)
    df = frequencies[1] - frequencies[0]
    bands = {}
    for name, low, high in [("20_20000", 20, 20000), ("1000_2000", 1000, 2000),
                            ("4000_8000", 4000, 8000)]:
        rms_power = power[(frequencies >= low) & (frequencies < high)].sum(0) * df
        bands[name + "_hz_dbfs"] = (10 * np.log10(np.maximum(rms_power, 1e-30))).tolist()
    return {"window_seconds": [0, 0.2], "bands": bands,
            "stereo_correlation": float(np.corrcoef(x.T)[0, 1]),
            "limitation": "short pre-roll; capture gain, hum and interface floor are not separated"}


def frames_for(audio, rate, pitch_cents, support):
    frames = []
    for note_index, (note, onset) in enumerate(NOTES):
        fundamental = 440 * 2 ** ((note - 12 - 69) / 12 + pitch_cents / 1200)
        for centre in onset + np.arange(0.2, 0.86, 0.1):
            count = int(0.12 * rate)
            time = np.arange(count) / rate - 0.06
            window = np.hanning(count)
            start = int((centre - 0.06) * rate)
            samples = audio[start:start + count]
            frequencies = fundamental * np.arange(1, int(900 / fundamental) + 1)
            coefficients = (np.exp(-2j * np.pi * frequencies[:, None] * time)
                            @ (samples * window[:, None])) / window.sum()
            amplitude = np.linalg.norm(coefficients, axis=1)
            keep = amplitude > amplitude.max() * 0.03
            frequencies, coefficients = frequencies[keep], coefficients[keep]
            frames.append({"time": centre, "note_index": note_index,
                           "frequencies": frequencies, "coefficients": coefficients,
                           "support": support_response(support, frequencies),
                           "weight": np.linalg.norm(coefficients, axis=1)
                                     + 0.05 * np.linalg.norm(coefficients)})
    return frames


def identify(audio, rate, pitch_cents, support):
    frames = frames_for(audio, rate, pitch_cents, support)
    quadrature = np.linspace(-0.06, 0.06, 25)
    weights = np.hanning(len(quadrature))
    weights /= weights.sum()
    training = [i for i, f in enumerate(frames) if f["note_index"] % 2 == 0]
    held_out = [i for i in range(len(frames)) if i not in training]

    def residual(parameters, indices):
        centre, depth, frequency, phase, left_gain, right_gain, output_balance = parameters
        result = []
        for index in indices:
            frame = frames[index]
            triangle = 1 - 4 * abs(((frequency * (frame["time"] + quadrature) + phase) % 1) - 0.5)
            a, b = centre + depth * triangle, centre - depth * triangle
            omega = -2j * np.pi * frame["frequencies"][:, None]
            left = 1 + left_gain * frame["support"] * (np.exp(omega * a) @ weights)
            right = 1 + right_gain * frame["support"] * (np.exp(omega * b) @ weights)
            observed = frame["coefficients"]
            error = (observed[:, 1] * left - output_balance * observed[:, 0] * right) / frame["weight"]
            result.extend(error.real)
            result.extend(error.imag)
        return np.asarray(result)

    # These are broad identification search bounds, not component tolerances.
    # A boundary solution is reported and fails even provisional qualification.
    low = np.array([0.002, 0.0005, 0.45, -2, 0.5, 0.5, 0.8])
    high = np.array([0.005, 0.0035, 0.65, 2, 2, 2, 1.2])
    best = None
    for frequency in (0.5, 0.55, 0.6):
        for phase in (0, 0.25, 0.5, 0.75):
            initial = [0.0035, 0.002, frequency, phase, 1.2, 1.2, 1]
            candidate = least_squares(lambda p: residual(p, training), initial,
                                      bounds=(low, high), loss="soft_l1", f_scale=0.08,
                                      diff_step=1e-4, max_nfev=120)
            error = float(np.mean(residual(candidate.x, training) ** 2))
            if best is None or error < best[0]:
                best = (error, candidate)
    p = best[1].x
    train_rmse = math.sqrt(best[0])
    held_rmse = float(np.sqrt(np.mean(residual(p, held_out) ** 2)))
    boundary = bool(np.any(np.minimum(p - low, high - p) / (high - low) < 0.005))
    return {"provisional_centre_seconds": float(p[0]),
            "provisional_depth_seconds": float(p[1]),
            "provisional_delay_endpoints_seconds": [float(p[0] - p[1]), float(p[0] + p[1])],
            "provisional_rate_hz": float(p[2]), "phase_cycles": float(p[3] % 1),
            "wet_gains": p[4:6].tolist(), "output_balance": float(p[6]),
            "declared_pitch_cents": pitch_cents, "training_rmse": train_rmse,
            "held_out_rmse": held_rmse, "near_search_boundary": boundary,
            "held_out_consistent": bool(not boundary and p[0] > p[1] and held_rmse <= 2 * train_rmse),
            "training_midi_notes": [36, 60, 84], "held_out_midi_notes": [48, 72],
            "window_rmse": [{"seconds": float(f["time"]),
                             "rmse": float(np.sqrt(np.mean(residual(p, [i]) ** 2)))}
                            for i, f in enumerate(frames)]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--midi", type=Path)
    parser.add_argument("--events", type=Path)
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--model", type=Path)
    parser.add_argument("--support", type=Path)
    parser.add_argument("--reference-pitch-cents", type=float, default=0.0)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if bool(args.midi) != bool(args.events):
        parser.error("--midi and --events must be supplied together")
    if args.midi:
        export_events(args.midi, args.events, MIDI_HASH, end_seconds=12.0)
    if args.reference:
        if not all([args.model, args.support, args.output]):
            parser.error("--reference requires --model, --support and --output")
        if not math.isfinite(args.reference_pitch_cents) or abs(args.reference_pitch_cents) > 50:
            parser.error("declared pitch must be finite and within +/-50 cents")
        rate, audio = load_audio(args.reference)
        pcm_hash = hashlib.sha256(audio.astype("<f4").tobytes()).hexdigest()
        if rate != 96000 or audio.shape[1] != 2 or pcm_hash != PCM_HASH:
            raise ValueError("reference is not the frozen, unresampled corrected-bank stereo capture")
        model_rate, model_audio = load_audio(args.model)
        if model_rate != 48000 or model_audio.shape != (604550, 2):
            raise ValueError("model must be the exact 48 kHz A11 event-prefix render")
        support = json.loads(args.support.read_text())
        known = identify(model_audio, model_rate, 0.0, support)
        nominal = support["mode_one"]
        recovery = {"centre_error_seconds": known["provisional_centre_seconds"] - nominal["centre_s"],
                    "depth_error_seconds": known["provisional_depth_seconds"] - nominal["depth_s"],
                    "rate_error_hz": known["provisional_rate_hz"] - nominal["rate_hz"]}
        # A coarse estimator screen, not a hardware tolerance or confidence
        # interval. Actual recovery errors and both held-out scores are kept.
        recovery["passes_screen"] = (abs(recovery["centre_error_seconds"]) < 0.0001
                                      and abs(recovery["depth_error_seconds"]) < 0.0001
                                      and abs(recovery["rate_error_hz"]) < 0.002
                                      and known["held_out_consistent"])
        hardware = identify(audio, rate, args.reference_pitch_cents, support)
        result = {"source": SOURCE, "audio_archive": AUDIO_URL, "midi_archive": MIDI_URL,
                  "original_aiff_sha256": AIFF_HASH, "reference_pcm_sha256": pcm_hash,
                  "reference_file_sha256": sha256(args.reference),
                  "model_file_sha256": sha256(args.model), "support_sha256": sha256(args.support),
                  "scope": "A11 on one serviced Juno-106; chorus-part identity and capture gain unverified",
                  "calibration_applied": False,
                  "nominal_calibration_qualified": False,
                  "identification_screen_passed": recovery["passes_screen"] and hardware["held_out_consistent"],
                  "limitations": ["no absolute voltage/noise reference", "no wet-only or physical I+II capture",
                                  "triangular delay and shipping support are identification assumptions",
                                  "another complete take and identified original chorus parts are required before nominal calibration"],
                  "model_recovery": recovery, "model": known, "hardware": hardware,
                  "model_preroll": noise_observations(model_audio, model_rate),
                  "hardware_preroll": noise_observations(audio, rate)}
        args.output.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n")
        print(json.dumps({"model_recovery": recovery,
                          "hardware_held_out_consistent": hardware["held_out_consistent"],
                          "calibration_applied": False}, indent=2))
    elif not args.midi:
        parser.error("supply --reference or --midi/--events")


if __name__ == "__main__":
    main()
