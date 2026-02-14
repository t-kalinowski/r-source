# Memory + GC (Agent Scope)

Focus: interpreter-local heap/protect stacks, ownership transfer/splicing, GC coordination.

Key files:

- `src/main/memory.c`
- `src/include/Defn.h`
- `src/include/Rinlinedfuns.h`

Target model:

- each subinterpreter has its own protect stack,
- each subinterpreter allocates from its own heap/arena,
- returned values can be safely transferred/spliced to the main interpreter domain,
- worker GC should avoid pausing unrelated threads whenever possible.

Non-negotiable perf constraint:

- serial main-thread allocation/protect/eval path must be near-upstream when threadpool idle.

Investigation priority for regressions:

1. protect/unprotect hot path,
2. allocVector/CONS fast paths,
3. evaluator hot-state indirection,
4. shared-state coordination branches on serial path.
