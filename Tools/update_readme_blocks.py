#!/usr/bin/env python3
"""Rewrite the build-owned README blocks of a Protocodus product (G3, G5, G6).

Master copy: reason-rack-extensions/Tools/portfolio/Tools/update_readme_blocks.py.
Every canonical repo carries a byte-identical copy in Tools/, and
Tools/check_repo_standard.py fails on a drifted one.

  update_readme_blocks.py demos --readme README.md --manifest <out-dir>/demos.json
      rewrites the table between <!-- audio-demos-begin --> and
      <!-- audio-demos-end --> from the manifest Render<Name>PresetDemos wrote,
      after checking that every MP3 it names is beside it

  update_readme_blocks.py download --readme README.md --url <artifact-url>
          --build <run-number> --commit <sha> [--release-url URL --release-tag vX.Y.Z]
      rewrites the block between <!-- build-download-begin --> and
      <!-- build-download-end --> with the main build's collected artifact and,
      once there is one, the latest GitHub Release (G5a)

Either command fails, leaving the README untouched, when its marker pair is
missing, repeated or out of order, so a build cannot silently skip a block.
Python 3.12 standard library only.
"""

import argparse
import json
import os
from pathlib import Path
import re
import sys

AUDIO_LINK_PREFIX = "Docs/audio/"
DEMO_FILE = re.compile(r"[0-9]{2,}-[a-z0-9]+(?:-[a-z0-9]+)*\.mp3")


class BlockError(Exception):
    pass


def replace_block(text, name, body):
    """Replace what sits between one ordered <!-- name-begin ... --> / <!-- name-end --> pair."""
    begin = re.compile(rf"<!-- {re.escape(name)}-begin(?:\s[^>]*)?-->")
    end = f"<!-- {name}-end -->"
    begins = list(begin.finditer(text))
    if len(begins) != 1 or text.count(end) != 1 or text.index(end) < begins[0].end():
        raise BlockError(f"README needs exactly one ordered <!-- {name}-begin --> / {end} pair")
    return text[: begins[0].end()] + body + text[text.index(end):]


def demo_table(manifest_path):
    manifest = json.loads(Path(manifest_path).read_text(encoding="utf-8"))
    demos = manifest.get("demos") or []
    if not demos:
        raise BlockError(f"{manifest_path}: lists no demos")
    rows = ["", "", "| # | Preset | Demo | Length | Peak |", "| --- | --- | --- | --- | --- |"]
    for expected, demo in enumerate(demos, start=1):
        name = demo.get("file", "")
        if demo.get("number") != expected or not DEMO_FILE.fullmatch(name):
            raise BlockError(f"{manifest_path}: demo {expected} is not a numbered <NN>-<slug>.mp3 in program order")
        mp3 = Path(manifest_path).parent / name
        data = mp3.read_bytes() if mp3.is_file() else b""
        if len(data) < 4096 or not (data[:3] == b"ID3" or data[0] == 0xFF):
            raise BlockError(f"{name} is missing or not a complete MP3")
        preset = str(demo["preset"]).replace("|", "\\|")
        rows.append(f"| {expected} | {preset} | [{name}]({AUDIO_LINK_PREFIX}{name}) "
                    f"| {demo['seconds']:.1f} s | {demo['peak_dbfs']:.1f} dBFS |")
    return "\n".join(rows) + "\n\n"


def download_block(url, build, commit, repository, release_url, release_tag):
    if not re.fullmatch(rf"https://github\.com/{re.escape(repository)}/actions/runs/[0-9]+/artifacts/[0-9]+", url):
        raise BlockError(f"--url must be a GitHub Actions artifact URL of {repository}")
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise BlockError("--commit must be a full 40-character commit SHA")
    if not build.isdigit():
        raise BlockError("--build must be the main build's run number")
    lines = [""]
    if release_url:
        lines += [f"**[Latest release {release_tag}]({release_url})**: signed packages and `SHA256SUMS`.", ""]
    lines.append(f"**[Download main build {build}]({url})**, built from "
                 f"[`{commit[:12]}`](https://github.com/{repository}/commit/{commit}). "
                 "It holds the macOS, Windows and Linux packages, `SHA256SUMS` and `BUILD.txt`.")
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    demos = commands.add_parser("demos", help="rewrite the G3 audio demo table")
    demos.add_argument("--readme", required=True)
    demos.add_argument("--manifest", required=True)
    download = commands.add_parser("download", help="rewrite the G5 download block")
    download.add_argument("--readme", required=True)
    download.add_argument("--url", required=True)
    download.add_argument("--build", required=True)
    download.add_argument("--commit", required=True)
    download.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY"))
    download.add_argument("--release-url")
    download.add_argument("--release-tag")
    args = parser.parse_args(argv)

    try:
        readme = Path(args.readme)
        text = readme.read_text(encoding="utf-8")
        if args.command == "demos":
            text = replace_block(text, "audio-demos", demo_table(args.manifest))
        else:
            if not args.repository:
                raise BlockError("--repository (or GITHUB_REPOSITORY) is required")
            if bool(args.release_url) != bool(args.release_tag):
                raise BlockError("--release-url and --release-tag go together")
            text = replace_block(text, "build-download", download_block(
                args.url, args.build, args.commit, args.repository, args.release_url, args.release_tag))
        readme.write_text(text, encoding="utf-8")
    except (BlockError, OSError, KeyError, ValueError, TypeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
