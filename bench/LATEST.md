# Benchmark Artifact Report

This file is generated from committed artifacts in `bench/results/`.
Regenerate with `tools/mtl-bench-refresh.sh` and inspect with `git diff bench/LATEST.md README.md bench/results/`.

## Serial Parity vs R-devel (`ratio = mtl / rdevel`)

| workload | mtl_lapply_s | rdevel_lapply_s | ratio |
| --- | --- | --- | --- |
| alloc_pressure | 2.339 | 2.443 | 0.957 |
| cos_seq | 3.072 | 3.279 | 0.937 |
| etl_group_mean | 2.623 | 2.878 | 0.911 |

## Serial Parity vs System R (`ratio = mtl / system`)

| workload | mtl_lapply_s | system_lapply_s | ratio |
| --- | --- | --- | --- |
| alloc_pressure | 2.339 | 2.158 | 1.084 |
| cos_seq | 3.072 | 3.094 | 0.993 |
| etl_group_mean | 2.623 | 2.412 | 1.087 |

## `mtlapply` Scaling (from `bench/results/mtl_latest.rds`)

| workload | threads | lapply_s | mtlapply_s | speedup | efficiency |
| --- | --- | --- | --- | --- | --- |
| alloc_pressure | 1 | 2.339 | 2.398 | 0.975 | 0.975 |
| alloc_pressure | 2 | 2.339 | 1.314 | 1.780 | 0.890 |
| alloc_pressure | 4 | 2.339 | 0.741 | 3.157 | 0.789 |
| alloc_pressure | 8 | 2.339 | 0.400 | 5.848 | 0.731 |
| cos_seq | 1 | 3.072 | 3.092 | 0.994 | 0.994 |
| cos_seq | 2 | 3.072 | 1.630 | 1.885 | 0.942 |
| cos_seq | 4 | 3.072 | 0.898 | 3.421 | 0.855 |
| cos_seq | 8 | 3.072 | 0.535 | 5.742 | 0.718 |
| etl_group_mean | 1 | 2.623 | 2.572 | 1.020 | 1.020 |
| etl_group_mean | 2 | 2.623 | 1.290 | 2.033 | 1.017 |
| etl_group_mean | 4 | 2.623 | 0.659 | 3.980 | 0.995 |
| etl_group_mean | 8 | 2.623 | 0.337 | 7.783 | 0.973 |

## Threadpool Matrix Benchmark

| case | rows | cols | n | threads | reps | lapply_median_s | mtlapply_median_s | speedup | efficiency |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| matmul_1000_n20 | 1000 | 1000 | 20 | 8 | 3 | 5.102 | 0.801 | 6.370 | 0.796 |
| matmul_100_n20000 | 100 | 100 | 20000 | 8 | 3 | 6.120 | 0.807 | 7.584 | 0.948 |

## `background()` / `wait()` Burst Benchmark

| mode | threads | elapsed_s | throughput_req_s | p50_s | p95_s | p99_s | speedup_vs_serial | p95_gain_vs_serial |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| background | 2 | 0.088 | 1090.909 | 0.047 | 0.086 | 0.087 | 6.273 | 6.099 |
| background | 4 | 0.087 | 1103.448 | 0.048 | 0.087 | 0.087 | 6.345 | 6.029 |
| background | 8 | 0.086 | 1116.279 | 0.046 | 0.086 | 0.086 | 6.419 | 6.099 |
| serial | 1 | 0.552 | 173.913 | 0.279 | 0.524 | 0.546 | 1.000 | 1.000 |

## Shiny Full Loadtest Artifact

| mode | sessions_total | sessions_completed | sessions_failed | median_total_s | median_busy_s | p95_busy_s | throughput_completed_sess_per_s |
| --- | --- | --- | --- | --- | --- | --- | --- |
| sync | 8 | 8 | 0 | 175.144 | 153.024 | 153.366 | 0.046 |
| threadpool | 8 | 8 | 0 | 23.140 | 1.024 | 1.137 | 0.332 |
