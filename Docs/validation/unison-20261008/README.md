# Canonical Unison validation, 2026-10-08

These are source-level software receipts for the canonical engine change on top
of `ded94edc84af697d66f0590dda8286b1308e0d2c`. They do not claim a physical
JUNO-106 recording or exact host-event/serial timing equivalence.

- `oracle_probe.cpp` and `baseline-oracle.log` record the original direct-engine
  with its default `NormalizedServiceChart` timing versus A-5/UART/B-2
  observations; it did not select the Rack `MeasuredChartGeometry` profile.
  The bounded interpretation is in
  `../../unison-hardware.md`.
- `unison-hardware-red.log` demonstrates the original six-card startup failure
  against the unchanged baseline DSP objects.
- `unison-hardware-native-v2.log` compiles the current core and new contract
  together as native C++20, without the embedded define: 33,148 assertions pass.
- `unison-hardware-embedded-v2-build.log` and `unison-hardware-embedded-v2.log`
  link the final rebuilt C++17 embedded core: 33,148 assertions pass.
- `native-engine-*.log` cover the existing native engine note-handling,
  DCO/coupling, and M82C53 PIT scenarios. All three focused selections pass.
- `native-unison-phase*.log` cover the phase/count/retarget checks plus current
  product Character/Aging endpoints. Analogue ramp dispersion varies; the six
  settled counters keep equal frequency and stable relative phase.
- `focused-checks.json` retains compiler identity, source hashes, object build
  commands, and run results. Absolute paths identify the original checkout and
  output location; they can be replaced with the corresponding local paths.

The native cache contains all 13 translation units from `YOUKNOW_DSP_SOURCES`,
compiled with C++20, `-O3 -DNDEBUG -Wall -Wextra -Wpedantic -Werror`. The active
product header selects the hardware-realism profile by default, matching the
CMake default. The embedded check uses the final Rack validation object cache;
its compile manifest remains with the port's validation receipts.

The new contract is also registered in CMake as `YouKnow.UnisonHardware` and
`YouKnow.EmbeddedUnisonHardware`. The updated phase check is
`YouKnow.UnisonPhase`. The unrelated full engine thermal/CPU suite was not run
for this change; no concurrent timing diagnostic is used as performance evidence.
