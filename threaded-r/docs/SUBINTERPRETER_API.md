# Subinterpreter API (Source Investigation)

This document records what the subinterpreter API actually is in the current
branch, based on source inspection.

Scope:

- Internal/runtime API only (C internals and runtime behavior).
- Not user-facing R wrappers (`mtlapply`, `background`, ...), except where
  they drive subinterpreter lifecycle.

Primary sources:

- `src/include/Defn.h`
- `src/main/main.c`
- `src/main/mtl-interpreter.c`
- `src/main/mtl-compat.c`
- `src/main/apply.c`
- `src/main/memory.c`
- `src/main/envir.c`
- `src/main/options.c`
- `src/main/errors.c`
- `src/main/context.c`
- `src/main/names.c`
- `src/main/Rdynload.c`
- `src/main/source.c`
- `src/main/gram.c`
- `src/main/dotcode.c`

## 1) What is the current subinterpreter API?

There is no single dedicated `subinterpreter.c` API surface yet. The current
API is a set of internal functions/macros split across `Defn.h`, `memory.c`,
and `apply.c`.

### 1.1 Core state object

`R_InterpreterState` in `src/include/Defn.h:1583` is the central object.
It carries:

- heap/GC state pointer and flags (`heap`, `gcEnabled`, `in_gc`),
- eval/error stacks (`currentExpr`, `returnedValue`, handlers/restarts),
- protect stack (`ppStackTop`, `ppStack`),
- bytecode stack and frame state (`bcNodeStack*`, `bcProt*`, `bc*` fields),
- context pointers (`globalContext`, `toplevelContext`, `sessionContext`, ...),
- worker flags/options snapshots (`isMTLWorker`, `mtlOptionsBase`, `mtlOptions`),
- legacy worker-global fields (`mtlGlobalEnvRedirect`, `workerGlobalEnv`).

### 1.2 Lifecycle entrypoints

Declared in `src/include/Defn.h:1643-1648`, implemented in
`src/main/memory.c`:

- `R_InitInterpreterProtectStack()` (`src/main/memory.c:3308`)
- `R_InitInterpreterBCNodeStack()` (`src/main/memory.c:3322`)
- `R_InitInterpreterHeap()` (`src/main/memory.c:3361`)
- `R_DestroyInterpreterHeap()` (`src/main/memory.c:3386`)
- `R_RegisterInterpreterState()` (`src/main/memory.c:3285`)
- `R_UnregisterInterpreterState()` (`src/main/memory.c:3293`)

### 1.3 TLS and compatibility entrypoints

- `R_mtl_interpreter_tls_or_main()` (`src/main/mtl-interpreter.c:19`) for
  out-of-line TLS lookup.
- `R_mtl_interpreter_set()` inline in `src/include/Defn.h:1780`.
- `R_mtl_set_compat_interpreter()` (`src/main/mtl-compat.c:99`) to keep the
  legacy TLS symbol `R_Interpreter` and legacy `_R_*` globals compatible.
- `R_mtl_sync_compat_exports()` (`src/main/mtl-compat.c:72`) to synchronize
  legacy exported globals when safe (`!R_MTL_THREADING_ACTIVE`).

### 1.4 Threaded-mode gates and locks

- Mode flag: `R_MTL_THREADING_ACTIVE` (`src/include/Defn.h:1674-1680`).
- Setter: `R_mtl_set_threading_active()` (`src/main/memory.c:216`).
- Heap coordination lock family:
  - `R_mtl_heap_lock/unlock/unlock_all` (`src/main/memory.c:376-425`).
- Global lock family:
  - `R_mtl_global_lock/unlock/unlock_all` (`src/main/memory.c:446-469`).
- Shared-env reader/writer gate:
  - `R_mtl_shared_env_reader_enter/exit`,
  - `R_mtl_shared_env_writer_begin/end/unlock_all`
  in `src/main/apply.c:399-510`.

### 1.5 Main-thread RPC entrypoint

- `R_mtl_invoke_on_main_reason()` (`src/main/apply.c:1044`) is the internal
  control-plane bridge for worker->main operations that must touch shared
  process runtime state.

## 2) Worker subinterpreter lifecycle (actual flow)

### 2.1 Construction

Workers are created in `mtl_pool_ensure_threads()` (`src/main/apply.c:1491`):

1. Allocate `mtl_worker_t` with embedded `R_InterpreterState`.
2. `mtl_interp_init_from_main()` (`src/main/apply.c:264`) sets defaults.
3. Initialize per-interpreter stacks/heap:
   - `R_InitInterpreterProtectStack` (`:327`)
   - `R_InitInterpreterBCNodeStack` (`:330`)
   - `R_InitInterpreterHeap` (`:333`)
4. Spawn thread running `mtl_pool_worker_main()` (`:1519`).

### 2.2 Thread start

`mtl_pool_worker_main()` (`src/main/apply.c:1166`) does:

1. Register interpreter in global interpreter registry (`:1173`).
2. Switch TLS interpreter pointer (`R_mtl_interpreter_set`, `:1176`).
3. Switch compatibility interpreter pointer (`R_mtl_set_compat_interpreter`, `:1177`).
4. Install a per-thread toplevel context backing `R_GlobalContext` usage
   assumptions (`:1179-1207`).
5. Enter worker loop (jobs/futures) (`:1214+`).

### 2.3 Per-evaluation reset

Before each eval, `mtl_interp_reset_for_eval()` (`src/main/apply.c:336`) resets:

- handler/restart stacks,
- warning/error flags,
- eval depth,
- bytecode stack tops,
- worker-local options to base snapshot.

Worker code also refreshes `R_Toplevel` snapshots before `R_tryEvalSilent()`
(`src/main/apply.c:1293-1311`, `1397-1411`).

### 2.4 Teardown

On worker thread exit:

- restore saved TLS/compat pointers (`src/main/apply.c:1476-1477`),
- unregister interpreter (`:1486`).

Important current behavior:

- explicit interpreter-heap destruction is not performed during worker teardown
  in this path; comment states OS reclaim at process exit (`:1482-1485`).

## 3) Stack/context contract in current implementation

### 3.1 What is explicit today

- Each worker has its own protect stack and bytecode stack.
- Each worker has its own toplevel context chain base (`R_Toplevel` per thread).
- Each evaluation resets worker interpreter stack-like state.

This is a concrete "entry reset" pattern (good base for a future formal API).

### 3.2 What is not explicit today

There is no first-class, explicit "read-only stack-frame barrier" API such as:

- "freeze frames above depth N",
- "forbid writes crossing stack fence",
- "capability-tagged frame mutation policy".

Instead, safety is currently enforced mostly through:

- environment mutation guards,
- heap ownership checks,
- main-thread RPC for shared global caches/tables,
- error-unwind lock release discipline.

## 4) Mutation and read-only boundaries (actual behavior)

### 4.1 Environment mutation policy

Enforced in `src/main/envir.c`:

- assignment to `globalenv()` from worker errors
  (`mtl_check_globalenv_assignment`, `:1331-1336`),
- shared env writes from worker error
  (`mtl_worker_shared_env_write`, `:1338-1351`,
  checked in `defineVar` / `setVarInFrame`).

Shared-env reads by workers are coordinated with a reader/writer gate:

- reader enter/exit (`src/main/apply.c:399-453`),
- main-thread writer begin/end waits for reader quiescence (`:455-495`).

### 4.2 Options mutation policy

Workers use per-job options snapshots:

- lazy snapshot and local copy-on-write
  (`src/main/options.c:133-169`, `368-375`),
- setting `options(threads=...)` from worker is forbidden (`:171-179`, `:379-381`).

### 4.3 Process-global mutation policy

Example explicit guard:

- `setwd()` forbidden in worker (`src/main/util.c:849-850`).

### 4.4 Finalizer policy

- R-level finalizers in workers are forbidden:
  `R_MakeWeakRef` guard (`src/main/memory.c:2352-2353`).

## 5) Global-state operations: RPC vs lock

Current code uses two patterns.

### 5.1 Worker->main RPC (preferred for global tables)

Via `R_mtl_invoke_on_main_reason` (`src/main/apply.c:1044`), used for:

- symbol interning `install` / `installNoTrChar` (`src/main/names.c:1326`, `1388`),
- CHARSXP interning `mkCharLenCE` (`src/main/envir.c:4888`),
- `dyn.load` / `dyn.unload` (`src/main/Rdynload.c:1286`, `1314`),
- C-callable registration/lookup (`src/main/Rdynload.c:1847-1874`),
- targeted on-main eval helper `.Internal(mtonmain())` (`src/main/apply.c:2341`).

### 5.2 Global lock in worker thread (when args/state are worker-local)

Used for operations that cannot safely shuttle worker-owned SEXPs to main:

- parser paths (`do_parse`, `R_ParseConn`, `R_ParseVector`):
  `src/main/source.c:215-227`, `src/main/gram.c:4319-4354`,
- `.C` / `.Fortran` path (`do_dotCode`):
  `src/main/dotcode.c:2765-2772`.

`.Call` path is not blanket-locked (`src/main/dotcode.c:1486-1489`).

## 6) Heap/GC ownership model and subinterpreter constraints

### 6.1 Per-interpreter heap

Worker heap initialization:

- `R_InitInterpreterHeap` marks worker heap with `isWorker=1` and sets
  worker-local size budgets (`src/main/memory.c:3361-3384`).

### 6.2 Ownership checks

- `R_mtl_current_heap_owns()` (`src/main/memory.c:1316`) is a core predicate
  for "is this object private to this interpreter heap?"
- GC ownership uses `mtl_gc_owns_node()` (`:1294`) and owner maps.

### 6.3 Heap transfer

- `R_mtl_adopt_worker_heap()` (`src/main/memory.c:3449`) splices worker heap
  structures into destination heap and resets worker heap roots/lists.

This is currently the main result-transfer mechanism for threaded `mtlapply`.

## 7) Error and unwind contract

- Worker errors use per-thread error buffers (`src/main/errors.c:721-730`).
- Worker error path avoids recursive full error machinery:
  `mtl_worker_jump_to_top_simple()` (`src/main/errors.c:742-747`),
  worker branch in `verrorcall_dflt` (`:766-787`).
- On longjmp/toplevel error, internal locks are force-released:
  `src/main/context.c:245-250`, `823-829`.

For `mtlapply`, job cleanup on unwind:

- sets cancel/error, aborts pending RPC, waits worker quiescence:
  `src/main/apply.c:1737-1790`.

## 8) Current limitations relevant to API design

1. No formal subinterpreter API module yet.
- Behavior is spread across `apply.c`, `memory.c`, `Defn.h`.

2. No explicit stack-frame immutability barrier API.
- Current model enforces read-only behavior via env/heap/global guards.

3. Nested worker `mtlapply` is serial fallback.
- worker-invoked `mtlapply` uses `mtl_serial_apply_no_pool`:
  `src/main/apply.c:1919-1923`.

4. Worker JIT is disabled.
- `R_CheckJIT` worker guard (`src/main/eval.c:1600-1603`).

5. Legacy fields exist but are not active policy today.
- `mtlGlobalEnvRedirect`, `workerGlobalEnv` are present in state but not used
  as a current "shadow globalenv" mechanism.

## 9) Implications for a cleaner subinterpreter API (for the rebuild)

A natural internal API should make these boundaries explicit:

1. Lifecycle API
- create/init/register,
- enter-thread (TLS + compat),
- eval-reset,
- teardown/destroy.

2. Entry snapshot API
- capture baseline context/stack/options state,
- reset to baseline before each task.

3. Mutability capability API
- explicit policy for env categories:
  - local/private heap env: mutable,
  - shared/search-path/global/base env: read-only.

4. Global operation routing API
- central registry: RPC vs global-lock.
- reason-tagged telemetry for routing pressure.

5. Ownership/transfer API
- explicit result handoff contract (adopt/splice/copy).
- explicit prohibition rules for cross-heap mutation.

6. Unwind safety API
- one place for lock-release-on-unwind invariants.

The current code already contains most primitives needed for this shape; they
are just not yet consolidated into one intentional interface.
