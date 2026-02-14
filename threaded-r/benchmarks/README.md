# Benchmarks

This directory is the canonical perf harness for the clean rebuild.

Design rules:

- Benchmark outputs are plain CSV artifacts committed per checkpoint.
- Dashboard is rendered from:
  - `benchmarks/results/current.csv` (latest run)
  - `benchmarks/baselines/serial_baseline.csv` (serial baseline contract)
- No hidden reference `.rds` artifacts are required by the report.

## Run

```sh
Rscript benchmarks/scripts/run_benchmarks.R benchmarks/results/current.csv
Rscript benchmarks/scripts/render_dashboard.R \
  benchmarks/results/current.csv \
  benchmarks/baselines/serial_baseline.csv \
  benchmarks/dashboard.html
```

Useful knobs:

- `TR_BENCH_ITERS` (default `5`)
- `TR_BENCH_THREADS` (default `1,2,4,8`)
- `TR_BENCH_COS_OUTER`, `TR_BENCH_COS_K`
- `TR_BENCH_ALLOC_OUTER`, `TR_BENCH_ALLOC_K`

## Benchmark suite requirements

- serial parity scenarios,
- threaded scaling scenarios,
- nested/queue scenarios,
- package/native smoke-oriented scenarios.
