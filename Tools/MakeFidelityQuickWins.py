#!/usr/bin/env python3
"""Build a reproducible, blind-label comparison package outside the repository.

Both executables must compile the identical RenderFidelityQuickWins.cpp against
their OWN revision's headers and matching Release DSP archive. A is unchanged
shipping; B is the declared candidate. For mechanism isolation, build B with
only that mechanism changed. A full revised-build comparison is useful but is
explicitly labelled as grouped evidence. No output is overwritten.

Unnormalized IEEE-float renders stay in raw/SCORE/A.wav and B.wav. Listening
SCORE/A.wav and B.wav use whole-file stereo RMS matching and one shared trim
to -6 dBFS peak, encoded as PCM24 without resampling or silence removal. The
complete event/control manifests and SHA-256s are written before handover.
No recording is a Juno-106 capture or proof of an audible improvement.
"""

from __future__ import annotations

import argparse
import array
import hashlib
import html
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import wave

from MakeListeningPagePortable import make_portable


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_float(path: Path) -> array.array:
    data = path.read_bytes()
    if (len(data) < 44 or data[:4] != b"RIFF" or data[8:16] != b"WAVEfmt "
            or data[36:40] != b"data"
            or struct.unpack_from("<IHHIIHH", data, 16)
            != (16, 3, 2, 48000, 384000, 8, 32)
            or struct.unpack_from("<I", data, 40)[0] != len(data) - 44
            or struct.unpack_from("<I", data, 4)[0] != len(data) - 8
            or (len(data) - 44) % 8):
        raise ValueError(f"Not a canonical 48 kHz stereo float WAV: {path}")
    result = array.array("f")
    result.frombytes(data[44:])
    if sys.byteorder != "little":
        result.byteswap()
    if not result or not all(math.isfinite(x) for x in result):
        raise ValueError(f"Empty or non-finite audio: {path}")
    return result


def level(samples: array.array) -> tuple[float, float]:
    return max(abs(x) for x in samples), math.sqrt(math.fsum(x * x for x in samples) / len(samples))


def db(value: float) -> float:
    return 20.0 * math.log10(max(1e-18, value))


def write_pcm24(path: Path, samples: array.array, gain: float) -> tuple[float, float]:
    path.parent.mkdir(parents=True, exist_ok=True)
    data = bytearray(3 * len(samples))
    energy = 0.0
    peak = 0.0
    for i, sample in enumerate(samples):
        value = round(sample * gain * 8388607)
        if not -8388608 <= value <= 8388607:
            raise ValueError("PCM24 clipping; matching protocol failed")
        realized = value / 8388607.0
        energy += realized * realized
        peak = max(peak, abs(realized))
        data[3 * i:3 * i + 3] = (value & 0xFFFFFF).to_bytes(3, "little")
    with wave.open(str(path), "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(3)
        output.setframerate(48000)
        output.writeframes(data)
    return peak, math.sqrt(energy / len(samples))


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--baseline-renderer", type=Path, required=True)
    p.add_argument("--candidate-renderer", type=Path, required=True)
    p.add_argument("--baseline-revision", required=True)
    p.add_argument("--candidate-revision", required=True)
    p.add_argument("--scope", required=True,
                   help="Precisely name changed mechanisms; state whether isolated or grouped")
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--scores", nargs="+", default=["oscillator", "pulse", "sub", "unison", "chorus", "switching", "pedal", "ensemble"])
    p.add_argument("--timing", choices=["original", "direct"], default="original")
    p.add_argument("--quality", choices=[1, 2, 4], type=int, default=1)
    p.add_argument("--baseline-cache", type=Path,
                   help="Reuse/store hash-verified baseline renders by timing, quality and score")
    return p.parse_args()


def run(args: argparse.Namespace) -> None:
    out = args.output.resolve()
    if out.exists() and any(out.iterdir()):
        raise ValueError("Output must be new or empty; preserve earlier listening evidence")
    renderer = {"A": args.baseline_renderer.resolve(), "B": args.candidate_renderer.resolve()}
    available = {}
    for letter, path in renderer.items():
        available[letter] = subprocess.check_output([str(path), "--list"], text=True).splitlines()
    if available["A"] != available["B"] or any(s not in available["A"] for s in args.scores):
        raise ValueError("Executables have different scores or requested score is unsupported")
    if len(set(args.scores)) != len(args.scores):
        raise ValueError("Duplicate score names")
    out.mkdir(parents=True, exist_ok=True)
    manifest = {
        "protocol": "youknow-fidelity-2026-10-15-v1",
        "scope": args.scope,
        "revisions": {"A": args.baseline_revision, "B": args.candidate_revision},
        "renderer_sha256": {letter: sha256(path) for letter, path in renderer.items()},
        "source_sha256": sha256(Path(__file__).with_name("RenderFidelityQuickWins.cpp")),
        "packager_sha256": sha256(Path(__file__)),
        "player_builder_sha256": sha256(Path(__file__).with_name("MakeListeningPagePortable.py")),
        "timing": args.timing, "quality": args.quality, "scores": {},
    }
    rows = []
    controls = []
    for score in args.scores:
        raw_dir = out / "raw" / score
        raw_dir.mkdir(parents=True)
        raw, info = {}, {}
        for letter, executable in renderer.items():
            path = raw_dir / f"{letter}.wav"
            print(f"Rendering {score} {letter}...", flush=True)
            cached = (args.baseline_cache / args.timing / f"{args.quality}x" / score
                      if letter == "A" and args.baseline_cache else None)
            if cached is not None and (cached / "A.json").is_file():
                record = json.loads((cached / "A.json").read_text())
                if (record["renderer_sha256"] != manifest["renderer_sha256"]["A"]
                        or record["source_sha256"] != manifest["source_sha256"]
                        or record["audio_sha256"] != sha256(cached / "A.wav")):
                    raise ValueError(f"Stale or modified baseline cache for {score}")
                info[letter] = record["render"]
                shutil.copyfile(cached / "A.wav", path)
            else:
                record = subprocess.check_output(
                    [str(executable), "--render", score, str(path), args.timing, str(args.quality)], text=True)
                info[letter] = json.loads(record)
                if cached is not None:
                    cached.mkdir(parents=True, exist_ok=True)
                    if (cached / "A.wav").exists():
                        raise ValueError(f"Incomplete baseline cache exists for {score}")
                    shutil.copyfile(path, cached / "A.wav")
                    (cached / "A.json").write_text(json.dumps({
                        "renderer_sha256": manifest["renderer_sha256"]["A"],
                        "source_sha256": manifest["source_sha256"],
                        "audio_sha256": sha256(path), "render": info[letter],
                    }, indent=2) + "\n")
            raw[letter] = read_float(path)
        # These controls, events, seeds and callback schedule must be identical;
        # profile differences, if any, are part of the declared build scope.
        for field in ("protocol", "score", "sample_rate", "channels", "frames", "max_block_size",
                      "preroll_frames", "quality_requested", "quality_applied", "timing",
                      "thermal_start", "seed_policy", "kernel", "patch", "events"):
            if info["A"][field] != info["B"][field]:
                raise ValueError(f"Comparison mismatch in {score}: {field}")
        if len(raw["A"]) != len(raw["B"]):
            raise ValueError(f"Mismatched render lengths in {score}")
        levels = {letter: level(samples) for letter, samples in raw.items()}
        if min(value[1] for value in levels.values()) < 1e-7:
            raise ValueError(f"Silent render in {score}")
        match = levels["A"][1] / levels["B"][1]
        common = 10.0 ** (-6.0 / 20.0) / max(levels["A"][0], levels["B"][0] * match)
        gains = {"A": common, "B": common * match}
        realized = {}
        for letter in ("A", "B"):
            path = out / score / f"{letter}.wav"
            realized[letter] = write_pcm24(path, raw[letter], gains[letter])
            info[letter].update(raw_sha256=sha256(raw_dir / f"{letter}.wav"),
                               listening_sha256=sha256(path),
                               listening_trim_db=db(gains[letter]),
                               listening_peak_dbfs=db(realized[letter][0]),
                               listening_rms_dbfs=db(realized[letter][1]))
            rows.append(f"| {score} | {letter} | {db(levels[letter][0]):.4f} | {db(levels[letter][1]):.4f} | {db(gains[letter]):.4f} |")
        mismatch_db = abs(db(realized["A"][1] / realized["B"][1]))
        if mismatch_db > 0.001:
            raise ValueError(f"RMS matching failed in {score}: {mismatch_db} dB")
        delta_rms = math.sqrt(math.fsum((a - b) ** 2 for a, b in zip(raw["A"], raw["B"])) / len(raw["A"]))
        manifest["scores"][score] = {"renders": info, "matched_rms_difference_db": mismatch_db,
                                      "raw_difference_dbr": db(delta_rms / levels["A"][1])}
        guide = html.escape(info["A"]["description"])
        controls.append(f'<section><h2>{html.escape(score.title())}</h2><p>{guide}</p>'
                        f'<div class="pair"><label>A<audio controls preload="none" src="{score}/A.wav"></audio></label>'
                        f'<label>B<audio controls preload="none" src="{score}/B.wav"></audio></label></div>'
                        '<p class="quiet">Whole-file stereo RMS matched; shared peak ceiling −6 dBFS.</p>'
                        '<details><summary>Raw A/B · original gain, no loudness matching</summary>'
                        '<p class="quiet">Use these when judging note duration and release. Original output gain is retained.</p>'
                        f'<div class="pair"><label>A<audio controls preload="none" src="raw/{score}/A.wav"></audio></label>'
                        f'<label>B<audio controls preload="none" src="raw/{score}/B.wav"></audio></label></div></details></section>')
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (out / "key.md").write_text(
        "A: unchanged shipping engine.\n\nB: candidate engine.\n\n"
        f"Comparison scope: {args.scope}\n\n"
        f"A revision: `{args.baseline_revision}`.\n\nB revision: `{args.candidate_revision}`.\n\n"
        "Identical score source, integer-frame events, controls, deterministic engine/card/noise seeds, "
        f"48 kHz, at most 128 frames per callback, {args.quality}x requested quality, {args.timing} timing, "
        "settled thermal start and 250 ms discarded preroll. The callback splits at event boundaries. "
        "Render manifests contain every event and score-controlled patch value; binary/source/audio "
        "SHA-256s identify the files. Raw float32 files retain amplitude and all silence. "
        "Listening WAVs are PCM24; no resampling, dynamic limiting or silence removal.\n\n"
        "Whole-file stereo RMS: B is scaled to A, then both receive one shared trim so the larger "
        "peak is −6 dBFS. Reported trims include both operations. The final quantized WAVs agree "
        "within 0.001 dB RMS. This intentionally hides output-level differences; consult raw files "
        "when level or release duration is itself under judgment.\n\n"
        "| Score | Letter | Raw peak dBFS | Raw RMS dBFS | Listening trim dB |\n"
        "|---|---|---:|---:|---:|\n" + "\n".join(rows) + "\n\n"
        "These compare implementations, not hardware captures. A changed waveform is not proof "
        "of an audible improvement. No user listening verdict has been recorded.\n")
    (out / "index.html").write_text(
        '<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'
        '<title>YouKnow listening tests</title><style>body{font:17px system-ui;max-width:900px;margin:3rem auto;padding:0 1.4rem;background:#10161d;color:#e4eaf1}'
        'h1{font-weight:650}section{margin:2rem 0;padding:1.3rem;border:1px solid #35404c;border-radius:12px}'
        '.pair{display:flex;gap:1rem;flex-wrap:wrap}.pair label{display:grid;gap:.4rem;flex:1}audio{max-width:100%;width:100%}'
        'a{color:#87c5ff}.quiet{color:#9eabb9;font-size:.9rem}</style><h1>YouKnow · A/B listening</h1>'
        '<p>Compare the same short performance. Use headphones for stereo chorus. The identity key is separate; '
        'leave it unread until you have listened. Each pair is level matched across the complete file.</p>'
        + "\n".join(controls)
        + '<p><a href="key.md">Reveal identity key and gain trims</a> · <a href="manifest.json">Reproducibility manifest</a></p></html>\n')
    make_portable(out / "index.html")
    print(f"Wrote {len(args.scores)} matched pairs, raw archives, manifest, key and portable listening page to {out}")


if __name__ == "__main__":
    try:
        run(parse_args())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"MakeFidelityQuickWins: {error}", file=sys.stderr)
        sys.exit(1)
