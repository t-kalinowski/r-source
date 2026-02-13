# Serial Runtime Diagnose

Generated: 2026-02-13 12:32:41 EST

## Build Info

- ref: `/Users/tomasz/github/wch/r-source-threads/reference-r-devel-clean/build-ref-shlib`
- ref version: `R Under development (unstable) (2026-02-10 r99999)`
- mtl: `/Users/tomasz/github/wch/r-source-threads/build-mtl-shlib`
- mtl version: `R Under development (unstable) (2026-02-10 r99999)`
- mtl runtime stats enabled: `TRUE`
- mtl shared-env stats enabled: `TRUE`

## Median Runtime Comparison

| workload | ref_median_s | mtl_median_s | ratio_mtl_vs_ref | pct_diff |
| --- | --- | --- | --- | --- |
| alloc_small | 0.619 | 0.838 | 1.354 | 35.380 |
| lookup_only | 0.410 | 0.461 | 1.124 | 12.439 |
| no_alloc_loop | 0.361 | 0.482 | 1.335 | 33.518 |
| readme_alloc_pressure | 2.012 | 2.522 | 1.253 | 25.348 |
| readme_etl_group_mean | 1.670 | 2.148 | 1.286 | 28.623 |

## MTL Runtime Counter Means (Per Run)

| counter | mean_per_run |
| --- | --- |
| runtime.alloc.fastpath.calls | 7691228.286 |
| runtime.alloc.fastpath.serial | 7691228.286 |

## MTL Shared-Env Counter Means (Per Run)

| counter | mean_per_run |
| --- | --- |
| shared.env.mutcheck.calls | 1932.619 |
| shared.env.mutcheck.inactive | 1932.619 |

