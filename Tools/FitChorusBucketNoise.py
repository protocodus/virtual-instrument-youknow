#!/usr/bin/env python3
"""Identify an EFFECTIVE packet covariance, not microscopic MN3009 sources.

The four SHA-pinned April2026 captures of Juno106 #439522 contain seven
release-qualified ModeI idle patches (14 channel observations). RELEASE<=40,
NOISE=0 and idle from last note-off+1.2s to next patch-0.1s follow the existing
AnalyzeChorusIdleFloors protocol. Welch/Hann0.2s,50%overlap measures full PSD.
Fit32 logarithmic mean-density bands250Hz..16kHz, not one high/low contrast.
Remove50/60Hz harmonics +/-5Hz below2kHz and narrow peaks >12dB above the
205Hz local median, expanded +/-10Hz. Every retained bin/removed fraction is
reported. This cannot separate recording-chain noise or unknown broad EQ.
The fixed peak-mask rule reads each observed PSD, including held-out bins;
holdout claims are conditional on that common preprocessing, not untouched
frequency data. Mask thresholds are specified before fitting eta.

Thornber(BSTJ53,1974,pp1237/1244) and Weckler/Buss(Reticon1977,p5/Fig6)
motivate independent storage plus opposite-packet transfer fluctuations:
packet PSD=V*(1-eta*cos(2*pi*f/fCP)). After the rectangular physical hold and
the actual finite-follower/JFET post circuit, density is proportional to
|Hpost|^2*sinc(f/fCP)^2/fCP*(1-eta*cos). Normalize at EACH clock to retain
the existing20Hz..20kHz A-weighted output budget. This is no arbitrary EQ.
https://vtda.org/pubs/BSTJ/vol53-1974/articles/bstj53-7-1211.pdf
https://www.imagesensors.org/Past%20Workshops/Marvin%20White%20Collection/1977%20Short%20Course/1977%203%20Weckler.pdf#page=8

Freeze product OwnerBlend delay centre/sweep/rate from the C++ support export.
Use ONE unknown phase per patch, opposite-phase L/R clocks, and fCP=128/delay.
Neither channel may choose an independent clock or sweep. Integrate the LFO
over each actual idle window at32 midpoint times; phase grid128 points.
Fit one global eta on banksA1/A2 and even-indexed frequency bands only.
Choose each patch phase and each channel recording-gain nuisance on those
same even bands only. Lock eta for B3/B4 patch AND odd-band validation.
Held-out patch phases/gains still use their even bands; those nuisances are
conditional validation, not independent out-of-unit hardware predictions.
The odd bands are never used to estimate eta, gain or phase. ModeII's two
qualified patches are a secondary locked-eta check, not a separate fit.

Only eta=2*r*c/(1+c*c) is identified by this PSD family. r and c, per-bucket
kT/C, transfer efficiency and the true internal distribution are NOT
identified. Product r=eta,c=1 is a representative algebraic convention.
The original chorus board belongs to ONE serviced instrument; its voice
cards are Borish replacements. This effective calibration may absorb
unidentified clock geometry, recording response or other broad noise.
The existing2.37 absolute-level factor is NOT refit here. Independent C++
fixed-clock covariance, normalization and actual-path spectra qualify the
implementation, separately from this conditional capture fit.

Requires optional NumPy/SciPy; never registered as an unconditional DSP CI
test. Export current finite support/OwnerBlend with:
  YouKnowChorusBucketNoiseTests --support > support.json
  OPENBLAS_NUM_THREADS=1 python3 Tools/FitChorusBucketNoise.py \
      --captures INPUT_DIR --midi-zip 106_calibration.zip --support support.json \
      --output NEW_REPORT.json
--tone-db9/15, --phases256, --time-points64 and --train-banksB3x,B4x
repeat explicit sensitivity checks. Reports pin all inputs and this tool.
The optional --fixed-measurements and --engine-measurements arguments check
directories from the C++ --measure/--engine-measure exporters against the
independent physical PSD and the unchanged full-engine A-weighted level.
"""
import argparse
import csv
import hashlib
import json
import platform
import zipfile
from pathlib import Path

import numpy as np
import scipy
from scipy.ndimage import median_filter, maximum_filter1d
from scipy.optimize import minimize_scalar
from scipy.signal import welch
import AnalyzeChorusIdleFloors as idle
from AnalyzeChorusNoise import response


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def observations(args):
    result = []
    with zipfile.ZipFile(args.midi_zip) as archive:
        for bank, pins in idle.BANKS.items():
            data = archive.read(f"106_calibration/bank_midi_106/bank_{bank}.mid")
            if idle.sha256(data) != pins["midi"]:
                raise ValueError(f"unexpected MIDI sequence {bank}")
            fs, audio = idle.checked_capture(args.captures/f"bank_{bank}_bip.aif", bank)
            for patch in idle.patches(idle.timeline(data), len(audio)/fs):
                if not patch["idle_seconds"] or patch["release"] > 40 or patch["noise"] or patch["chorus"] == "off":
                    continue
                begin, end = (round(t*fs) for t in patch["idle_seconds"])
                f, density = welch(audio[begin:end], fs, window="hann", nperseg=fs//5,
                                   noverlap=fs//10, detrend="constant", scaling="density", axis=0)
                result.append(dict(bank=bank, patch=patch["patch"], mode=patch["chorus"],
                                   seconds=patch["idle_seconds"], f=f, density=density,
                                   release=patch["release"], noise=patch["noise"]))
            del audio
    return result


def prepare(rows, support, args):
    edges = np.geomspace(250, 16000, 33)
    records = []
    for row in rows:
        f = row["f"]
        h = np.abs(response(support, f))**2
        normf = f[(f >= 20) & (f <= 20000)]
        normh = np.abs(response(support, normf))**2*idle.a_weighting(normf)
        clockgrid = np.linspace(10000, 200000, 1025)
        moment = []
        for clock in clockgrid:
            weight = normh*np.sinc(normf/clock)**2
            moment.append((weight*np.cos(2*np.pi*normf/clock)).sum()/weight.sum())
        timing = support["timing"][row["mode"]]
        time = (np.arange(args.time_points)+.5)/args.time_points*(row["seconds"][1]-row["seconds"][0])
        phase = (np.arange(args.phases)[:, None]/args.phases+timing["rate"]*time[None, :]) % 1
        triangle = 4*np.minimum(phase, 1-phase)-1
        channels = []
        for channel, sign in enumerate((1, -1)):
            clock = 128/(timing["centre"]+sign*timing["sweep"]*triangle)
            assert clock.min() >= 10000 and clock.max() <= 200000
            p = row["density"][:, channel]
            keep = np.ones(len(f), bool)
            for base in (50, 60):
                for multiple in range(1, 2000//base+1):
                    keep &= np.abs(f-base*multiple) > 5
            width = int(round(205/(f[1]-f[0]))) | 1
            peaks = p > median_filter(p, size=width, mode="nearest")*10**(args.tone_db/10)
            keep &= ~maximum_filter1d(peaks, size=5)
            masks = [(f >= low) & (f < high) & keep for low, high in zip(edges[:-1], edges[1:])]
            assert all(mask.any() for mask in masks)
            y = np.array([10*np.log10(p[mask].mean()) for mask in masks])
            b0 = np.empty((args.phases, args.time_points, 32))
            b1 = np.empty_like(b0)
            for k, mask in enumerate(masks):
                x = f[mask][None, None, :]/clock[:, :, None]
                weight = h[mask][None, None, :]*np.sinc(x)**2/clock[:, :, None]
                b0[:, :, k] = weight.mean(2)
                b1[:, :, k] = (weight*np.cos(2*np.pi*x)).mean(2)
            channels.append(dict(y=y, b0=b0, b1=b1, m=np.interp(clock, clockgrid, moment),
                                 counts=[int(mask.sum()) for mask in masks],
                                 available=[int(((f >= a) & (f < b)).sum()) for a, b in zip(edges[:-1], edges[1:])]))
        records.append(dict(bank=row["bank"], patch=row["patch"], mode=row["mode"],
                            seconds=row["seconds"], channels=channels))
    return records, edges


def score(records, eta, selected=None):
    fit = np.arange(32) % 2 == 0
    errors, detail = [], []
    for patch in records:
        if selected and not selected(patch):
            continue
        residuals, gains = [], []
        for channel in patch["channels"]:
            prediction = 10*np.log10(((channel["b0"]-eta*channel["b1"])/(1-eta*channel["m"][:, :, None])).mean(1))
            gain = (channel["y"][fit][None, :]-prediction[:, fit]).mean(1)
            residuals.append(channel["y"][None, :]-prediction-gain[:, None])
            gains.append(gain)
        residuals = np.asarray(residuals)
        # One phase jointly minimizes both channel residuals, using even bands.
        index = np.argmin((residuals[:, :, fit]**2).sum(axis=(0, 2)))
        errors.extend(residuals[:, index, :])
        detail.append(dict(bank=patch["bank"], patch=patch["patch"], mode=patch["mode"],
                           phase_index=int(index), gains_db=[float(g[index]) for g in gains],
                           errors_db=residuals[:, index, :].tolist()))
    return np.asarray(errors), detail


def implementation_checks(fixed, engines, support):
    """Actual finite-follower held events and complete engine; no fitted gain.

    --measure uses4s per fixed clock, discard0.5s; --engine-measure uses8s
    of real moving-clock output, discard1s. Welch/Hann0.2s/50%overlap. The
    fixed-clock analytical PSD is an independent source-law oracle. The
    coarse-grid upper-band residual is reported, never equalized away.
    """
    result = {}
    if fixed:
        rows = []
        for item in csv.DictReader((fixed/"index.csv").open()):
            fs, clock = int(item["sample_rate"]), int(item["clock_hz"])
            eta, amplitude = float(item["fraction"]), float(item["source_amplitude"])
            samples = np.fromfile(fixed/item["file"], dtype="<f4")[fs//2:]
            f, p = welch(samples, fs, nperseg=fs//5, noverlap=fs//10, scaling="density")
            selected = (f >= 20) & (f < 20000)
            frequencies = f[selected]
            h = np.abs(response(support, frequencies))**2
            weight = h*idle.a_weighting(frequencies)*np.sinc(frequencies/clock)**2
            moment = (weight*np.cos(2*np.pi*frequencies/clock)).sum()/weight.sum()
            theory = (2*amplitude**2/(3*clock)*np.sinc(frequencies/clock)**2*h
                      *(1-eta*np.cos(2*np.pi*frequencies/clock))/(1-eta*moment))
            bands = [(200,1000),(1000,4000),(4000,8000),(8000,12000),(12000,16000)]
            errors = []
            for a,b in bands:
                band = (frequencies >= a) & (frequencies < b)
                errors.append(float(10*np.log10(p[selected][band].sum()/theory[band].sum())))
            level = float(10*np.log10((p[selected]*idle.a_weighting(frequencies)).sum()*(f[1]-f[0])))
            rows.append(dict(rate=fs,clock=clock,eta=eta,errors_db=errors,level_a_dbfs=level,
                             audio_sha256=sha(fixed/item["file"])))
        maximum = max(abs(error) for row in rows for error in row["errors_db"])
        result["fixed_clock"] = dict(bands_hz=bands,rows=rows,maximum_absolute_band_error_db=maximum)
        if maximum > .85:
            raise ValueError("actual held-packet PSD exceeded independent oracle tolerance")
    if engines:
        rows = []
        for item in csv.DictReader((engines/"index.csv").open()):
            fs = int(item["sample_rate"])
            samples = np.fromfile(engines/item["file"],dtype="<f4").reshape(2,-1)[:,fs:].T
            f, p = welch(samples,fs,nperseg=fs//5,noverlap=fs//10,scaling="density",axis=0)
            selected = (f >= 20) & (f < 20000)
            level = 10*np.log10((p[selected]*idle.a_weighting(f[selected])[:,None]).sum(0)*(f[1]-f[0]))
            low = p[(f >= 200) & (f < 2000)].mean(0)
            high = p[(f >= 2000) & (f < 8000)].mean(0)
            rows.append(dict(quality=int(item["quality"]),mode=int(item["mode"]),eta=float(item["fraction"]),
                             level_a_dbfs=level.tolist(),contrast_db=(10*np.log10(high/low)).tolist(),
                             audio_sha256=sha(engines/item["file"]),elapsed_seconds=float(item["elapsed"])))
        differences = []
        for row in rows:
            if row["eta"] == 0:
                colored = next(x for x in rows if x["eta"] != 0 and x["mode"] == row["mode"] and x["quality"] == row["quality"])
                differences.append(dict(mode=row["mode"],quality=row["quality"],
                                        a_weighted_change_db=(np.asarray(colored["level_a_dbfs"])-row["level_a_dbfs"]).tolist(),
                                        density_contrast_change_db=(np.asarray(colored["contrast_db"])-row["contrast_db"]).tolist()))
        maximum = max(abs(x) for row in differences for x in row["a_weighted_change_db"])
        result["full_engine"] = dict(rows=rows,changes=differences,maximum_absolute_a_weighted_change_db=maximum)
        if maximum > .12:
            raise ValueError("complete moving-clock engine did not preserve A-weighted calibration")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("captures", "midi-zip", "support", "output"):
        parser.add_argument("--"+name, type=Path, required=True)
    parser.add_argument("--phases", type=int, default=128)
    parser.add_argument("--time-points", type=int, default=32)
    parser.add_argument("--tone-db", type=float, default=12)
    parser.add_argument("--train-banks", default="A1x,A2x")
    parser.add_argument("--fixed-measurements", type=Path)
    parser.add_argument("--engine-measurements", type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise ValueError("report exists; preserve prior evidence")
    support = json.loads(args.support.read_text())
    assert support["sample_rate"] >= 768000
    records, edges = prepare(observations(args), support, args)
    banks = set(args.train_banks.split(","))
    train = lambda p: p["mode"] == "I" and p["bank"] in banks
    held = lambda p: p["mode"] == "I" and p["bank"] not in banks
    fit = np.arange(32) % 2 == 0
    objective = lambda eta: float(np.mean(score(records, eta, train)[0][:, fit]**2))
    solved = minimize_scalar(objective, bounds=(0, .999999), method="bounded", options={"xatol":1e-7})
    eta = float(solved.x)
    models = {}
    for name, value in (("iid", 0), ("qualified", eta), ("ideal_difference", 1)):
        training, detail = score(records, value, train)
        holdout, hdetail = score(records, value, held)
        modeii, iidetail = score(records, value, lambda p: p["mode"] == "II")
        models[name] = dict(eta=value, train_rmse_db=float(np.sqrt(np.mean(training[:, fit]**2))),
                            train_patch_frequency_holdout_rmse_db=float(np.sqrt(np.mean(training[:, ~fit]**2))),
                            patch_and_frequency_holdout_rmse_db=float(np.sqrt(np.mean(holdout[:, ~fit]**2))),
                            mode_ii_locked_eta_frequency_holdout_rmse_db=float(np.sqrt(np.mean(modeii[:, ~fit]**2))),
                            patches=detail+hdetail+iidetail)
    # Leave-one-training-patch-out sensitivity estimates, still no heldout
    # data enters eta. Bounds concern this model family/unit, not a population.
    leave_one_out = []
    for patch in filter(train, records):
        choose = lambda p: train(p) and (p["bank"],p["patch"]) != (patch["bank"],patch["patch"])
        q = minimize_scalar(lambda e: float(np.mean(score(records,e,choose)[0][:,fit]**2)),
                            bounds=(0,.999999),method="bounded",options={"xatol":1e-7})
        leave_one_out.append(dict(omitted=f'{patch["bank"]}-p{patch["patch"]}', eta=float(q.x)))
    report = dict(scope="one-unit effective output-packet covariance, conditional shared-LFO model; microscopic sources unidentifiable",
                  versions=dict(python=platform.python_version(), numpy=np.__version__, scipy=scipy.__version__),
                  sha256=dict(tool=sha(__file__), idle_protocol=sha(idle.__file__), support=sha(args.support),
                              midi_zip=sha(args.midi_zip), captures={b:idle.BANKS[b]["aiff"] for b in idle.BANKS}),
                  protocol=dict(train_banks=sorted(banks), training_band_indices=list(np.flatnonzero(fit).astype(int)),
                                heldout_band_indices=list(np.flatnonzero(~fit).astype(int)),
                                phase_points=args.phases, temporal_midpoints=args.time_points,
                                tone_threshold_db=args.tone_db, band_edges_hz=edges.tolist(),
                                clocks="128/delay; fixed OwnerBlend opposite-phase stereo triangle, only one phase per patch",
                                gain="per-channel dB offset from even bands only; absolute noise budget not refit"),
                  effective_eta=eta, leave_one_training_patch_out=leave_one_out, models=models,
                  implementation=implementation_checks(args.fixed_measurements,args.engine_measurements,support),
                  retained_bins=[dict(bank=p["bank"],patch=p["patch"],mode=p["mode"],seconds=p["seconds"],
                                      channel_counts=[c["counts"] for c in p["channels"]],available=p["channels"][0]["available"]) for p in records])
    args.output.write_text(json.dumps(report,indent=2,allow_nan=False,default=lambda v:int(v))+"\n")
    print(json.dumps(dict(eta=eta,models={k:{a:b for a,b in v.items() if a!="patches"} for k,v in models.items()},
                          leave_one_training_patch_out=leave_one_out),indent=2))


if __name__ == "__main__":
    main()
