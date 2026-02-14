# Benchmark Artifact Report

This file is generated from committed artifacts in `bench/results/`.
Regenerate with `tools/mtl-bench-refresh.sh` and inspect with `git diff bench/LATEST.md README.md bench/results/`.

## Serial Parity vs R-devel (`ratio = mtl / rdevel`)

| workload | mtl_lapply_s | rdevel_lapply_s | ratio |
| --- | --- | --- | --- |
| alloc_pressure | 2.593 | 2.443 | 1.061 |
| cos_seq | 3.267 | 3.279 | 0.996 |
| etl_group_mean | 2.689 | 2.878 | 0.934 |

## Serial Parity vs System R (`ratio = mtl / system`)

| workload | mtl_lapply_s | system_lapply_s | ratio |
| --- | --- | --- | --- |
| alloc_pressure | 2.593 | 2.158 | 1.202 |
| cos_seq | 3.267 | 3.094 | 1.056 |
| etl_group_mean | 2.689 | 2.412 | 1.115 |

## Minimal Serial Kernels vs R-devel (`ratio = mtl / rdevel`)

| kernel | mtl_s | rdevel_s | ratio | pct_diff |
| --- | --- | --- | --- | --- |
| alloc_small_sum_x_6e5 | 0.854 | 0.684 | 1.249 | 24.854 |
| empty_for_8e7 | 0.356 | 0.336 | 1.060 | 5.952 |
| scalar_add_4e7 | 0.380 | 0.372 | 1.022 | 2.151 |
| vector_alloc_64_x_2e6 | 0.521 | 0.426 | 1.223 | 22.300 |
| vector_list_build_x_6e5 | 0.502 | 0.329 | 1.526 | 52.584 |

## `mtlapply` Scaling (from `bench/results/mtl_latest.rds`)

| workload | threads | lapply_s | mtlapply_s | speedup | efficiency |
| --- | --- | --- | --- | --- | --- |
| alloc_pressure | 1 | 2.593 | 2.585 | 1.003 | 1.003 |
| alloc_pressure | 2 | 2.593 | 1.422 | 1.823 | 0.912 |
| alloc_pressure | 4 | 2.593 | 0.755 | 3.434 | 0.859 |
| alloc_pressure | 8 | 2.593 | 0.401 | 6.466 | 0.808 |
| cos_seq | 1 | 3.267 | 3.249 | 1.006 | 1.006 |
| cos_seq | 2 | 3.267 | 1.745 | 1.872 | 0.936 |
| cos_seq | 4 | 3.267 | 0.930 | 3.513 | 0.878 |
| cos_seq | 8 | 3.267 | 0.546 | 5.984 | 0.748 |
| etl_group_mean | 1 | 2.689 | 2.647 | 1.016 | 1.016 |
| etl_group_mean | 2 | 2.689 | 1.316 | 2.043 | 1.022 |
| etl_group_mean | 4 | 2.689 | 0.663 | 4.056 | 1.014 |
| etl_group_mean | 8 | 2.689 | 0.336 | 8.003 | 1.000 |

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
