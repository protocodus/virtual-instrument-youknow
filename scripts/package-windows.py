#!/usr/bin/env python3
"""Package the Release x64 VST3, CLAP and standalone artifacts for distribution."""

import argparse
import hashlib
from pathlib import Path
import re
import tempfile
import zipfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from distribution import identity, output_dir, manifest, require_new_destination


def package(build_dir: Path) -> Path:
    project_dir = Path(__file__).resolve().parent.parent
    cache = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8")
    versions = re.findall(r"^CMAKE_PROJECT_VERSION:STATIC=(.+)$", cache, re.MULTILINE)
    if len(versions) != 1 or not re.fullmatch(r"[0-9]+(?:\.[0-9]+){2}", versions[0]):
        raise ValueError("CMakeCache.txt must contain one valid project version")
    build_numbers = re.findall(r"^YOUKNOW_BUILD_NUMBER:STRING=(.+)$", cache, re.MULTILINE)
    if len(build_numbers) != 1 or not re.fullmatch(r"[1-9][0-9]*(?:\.[1-9][0-9]*)?", build_numbers[0]):
        raise ValueError("CMakeCache.txt must contain one valid build number")
    distribution_versions = re.findall(
        r"^YOUKNOW_DISTRIBUTION_VERSION:INTERNAL=(.+)$", cache, re.MULTILINE
    )
    distribution_version = f"{versions[0]}-build.{build_numbers[0]}"
    if distribution_versions != [distribution_version]:
        raise ValueError("CMakeCache.txt distribution version must match project version and build number")

    artifacts = build_dir / "YouKnow_artefacts" / "Release"
    required = (
        "VST3/YouKnow.vst3/Contents/x86_64-win/YouKnow.vst3",
        "CLAP/YouKnow.clap",
        "Standalone/YouKnow.exe",
    )
    for relative in required:
        path = artifacts / relative
        if not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f"Missing or empty Windows x64 binary: {path}")

    files = {
        path.relative_to(artifacts).as_posix(): path
        for path in (artifacts / "VST3" / "YouKnow.vst3").rglob("*")
        if path.is_file()
    }
    for relative in required[1:]:
        files[relative] = artifacts / relative
    for relative in (
        "LICENSE", "THIRD_PARTY_NOTICES.md", "PRIVACY.md",
        "ThirdParty/JUCE-LICENSE.md", "ThirdParty/CLAP-LICENSE.md",
        "INSTALL_MACOS.md", "INSTALL_WINDOWS.md", "INSTALL_LINUX.md",
    ):
        files[relative] = project_dir / relative
    files["README.md"] = project_dir / "USER_GUIDE.md"
    for path in files.values():
        if not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f"Missing or empty distribution file: {path}")

    compiled = identity(build_dir, formats=("VST3", "CLAP", "Standalone"))
    dist = output_dir(compiled, "Windows", "x64")
    require_new_destination(dist)
    dist.mkdir(parents=True, exist_ok=True)
    archive = dist / f"YouKnow-{distribution_version}-Windows-x64.zip"
    # Complete and verify both files before replacing any previous package.
    # A failed ZIP write used to truncate the old archive while leaving its
    # checksum unchanged. Stage on the same filesystem for atomic file moves;
    # the context also removes partial work on an exception.
    with tempfile.TemporaryDirectory(prefix=".youknow-package-", dir=dist) as work:
        staged_archive = Path(work) / archive.name
        staged_checksums = Path(work) / (archive.stem + "-SHA256SUMS.txt")
        with zipfile.ZipFile(staged_archive, "w", compression=zipfile.ZIP_DEFLATED) as output:
            for relative, path in sorted(files.items()):
                output.write(path, relative)
        with zipfile.ZipFile(staged_archive) as output:
            damaged = output.testzip()
            if damaged is not None:
                raise ValueError(f"Corrupt ZIP member: {damaged}")
        digest = hashlib.sha256()
        with staged_archive.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        manifest(compiled, Path(work), "Windows", "x64", [staged_archive])
        for staged in Path(work).iterdir():
            staged.replace(dist / staged.name)

    return archive


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build-win"))
    args = parser.parse_args()
    print(f"Packaged {package(args.build_dir.resolve())}")
