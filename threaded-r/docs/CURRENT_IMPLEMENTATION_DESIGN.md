# Threaded R: Current Implementation Design (Source-Verified)

This document describes the implementation that exists **today** in this tree (not a proposed redesign), with direct source anchors for each claim.

Scope:

- Sub-interpreter runtime shape and APIs.
- `mtlapply`, `background`, `then`, `wait`, `cancel`.
- Serial fast path vs threaded mode switching.
- Heap/GC model and cross-heap result handling.
- Environment/global-state guardrails.
- Worker/main RPC for shared process state.
- Error handling and unwind behavior.

Primary source files:

- `src/include/Defn.h`
- `src/main/main.c`
- `src/main/mtl-compat.c`
- `src/main/mtl-interpreter.c`
- `src/main/apply.c`
- `src/main/memory.c`
- `src/main/envir.c`
- `src/main/options.c`
- `src/main/errors.c`
- `src/main/Rdynload.c`
- `src/main/dotcode.c`
- `src/main/source.c`
- `src/main/gram.c`
- `src/main/util.c`
- `src/library/base/R/mtlapply.R`
- `src/library/base/R/background.R`
- `src/library/base/man/mtlapply.Rd`
- `src/library/base/man/background.Rd`
- `tests/mtlapply.R`
- `tests/mtfuture.R`

## 1) Public API Surface (Current)

### 1.1 User-facing R functions

- `mtlapply(X, FUN, ...)` in `src/library/base/R/mtlapply.R:19`
- `background(expr, env = parent.frame())` in `src/library/base/R/background.R:84`
- `then(future, fn, ...)` in `src/library/base/R/background.R:96`
- `wait(futures, timeout = Inf)` in `src/library/base/R/background.R:119`
- `cancel(future)` in `src/library/base/R/background.R:166`

### 1.2 Option controls

- Pool size: `options(threads = n)`:
  - consumed by `mtlapply` wrapper: `src/library/base/R/mtlapply.R:22`
  - consumed by `background`/`then` internals: `src/main/apply.c:2372-2377`, `src/main/apply.c:2441-2446`
- Chunking for large `mtlapply`: `options(mtlapply.chunk_size = n)` in `src/library/base/R/mtlapply.R:30-45`.

### 1.3 Internal entrypoints

Registered internals include:

- `mtlapply`, `mtbackground`, `mtthen`, `mtwait`, `mtcancel`
- diagnostics and control: `mtlpoolstats`, `mtlrpcstats`, `mtlsharedenvstats`, `mtlruntimestats`, `mtnotifystats`, `mtlpoolreset`, `mtnotifyfd`, `mtnotifydrain`, `mtnotifysignal`, `mtlisworker`, `mtonmain`

See `src/main/names.c:657-676`.

## 2) Core Runtime Model

### 2.1 Interpreter state (sub-interpreter object)

`R_InterpreterState` is the per-interpreter state carrier in `src/include/Defn.h:1583-1641`.

Key fields used by threading:

- Heap/GC and stack state:
  - `heap`, `gcEnabled`, `in_gc`
  - protect stack: `ppStackTop`, `ppStack`
  - bytecode stack state: `bcNodeStack*`, `bcProt*`
- Eval/error/warning state:
  - `currentExpr`, `returnedValue`, `handlerStack`, `restartStack`
  - `inError`, `inWarning`, `inPrintWarnings`
- Worker-specific:
  - `isMTLWorker`
  - options snapshots: `mtlOptionsBase`, `mtlOptions`
  - legacy/experimental fields still present: `mtlGlobalEnvRedirect`, `workerGlobalEnv`

Main interpreter instance:

- `R_Interpreter0` initialized in `src/main/main.c:59-92`.

TLS plumbing:

- `R_InterpreterTLS` / `R_InterpreterMain` declarations in `src/include/Defn.h:1753-1764`.
- out-of-line TLS helper `R_mtl_interpreter_tls_or_main()` in `src/main/mtl-interpreter.c:19-22`.

### 2.2 Compatibility exports for prebuilt binaries

- Legacy TLS symbol `R_Interpreter` + legacy `_R_*` globals exported in `src/main/mtl-compat.c:48-70`.
- Sync routine `R_mtl_sync_compat_exports()` in `src/main/mtl-compat.c:72-97` (no-op while threaded mode active).
- Worker thread swaps compat pointer via `R_mtl_set_compat_interpreter()` in `src/main/mtl-compat.c:99-105`.

## 3) Data Structures (Thread Pool / Jobs / Futures)

Defined in `src/main/apply.c`.

### 3.1 `mtl_pool_t` (`src/main/apply.c:139-164`)

- Lifecycle: `inited`, `shutdown`
- Worker arrays: `nthreads`, `threads`, `workers`
- Coordination: `mu`, `cv`
- Job stack: `job_depth`, `job_top`, `gen`
- Worker->main RPC queue: `rpc_head`, `rpc_tail`, `rpc_aborted`
- Futures queue/list: `future_q_head`, `future_q_tail`, `future_all`, `future_running`
- Notify pipe fds: `notify_fd_read`, `notify_fd_write`

### 3.2 `mtl_job_t` (`src/main/apply.c:91-111`)

- Work payload: `XX`, `FUN`, `eval_env`, `tail0`, `n`
- Output transport: `results` (worker-owned SEXP pointers)
- Scheduling: per-worker work ranges `range_next`, `range_end`
- State: `error`, `cancel_requested`, `active_eval_workers`, `workers_done`
- Ownership/lifetime: `refcount`, `parent_job`
- Error text + mutex: `errmsg`, `err_mutex`

### 3.3 `mtl_future_t` (`src/main/apply.c:71-89`)

- Promise-like shape:
  - `expr`, `env`, `value`
  - chain support: `parent`, `cont_fun`, `cont_args`, `deps_head`, `next_dep`, `dep_count`
- Status and cancellation:
  - `status` (`PENDING/RUNNING/FULFILLED/REJECTED/CANCELLED`)
  - `cancel_requested`, `detached`, `enqueued`
- Error text: `errmsg`

### 3.4 `mtl_worker_t` (`src/main/apply.c:113-120`)

- `id`, `pool`, `seen_gen`
- per-worker lookup flag: `in_shared_lookup`
- embedded sub-interpreter state: `interp`

## 4) Architecture Diagrams

### 4.1 High-level topology

```text
                     +---------------------------+
                     |         Main thread       |
                     |---------------------------|
                     | do_mtlapply / mtwait      |
                     | services RPC queue         |
                     | adopts worker heaps        |
                     +-------------+-------------+
                                   |
                     mtl_pool.mu + mtl_pool.cv
                                   |
          +------------------------+------------------------+
          |                                                 |
   +------+-------+                                  +------+-------+
   | job stack    |                                  | future queue |
   | job_top/depth|                                  | pending/running
   +------+-------+                                  +------+-------+
          |                                                 |
   +------+------------------- workers ---------------------+------+
   |                    |                     |                    |
 +---+               +---+                 +---+                +---+
 |W0 |               |W1 |       ...       |Wk |                |Wn |
 |interp state       |interp state         |interp state         |...
 |heap + stacks      |heap + stacks        |heap + stacks
 +---+               +---+                 +---+
```

### 4.2 Mode switch state machine

```text
Idle mode
  R_MTL_THREADING_ACTIVE = 0
  (serial fast paths)
        |
        | enqueue mtlapply job OR futures queued/running
        v
Threaded mode
  R_MTL_THREADING_ACTIVE = 1
  (worker-aware paths enabled)
        |
        | no active jobs and no queued/running futures
        v
Idle mode
```

Implementation:

- Active-task predicate: `mtl_pool_has_active_tasks_locked()` in `src/main/apply.c:962-970`.
- Flag update: `mtl_set_threading_active_locked()` in `src/main/apply.c:972-978`.
- Global flag setter: `R_mtl_set_threading_active()` in `src/main/memory.c:216-230`.

### 4.3 Worker->main RPC flow

```text
Worker calls R_mtl_invoke_on_main_reason(fun,data,reason)
  -> enqueue mtl_main_req_t on pool.rpc_head/tail
  -> signal cv (+ notify fd)
  -> worker waits on req.cv

Main thread (while waiting in mtlapply/wait/cancel)
  -> mtl_rpc_service_locked()
  -> R_ToplevelExec(fun,data)
  -> stores result/error on req, signals worker
```

Code:

- request API: `src/main/apply.c:1039-1122`
- service loop: `src/main/apply.c:666-701`
- abort: `src/main/apply.c:703-732`

## 5) `mtlapply` Execution Flow (Current)

### 5.1 Call path

- R wrapper (`src/library/base/R/mtlapply.R`):
  - validate `threads` from `options(threads)`
  - if `threads == 1`, direct fallback to `lapply` (`:26-27`)
  - chunk large input by `options(mtlapply.chunk_size)` (`:29-45`)
  - call `.Internal(mtlapply(...))`
- Internal implementation: `do_mtlapply()` in `src/main/apply.c:1861-2042`.

### 5.2 Worker thread lifecycle

Workers are created lazily and persist:

- pool init: `mtl_pool_init_if_needed()` `src/main/apply.c:615-630`
- ensure size: `mtl_pool_ensure_threads()` `src/main/apply.c:1491-1525`
- worker main loop: `mtl_pool_worker_main()` `src/main/apply.c:1166-1489`

Each worker:

- registers interpreter state (`src/main/apply.c:1173`)
- switches TLS/compat interpreter (`:1175-1177`)
- initializes per-thread toplevel context (`:1179-1207`)
- loops waiting for job or future (`:1219-1247`)

### 5.3 Work scheduling

- `mtlapply` jobs are stacked (`job_top`, `job_depth`) in `mtlapply_run()` (`src/main/apply.c:1697-1700`).
- Worker claims indices using per-worker ranges + stealing:
  - `mtl_job_init_ranges()` `:759-779`
  - `mtl_job_claim_index()` `:791-807`
- Nested top-of-stack job precedence is encoded via `job_top` and generation `gen`.

### 5.4 Result transport and ownership

- Worker stores per-index result in `job->results[i]` (`src/main/apply.c:1349`).
- Worker roots results in worker `preciousList` (`:1350-1355`).
- Main thread adopts each worker heap (`R_mtl_adopt_worker_heap`) before attaching results (`src/main/apply.c:2003-2021`).
- Final answer list receives pointers without serialization (`:2009-2013`).

## 6) Futures (`background`/`then`/`wait`/`cancel`)

### 6.1 API behavior

- `background()` returns classed `mt_future` list with fields `expr`, `env`, `value` and external pointer attr `ptr` (`src/library/base/R/background.R:84-93`).
- `then()` creates continuation future (`src/library/base/R/background.R:96-107`).
- `wait()` returns next completed future from a set (`src/library/base/R/background.R:119-164`).
- `cancel()` is best-effort pending/running cancel (`src/library/base/R/background.R:166-173`).

### 6.2 Internal mechanics

- enqueue background: `do_mtbackground()` `src/main/apply.c:2358-2419`
- enqueue continuation: `do_mtthen()` `src/main/apply.c:2422-2504`
- wait-any: `do_mtwait()` `src/main/apply.c:2507-2594`
- cancel: `do_mtcancel()` `src/main/apply.c:2650-2683`

`wait()` implementation details:

- uses `pthread_cond_wait` / `pthread_cond_timedwait` on `mtl_pool.cv`, not a spin loop (`src/main/apply.c:2561-2566`).
- main thread also services RPC while waiting (`src/main/apply.c:2543-2545`).

### 6.3 Notify fd for event-loop integration

- pool holds pipe fds (`src/main/apply.c:162-163`).
- signal on RPC enqueue and future completion (`src/main/apply.c:564-581`, `1089-1091`, `876`).
- read side exposed by `.Internal(mtnotifyfd)` and drain helper `.Internal(mtnotifydrain)` (`src/main/apply.c:2596-2628`).
- R helpers `.mt_notify_fd()` / `.mt_notify_drain()` in `src/library/base/R/background.R:109-117`.

## 7) Serial Path Preservation (Current Implementation)

This implementation already has explicit idle-mode fast paths. The intended contract is: when threadpool has no active work, serial code stays close to upstream behavior.

### 7.1 Global mode gate

- Single runtime flag: `R_MTL_THREADING_ACTIVE` in `src/include/Defn.h:1674-1680`.
- Flag off unless pool has active tasks (`src/main/apply.c:962-978`).

### 7.2 Fast paths guarded by `!R_MTL_THREADING_ACTIVE`

Examples:

- env assignment/update:
  - `defineVar`: early fast path in `src/main/envir.c:2022-2025`
  - `setVarInFrame`: early fast path in `src/main/envir.c:2161-2163`
- heap coordination:
  - `heap_alloc_enter/exit` early returns in `src/main/memory.c:282-286`, `340-344`
  - `R_mtl_heap_lock/unlock` no-op when inactive: `src/main/memory.c:376-405`
- protect stack hot path uses `R_Interpreter0` directly when inactive:
  - `protect`: `src/main/memory.c:5498-5503`
  - `unprotect`: `src/main/memory.c:5516-5521`
  - similar for `unprotect_ptr`, `R_ProtectWithIndex`, `R_Reprotect`: `src/main/memory.c:5534-5624`
- findVar serial loop bypasses worker gate checks:
  - `src/main/envir.c:1485-1496`, `1513-1521`, `1554-1565`
- arithmetic reuse keeps a serial branch:
  - `src/main/arithmetic.h:80-96`
  - scalar helpers `src/main/arithmetic.c:378-395`

### 7.3 Explicit design requirement (this project direction)

Target behavior to preserve:

- Keep serial path at parity when pool idle.
- Activate worker-aware coordination only while active tasks exist.

Current mechanism enabling this target is the mode gate above; any future changes should preserve this property.

## 8) Heap / GC / Ownership Model (Current)

### 8.1 Per-interpreter heap state

Heap state struct: `R_mtl_heap_state_` in `src/main/memory.c:964-984`.

- Main heap singleton: `R_MainHeapState` (`src/main/memory.c:986`).
- Worker heaps allocated via `R_InitInterpreterHeap()` (`src/main/memory.c:3361-3384`).

### 8.2 Ownership checks

- runtime ownership query: `R_mtl_current_heap_owns()` in `src/main/memory.c:1316-1327`.
- GC ownership helper: `mtl_gc_owns_node()` in `src/main/memory.c:1294-1312`.
- page header owner metadata: `PAGE_HEADER.owner` `src/main/memory.c:933-940`.
- large-node owner map: `src/main/memory.c:1028-1105`.

### 8.3 Main-heap routing for global intern tables

- `R_mtl_switch_to_main_heap` / `R_mtl_restore_heap`: `src/main/memory.c:993-1007`.
- used for intern-table correctness where needed (symbol/CHARSXP operations are also routed by RPC; see sections 10 and 11).

### 8.4 Worker heap adoption

- `R_mtl_adopt_worker_heap()` `src/main/memory.c:3449-3552`.
- Splices page lists and node lists into destination heap under heap lock (`:3464-3545`), then resets worker heap (`:3547-3552`).

### 8.5 Current GC status in workers

- Worker interpreters are initialized with `gcEnabled = 0` in `src/main/apply.c:266-271`.
- Comment states this avoids corruption while workers hold shared references to main-heap objects.

## 9) Environment / Global-State Semantics

### 9.1 Lookup

- worker read coordination for shared env lookup:
  - reader enter/exit API in `src/main/apply.c:399-453`
  - used in lookup paths in `src/main/envir.c:1501-1505`, `1523-1527`, `1570-1574`, `1591-1594`, and `findFun` path `1891-1904`.
- worker lookups force read-only treatment on values:
  - `ENSURE_NAMEDMAX` in `findVarInFrame3` (`src/main/envir.c:1018-1023`, `1051-1053`, `1067-1069`, `1325-1326`).

### 9.2 Mutation guardrails

- direct assignment to global env from worker errors:
  - check in `src/main/envir.c:1331-1336`
- writes to shared environments from worker error:
  - checks in `src/main/envir.c:1340-1351`, called by
  - `defineVar` (`src/main/envir.c:2029-2030`)
  - `setVarInFrame` (`src/main/envir.c:2167-2168`)
- `setwd()` in worker errors: `src/main/util.c:849-850`

### 9.3 Main-thread shared-env mutation gate

- writer begin/end/wait path:
  - `R_mtl_shared_env_writer_begin()` `src/main/apply.c:455-476`
  - `R_mtl_shared_env_writer_end()` `src/main/apply.c:478-495`
- env mutation classification:
  - `mtl_main_shared_env_mutation()` `src/main/envir.c:1414-1445`
  - `mtl_main_shared_searchpath_mutation()` `src/main/envir.c:1447-1457`

## 10) Options Semantics in Workers

### 10.1 Worker-local options snapshot

- options snapshot duplicated per job for workers in `src/main/apply.c:1250-1264`.
- reset before each eval in `mtl_interp_reset_for_eval()` `src/main/apply.c:347-349`.

### 10.2 `options()` behavior

- worker path uses local copy-on-write options list:
  - init/snapshot helpers in `src/main/options.c:133-169`
  - local set path `SetOptionLocal` in `src/main/options.c:368-375`
- setting `threads` in worker is explicitly disallowed:
  - `worker_disallows_option_set` `src/main/options.c:171-179`
  - error at `src/main/options.c:379-381`

## 11) Native Interface Behavior (`.Call`, `.External`, loading)

### 11.1 `.Call`

- `.Call` uses `do_dotcall` -> `do_dotcall_impl` without blanket worker global lock in `src/main/dotcode.c:1410-1489`.
- This means native code may run concurrently in worker interpreters; global-state-sensitive internals are protected elsewhere by targeted lock/RPC.

### 11.2 `.C` / `.Fortran`

- worker path for `do_dotCode` takes `R_mtl_global_lock` around `do_dotCode_impl`:
  - `src/main/dotcode.c:2765-2773`.

### 11.3 Package load/unload and C-callable registry

- `dyn.load` / `dyn.unload` from worker are routed to main via RPC:
  - `src/main/Rdynload.c:1286-1295`, `1314-1317`.
- `R_RegisterCCallable` / `R_GetCCallable` from worker are RPC-routed:
  - `src/main/Rdynload.c:1849-1857`, `1864-1872`.

### 11.4 Symbol / string interning

- `install` / `installNoTrChar` from worker use main RPC:
  - `src/main/names.c:1326-1336`, `1388-1403`.
- `mkCharLenCE` from worker uses main RPC and copies input buffer before request:
  - `src/main/envir.c:4888-4905`.

### 11.5 Parsing and related global parser state

- worker parse paths run under global lock in worker thread:
  - `do_parse`: `src/main/source.c:217-225`
  - `R_ParseConn`: `src/main/gram.c:4319-4329`
  - `R_ParseVector`: `src/main/gram.c:4341-4354`

## 12) Error Handling and Recovery

### 12.1 Worker error unwind model

- Worker-specific direct unwind in `errors.c`:
  - `mtl_worker_jump_to_top_simple()` `src/main/errors.c:742-747`
  - worker branch in `verrorcall_dflt` `src/main/errors.c:766-787`

### 12.2 `mtlapply` job cancellation on unwind

- `R_UnwindProtect` wraps job run in `do_mtlapply`:
  - `src/main/apply.c:1978-1984`
- cleanup sets error/cancel, aborts RPC, waits worker quiescence:
  - `src/main/apply.c:1737-1790`

### 12.3 Lock-release safety on non-local jumps

- unlock all internal locks before longjmp in `R_jumpctxt`:
  - `src/main/context.c:245-250`
- similar safety in `R_ToplevelExec` error path:
  - `src/main/context.c:823-829`

### 12.4 Pool survivability after errors

- `do_mtlapply` keeps pool alive after worker error and rethrows consolidated message:
  - `src/main/apply.c:1987-2001`.

## 13) Nested Behavior (Current)

- Pool has a job stack (`job_top`, `job_depth`) and generation signaling (`src/main/apply.c:149-152`, `1697-1702`).
- However, `mtlapply` called from a worker currently executes serially without pool participation:
  - `if (in_worker) ... mtl_serial_apply_no_pool(...)` in `src/main/apply.c:1919-1923`.
- So nested `mtlapply` in worker closures is correct semantically but not nested-parallel.

## 14) Known Guardrails / Limitations (Current)

- Global env writes from workers are forbidden (`src/main/envir.c:1333-1335`).
- Shared env writes from workers are forbidden (`src/main/envir.c:2029-2030`, `2167-2168`).
- `setwd()` forbidden in workers (`src/main/util.c:849-850`).
- Worker setting `options(threads=...)` forbidden (`src/main/options.c:171-179`, `379-381`).
- R-level finalizers in workers forbidden (`src/main/memory.c:2352-2353`).
- Worker JIT compilation disabled (`src/main/eval.c:1598-1603`).

## 15) Verification Ledger (Design Points -> Source)

The list below maps each design point to a concrete implementation location.

1. Sub-interpreter state object exists and is per-thread.
   - `src/include/Defn.h:1583-1641`
2. Worker threads hold embedded interpreter state.
   - `src/main/apply.c:113-120`
3. Worker threads initialize protect stack, BC node stack, and heap.
   - `src/main/apply.c:326-334`
4. Worker threads set TLS + compat interpreter before eval.
   - `src/main/apply.c:1175-1177`
5. Pool is persistent and grows to needed size.
   - `src/main/apply.c:1491-1525`
6. `mtlapply` uses option-driven thread count and `threads=1` wrapper fallback.
   - `src/library/base/R/mtlapply.R:22-27`
7. Chunking is wrapper-level and controlled by `mtlapply.chunk_size`.
   - `src/library/base/R/mtlapply.R:30-45`
8. Main thread coordinates worker job completion and services RPC.
   - `src/main/apply.c:1709-1715`
9. Worker results are transferred through worker heaps + main adoption.
   - `src/main/apply.c:1349-1355`, `2003-2021`
10. Futures share same pool and queue.
    - `src/main/apply.c:158-161`, `2364-2381`, `2448-2450`
11. `wait` is condvar-based (not spin) and wait-any.
    - `src/main/apply.c:2541-2566`
12. Notify fd exists for external loop wakeups.
    - `src/main/apply.c:540-562`, `2596-2628`
13. Threaded mode gate toggles by active jobs/futures.
    - `src/main/apply.c:962-978`
14. Heap and env hot paths have explicit idle fast branches.
    - `src/main/memory.c:282-286`, `340-344`, `5498-5521`
    - `src/main/envir.c:2022-2025`, `2161-2163`, `1485-1496`
15. Worker global env writes error cleanly.
    - `src/main/envir.c:1331-1336`
16. Shared env writes from workers error cleanly.
    - `src/main/envir.c:1340-1351`, `2029-2030`, `2167-2168`
17. Main-thread shared-env mutations wait for worker readers.
    - `src/main/apply.c:455-476`
18. Symbol and CHARSXP interning from workers is main-thread RPC.
    - `src/main/names.c:1326-1336`, `1388-1403`
    - `src/main/envir.c:4890-4905`
19. `dyn.load`/`dyn.unload` worker calls are main-thread RPC.
    - `src/main/Rdynload.c:1286-1295`, `1314-1317`
20. `.Call` path is not globally locked by default.
    - `src/main/dotcode.c:1410-1489`
21. `.C`/`.Fortran` worker path is global-locked.
    - `src/main/dotcode.c:2767-2771`
22. Worker errors use simplified unwind path.
    - `src/main/errors.c:739-747`, `766-787`
23. Lock cleanup on longjmp/toplevel error is explicit.
    - `src/main/context.c:245-250`, `823-829`
24. Worker `options()` writes are local snapshots; `threads` set disallowed.
    - `src/main/apply.c:1250-1264`
    - `src/main/options.c:133-169`, `171-179`, `379-381`
25. Worker `setwd` is forbidden.
    - `src/main/util.c:849-850`
26. R-level finalizers from worker are forbidden.
    - `src/main/memory.c:2352-2353`
27. Worker JIT disabled.
    - `src/main/eval.c:1598-1603`
28. Nested `mtlapply` in worker runs serial path.
    - `src/main/apply.c:1919-1923`
29. Background/then R wrappers snapshot lexical environment.
    - `src/library/base/R/background.R:19-50`, `73-81`, `84-88`, `96-101`
30. Tests cover these semantics and recovery paths.
    - `tests/mtlapply.R`
    - `tests/mtfuture.R`

## 16) Explicit Project Direction: Serial-Path Preservation + Mode Switch

For the next implementation iteration, preserve this contract explicitly:

1. **Serial idle mode should stay as close as possible to upstream path.**
   - Keep `R_MTL_THREADING_ACTIVE=0` fast branches intact.
   - Avoid new synchronization and extra indirections on the default serial path.
2. **Switch into threaded/sub-interpreter mode only while active work exists.**
   - `mtlapply` active job, queued futures, or running futures.
3. **Allow small overhead in threaded mode**, but keep main-thread coordination overhead minimal.
   - Prefer targeted lock/RPC around specific global-state touchpoints, not a coarse global interpreter lock.

This is consistent with current gating architecture (`src/main/apply.c:962-978`) and should remain a hard requirement moving forward.
