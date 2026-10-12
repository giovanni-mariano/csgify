# Decomposition benchmarks

This harness measures bounded `decompose-apply` runs while keeping large or
licensed STEP models outside the repository. `cases.json` identifies such
models by SHA-256, preventing an accidental comparison with a different file.

Build csgify first, then run the small harness check:

```bash
python3 benchmarks/decomposition/run.py --case l_shape \
  --output /tmp/csgify-benchmark-l-shape
```

Run the Triangle comparison:

```bash
python3 benchmarks/decomposition/run.py --case triangle --repeat 3 \
  --output /tmp/csgify-benchmark-triangle
```

Every repetition retains the MCNP/STEP outputs, report, progress stream,
decision log, stdout/stderr, GNU `time -v` evidence, and a normalized
`result.json`. The command returns non-zero if any repetition times out or
csgify does not complete successfully; partial artifacts remain available for
diagnosis.

Wall-time thresholds are intentionally not ordinary CTest tests. Correctness
belongs in CI; performance comparisons belong on a controlled machine with a
known compiler, OCCT build, and input digest.
