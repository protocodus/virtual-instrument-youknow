# 1.3.5 simulation efficiency

The reference is canonical 1.3.4, commit
`88b5379c3ff2ac60af444077e1fbc62ffa32e7cc`. The circuit equations,
solver policies, sample grids, converter timing, random-number chronology,
three reconstruction refinements, and product gains remain the same.

The changes remove redundant work:

- `finiteValue` classifies IEEE binary32/binary64 by their exponent bits.
  The SDK runtime otherwise calls its full floating-point classifier at
  frequently evaluated safety guards. Character-type object copying is valid
  in C++17 and becomes register operations with the supported compilers.
  Every existing guard and fallback remains in place. Other floating layouts
  retain `std::isfinite`.
- The VCA current cache retains the last exact evaluation as well as its
  interpolation cell. Charge, temperature, and immutable circuit span must
  match exactly. This avoids repeated identical endpoint/RK-start queries;
  it introduces no tolerance, control holding, or new interpolation law.
  Each cache grows from 48 to 64 bytes: 256 additional bytes per engine.
- The 4-by-4 covariance multiplication visits independent columns together,
  preserving the ordered four-term sum for each element. It is inlined so
  the compiler can keep intermediates in registers. The final transition
  matrix square is omitted where its result has no subsequent reader.

`YouKnow.Finite` and `YouKnow.EmbeddedFinite` compare 2,025,160 float/double
patterns against the standard classifier, including signed zeros, subnormals,
infinities, NaN payloads, floating exception flags, and invalid VCF states.
`YouKnow.VcaEvaluationCache` compares 1,622,985 evaluations against the prior
calculation, including all table cells, cache resets/copies, invalidation,
temperature/span boundaries, and driven RK4 trajectories. The shot-noise
test's frozen covariance references use the old matrix multiplication
independently of the production helper.

Run the normal unit tier with:

```sh
ctest --test-dir BUILD -L unit --output-on-failure
```

Audio comparison and CPU timing must use different scopes. A numerical
threshold pass alone does not establish bit identity; compare raw float
bytes as well. Engine-only timing does not establish SDK-module, Reason
scheduling, or translated-chip performance. The Rack project's validation
record contains the measured workloads and receipts for its build.
