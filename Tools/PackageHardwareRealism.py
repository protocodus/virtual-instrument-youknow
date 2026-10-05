#!/usr/bin/env python3
"""Package immutable RenderHardwareRealism float archives into PCM24 auditions.

Usage: PackageHardwareRealism.py DIR A B [--self-test]
Archives live in DIR/raw/LABEL/{manifest.json,montage.wav,five passages.wav}.
One stereo RMS match is computed from the25s montage and reused for every
passage. Signed residual is Bmatched-Amatched; its normalized copy has the same
montage RMS as A/B. One common peak trim covers A, B and normalized residual,
preserving these relationships while placing the highest peak at-6dBFS.
No time alignment, resampling, limiting, denoising or per-section normalization.
True unboosted residuals remain available as PCM24 and float32; raw archives
are never written. Residual size quantifies change, not hardware accuracy.
"""

import argparse
import hashlib
import html
import json
from pathlib import Path
import re
import struct
import tempfile
import wave

import numpy as np
from scipy.io import wavfile


RATE = 48000
PROTOCOL = "youknow-hardware-realism-v1"
PEAK = 10 ** (-6 / 20)
SCENES = (
    ("01-chorus-motion", "Chorus motion", "Stereo width, pitch movement and comb-filter motion in Modes I and II."),
    ("02-quiet-vca", "Quiet VCA", "The end of each decay and release; the passage keeps its true quiet level."),
    ("03-wave-sub", "WAVE and sub", "Bass and harmonics as sub level and pulse width change."),
    ("04-bbd-headroom", "BBD headroom", "Wet-path compression and harmonic texture in a loud six-note chorus chord."),
    ("05-resonance-drive", "Resonance and drive", "Bass loss, resonant color and the transition into self-oscillation."),
)
CONTRACT = (
    "protocol", "renderer_source_sha256", "compiler", "sample_rate", "block_size",
    "quality_requested", "polyphony", "output_route", "output_selector",
    "output_load_ohms", "output_capacitance_pf", "latency_compensation",
    "discarded_preroll_seconds", "thermal_start", "noise_seeds", "product_profile",
    "numerical_modes", "unit_character", "aging", "score_kind",
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def db(value):
    return float(20 * np.log10(max(float(value), 1e-18)))


def rms(audio):
    return float(np.sqrt(np.mean(np.square(audio, dtype=np.float64))))


def peak(audio):
    return float(np.max(np.abs(audio)))


def levels(audio):
    return {"rms_dbfs": db(rms(audio)), "peak_dbfs": db(peak(audio))}


def fnv_float(audio):
    # Same little-endian, interleaved float32 identity as the C++ renderer.
    value = 14695981039346656037
    for byte in np.asarray(audio, dtype="<f4", order="C").tobytes():
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}"


def read_archive(path):
    # Only the renderer's canonical44-byte IEEE-float stereo WAV is accepted.
    data = path.read_bytes()
    require(len(data) >= 44, f"short WAV: {path}")
    header = struct.unpack("<4sI4s4sIHHIIHH4sI", data[:44])
    require(header[0] == b"RIFF" and header[2] == b"WAVE"
            and header[3] == b"fmt " and header[4] == 16
            and header[5:8] == (3, 2, RATE) and header[8:11] == (RATE * 8, 8, 32)
            and header[11] == b"data", f"not canonical48kHz stereo float32: {path}")
    require(header[1] == len(data) - 8 and header[12] == len(data) - 44
            and header[12] > 0 and header[12] % 8 == 0, f"WAV length mismatch: {path}")
    audio = np.frombuffer(data, dtype="<f4", offset=44).reshape(-1, 2).astype(np.float64)
    require(np.isfinite(audio).all(), f"non-finite audio: {path}")
    return audio


def check_manifest(a, b):
    for key in CONTRACT:
        require(key in a and key in b and a[key] == b[key], f"A/B contract differs: {key}")
    require(a["protocol"] == PROTOCOL, "unsupported score protocol")
    require(a["sample_rate"] == RATE and a["quality_requested"] == 1
            and a["block_size"] == 128 and a["polyphony"] == 6
            and a["output_route"] == "LINE", "unsupported render settings")
    for manifest in (a, b):
        for key in ("dsp_source_sha256", "renderer_source_sha256"):
            require(re.fullmatch(r"[0-9a-f]{64}", manifest[key]) is not None,
                    f"missing SHA256 build identity: {key}")
        require(len(manifest["sections"]) == 5, "five passages required")
    require(a["variant"] == "baseline" and b["variant"] == "candidate",
            "A must be baseline; B must be candidate")
    require(b["candidate_apis_available"] == 1, "B lacks candidate APIs")
    for section_a, section_b, (slug, _, _) in zip(a["sections"], b["sections"], SCENES):
        require(section_a["slug"] == section_b["slug"] == slug, "passage order differs")
        for key in ("start_frame", "frames", "latency_samples", "initial_patch", "events"):
            require(section_a[key] == section_b[key], f"passage contract differs: {slug}/{key}")
        require(0 <= section_a["latency_samples"] < 4096, "invalid declared latency")
    require(a["montage"]["frames"] == b["montage"]["frames"] == RATE * 25,
            "montage must be25s in both builds")


def load_pair(directory, label_a, label_b):
    manifests = []
    recordings = []
    hashes = {}
    for label in (label_a, label_b):
        require(re.fullmatch(r"[A-Za-z0-9_.-]+", label) is not None
                and label not in (".", ".."), "unsafe archive label")
        raw = directory / "raw" / label
        manifest_path = raw / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        require(manifest["label"] == label, "archive label differs from its manifest")
        montage = read_archive(raw / "montage.wav")
        require(len(montage) == manifest["montage"]["frames"], "montage frames differ from manifest")
        require(fnv_float(montage) == manifest["montage"]["float_fnv1a64"], "montage sample identity differs")
        parts = []
        end = 0
        label_hashes = {"manifest.json": sha256(manifest_path), "montage.wav": sha256(raw / "montage.wav")}
        for section in manifest["sections"]:
            slug = section["slug"]
            require(slug in [x[0] for x in SCENES], "unknown passage")
            part_path = raw / f"{slug}.wav"
            part = read_archive(part_path)
            require(section["start_frame"] == end and len(part) == section["frames"],
                    "passage duration/offset differs from manifest")
            require(fnv_float(part) == section["float_fnv1a64"], "passage sample identity differs")
            require(np.array_equal(part, montage[end:end + len(part)]), "passage differs from montage slice")
            end += len(part)
            parts.append(part)
            label_hashes[f"{slug}.wav"] = sha256(part_path)
        require(end == len(montage), "passages do not cover montage")
        require(rms(montage) > 1e-12, "silent montage")
        manifests.append(manifest)
        recordings.append({"montage": montage, **{x[0]: part for x, part in zip(SCENES, parts)}})
        hashes[label] = label_hashes
    check_manifest(*manifests)
    return manifests, recordings, hashes


def gains(a, b):
    require(a.shape == b.shape and a.ndim == 2 and a.shape[1] == 2, "A/B frame counts differ")
    require(np.isfinite(a).all() and np.isfinite(b).all(), "non-finite pair")
    require(min(rms(a), rms(b)) > 1e-12, "silent pair")
    match_b = rms(a) / rms(b)
    residual = b * match_b - a
    residual_rms = rms(residual)
    boost = rms(a) / residual_rms if residual_rms > 0 else 1.0
    common = PEAK / max(peak(a), peak(b) * match_b, peak(residual), peak(residual) * boost)
    return match_b, common, boost


def pcm24(path, audio):
    require(np.isfinite(audio).all() and peak(audio) <= PEAK + 1e-12,
            "PCM24 listening copy exceeds its-6dB peak ceiling")
    integer = np.rint(audio * 8388608.0).astype(np.int32)
    require(int(np.max(np.abs(integer.astype(np.int64)))) < 8388608, "PCM24 would clip")
    unsigned = integer.astype(np.uint32).reshape(-1)
    packed = np.empty((len(unsigned), 3), dtype=np.uint8)
    packed[:, 0] = unsigned & 255
    packed[:, 1] = (unsigned >> 8) & 255
    packed[:, 2] = (unsigned >> 16) & 255
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(2)
        stream.setsampwidth(3)
        stream.setframerate(RATE)
        stream.writeframes(packed.tobytes())
    # Measure the actual delivered quantized sample values, not just targets.
    return integer.astype(np.float64) / 8388608.0


def package(directory, label_a="A", label_b="B"):
    directory = directory.resolve()
    manifests, recordings, hashes = load_pair(directory, label_a, label_b)
    a, b = (x["montage"] for x in recordings)
    match_b, common, boost = gains(a, b)
    output = directory / "listening"
    require(not output.exists(), "listening output already exists; preserve frozen review evidence")
    output.mkdir()
    report = {
        "protocol": PROTOCOL,
        "packager_source_sha256": sha256(Path(__file__)),
        "manifest_sha256": {x: hashes[x]["manifest.json"] for x in hashes},
        "archive_sha256": hashes,
        "baseline_revision": manifests[0]["git_revision"],
        "candidate_revision": manifests[1]["git_revision"],
        "archive_labels": {"A": label_a, "B": label_b},
        "dsp_source_sha256": {"A": manifests[0]["dsp_source_sha256"], "B": manifests[1]["dsp_source_sha256"]},
        "renderer_source_sha256": manifests[0]["renderer_source_sha256"],
        "sample_rate": RATE,
        "matching_scope": "whole25s montage; identical gains reused on every passage",
        "A_gain": common, "B_gain": common * match_b,
        "A_trim_db": db(common), "B_trim_db": db(common * match_b),
        "B_rms_match_db": db(match_b), "common_headroom_trim_db": db(common),
        "difference_normalization_gain": boost, "difference_normalization_db": db(boost),
        "difference_is_exactly_zero": bool(np.array_equal(a, b * match_b)),
        "difference_convention": "Bmatched-Amatched; normalized copy additionally uses the one montage boost",
        "peak_ceiling_dbfs": -6,
        "timing": "declared equal processing latency only; no time alignment",
        "causal_scope": "all five candidate mechanisms active in every B passage",
        "sections": {},
    }
    root_manifest = directory / "build-manifest.json"
    if root_manifest.exists():
        report["build_manifest_sha256"] = sha256(root_manifest)
    for slug in ("montage", *(x[0] for x in SCENES)):
        part_a, part_b = (x[slug] for x in recordings)
        matched_a, matched_b = part_a * common, part_b * match_b * common
        diff = matched_b - matched_a
        normalized = diff * boost
        scene_dir = output / slug
        scene_dir.mkdir()
        delivered = {}
        for name, samples in (("A", matched_a), ("B", matched_b), ("Diff-true", diff), ("Diff-normalized", normalized)):
            # Common headroom covers both the true and normalized residual.
            if name == "Diff-true":
                require(peak(samples) < 1, "true residual would clip; retain float archive")
                wavfile.write(scene_dir / "Diff-true-float.wav", RATE, samples.astype(np.float32))
                if peak(samples) > PEAK:
                    raise ValueError("true residual exceeds common playback ceiling")
            delivered[name] = pcm24(scene_dir / f"{name}.wav", samples)
        gap = np.zeros((int(.75 * RATE), 2))
        pcm24(scene_dir / "AB.wav", np.concatenate((matched_a, gap, matched_b)))
        rms_delta = db(rms(delivered["B"]) / max(rms(delivered["A"]), 1e-18))
        if slug == "montage":
            require(abs(rms_delta) < .001, "delivered montage RMS matching exceeds tolerance")
        reference = rms(matched_a)
        metrics = {
            "frames": len(part_a), "seconds": len(part_a) / RATE,
            "raw": {"A": levels(part_a), "B": levels(part_b)},
            "raw_B_minus_A_rms_db": db(rms(part_b) / max(rms(part_a), 1e-18)),
            "raw_unmatched_difference_rms_dbc": db(rms(part_b - part_a) / max(rms(part_a), 1e-18)),
            "matched_difference_rms_dbc": db(rms(diff) / max(reference, 1e-18)),
            "matched_difference_peak_relative_A_rms_db": db(peak(diff) / max(reference, 1e-18)),
            "delivered_rms_B_minus_A_db": rms_delta,
            "normalization_boost_db": db(boost),
            "delivered": {name: levels(audio) for name, audio in delivered.items()},
            "delivered_sha256": {path.name: sha256(path) for path in sorted(scene_dir.glob("*.wav"))},
        }
        report["sections"][slug] = metrics
    (directory / "metrics.json").write_text(json.dumps(report, indent=2) + "\n")
    write_player(directory, report)
    return report


def write_player(directory, report):
    label_a = report["archive_labels"]["A"]
    label_b = report["archive_labels"]["B"]
    sections = []
    for slug, title, guidance in (("montage", "Complete25s comparison", "All five passages, with their relative levels preserved."), *SCENES):
        players = (f'<div class="switch-player" data-slug="{slug}">'
                   '<div class="choices"><button data-choice="A" aria-pressed="true">A · Before</button>'
                   '<button data-choice="B" aria-pressed="false">B · After</button>'
                   '<button data-choice="Diff-normalized" aria-pressed="false">Difference · normalized</button></div>'
                   '<div class="transport"><button class="play">Play</button><input class="seek" type="range" min="0" max="25" value="0" step=".001" aria-label="Playback position">'
                   '<span class="position">0:00</span><span class="status" role="status"></span></div></div>')
        metric = report["sections"][slug]
        sections.append(f'<section><h2>{html.escape(title)}</h2><p>{html.escape(guidance)}</p><div class="players">{players}</div>'
                        f'<p class="small">Residual {metric["matched_difference_rms_dbc"]:.3f}dBc; difference boost {report["difference_normalization_db"]:+.3f}dB. '
                        f'<a href="listening/{slug}/A.wav">A file</a> · <a href="listening/{slug}/B.wav">B file</a> · '
                        f'<a href="listening/{slug}/AB.wav">A then B, with0.75s gap</a> · '
                        f'<a href="listening/{slug}/Diff-true.wav">True unboosted difference</a> · '
                        f'<a href="listening/{slug}/Diff-true-float.wav">Float residual archive</a></p></section>')
    (directory / "index.html").write_text("""<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>YouKnow hardware realism comparison</title><link rel="icon" href="data:,"><style>
body{max-width:1100px;margin:44px auto;padding:0 24px;background:#faf9f6;color:#172126;font:16px/1.6 system-ui,sans-serif}h1{font-size:32px;line-height:1.2}h2{font-size:21px}section{padding:20px 0;border-top:1px solid #c6cdcf}.switch-player{width:100%}.choices,.transport{display:flex;flex-wrap:wrap;gap:12px;margin:12px 0;align-items:center}button{padding:9px 15px;border:1px solid #879599;border-radius:5px;background:white;color:#172126;font:inherit;cursor:pointer}button[aria-pressed=true]{background:#07566d;color:white}.seek{flex:1;min-width:170px;accent-color:#07566d}.status{font-size:13px}.small{font-size:14px;color:#455259}a{color:#07566d}p{max-width:950px}
</style><h1>YouKnow · hardware realism comparison</h1>
<p>A is the archived starting DSP. B enables the five hardware-grounded candidates. Both render the same25s score through the product engine at48kHz, requested1× quality, six voices and LINE output. These are software comparisons; the difference shows change, not hardware accuracy.</p>
<p>Choose A or B, press Play and switch letters while listening. The three streams run together on the same audio clock, so switching preserves their sample position. One whole-montage stereo RMS match applies to every passage. The normalized difference uses one common boost; it is intentionally exaggerated. A, B and that difference share one peak trim, preserving their RMS relationship with at least6dB headroom. An exact-null difference stays silent.</p>
<p class="small">The passages emphasize individual mechanisms while all five candidates remain enabled in B. They are not isolated one-change tests. Quiet tails stay quiet; no passage receives its own loudness trim. <a href="metrics.json">Exact trims, residuals and archive hashes</a> · <a href="raw/A/manifest.json">A render manifest</a> · <a href="raw/B/manifest.json">B render manifest</a></p>
""".replace("raw/A/manifest.json", f"raw/{label_a}/manifest.json")
    .replace("raw/B/manifest.json", f"raw/{label_b}/manifest.json") + "\n".join(sections) + """
<script>
let context=null,active=null,transportRequest=0;
const choiceNames=['A','B','Diff-normalized'];
const clock=t=>`${Math.floor(t/60)}:${String(Math.floor(t%60)).padStart(2,'0')}`;
for(const element of document.querySelectorAll('.switch-player')){
 const play=element.querySelector('.play'),seek=element.querySelector('.seek');
 const position=element.querySelector('.position'),status=element.querySelector('.status');
 let buffers=null,loading=null,sources=[],gains=[],choice='A',playing=false,offset=0,started=0,request=0;
 const current=()=>playing?Math.min(buffers[0].duration,offset+Math.max(0,context.currentTime-started)):offset;
 const pause=()=>{if(playing)offset=current();playing=false;for(const source of sources){source.stop();source.disconnect();}for(const gain of gains)gain.disconnect();sources=[];gains=[];play.textContent='Play';};
 const load=async()=>{
  if(buffers)return;
  if(!loading){status.textContent='Loading…';loading=Promise.all(choiceNames.map(async name=>{
   const response=await fetch(`listening/${element.dataset.slug}/${name}.wav`);
   if(!response.ok)throw Error(`Audio unavailable (${response.status})`);
   return context.decodeAudioData(await response.arrayBuffer());
  }));}
  buffers=await loading;seek.max=buffers[0].duration;status.textContent='';
 };
 const start=async()=>{
  const token=++request,globalToken=++transportRequest;
  try{
   if(!context)context=new (window.AudioContext||window.webkitAudioContext)();
   await context.resume();await load();if(token!==request||globalToken!==transportRequest)return;
   if(active&&active!==pause)active();active=pause;
   if(offset>=buffers[0].duration)offset=0;
   started=context.currentTime+.03;playing=true;
   sources=buffers.map((buffer,index)=>{
    const source=context.createBufferSource(),gain=context.createGain();
    source.buffer=buffer;gain.gain.value=choiceNames[index]===choice?1:0;
    source.connect(gain);gain.connect(context.destination);gains.push(gain);
    source.start(started,offset);return source;
   });play.textContent='Pause';
  }catch(error){playing=false;loading=null;status.textContent=`${error.message}. Use the audio file links below.`;}
 };
 play.addEventListener('click',()=>{if(playing){++request;++transportRequest;pause();}else start();});
 for(const button of element.querySelectorAll('[data-choice]'))button.addEventListener('click',()=>{
  choice=button.dataset.choice;
  for(const other of element.querySelectorAll('[data-choice]'))other.setAttribute('aria-pressed',String(other===button));
  const switchAt=context?context.currentTime+.005:0;
  for(let index=0;index<gains.length;index++)gains[index].gain.setValueAtTime(choiceNames[index]===choice?1:0,switchAt);
 });
 seek.addEventListener('input',()=>{const resume=playing;++request;++transportRequest;pause();offset=Number(seek.value);position.textContent=clock(offset);if(resume)start();});
 const update=()=>{if(playing){const t=current();seek.value=t;position.textContent=clock(t);if(t>=buffers[0].duration){pause();offset=0;seek.value=0;}}requestAnimationFrame(update);};update();
}
</script></html>
""")


def self_test():
    # Pure gain must disappear under RMS matching; an additive harmonic must
    # survive with its sign intact and the promised normalized RMS/headroom.
    time = np.arange(8192) / RATE
    a = np.column_stack((.2 * np.sin(2 * np.pi * 440 * time), .15 * np.sin(2 * np.pi * 330 * time)))
    match, common, boost = gains(a, 2 * a)
    require(match == .5 and np.array_equal(2 * a * match - a, np.zeros_like(a)), "pure gain did not null")
    b = a + .012 * np.column_stack((np.sin(2 * np.pi * 880 * time), np.sin(2 * np.pi * 660 * time)))
    match, common, boost = gains(a, b)
    diff = (b * match - a) * common
    require(np.allclose(diff, b * (match * common) - a * common, atol=1e-16), "difference sign/gains differ")
    require(abs(db(rms(diff * boost) / rms(a * common))) < 1e-10, "normalized residual RMS differs")
    require(max(peak(a * common), peak(b * match * common), peak(diff * boost)) <= PEAK + 1e-15,
            "common peak trim failed")
    with tempfile.TemporaryDirectory(prefix="youknow-realism-package-") as temporary:
        path = Path(temporary) / "test.wav"
        quantized = pcm24(path, a * common)
        with wave.open(str(path), "rb") as stream:
            require((stream.getnchannels(), stream.getsampwidth(), stream.getframerate(), stream.getnframes())
                    == (2, 3, RATE, len(a)), "PCM24 header failed")
        require(np.max(np.abs(quantized - a * common)) <= 1 / 16777216, "PCM24 quantization failed")
    manifest = {key: "same" for key in CONTRACT}
    manifest.update(protocol=PROTOCOL, renderer_source_sha256="1" * 64,
                    dsp_source_sha256="2" * 64, sample_rate=RATE, quality_requested=1,
                    block_size=128, polyphony=6, output_route="LINE", variant="baseline",
                    candidate_apis_available=0, montage={"frames": RATE * 25})
    manifest["sections"] = [dict(slug=slug, start_frame=index * 10, frames=10,
                                 latency_samples=8, initial_patch={}, events=[])
                            for index, (slug, _, _) in enumerate(SCENES)]
    candidate = json.loads(json.dumps(manifest))
    candidate.update(variant="candidate", candidate_apis_available=1)
    check_manifest(manifest, candidate)
    for mutation, reason in ((lambda m: m.update(sample_rate=44100), "rate"),
                             (lambda m: m["sections"][0].update(frames=11), "frames"),
                             (lambda m: m["sections"][0].update(latency_samples=9), "latency/delay")):
        invalid = json.loads(json.dumps(candidate))
        mutation(invalid)
        try:
            check_manifest(manifest, invalid)
        except ValueError:
            pass
        else:
            raise ValueError(f"{reason} mismatch was accepted")
    require(fnv_float(a) != fnv_float(np.roll(a, 1, axis=0)), "one-sample shift failed archive identity check")
    # Exercise the complete packaging path using the exact C++ WAV layout,
    # including25s montage slices, archive binding, relative section levels,
    # delivered headers/hashes and refusal to replace frozen evidence.
    with tempfile.TemporaryDirectory(prefix="youknow-realism-integration-") as temporary:
        directory = Path(temporary)
        seconds = (8, 5, 4, 4, 4)
        for label, source, variant in (("A", a, "baseline"), ("B", b * 1.09, "candidate")):
            raw = directory / "raw" / label
            raw.mkdir(parents=True)
            test_manifest = json.loads(json.dumps(manifest))
            test_manifest.update(label=label, git_revision="synthetic", variant=variant,
                                 candidate_apis_available=int(variant == "candidate"))
            parts = [np.tile(source, (int(np.ceil(length * RATE / len(source))), 1))[:length * RATE]
                     * (1 - .12 * index) for index, length in enumerate(seconds)]
            montage = np.concatenate(parts)

            def write_float(path, samples):
                payload = np.asarray(samples, dtype="<f4", order="C").tobytes()
                header = struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + len(payload),
                                     b"WAVE", b"fmt ", 16, 3, 2, RATE, RATE * 8, 8, 32,
                                     b"data", len(payload))
                path.write_bytes(header + payload)

            offset = 0
            for section, samples in zip(test_manifest["sections"], parts):
                section.update(start_frame=offset, frames=len(samples), float_fnv1a64=fnv_float(samples))
                write_float(raw / f'{section["slug"]}.wav', samples)
                offset += len(samples)
            test_manifest["montage"].update(float_fnv1a64=fnv_float(montage))
            write_float(raw / "montage.wav", montage)
            (raw / "manifest.json").write_text(json.dumps(test_manifest))
        archive_hashes = {str(path): sha256(path) for path in (directory / "raw").rglob("*") if path.is_file()}
        report = package(directory)
        require(all(sha256(Path(path)) == digest for path, digest in archive_hashes.items()), "packaging modified raw archives")
        require(abs(report["sections"]["montage"]["delivered_rms_B_minus_A_db"]) < .001, "packaged RMS match failed")
        require(abs(report["sections"]["montage"]["delivered"]["Diff-normalized"]["rms_dbfs"]
                    - report["sections"]["montage"]["delivered"]["A"]["rms_dbfs"]) < .001,
                "packaged normalized residual RMS differs")
        for slug, _, _ in SCENES:
            with wave.open(str(directory / "listening" / slug / "AB.wav"), "rb") as stream:
                require(stream.getnframes() == report["sections"][slug]["frames"] * 2 + int(.75 * RATE), "A-gap-B duration differs")
        try:
            package(directory)
        except ValueError:
            pass
        else:
            raise ValueError("frozen listening evidence was overwritten")
        shifted = read_archive(directory / "raw" / "B" / "montage.wav")
        write_float(directory / "raw" / "B" / "montage.wav", np.roll(shifted, 1, axis=0))
        try:
            load_pair(directory, "A", "B")
        except ValueError as error:
            require("sample identity" in str(error), "shifted archive rejected for unexpected reason")
        else:
            raise ValueError("one-sample archive shift was accepted")
    print("Self-test passed: gain null, signed residual, normalized RMS/headroom, PCM24, full25s packaging, frozen archives and rate/frame/latency/shift rejection.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, nargs="?")
    parser.add_argument("label_a", nargs="?", default="A")
    parser.add_argument("label_b", nargs="?", default="B")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    if args.directory:
        report = package(args.directory, args.label_a, args.label_b)
        print(json.dumps({"A_trim_db": report["A_trim_db"], "B_trim_db": report["B_trim_db"],
                          "difference_normalization_db": report["difference_normalization_db"],
                          "montage_residual_dbc": report["sections"]["montage"]["matched_difference_rms_dbc"],
                          "player": str(args.directory.resolve() / "index.html")}, indent=2))
    elif not args.self_test:
        parser.error("directory or --self-test required")


if __name__ == "__main__":
    main()
