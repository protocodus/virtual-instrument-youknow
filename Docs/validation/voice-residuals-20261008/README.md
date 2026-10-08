# Conservative voice residual validation, 2026-10-08

These software tests validate the explicitly estimated residuals introduced
above `41bc471985d1706be54f45ced6cbecd14fa78715`; they are not hardware measurements.
The model choices and evidence limits are in `../../unison-hardware.md`.

- `voice-residual-red.log` links the new fixture against the unchanged prior
  native DSP. It fails because a full service residual is still 10 cents
  rather than the new 2.5-cent estimate.
- `voice-residual-native-v2.log` and `voice-residual-embedded-v2.log` each pass
  614,824 assertions. Their printed numeric diagnostics match exactly across
  native C++20 and embedded C++17. They check service-point interpolation,
  independently derived AR variance, RES endpoint clipping, all three VCA
  control routes, both input-trim/coupling uses, thermal residuals, retirement,
  finite audio and post-release output below the established dry circuit floor.
- `native-engine-service-v2.log` runs the maintained full six-card resonance
  amplitude test, both 248/992 Hz output-frequency checkpoints on every card,
  retirement and thermal-cache checks. Cleared-residual amplitudes range
  4.79604–4.80407 Vpp, inside the unchanged 4.8 ±0.048 V numerical gate;
  the worst frequency error is 2.02095 cents, inside the unchanged ±10-cent
  service acceptance. Reproduce with `YOUKNOW_VOICE_RESIDUAL_TESTS_ONLY=1`.
- `native-circuit-voice-control.log` runs the existing independent envelope/VCA
  laws, including the revised worst-case control-offset silence assertion.
  `circuit-voice-control.cpp` is the small same-translation-unit selector.
- `native-unison-hardware.log` passes 33,148 assertions. The phase suite in
  `native-unison-phase.log` still finds equal settled counts and stable relative
  phase across the current product Character/Aging endpoints.
- `focused-checks.json` records exact commands, source hashes and results.
  Absolute paths identify the original checkout and can be replaced locally.

All 13 native CMake DSP translation units were compiled separately with C++20,
`-O3 -DNDEBUG -Wall -Wextra -Wpedantic -Werror`, without the embedded define.
The embedded fixture links the independently rebuilt port C++17 object cache.
No frozen DSP tables were regenerated. No full engine thermal/CPU sweep or
performance measurement was run for this validation.
