#!/usr/bin/env python3
"""Resolve stable versioned output paths and verify compiled build identities."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

PRODUCT = "YouKnow"
PROJECT = Path(__file__).resolve().parent.parent


def stable_dist_root(project=PROJECT):
    if os.environ.get("DIST_ROOT"):
        return Path(os.environ["DIST_ROOT"]).expanduser().resolve()
    try:
        def git(*args):
            return subprocess.check_output(["git", "-C", str(project), "rev-parse", *args], text=True, stderr=subprocess.DEVNULL).strip()
        top = Path(git("--show-toplevel")).resolve()
        common = Path(git("--path-format=absolute", "--git-common-dir")).resolve()
        return common.parent / project.resolve().relative_to(top) / "dist"
    except (subprocess.CalledProcessError, FileNotFoundError, ValueError):
        return project.resolve() / "dist"


def identity(build, config="Release", formats=("VST3", "Standalone")):
    cache = (build / "CMakeCache.txt").read_text()
    values = {}
    for key, kind, field in (("CMAKE_PROJECT_VERSION", "STATIC", "version"),
            (PRODUCT.upper()+"_BUILD_NUMBER", "STRING", "build_number"),
            (PRODUCT.upper()+"_DISTRIBUTION_VERSION", "INTERNAL", "distribution_version")):
        matches = re.findall(r"^"+key+":"+kind+r"=(.*)$", cache, re.M)
        if len(matches) != 1:
            raise ValueError("CMake cache must contain one valid build number/version identity")
        values[field] = matches[0]
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){2}", values["version"]):
        raise ValueError("CMake cache must contain one valid project version")
    if not re.fullmatch(r"[1-9][0-9]*(?:\.[1-9][0-9]*)?", values["build_number"]):
        raise ValueError("CMake cache must contain one valid build number")
    if values["distribution_version"] != values["version"]+"-build."+values["build_number"]:
        raise ValueError("distribution version must match project version and build number")
    if "BUILD_NUMBER" in os.environ and os.environ["BUILD_NUMBER"] != values["build_number"]:
        raise ValueError("BUILD_NUMBER disagrees with the configured build; rebuild before packaging")
    if "VERSION" in os.environ and os.environ["VERSION"] != values["version"]:
        raise ValueError("VERSION disagrees with the configured version")
    if "GITHUB_RUN_ID" in os.environ and "BUILD_NUMBER" not in os.environ:
        requested = os.environ["GITHUB_RUN_ID"]+"."+os.environ.get("GITHUB_RUN_ATTEMPT", "1")
        if requested != values["build_number"]:
            raise ValueError("CI run identity disagrees with the configured build")
    configured = json.loads((build / (PRODUCT+"-build-identity.json")).read_text())
    for key, value in {"product": PRODUCT, **values}.items():
        if configured.get(key) != value:
            raise ValueError("configured identity mismatch: "+key)
    for fmt in formats:
        compiled = build / (PRODUCT+"_artefacts") / config / fmt / (PRODUCT+"-build-identity.json")
        if json.loads(compiled.read_text()) != configured:
            raise ValueError("compiled identity mismatch; rebuild "+fmt)
    return configured


def output_dir(record, platform, arch):
    if platform not in ("macOS", "Windows", "Linux") or arch not in ("universal", "arm64", "x86_64", "x64"):
        raise ValueError("unsupported platform/architecture")
    return stable_dist_root() / record["version"] / ("build."+record["build_number"]) / (platform+"-"+arch)


def require_new_destination(directory):
    if directory.exists() and any(directory.iterdir()):
        raise ValueError("distribution identity already exists; choose a new build number: "+str(directory))


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024*1024), b""):
            h.update(block)
    return h.hexdigest()


def manifest(record, directory, platform, arch, files):
    stem = PRODUCT+"-"+record["distribution_version"]+"-"+platform+"-"+arch
    artifacts = {path.name: digest(path) for path in files}
    target = directory / (stem+".manifest.json")
    target.write_text(json.dumps({**record, "platform":platform, "architectures":(["arm64", "x86_64"] if arch=="universal" else ["x86_64" if arch=="x64" else arch]),
        "artifacts_sha256":artifacts}, indent=2)+"\n")
    files = [*files, target]
    sums = directory / (stem+"-SHA256SUMS.txt")
    sums.write_text("".join(digest(path)+"  "+path.name+"\n" for path in files))
    return target, sums


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--platform", required=True)
    parser.add_argument("--arch", required=True)
    parser.add_argument("--formats", nargs="+", default=["VST3", "Standalone"])
    parser.add_argument("--field", default="directory", choices=["directory", "source_revision", "source_state"])
    parser.add_argument("--require-new", action="store_true")
    parser.add_argument("--manifest", nargs="*", type=Path)
    args=parser.parse_args()
    record=identity(args.build_dir.resolve(), args.config, args.formats)
    directory=output_dir(record,args.platform,args.arch)
    if args.require_new:
        require_new_destination(directory)
    if args.manifest is not None:
        print(*manifest(record,directory,args.platform,args.arch,args.manifest),sep="\n")
    else:
        print(directory if args.field=="directory" else record[args.field])

if __name__=="__main__":
    main()
