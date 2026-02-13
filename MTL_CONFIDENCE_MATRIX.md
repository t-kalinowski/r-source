# MTL Drop-In Confidence Matrix

This document defines additional correctness and performance checks aimed at
one goal: confidence that this build can be used as a drop-in replacement for
regular R across real workflows.

## Objectives

1. Correctness parity: user-visible behavior matches regular R for serial code.
2. Compatibility parity: existing package binaries and source installs keep
working.
3. Safety: threaded execution fails cleanly on unsupported global mutation.
4. Performance: no meaningful serial regressions; threaded workloads show
expected speedups where parallelism exists.

## Existing Coverage (Already in Repo)

1. Core threaded runtime and error-recovery:
`tests/mtlapply.R`, `tests/mtfuture.R`, `tools/mtl-soak-smoke.sh`.
2. ABI/package compatibility:
`tools/mtl-abi-smoke.R`, `tools/mtl-load-standard-library-smoke.sh`,
`tools/mtl-framework-library-smoke.sh`, `tools/mtl-dropin-smoke.R`.
3. Representative package workflows:
`tools/mtl-dplyr-smoke.sh`, `tools/mtl-worker-native-smoke.R`,
`tools/mtl-reticulate-smoke.R`, `tools/mtl-rsession-smoke.sh`.
4. Baseline performance guardrails:
`tools/mtl-perf-smoke.sh`, `tools/mtl-threadpool-perf-smoke.R`,
`tools/mtl-shiny-background-smoke.R`, `bench/readme_bench_run.R`.

## New Workloads Added

New base-R workload suite:

1. Script: `tools/mtl-dropin-workloads.R`
2. Focus:
closure capture, promise forcing, tryCatch/warnings, connection I/O,
regex/symbol interning, serialize/unserialize roundtrip, small linear model fit,
nested apply compute, local superassignment behavior.
3. Also checks:
global assignment and `setwd()` rejection from workers are clean/recoverable.
4. Artifact:
CSV with per-workload `lapply` and `mtlapply(threads)` medians.

Example run:

```sh
build-mtl-shlib/bin/R --vanilla -q -f tools/mtl-dropin-workloads.R --args \
  bench/results/dropin_workloads_latest.csv 1,2,4,8 3
```

## Additional Workloads To Add Next

### Package behavior matrix

1. `data.table`:
grouped aggregations, joins, keyed subset, in-place update in serial path.
2. `rlang`/`vctrs`:
quosure capture/eval, vec recycle/cast/ptype2, condition signaling.
3. `Rcpp`/`inline`:
compile simple native routines, call from serial and worker contexts.
4. `Matrix`:
sparse ops, `%*%`, Cholesky/solve, class dispatch.
5. `reticulate`:
`py_config()`, `import("sys")$executable`, `py_eval("1+1")`.

### Language/runtime edge cases

1. Active bindings and delayed promises in parent envs.
2. `on.exit`, restarts, and nested `tryCatch` in worker closures.
3. Connection classes: `file`, `gzfile`, `textConnection`, `url` (local file URL).
4. S3/S4 dispatch-heavy closures.
5. `compiler` bytecode behavior on worker-evaluated closures.

### Background/wait patterns

1. Long chains of `background() -> then() -> then()` with mixed success/failure.
2. Wait-any loop over heterogeneous job durations.
3. Cancellation under load with follow-up correctness checks.
4. Mixed queue:
`background()` jobs and `mtlapply()` jobs active simultaneously.

## Performance Matrix and Pass Criteria

Use heavier runtimes to suppress noise. Per-case target is roughly 1 to 15
seconds per measurement sample.

### Serial parity gates

1. Baseline:
`lapply` on reference R-devel vs MTL build.
2. Threshold:
fail if median ratio exceeds `1.10` on any guarded serial workload.
3. Candidate serial workloads:
`bench/readme_bench_run.R` plus `tools/mtl-dropin-workloads.R` workloads.

### Threaded scaling gates

1. Cases that should scale:
independent CPU-heavy closures and matrix workloads.
2. Report:
speedup and efficiency for `threads = 1,2,4,8`.
3. Threshold:
for current 8-thread matrix smoke, enforce minimum efficiency floor.

### Non-scaling expected cases

1. Global mutation attempts:
must error cleanly, not crash, not poison session.
2. Small/trivial closures:
may not speed up; must remain correct and stable.
3. Lock-serialized operations:
must remain correct with bounded overhead.

## Suggested Validation Tiers

### Per-commit tier

1. `make -C build-mtl-shlib -j8`
2. `build-mtl-shlib/bin/R --vanilla -q -f tests/mtlapply.R`
3. `build-mtl-shlib/bin/R --vanilla -q -f tests/mtfuture.R`
4. `build-mtl-shlib/bin/R --vanilla -q -f tools/mtl-dropin-workloads.R --args \
bench/results/dropin_workloads_latest.csv 1,2,4 2`
5. `tools/mtl-perf-smoke.sh build-mtl-shlib /usr/local/bin/R-devel 1.10`

### Daily tier

1. `tools/mtl-validation-smoke.sh build-mtl-shlib /Users/tomasz/Library/R/arm64/4.6/library 4 build-mtl-shlib/library /usr/local/bin/R-devel 1.10`
2. `tools/mtl-rsession-smoke.sh build-mtl-shlib`
3. `tools/mtl-bench-refresh.sh build-mtl-shlib R /usr/local/bin/R-devel`

### Soak tier

1. `tools/mtl-soak-smoke.sh build-mtl-shlib 50`
2. Extended package workflow runs (`dplyr`, `reticulate`, `quickr`, `Rcpp` paths).
3. Long background/wait stress with artifact capture.

## Artifact Discipline

1. Keep generated benchmark artifacts in repo-local paths:
`bench/results/` and `bench/artifacts/`.
2. Refresh summary docs from artifacts:
`bench/LATEST.md` and `README.md`.
3. Treat performance artifacts as regression evidence and commit at checkpoints.
