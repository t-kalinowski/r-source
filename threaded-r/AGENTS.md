# AGENTS (Minimal Global Context)

Purpose: this file is intentionally small and should be loaded in every agent context.

## Project objective

Rebuild threaded/subinterpreter support on a clean upstream R codebase with:

- no serial-performance tax when threadpool is idle,
- practical threaded speedups when workloads are safe,
- package ABI compatibility and drop-in usability,
- explicit benchmark/test gates at every commit.

## Source map (quick navigation)

- `src/main/eval.c`: evaluator/bytecode hot paths.
- `src/main/memory.c`: allocation, GC, protect stack operations.
- `src/main/envir.c`: environment lookup/assignment semantics.
- `src/main/errors.c`: condition handling, unwind behavior.
- `src/main/apply.c`: `lapply`-family runtime behavior.
- `src/main/context.c`: context stack, longjmp/unwind boundaries.
- `src/main/options.c`: options semantics and mutation controls.
- `src/main/dotcode.c`: `.Call` / `.External` interfaces.
- `src/main/main.c` + `src/unix/*`: startup/runtime wiring.
- `src/include/Defn.h` / `src/include/Rinternals.h` / `src/include/Rinlinedfuns.h`: core macros/ABI/inlined runtime primitives.
- `src/library/*`: base/recommended R code + C sources for core packages.
- `tests/`: runtime regression tests.

## Layered agent docs

Load these only when needed:

- `agents/runtime-core.md`
- `agents/eval-and-environments.md`
- `agents/memory-gc.md`
- `agents/threadpool-and-futures.md`
- `agents/package-abi.md`
- `agents/bench-and-validation.md`
- `docs/AGENT_FIRST_PATTERNS.md`
- `docs/KNOWLEDGE_BASE_LAYOUT.md`
- `docs/SUBINTERPRETER_API.md`

## Harness quickstart

Run from `threaded-r/`:

- `make init-layout`
- `make bootstrap`
- `make docs-check`
- `make smoke`
- `make partial`
- `make full`
- `make release`

Mode semantics:

- `TR_MODE=strict`: no unintentional package/library leakage.
- `TR_MODE=dropin`: realistic compatibility path with user/system libraries.

Primary binaries (override as needed):

- `TR_MTL_R_BIN` (threaded build under test)
- `TR_REF_R_BIN` (reference upstream build)
- `TR_SYSTEM_R_BIN` (system/dev R for baseline comparisons)

## Global rules

- Keep this file short and map-like. It is a table of contents, not the encyclopedia.
- The repository is the system of record for decisions, plans, and constraints.
- Use progressive disclosure: load one or two scoped docs for the task, not everything.
- Agents are encouraged to proactively update agent-facing artifacts (`AGENTS.md`, `agents/*.md`, `docs/*`, `plans/*`, harness docs, benchmark docs) whenever workflow, constraints, invariants, or validation procedures change.
- Do not request elevated permissions just to stop runaway processes. Use bounded runs (`gtimeout`), TTY interrupts (`Ctrl-C` via `write_stdin("\u0003")`), and MCP-console session restart for process-tree cleanup; escalate only for explicit user-requested OS-level kill actions.
- Prefer one happy-path implementation; fail fast on violated assumptions.
- Keep feature deltas small and reversible.
- Every runtime change must pass correctness + perf gates before merge.
- Do not accept threaded wins that regress serial baseline beyond thresholds.
- Encode recurring review feedback into docs, linters, or tests so quality compounds.
- Treat plans as first-class artifacts in `docs/exec-plans/` with active/completed state.
