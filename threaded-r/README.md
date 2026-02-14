# threaded-r seed

This directory is the seed for a clean re-implementation of threaded R support.

Goal: start from a fresh upstream `R-devel` fork and re-apply only proven ideas from this exploration with strict validation gates.

## Top-level structure

- `r-source/`: clean upstream R checkout to patch incrementally.
- `builds/`: local build trees (`mtl`, `ref`, `mtl-clang`) for fast compare loops.
- `artifacts/`: generated validation/benchmark artifacts (latest + history).
- `harness/`: staged validation entrypoints and guardrail checks.
- `benchmarks/`: reproducible benchmark scripts + dashboard render inputs.
- `docs/`, `agents/`, `plans/`, `tests/`: knowledge system, execution plans, and test matrix.

## Standard artifact pattern

- Bench artifacts:
  - `artifacts/bench/latest/`: most recent benchmark run outputs.
  - `artifacts/bench/history/`: timestamped benchmark snapshots.
- Check artifacts:
  - `artifacts/checks/latest/`: latest stage status files.
  - `artifacts/checks/history/`: timestamped stage status history.
- Data interchange format:
  - CSV for benchmark results and baselines.
  - `.status` key-value files for check stage outcomes.

## Standard staged checks

From `threaded-r/`:

```sh
make init-layout
make smoke
make partial
make full
make release
make abi
```

What these enforce:

- smoke: fast correctness + strict-path leakage checks.
- partial: smoke + threaded tests + package ABI smoke.
- full: partial + benchmark refresh + optional long `check-all`.
- release: strict full path + drop-in smoke path.

## Key guardrails

- strict mode disallows accidental system library leakage.
- drop-in mode validates compatibility with real user/system package paths.
- ABI smoke checks package namespace loading for compiled packages.
- benchmark thresholds protect serial parity and threaded efficiency.
