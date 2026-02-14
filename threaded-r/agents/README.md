# Scoped Agent Docs

These files are intentionally split to keep agent context small.

Suggested loading pattern:

1. Always load top-level `threaded-r/AGENTS.md`.
2. Load exactly one or two scoped docs for the active task area.
3. Avoid loading unrelated scope docs in the same context window.

Scope map:

- `runtime-core.md`: process/runtime lifecycle + unwind boundaries.
- `eval-and-environments.md`: closure semantics + mutation guardrails.
- `memory-gc.md`: alloc/protect/GC and ownership transfer.
- `threadpool-and-futures.md`: queue model + user API.
- `package-abi.md`: package/native compatibility constraints.
- `bench-and-validation.md`: benchmark and regression gates.

Agent-first operating docs:

- `../docs/AGENT_FIRST_PATTERNS.md`
- `../docs/KNOWLEDGE_BASE_LAYOUT.md`
