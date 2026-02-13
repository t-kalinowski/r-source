# Serial Runtime Diagnose

Generated: 2026-02-13 12:27:29 EST

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
| alloc_small | 0.619 | 0.825 | 1.333 | 33.279 |
| lookup_only | 0.410 | 0.437 | 1.066 | 6.585 |
| no_alloc_loop | 0.361 | 0.471 | 1.305 | 30.471 |
| readme_alloc_pressure | 2.012 | 2.450 | 1.218 | 21.769 |
| readme_etl_group_mean | 1.670 | 2.141 | 1.282 | 28.204 |

## MTL Runtime Counter Means (Per Run)

| counter | mean_per_run |
| --- | --- |
| runtime.alloc.fastpath.calls | 7691221.286 |
| runtime.alloc.fastpath.serial | 7691221.286 |

## MTL Shared-Env Counter Means (Per Run)

| counter | mean_per_run |
| --- | --- |
| shared.env.mutcheck.calls | 1932.619 |
| shared.env.mutcheck.inactive | 1932.619 |

