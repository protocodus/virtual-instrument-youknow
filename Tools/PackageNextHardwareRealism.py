#!/usr/bin/env python3
"""Package RenderNextHardwareRealism archives using the verified core packager.

Usage: PackageNextHardwareRealism.py DIR A B, or --self-test.
The original comparison renderer/packager protocol stays unchanged. This
driver selects the further-five score, preserves one montage-wide RMS match,
and records both Python source fingerprints. Differences quantify change,
not closeness to hardware.
"""

import argparse
import json
from pathlib import Path

import PackageHardwareRealism as core


core.PROTOCOL = "youknow-next-hardware-realism-v1"
core.SCENES = (
    ("01-chorus-insertion", "Chorus insertion gain",
     "Direct timing in both versions. Listen to wet balance and comb coloration in Modes I and II."),
    ("02-quiet-temperature", "Quiet tails at the settled temperature prior",
     "Direct timing in both versions. Two decays/releases use 5% and 2% sustain; their true quiet levels are preserved."),
    ("03-resonance-offset", "Resonance offset estimate",
     "Direct timing in both versions. Listen to resonant color and transients as resonance reaches full travel."),
    ("04-panel-routing", "Physical panel gestures",
     "Original timing in both versions. The same cutoff, PWM and sub gesture snapshots exercise the panel input adapter."),
    ("05-full-tone-routing", "Received full-tone MIDI",
     "Original timing in both versions. Changed and identical 24-byte tone frames exercise message identity and ordering."),
)
core.CONTRACT += ("firmware_event_sources", "performance_timing")

_write_player = core.write_player


def write_player(directory, report):
    _write_player(directory, report)
    path = directory / "index.html"
    text = path.read_text()
    text = text.replace(
        "The passages emphasize individual mechanisms while all five candidates remain enabled in B. They are not isolated one-change tests.",
        "The three tone passages use Direct timing in both versions; the two input-routing passages use Original timing in both. "
        "All selected tone estimates remain enabled in B, so these are not isolated one-change tests.")
    path.write_text(text)


core.write_player = write_player


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", nargs="?", type=Path)
    parser.add_argument("baseline", nargs="?", default="A")
    parser.add_argument("candidate", nargs="?", default="B")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        core.self_test()
        return
    if args.directory is None:
        parser.error("directory is required")
    for label, expected_apis in ((args.baseline, 0), (args.candidate, 1)):
        core.require(core.re.fullmatch(r"[A-Za-z0-9_.-]+", label) is not None
                     and label not in (".", ".."), "unsafe archive label")
        manifest = json.loads((args.directory / "raw" / label / "manifest.json").read_text())
        core.require(manifest["candidate_apis_available"] == expected_apis,
                     "baseline/candidate archive uses the wrong compiled routing APIs")
        for index, section in enumerate(manifest["sections"]):
            core.require(section.get("performance_timing") == ("Direct" if index < 3 else "Original"),
                         "passage has the wrong performance timing mode")
    report = core.package(args.directory, args.baseline, args.candidate)
    report["packager_core_source_sha256"] = report["packager_source_sha256"]
    report["packager_source_sha256"] = core.sha256(Path(__file__))
    report["causal_scope"] = (
        "Three tone estimates selected in B; the two adapter differences are exercised in Original sections4/5. "
        "Passages are not isolated single-change tests.")
    (args.directory.resolve() / "metrics.json").write_text(json.dumps(report, indent=2) + "\n")
    (args.directory.resolve() / "key.md").write_text(
        "# Comparison key\n\n"
        "A is the frozen shipping DSP at `629087f`. B includes the three further component estimates "
        "and the two Original input-adapter fixes. The three tone passages use Direct timing on both sides; "
        "the panel-gesture and full-tone MIDI passages use Original timing on both sides. "
        "The comparison does not audition the separate change to the new-instance timing default.\n\n"
        "One stereo RMS match covers the complete 25-second montage and is reused for every excerpt. "
        f"B receives {report['B_rms_match_db']:+.6f} dB matching gain; both sides then receive "
        f"{report['common_headroom_trim_db']:+.6f} dB common headroom trim. "
        "The true difference is `Bmatched - Amatched`. Its normalized audition receives "
        f"{report['difference_normalization_db']:+.6f} dB additional gain, also shared across all excerpts. "
        "The true unboosted difference remains available as PCM24 and float32.\n\n"
        "Only the declared processing latency is removed. There is no time alignment, resampling or "
        "per-excerpt loudness adjustment. Differences show what changed; they do not prove closeness to hardware. "
        "No by-ear verdict has been recorded for this set. Exact gains, residual levels and hashes are in `metrics.json`.\n")
    print(json.dumps({"directory": str(args.directory.resolve()),
                      "B_rms_match_db": report["B_rms_match_db"],
                      "difference_normalization_db": report["difference_normalization_db"],
                      "montage_residual_dbc": report["sections"]["montage"]["matched_difference_rms_dbc"]}))


if __name__ == "__main__":
    main()
