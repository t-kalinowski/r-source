# Eval + Environments (Agent Scope)

Focus: evaluator semantics, closure environment resolution, global mutation guardrails.

Key files:

- `src/main/eval.c`
- `src/main/envir.c`
- `src/main/apply.c`

Target semantics:

- Worker evaluation should support ordinary closure code and lexical scoping.
- Worker reads of global/search-path state are allowed.
- Worker writes to process-global state are rejected with clear recoverable errors.
- Main-thread serial `lapply` path should stay parity with upstream.

Guardrails to enforce:

- forbid `setwd()` and equivalent process-global mutations in workers,
- forbid assignment into `globalenv()` from workers,
- allow local mutation in closure-local environments,
- preserve normal traceback behavior on worker-thrown errors.
