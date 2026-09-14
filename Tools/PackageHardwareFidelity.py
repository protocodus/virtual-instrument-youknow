#!/usr/bin/env python3
"""Package already-rendered raw/A.wav and raw/B.wav into neutral A/B auditions.

Match whole-file stereo RMS, then apply one shared trim to put the larger
peak at -6 dBFS. Preserve raw files, record hashes and both trims, and measure
the level-matched residual separately from untrimmed gain. No resampling,
limiting, denoising, time alignment or circuit approximation is performed.
Requires NumPy/SciPy, like the hardware-capture analyzers in this directory.
"""
import argparse
import hashlib
import html
import json
from pathlib import Path

import numpy as np
from scipy.io import wavfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def db(value):
    return float(20 * np.log10(max(float(value), 1e-18)))


def rms(audio):
    return float(np.sqrt(np.mean(np.square(audio))))


def package(directory, description):
    paths = [directory / "raw" / f"{letter}.wav" for letter in "AB"]
    recordings = [wavfile.read(path) for path in paths]
    rate, a = recordings[0]
    rate_b, b = recordings[1]
    if rate != rate_b or a.shape != b.shape or a.ndim != 2 or a.shape[1] != 2:
        raise ValueError("A/B must have identical rates and stereo frame counts")
    if a.dtype.kind != "f" or b.dtype.kind != "f":
        raise ValueError("raw archives must be floating-point WAVs")
    a, b = a.astype(np.float64), b.astype(np.float64)
    if not np.isfinite(a).all() or not np.isfinite(b).all() or min(rms(a), rms(b)) <= 1e-12:
        raise ValueError("auditions must be finite and non-silent")
    matching = rms(a) / rms(b)
    shared = 10 ** (-6 / 20) / max(np.max(np.abs(a)), np.max(np.abs(b)) * matching)
    matched = [np.asarray(a * shared, dtype=np.float32),
               np.asarray(b * matching * shared, dtype=np.float32)]
    for letter, audio in zip("AB", matched):
        wavfile.write(directory / f"{letter}.wav", rate, audio)
    actual = [audio.astype(np.float64) for audio in matched]
    if abs(db(rms(actual[0]) / rms(actual[1]))) > .00001:
        raise ValueError("delivered RMS matching exceeds tolerance")
    if max(np.max(np.abs(x)) for x in actual) > 10 ** (-6 / 20) + 1e-7:
        raise ValueError("delivered audio exceeds its peak ceiling")
    residual = b * matching - a
    metrics = {
        "sample_rate": rate, "frames": len(a), "seconds": len(a) / rate,
        "raw_sha256": {letter: digest(path) for letter, path in zip("AB", paths)},
        "raw_rms_dbfs": {"A": db(rms(a)), "B": db(rms(b))},
        "raw_peak_dbfs": {"A": db(np.max(np.abs(a))), "B": db(np.max(np.abs(b)))},
        "raw_B_minus_A_rms_db": db(rms(b) / rms(a)),
        "trim_db": {"A": db(shared), "B": db(shared * matching)},
        "delivered_rms_delta_db": db(rms(actual[1]) / rms(actual[0])),
        "matched_residual_rms_relative_A_db": db(rms(residual) / rms(a)),
        "matched_residual_peak_relative_A_rms_db": db(np.max(np.abs(residual)) / rms(a)),
        "delivered_sha256": {letter: digest(directory / f"{letter}.wav") for letter in "AB"},
    }
    (directory / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    (directory / "key.md").write_text(
        f"A: unchanged starting engine. B: {description}\n\n"
        "Both files run the same score through the actual engine, with fixed seeds, "
        "48 kHz, requested 4× quality, 128-frame event-split blocks, the product "
        "HPF/filter/thermal selections, Unit Character 1, Aging 50%, and a settled "
        "thermal start. Chorus hiss is zero to expose the deterministic response. "
        "The score and physical controls are identical within each pair.\n\n"
        f"Whole-file stereo RMS matched; A trim {db(shared):+.9f} dB, "
        f"B trim {db(shared * matching):+.9f} dB. One common final peak trim "
        "places the higher peak at −6 dBFS. No time alignment or processing was "
        "applied beyond these gains. Raw files preserve output level.\n\n"
        "These are software before/after comparisons. Their residual measures "
        "difference, not accuracy against hardware or perceived improvement.\n")
    return metrics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    descriptions = {
        "vca": "service-calibrated VCA gain before the shared analog stages, with the corresponding final digital headroom trim.",
        "hold": "firmware-latched running-voice decisions for DCO reset and LFO delay after HOLD release.",
        "chorus": "comparison-only coupled C15/C16/C13 clock-mute circuit and retained stopped BBD state. Installed thresholds remain unmeasured; this candidate is not enabled by default.",
    }
    metrics = {name: package(args.directory / "audio" / name, description)
               for name, description in descriptions.items()}
    (args.directory / "audio" / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    labels = [("vca", "1 · Chords", "Listen to the body and texture as the six-note chords build."),
              ("hold", "2 · Pedal phrase", "Listen to each second note, immediately after the pedal release."),
              ("chorus", "3 · Chorus switching", "Listen around the short and long interruptions, and the final re-engagement.")]
    sections = []
    for name, title, guidance in labels:
        players = "".join(f'<label>{letter}<audio controls preload="metadata" src="audio/{name}/{letter}.wav"></audio></label>' for letter in "AB")
        sections.append(f'<section><h2>{html.escape(title)}</h2><p>{guidance}</p><div class="pair">{players}</div><p class="small"><a href="audio/{name}/key.md">Reveal key and trims</a> · <a href="audio/{name}/metrics.json">Measurements</a></p></section>')
    (args.directory / "index.html").write_text("""<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Juno-106 A/B listening</title><link rel="icon" href="data:,"><style>
body{max-width:920px;margin:52px auto;padding:0 24px;color:#181818;background:white;font:17px/1.6 system-ui,sans-serif}h1{font-size:34px;font-weight:600;line-height:1.2}h2{font-size:22px;font-weight:600}section{border-top:1px solid #ccc;margin-top:32px;padding-top:18px}.pair{display:flex;gap:30px;flex-wrap:wrap}label{display:grid;gap:8px;font-weight:600;flex:1}audio{width:100%;min-width:260px}.small{font-size:14px;color:#555}a{color:#175980}p{max-width:760px}
</style><h1>Juno-106 A/B listening</h1><p>Three short comparisons, each changing one mechanism. A is the starting engine. Both takes have matched whole-file stereo RMS and at least 6 dB of peak headroom.</p><p>Play either letter. Starting the other pauses the first and continues near the same position. The separate keys disclose the changes and exact level trims.</p>
""" + "\n".join(sections) + """
<section><h2>Original hardware context</h2><p class="small">Juno-106 #439522 with original DCOs and replacement Borish voice modules: saw, pulse, sub, noise and self-oscillation. This excerpt retains the original recording gain. It is not level-matched to the musical pairs.</p><audio controls preload="metadata" src="hardware/C.wav"></audio><p class="small"><a href="hardware/key.md">Recording provenance and settings</a></p></section>
<section><p><a href="research.md">Read the research report</a> · <a href="audio/metrics.json">All audio measurements</a></p><p class="small">These pairs compare software revisions. The chorus pair is an experimental circuit comparison; it is not the default. Real-hardware recording measurements and their limits are documented in the report.</p></section>
<script>
for(const section of document.querySelectorAll('section')){const players=[...section.querySelectorAll('audio')];for(const player of players){player.addEventListener('play',()=>{for(const other of players){if(other!==player&&!other.paused){const t=other.currentTime;other.pause();if(t<player.duration)player.currentTime=t;}}for(const other of document.querySelectorAll('audio')){if(other!==player&&!players.includes(other))other.pause();}});}}
</script></html>
""")
    print(json.dumps(metrics, indent=2))


if __name__ == "__main__":
    main()
