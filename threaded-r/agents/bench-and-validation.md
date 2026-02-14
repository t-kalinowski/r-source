# Bench + Validation (Agent Scope)

Focus: performance/correctness gating and reproducible reports.

Principles:

- benchmark artifacts are committed per checkpoint,
- dashboard is rendered from current run CSV + baseline serial CSV,
- no hidden reference-R artifacts besides baseline CSVs.

Required gates by default:

- runtime correctness smoke (`mtlapply` regressions),
- package ABI smoke (compiled + pure R package load paths),
- serial parity guard (MTL serial vs baseline serial),
- threaded scaling checks (`2/4/8` threads on representative workloads).

When investigating regressions:

- increase workload duration to suppress noise,
- keep single-purpose microbenches for hotpath attribution,
- append findings to investigation log with hypothesis/result/next step.
