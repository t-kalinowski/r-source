# Serial Runtime Diagnose

Generated: 2026-02-13 12:32:17 EST

## Build Info

- ref: `/Users/tomasz/github/wch/r-source-threads/reference-r-devel-clean/build-ref-shlib`
- ref version: `R Under development (unstable) (2026-02-10 r99999)`
- mtl: `/Users/tomasz/github/wch/r-source-threads/build-mtl-shlib`
- mtl version: `R Under development (unstable) (2026-02-10 r99999)`
- mtl runtime stats enabled: `FALSE`
- mtl shared-env stats enabled: `FALSE`

## Median Runtime Comparison

| workload | ref_median_s | mtl_median_s | ratio_mtl_vs_ref | pct_diff |
| --- | --- | --- | --- | --- |
| alloc_small | 0.619 | 0.834 | 1.347 | 34.733 |
| lookup_only | 0.410 | 0.458 | 1.117 | 11.707 |
| no_alloc_loop | 0.361 | 0.481 | 1.332 | 33.241 |
| readme_alloc_pressure | 2.012 | 2.559 | 1.272 | 27.187 |
| readme_etl_group_mean | 1.670 | 2.091 | 1.252 | 25.210 |

