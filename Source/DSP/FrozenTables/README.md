# Embedded immutable DSP data

`YOUKNOW_EMBEDDED_TARGET` selects these hexadecimal constants in the shared
engine, chorus and SUB control source. Native builds keep their existing
builders and defaults. The embedded path changes table storage and startup;
it uses the same interpolation and processing code.

The data include both compatibility and current physical component profiles:
BBD transfer curves and noise moments, VCF Hermite coefficients and resonance
trim, oscillator correction, diode and transistor control laws, service
calibration, firmware coefficients, and the temperature-dependent VCA circuit.
The latter contains 40 rows of 8,193 nodes, each with two binary64 currents and
two binary32 slopes: 7,865,280 bytes plus its span. It is shared read-only data,
not a copy in each engine instance. Its source accounts for most of this
directory's approximately 26 MiB size.

Regenerate from the current native builders with:

```sh
python3 Tools/generate_frozen_tables.py
python3 Tools/generate_frozen_tables.py --check
```

The check rebuilds all 24 data files, then reads them through the embedded
accessors and compares every emitted value exactly. The offline generators
include the shipping source directly so a copied mathematical implementation
cannot silently drift away from the engine. They expose private data only in
the generator translation units. They are not part of a shipping target.

The current files were generated with GCC 14.2 and glibc on Linux. The utility
uses GNU/ELF section garbage collection and accepts `--cxx`. Use the same
compiler and math library for exact reproduction: transcendental functions
on another platform can round their last bit differently. Such a mismatch
must be reviewed; it is not an instruction to overwrite established data.

The embedded engine and chorus compile as C++17 with no writable global data,
static initialization routines or static guard references in the inspected
GNU objects. Reason SDK target analysis and packaging still require the SDK.
