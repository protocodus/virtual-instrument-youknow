#!/usr/bin/env python3
"""Package the Release x64 VST3, CLAP and standalone artifacts for distribution."""

import argparse
import hashlib
from pathlib import Path
import re
import zipfile


def package(build_dir: Path) -> Path:
    project_dir = Path(__file__).resolve().parent.parent
    cache = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8")
    versions = re.findall(r"^CMAKE_PROJECT_VERSION:STATIC=(.+)$", cache, re.MULTILINE)
    if len(versions) != 1 or not re.fullmatch(r"[0-9]+(?:\.[0-9]+){1,3}", versions[0]):
        raise ValueError("CMakeCache.txt must contain one valid project version")

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

    dist = build_dir / "dist"
    dist.mkdir(parents=True, exist_ok=True)
    archive = dist / f"YouKnow-{versions[0]}-Windows-x64.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as output:
        for relative, path in sorted(files.items()):
            output.write(path, relative)
    with zipfile.ZipFile(archive) as output:
        damaged = output.testzip()
        if damaged is not None:
            raise ValueError(f"Corrupt ZIP member: {damaged}")
    digest = hashlib.sha256()
    with archive.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    (dist / "SHA256SUMS.txt").write_text(
        f"{digest.hexdigest()}  {archive.name}\n", encoding="utf-8"
    )
    return archive


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build-win"))
    args = parser.parse_args()
    print(f"Packaged {package(args.build_dir.resolve())}")
