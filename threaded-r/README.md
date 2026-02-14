# threaded-r seed

This directory is the seed for a clean re-implementation of threaded R support.

Goal: start from a fresh upstream `R-devel` fork and re-apply only the proven ideas from this exploration, in a disciplined, incremental sequence with correctness/performance gates at each step.

Contents:

- `AGENTS.md`: minimal always-loaded source map + working rules.
- `agents/`: scoped deep-dive notes for specific runtime areas.
- `docs/AGENT_FIRST_PATTERNS.md`: distilled harness-engineering patterns adapted to this project.
- `docs/KNOWLEDGE_BASE_LAYOUT.md`: knowledge-store structure and update workflow.
- `docs/README.md`: docs index for progressive disclosure.
- `docs/THREADING_PRODUCT_DIRECTION.md`: distilled design goals, invariants, constraints, API shape.
- `docs/CURRENT_IMPLEMENTATION_DESIGN.md`: source-verified architecture of the current implementation (structs, APIs, mode switch, guardrails).
- `docs/exec-plans/`: first-class execution plans (`active/`, `completed/`) and debt tracker.
- `plans/IMPLEMENTATION_ROADMAP.md`: phased implementation plan with checklists and gates.
- `benchmarks/`: benchmark harness scaffold + dashboard (`Rmd`) + baseline CSV contract.
- `tests/TEST_MATRIX.md`: correctness/perf validation matrix to enforce at each checkpoint.
