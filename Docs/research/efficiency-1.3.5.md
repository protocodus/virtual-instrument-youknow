# 1.3.5 simulation efficiency

The reference is canonical 1.3.4, commit
`88b5379c3ff2ac60af444077e1fbc62ffa32e7cc`. The circuit equations,
solver policies, sample grids, converter timing, random-number chronology,
three reconstruction refinements, and product gains remain the same.

`finiteValue` classifies IEEE binary32/binary64 by their exponent bits.
The SDK runtime otherwise calls its full floating-point classifier at
frequently evaluated safety guards. Character-type object copying is valid
in C++17 and becomes register operations with the supported compilers.
Every existing guard and fallback remains in place. Other floating layouts
retain `std::isfinite`. The change adds no instance state or allocations.

`YouKnow.Finite` and `YouKnow.EmbeddedFinite` compare 2,025,160 float/double
patterns against the standard classifier, including signed zeros, subnormals,
infinities, NaN payloads, floating exception flags, and invalid VCF states.

A last-evaluation VCA cache and covariance matrix rewrite were also evaluated.
Although they preserved audio bytes, paired native engine measurements found
small CPU regressions, especially during automation. Both were discarded.
The VCA and covariance implementations and their storage match 1.3.4 exactly.
The Rack validation record retains all trials and the isolated ablation runs.

Run the normal unit tier with:

```sh
ctest --test-dir BUILD -L unit --output-on-failure
```

Audio comparison and CPU timing must use different scopes. A numerical
threshold pass alone does not establish bit identity; compare raw float
bytes as well. Engine-only timing does not establish SDK-module, Reason
scheduling, or translated-chip performance. The Rack project's validation
record contains the measured workloads and receipts for its build.
