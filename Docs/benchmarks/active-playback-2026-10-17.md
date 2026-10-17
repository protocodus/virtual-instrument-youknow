# Active playback CPU — 17 October 2026

The 1.3.2 candidate removes repeated chorus arithmetic and specializes the existing four/six-term OTA shot-noise covariance recurrence. The chorus still normalizes its noise at each clock interval and takes exactly the same random draws; its fixed source coefficients refresh on input edits, reset, bypass reopening and disable/re-enable. Signed-zero inputs retain their original coefficient bits. The covariance uses the same terms and ordered expressions, with exact power-of-two scales replacing bounded `ldexp` calls. No circuit value, processing grid, approximation, cache tolerance or noise cadence changes.

The baseline is canonical `5884e72b0fabad7547609b39bb3ef683e7025e34` (1.3.1). Native arm64 Apple M1 Max, Apple Clang 21.0.0, macOS 26.5.1, C++17, `-O3 -fno-builtin -DYOUKNOW_EMBEDDED_TARGET=1`. The Rack product configuration uses six voices, 1x, Normal Circuit Rate, PolyZoned/Cubic/RK4Single, Aging 50%, chorus II and 64-frame blocks. This is engine-only native evidence, excluding Reason scheduling and Jukebox target translation.

## Paired comparison

The final comparison puts baseline and candidate engines in one executable under separate C++ namespaces and a common C ABI. It alternates event-preparation and timed call order every callback and construction order every repetition. The primary workload retriggers/transposes six notes every 512 callbacks; both engines have 128 active warmup callbacks and 4096 measured callbacks per repetition. Five repetitions at each rate run once with each of two predeclared object link orders, without concurrent task compilation/rendering.

| Object order | Rate | Baseline µs/block | Candidate µs/block | Median paired CPU reduction |
| --- | ---: | ---: | ---: | ---: |
| baseline first | 44100 | 506.792 | 499.872 | 1.34% |
| baseline first | 48000 | 500.377 | 495.314 | 1.24% |
| baseline first | 96000 | 347.123 | 341.454 | 1.58% |
| candidate first | 44100 | 509.414 | 501.676 | 1.58% |
| candidate first | 48000 | 504.715 | 496.458 | 1.48% |
| candidate first | 96000 | 343.034 | 335.804 | 2.14% |

CPU columns are independent medians; the last column is the median of per-repetition reductions.

All 30 repetitions favor the candidate; all first-call and second-call strata also favor it. The final native engine result is roughly **1–2% less active-playback CPU**, depending on rate and code placement. Every warmup and measured stereo sample matches by bits (over 16 million sample comparisons). These observations support the measured native engine workload; they do not turn the mixed separate-executable wrapper matrix into an overall Reason CPU claim.

[Raw paired data](active-playback-2026-10-17.csv). Both call-order strata, p50/p95, wall times and missed deadlines are retained. Across both link orders, baseline/candidate wall misses are 248/241 out of 122,880 measured callbacks each; this is not a zero-miss real-time acceptance result.

The broader separate-executable wrapper A/B/A2 matrix was mixed, with slower active cases up to 6.52%. All runs and discarded experiments remain in the Rack [validation report](https://github.com/protocodus/reason-rack-extensions/tree/main/Examples/YouKnow/Docs/validation/active-cpu-20261017); native binary placement and temporal drift motivated the paired comparison. The two link orders and both call-order strata help assess those effects without discarding the unfavorable runs. No actual Reason CPU reduction or new U45/host acceptance is claimed.

## Validation

- Both changes separately, and their combination, retain all 55 deterministic engine scenario fingerprints. Final wrapper, controller, routing, PWM, five-rate and 57,018-event fuzz contracts pass.
- The new chorus test compares exact stereo and physical/filter/RNG state to forced coefficient preparation through live changes, invalid values, signed zeros, resets, rate changes and bypass reopening. Native and embedded C++17 variants pass.
- The covariance regression adds 2,700 exact covariance/factor/innovation boundary cases and 13 exact scale checks while retaining the existing independent physical ODE oracle and thresholds.
- All 100 unit checks pass: 99 in the full run and the MIDI-source check after restoring source fixtures omitted by the sparse checkout. No test assertion, tolerance or timeout changed.
- Actual SDK 5 analyzer, target compilation and forbidden-global checks pass at 32/64 bits in Testing/Deployment for the consuming engine, chorus and wrapper.
- Fresh renders of all 100 Rack factory patches reproduce every recorded peak and RMS metric exactly. No sound value or level was recalibrated.

The Rack report includes reproducible paired-harness sources, build manifests, raw timings and factory measurements. Native results do not establish the CPU of the JUCE/IPO builds or the translated Reason target.
