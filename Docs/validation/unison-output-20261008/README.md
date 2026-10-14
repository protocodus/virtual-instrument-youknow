# Unison output trim validation — 2026-10-08

Both final focused executables pass **222,852 assertions**. The native build uses
C++20 and 13 freshly compiled DSP translation units; the embedded build uses
C++17 and `YOUKNOW_EMBEDDED_TARGET=1`, linking the 11 freshly built embedded
core objects. Its test translation unit additionally disables exceptions/RTTI.
Both use `-O3` and strict warnings.

The natural one-voice Poly/Unison oracle verifies bit-exact `Poly output * 0.8f`
on both stereo channels, LINE and PHONES, with unchanged internal filter and
timer state. Additional checks cover unity Poly output; startup/reset/reprepare;
null, empty and unprepared calls; exact `ceil(0.005 * sampleRate)` transition
endpoints; reversal without overshoot; quality changes; same-target Poly mode
changes; and identical audio across 1/17/64/256-frame callback partitions.
Timing cases span 8/44.1/48/96/192 kHz and quality factors 1/2/4. Audio oracles
span 44.1/48/96 kHz and all three factors.

The OriginalPerformance check deliberately retains an alternate circuit-mirror
mode to prove that this output policy follows the selected host mode. It does
not establish a real firmware delay. This digital trim is a requested product
policy, not a reconstruction of an original-hardware output circuit.

- [Native final output](native.log)
- [Embedded final output](embedded.log)
- [Source hashes and qualification receipt](receipt.json)
- [Exact final link/run commands and core compile references](commands.json)

`YouKnow.UnisonOutput` and `YouKnow.EmbeddedUnisonOutput` are registered in
CMake. These receipts record direct compilation/execution, not a CTest run.
Exploratory fixture iterations remain in the local raw validation directory;
only the final passing outputs are retained here.
