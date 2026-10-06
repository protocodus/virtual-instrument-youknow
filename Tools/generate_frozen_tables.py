#!/usr/bin/env python3
"""Generate (or verify) embedded immutable tables using native DSP builders.

Run with the same compiler and libm when checking exact reproduction: platforms
may differ by a last bit in transcendental functions. Shipping native builds
continue to use their original builders; embedded builds use the checked-in data.


YOUKNOW_EMBEDDED_TARGET selects hexadecimal constants in the canonical engine,
chorus and SUB control source. The storage/startup changes share exactly the
same interpolation and processing code with the native builders. The 24 sets
cover current and compatibility BBD transfer/noise, VCF interpolation/resonance,
oscillator correction, control junctions, service calibration and firmware data.
The temperature-dependent VCA set has 40 x 8,193 nodes (two binary64 currents and
two binary32 slopes per node): 7,865,280 bytes plus its span, shared read-only
rather than allocated in every engine. Its text is most of the ~26 MiB data.

The generators include the shipping source directly and expose private tables
only in their offline translation units. --check rebuilds the 24 data files and
compares each emitted value, then verifies the embedded accessors against those
same builders. No copied mathematical implementation supplies the oracle.

Generation environment: GCC 14.2, glibc 2.41, Linux; GNU/ELF section garbage
collection is required. Cross-platform last-bit transcendental differences must
be reviewed, not silently regenerated. Inspected embedded C++17 GNU objects have
no writable globals, static-initialization routines or static-guard references.
Licensed Reason target analysis and packaging remain separate SDK checks.
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
