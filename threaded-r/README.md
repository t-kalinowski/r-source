# threaded-r seed

This directory is the seed for a clean re-implementation of threaded R support.

Goal: start from a fresh upstream `R-devel` fork and re-apply only the proven ideas from this exploration, in a disciplined, incremental sequence with correctness/performance gates at each step.

Contents:

- `AGENTS.md`: minimal always-loaded source map + working rules.
- `agents/`: scoped deep-dive notes for specific runtime areas.
- `docs/THREADING_PRODUCT_DIRECTION.md`: distilled design goals, invariants, constraints, API shape.
- `docs/CURRENT_IMPLEMENTATION_DESIGN.md`: source-verified architecture of the current implementation (structs, APIs, mode switch, guardrails).
- `plans/IMPLEMENTATION_ROADMAP.md`: phased implementation plan with checklists and gates.
- `benchmarks/`: benchmark harness scaffold + dashboard (`Rmd`) + baseline CSV contract.
- `tests/TEST_MATRIX.md`: correctness/perf validation matrix to enforce at each checkpoint.
