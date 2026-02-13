# Benchmark Artifact Report

This file is generated from committed artifacts in `bench/results/`.
Regenerate with `tools/mtl-bench-refresh.sh` and inspect with `git diff bench/LATEST.md README.md bench/results/`.

## Serial Parity vs R-devel (`ratio = mtl / rdevel`)

| workload | mtl_lapply_s | rdevel_lapply_s | ratio |
| --- | --- | --- | --- |
| alloc_pressure | 0.101 | 0.109 | 0.927 |
| cos_seq | 0.408 | 0.495 | 0.824 |
| etl_group_mean | 0.979 | 1.043 | 0.939 |

## Serial Parity vs System R (`ratio = mtl / system`)

| workload | mtl_lapply_s | system_lapply_s | ratio |
| --- | --- | --- | --- |
| alloc_pressure | 0.101 | 0.103 | 0.981 |
| cos_seq | 0.408 | 0.433 | 0.942 |
| etl_group_mean | 0.979 | 0.902 | 1.085 |

## `mtlapply` Scaling (from `bench/results/mtl_latest.rds`)

| workload | threads | lapply_s | mtlapply_s | speedup | efficiency |
| --- | --- | --- | --- | --- | --- |
| alloc_pressure | 1 | 0.101 | 0.095 | 1.063 | 1.063 |
| alloc_pressure | 2 | 0.101 | 0.114 | 0.886 | 0.443 |
| alloc_pressure | 4 | 0.101 | 0.065 | 1.554 | 0.388 |
| alloc_pressure | 8 | 0.101 | 0.108 | 0.935 | 0.117 |
| cos_seq | 1 | 0.408 | 0.413 | 0.988 | 0.988 |
| cos_seq | 2 | 0.408 | 0.301 | 1.355 | 0.678 |
| cos_seq | 4 | 0.408 | 0.177 | 2.305 | 0.576 |
| cos_seq | 8 | 0.408 | 0.181 | 2.254 | 0.282 |
| etl_group_mean | 1 | 0.979 | 0.968 | 1.011 | 1.011 |
| etl_group_mean | 2 | 0.979 | 0.601 | 1.629 | 0.814 |
| etl_group_mean | 4 | 0.979 | 0.302 | 3.242 | 0.810 |
| etl_group_mean | 8 | 0.979 | 0.152 | 6.441 | 0.805 |

## Threadpool Matrix Benchmark

| case | rows | cols | n | threads | reps | lapply_median_s | mtlapply_median_s | speedup | efficiency |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| matmul_1000_n20 | 1000 | 1000 | 20 | 8 | 3 | 5.277 | 0.804 | 6.563 | 0.820 |
| matmul_100_n20000 | 100 | 100 | 20000 | 8 | 3 | 6.388 | 0.813 | 7.857 | 0.982 |

## `background()` / `wait()` Burst Benchmark

| mode | threads | elapsed_s | throughput_req_s | p50_s | p95_s | p99_s | speedup_vs_serial | p95_gain_vs_serial |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| background | 2 | 0.273 | 351.648 | 0.147 | 0.270 | 0.272 | 2.267 | 2.175 |
| background | 4 | 0.274 | 350.365 | 0.148 | 0.273 | 0.274 | 2.259 | 2.153 |
| background | 8 | 0.273 | 351.648 | 0.148 | 0.271 | 0.272 | 2.267 | 2.169 |
| serial | 1 | 0.619 | 155.089 | 0.313 | 0.588 | 0.612 | 1.000 | 1.000 |

## Shiny Full Loadtest Artifact

| mode | sessions_total | sessions_completed | sessions_failed | median_total_s | median_busy_s | p95_busy_s | throughput_completed_sess_per_s |
| --- | --- | --- | --- | --- | --- | --- | --- |
| sync | 8 | 8 | 0 | 175.144 | 153.024 | 153.366 | 0.046 |
| threadpool | 8 | 8 | 0 | 23.140 | 1.024 | 1.137 | 0.332 |
