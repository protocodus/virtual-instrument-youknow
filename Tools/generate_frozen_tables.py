#!/usr/bin/env python3
"""Generate (or verify) embedded immutable tables using native DSP builders.

Run with the same compiler and libm when checking exact reproduction: platforms
may differ by a last bit in transcendental functions. Shipping native builds
continue to use their original builders; embedded builds use the checked-in data.
"""
import argparse
import pathlib
import shutil
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--check", action="store_true")
parser.add_argument("--cxx", default="g++")
args = parser.parse_args()
target = root / "Source/DSP/FrozenTables"
with tempfile.TemporaryDirectory(prefix="youknow-frozen-") as temporary:
    temporary = pathlib.Path(temporary)
    generated = temporary / "tables"
    def generate(output, embedded=False):
        for name in ("Engine", "Chorus"):
            executable = temporary / (name + ("Embedded" if embedded else "Native"))
            defines = (["-DYOUKNOW_EMBEDDED_TARGET", "-DYOUKNOW_VERIFY_FROZEN_TABLES"]
                       if embedded else [])
            subprocess.run([args.cxx, "-std=c++20", "-O2", "-ffunction-sections",
                            "-fdata-sections", *defines,
                            str(root / f"Tools/GenerateFrozen{name}Tables.cpp"),
                            "-Wl,--gc-sections", "-o", str(executable)], check=True)
            subprocess.run([str(executable), str(output)], check=True)
    generate(generated)
    if args.check:
        expected = {path.name: path.read_bytes() for path in target.glob("*.inc")}
        actual = {path.name: path.read_bytes() for path in generated.glob("*.inc")}
        mismatches = sorted(name for name in expected.keys() | actual.keys()
                            if expected.get(name) != actual.get(name))
        if mismatches:
            raise SystemExit("Frozen data differ: " + ", ".join(mismatches))
        embedded = temporary / "embedded"
        generate(embedded, embedded=True)
        loaded = {path.name: path.read_bytes() for path in embedded.glob("*.inc")}
        if loaded != actual:
            raise SystemExit("Embedded table accessors differ from the native builders.")
        print(f"Verified {len(actual)} frozen data files and embedded accessors "
              "bit for bit against native builders.")
    else:
        target.mkdir(exist_ok=True)
        for path in generated.glob("*.inc"):
            shutil.copyfile(path, target / path.name)
        print(f"Generated {len(list(generated.glob('*.inc')))} frozen data files.")
