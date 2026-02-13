# Serial Runtime Diagnose

Generated: 2026-02-13 12:51:01 EST

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
| alloc_small | 0.619 | 0.828 | 1.338 | 33.764 |
| lookup_only | 0.410 | 0.435 | 1.061 | 6.098 |
| no_alloc_loop | 0.361 | 0.380 | 1.053 | 5.263 |
| readme_alloc_pressure | 2.012 | 2.488 | 1.237 | 23.658 |
| readme_etl_group_mean | 1.670 | 2.053 | 1.229 | 22.934 |

