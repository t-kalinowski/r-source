# Serial Runtime Diagnose

Generated: 2026-02-13 12:27:29 EST

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
| alloc_small | 0.619 | 0.824 | 1.331 | 33.118 |
| lookup_only | 0.410 | 0.442 | 1.078 | 7.805 |
| no_alloc_loop | 0.361 | 0.472 | 1.307 | 30.748 |
| readme_alloc_pressure | 2.012 | 2.474 | 1.230 | 22.962 |
| readme_etl_group_mean | 1.670 | 2.149 | 1.287 | 28.683 |

