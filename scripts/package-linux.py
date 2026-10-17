#!/usr/bin/env python3
"""Package the Release x64 VST3, CLAP and standalone artifacts for distribution."""

import argparse
import hashlib
from pathlib import Path
import re
import tarfile
import tempfile


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
    # A bundle directory or executable bit survives an interrupted or truncated
    # build; require every actual payload before archiving.
    required = (
        "VST3/YouKnow.vst3/Contents/x86_64-linux/YouKnow.so",
        "CLAP/YouKnow.clap",
        "Standalone/YouKnow",
    )
    for relative in required:
        path = artifacts / relative
        if not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f"missing or empty Linux x64 binary: {path}")
    if not (artifacts / "Standalone/YouKnow").stat().st_mode & 0o111:
        raise ValueError("the Linux standalone is not executable")

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
    ):
        files[relative] = project_dir / relative
    # The customer guide and the installation guides it links sit together at
    # the archive root.
    guide = project_dir / "Docs" / "guide"
    for name in ("install-macos.md", "install-windows.md", "install-linux.md"):
        files[name] = guide / name
    files["README.md"] = guide / "user-guide.md"
    # Retain the licence filename shipped in earlier Linux archives.
    files["JUCE-LICENSE.md"] = project_dir / "ThirdParty" / "JUCE-LICENSE.md"
    for path in files.values():
        if not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f"missing or empty distribution file: {path}")

    dist = build_dir / "dist"
    dist.mkdir(parents=True, exist_ok=True)
    archive = dist / f"YouKnow-{distribution_version}-Linux-x64.tar.gz"
    # Complete and verify both files before replacing any previous package, so
    # a failed write never leaves an archive its checksum does not cover.
    with tempfile.TemporaryDirectory(prefix=".youknow-package-", dir=dist) as work:
        staged_archive = Path(work) / archive.name
        staged_checksums = Path(work) / "SHA256SUMS.txt"
        with tarfile.open(staged_archive, "w:gz") as output:
            for relative, path in sorted(files.items()):
                output.add(path, relative, recursive=False)
        with tarfile.open(staged_archive) as output:
            output.getmembers()
        digest = hashlib.sha256()
        with staged_archive.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        staged_checksums.write_text(
            f"{digest.hexdigest()}  {archive.name}\n", encoding="utf-8"
        )
        staged_archive.replace(archive)
        staged_checksums.replace(dist / "SHA256SUMS.txt")

    # A reused build directory must not upload older builds alongside the new
    # archive while its checksum file covers only the current build.
    for previous in dist.glob("YouKnow-*-Linux-x64.tar.gz"):
        if previous != archive:
            previous.unlink()
    return archive


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build-dsp"))
    args = parser.parse_args()
    print(f"Packaged {package(args.build_dir.resolve())}")
