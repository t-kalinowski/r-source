/*
 *  R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 2000-2025  The R Core Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, a copy is available at
 *  https://www.R-project.org/Licenses/
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define R_USE_SIGNALS 1

#include <Defn.h>
#include <Internal.h>

#ifdef HAVE_PTHREAD
# include <pthread.h>
# include <stdatomic.h>
# include <stdint.h>
# include <limits.h>
# include <errno.h>
# include <time.h>
# include <unistd.h>
# include <fcntl.h>
#endif

static SEXP checkArgIsSymbol(SEXP x) {
    if (TYPEOF(x) != SYMSXP)
	error("argument must be a symbol");
    return x;
}

static const char *mtl_rpc_reason_name_lookup(int reason)
{
    static const char *names[R_MTL_RPC_REASON_COUNT] = {
	"other",
	"install",
	"installNoTrChar",
	"mkCharLenCE",
	"parseVector",
	"parseConn",
	"doParse",
    };
    if (reason < 0 || reason >= R_MTL_RPC_REASON_COUNT)
	return names[R_MTL_RPC_OTHER];
    return names[reason];
}

#ifdef HAVE_PTHREAD
	typedef enum {
	    MTL_FUTURE_PENDING = 0,
	    MTL_FUTURE_RUNNING = 1,
	    MTL_FUTURE_FULFILLED = 2,
	    MTL_FUTURE_REJECTED = 3,
	    MTL_FUTURE_CANCELLED = 4
	} mtl_future_status_t;

	typedef struct mtl_future_t_ {
	    SEXP expr;
	    SEXP env;
	    SEXP cont_fun;
	    SEXP cont_args;
	    SEXP value;
	    struct mtl_future_t_ *parent;
	    struct mtl_future_t_ *deps_head;
	    struct mtl_future_t_ *next_dep;
	    int dep_count;
	    mtl_future_status_t status;
	    int cancel_requested;
	    int detached;
	    int enqueued;
	    int main_showErrorMessages;
	    char errmsg[1024];
	    struct mtl_future_t_ *next_q;
	    struct mtl_future_t_ *next_all;
	} mtl_future_t;

	typedef struct mtl_job_t_ {
	    SEXP XX;
	    SEXP FUN;
	    SEXP eval_env;           /* call-site environment used for evaluation */
	    SEXP tail0;              /* DOTS as pairlist (main heap, duplicated per worker) */
	    R_xlen_t n;
	    SEXP *results;            /* C array of worker-owned SEXPs (adopted after join) */
	    atomic_long next;         /* next index to claim */
	    atomic_long *range_next;  /* per-worker next index for range scheduling */
	    long *range_end;          /* per-worker exclusive end index */
	    atomic_int error;         /* 0/1 */
	    atomic_int cancel_requested; /* 0/1 cooperative cancellation */
	    atomic_int active_eval_workers; /* workers currently evaluating/writing one item */
	    atomic_int workers_done;  /* workers that have fully exited this job */
	    int bg_threads;           /* number of worker threads assigned to this job */
	    atomic_int refcount;      /* heap lifetime across main + workers */
	    struct mtl_job_t_ *parent_job;
	    char errmsg[1024];
	    pthread_mutex_t err_mutex; /* protects errmsg */
	    int main_showErrorMessages;
	} mtl_job_t;

		typedef struct {
		    int id;
		    struct mtl_pool_t *pool;
		    unsigned long seen_gen;
		    atomic_int in_shared_lookup;
		    R_InterpreterState interp;
		} mtl_worker_t;

	typedef struct mtl_main_req_t {
	    /* Function to execute on the main thread (must not escape data). */
	    SEXP (*fun)(void *);
	    void *data;
	    int reason;

	    SEXP result;          /* valid when ok==1 */
	    int result_preserved; /* result is currently R_PreserveObject()-rooted */
	    int ok;               /* 1 success, 0 error/abort */
	    char errmsg[1024];    /* valid when ok==0 */

	    pthread_mutex_t mu;
	    pthread_cond_t cv;
	    int done;

	    struct mtl_main_req_t *next;
	} mtl_main_req_t;

	typedef struct mtl_pool_t {
	    int inited;
	    int shutdown;
	    int nthreads;
	    pthread_t *threads;
	    mtl_worker_t **workers;

	    pthread_mutex_t mu;
	    pthread_cond_t cv;

	    unsigned long gen;
	    int job_depth;
	    mtl_job_t *job_top; /* active job stack top */

	    /* Worker->main requests (e.g. global caches that must be mutated by main). */
	    mtl_main_req_t *rpc_head;
	    mtl_main_req_t *rpc_tail;
	    int rpc_aborted;

	    mtl_future_t *future_q_head;
	    mtl_future_t *future_q_tail;
	    mtl_future_t *future_all;
	    int future_running;
	    int notify_fd_read;
	    int notify_fd_write;
	} mtl_pool_t;

static int mtl_trace_cached = -1;
static int mtl_trace_enabled(void)
{
    if (mtl_trace_cached < 0)
	mtl_trace_cached = getenv("R_MTL_TRACE") ? 1 : 0;
    return mtl_trace_cached;
}

static atomic_int mtl_parallel_active = 0;
static atomic_int mtl_parallel_max = 0;
static atomic_ulong mtl_rpc_calls[R_MTL_RPC_REASON_COUNT];
static atomic_ulong mtl_rpc_errors[R_MTL_RPC_REASON_COUNT];
static atomic_ulong mtl_notify_signal_calls = 0;
static atomic_ulong mtl_notify_signal_write_ok = 0;
static atomic_ulong mtl_notify_signal_write_eagain = 0;
static atomic_ulong mtl_notify_signal_write_err = 0;
static atomic_ulong mtl_notify_drain_calls = 0;
static atomic_ulong mtl_notify_drain_bytes = 0;
static atomic_ulong mtl_notify_drain_eagain = 0;
static atomic_ulong mtl_notify_drain_err = 0;
static atomic_ulong mtl_notify_pipe_init_ok = 0;
static atomic_ulong mtl_notify_pipe_init_fail = 0;
static atomic_ulong mtl_shared_reader_enter_calls = 0;
static atomic_ulong mtl_shared_reader_retry_after_set = 0;
static atomic_ulong mtl_shared_reader_wait_loops = 0;
static atomic_ulong mtl_shared_reader_wait_condwait = 0;
static atomic_ulong mtl_shared_reader_exit_calls = 0;
static atomic_ulong mtl_shared_writer_begin_calls = 0;
static atomic_ulong mtl_shared_writer_wait_loops = 0;
static atomic_ulong mtl_shared_writer_wait_condwait = 0;
static atomic_ulong mtl_shared_writer_end_calls = 0;
static atomic_ulong mtl_shared_writer_unlock_all_calls = 0;
static atomic_ulong mtl_shared_writer_scan_calls = 0;
static atomic_ulong mtl_shared_writer_scan_workers = 0;
static int mtl_shared_stats_cached = -1;

static R_INLINE int mtl_shared_stats_enabled(void)
{
    if (mtl_shared_stats_cached < 0)
	mtl_shared_stats_cached = getenv("R_MTL_SHARED_STATS") ? 1 : 0;
    return mtl_shared_stats_cached;
}

static int mtl_rpc_sanitize_reason(int reason)
{
    if (reason < 0 || reason >= R_MTL_RPC_REASON_COUNT)
	return R_MTL_RPC_OTHER;
    return reason;
}

static unsigned long mtl_rpc_counter_read(atomic_ulong *x, int reset)
{
    if (reset)
	return atomic_exchange_explicit(x, 0, memory_order_relaxed);
    return atomic_load_explicit(x, memory_order_relaxed);
}

static void mtl_parallel_update_max(int cur)
{
    int old = atomic_load_explicit(&mtl_parallel_max, memory_order_relaxed);
    while (cur > old &&
	   !atomic_compare_exchange_weak_explicit(&mtl_parallel_max, &old, cur,
						 memory_order_relaxed,
						 memory_order_relaxed)) {
	/* retry */
    }
}

static int mtl_parallel_max_and_reset(void)
{
    return atomic_exchange_explicit(&mtl_parallel_max, 0, memory_order_relaxed);
}

static void mtl_parallel_begin(void)
{
    int cur = atomic_fetch_add_explicit(&mtl_parallel_active, 1, memory_order_relaxed) + 1;
    mtl_parallel_update_max(cur);
}

static void mtl_parallel_end(void)
{
    atomic_fetch_sub_explicit(&mtl_parallel_active, 1, memory_order_relaxed);
}
static int mtl_main_thread_inited = 0;
static pthread_t mtl_main_thread;
static pthread_mutex_t mtl_shared_env_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mtl_shared_env_cv = PTHREAD_COND_INITIALIZER;

static mtl_pool_t mtl_pool;
static atomic_int mtl_pool_threads_created = 0;
static atomic_int mtl_shared_env_mutate_pending = 0;
static R_THREAD_LOCAL int mtl_shared_env_reader_depth = 0;
static R_THREAD_LOCAL int mtl_shared_env_writer_depth = 0;
static R_THREAD_LOCAL mtl_worker_t *mtl_current_worker = NULL;
static SEXP mtl_dotOptionsSym = NULL;
static SEXP mtl_futurePtrSym = NULL;
static SEXP mtl_useFancyQuotesSym = NULL;

			static void mtl_interp_init_from_main(R_InterpreterState *st)
			{
		    st->heap = NULL;
		    /* Worker evaluation currently shares references to main-heap objects
		       (closures/environments/arguments). Keep worker GC disabled to
		       avoid tracing corruption while this representation is in use. */
		    st->gcEnabled = 0;
		    st->in_gc = 0;
		    st->mtlGlobalEnvRedirect = 0;
			    st->currentExpr = NULL;
			    st->returnedValue = R_NilValue;
			    st->handlerStack = R_NilValue;
			    st->restartStack = R_NilValue;
			    st->visible = TRUE;
		    st->showErrorMessages = 1;
		    st->allowOptionsSet = 0;
		    st->mtlOptionsBase = R_NilValue;
		    st->mtlOptions = R_NilValue;
		    st->isMTLWorker = 1;
		    st->workerGlobalEnv = NULL;
		    st->collectWarnings = 0;
	    st->warnings = R_NilValue;
	    st->evalDepth = 0;
    st->ppStackTop = 0;
    st->ppStack = NULL;
    st->parseError = 0;
    st->parseErrorCol = 0;
    st->parseErrorFile = NULL;
    st->parseErrorMsg[0] = '\0';
    st->parseContext[0] = '\0';
    st->parseContextLast = 0;
    st->parseContextLine = 0;
    st->cStackLimit = (uintptr_t) -1;
    st->oldCStackLimit = (uintptr_t) 0;
    st->cStackStart = (uintptr_t) -1;
    st->vStack = R_NilValue;
    st->preciousList = R_NilValue;
    st->expressions_keep = R_Expressions_keep;
    st->expressions = st->expressions_keep;
    st->bcNodeStackBase = NULL;
    st->bcNodeStackEnd = NULL;
    st->bcNodeStackTop = NULL;
    st->bcProtTop = NULL;
    st->bcProtCommitted = NULL;
    st->bcintactive = 0;
    st->bcpc = NULL;
    st->bcbody = NULL;
    st->bcframe = NULL;
    st->inError = 0;
    st->inWarning = 0;
    st->inPrintWarnings = 0;
    st->immediateWarning = 0;
    st->noBreakWarning = 0;
    st->next = NULL;
#ifdef R_USE_SIGNALS
    st->pendingPromises = NULL;
    st->toplevelContext = NULL;
    st->globalContext = NULL;
    st->sessionContext = NULL;
    st->exitContext = NULL;
#endif

	    /* Allocate per-interpreter protection stack for this worker. */
	    R_InitInterpreterProtectStack(st);

	    /* Allocate per-interpreter bytecode node stack for this worker. */
	    R_InitInterpreterBCNodeStack(st);

	    /* Allocate per-interpreter heap/GC state for this worker. */
	    R_InitInterpreterHeap(st);
	}

static void mtl_interp_reset_for_eval(R_InterpreterState *st, const mtl_job_t *job, SEXP eval_env)
{
    st->currentExpr = NULL;
    st->returnedValue = R_NilValue;
    st->handlerStack = R_NilValue;
    st->restartStack = R_NilValue;
    st->visible = TRUE;
    st->showErrorMessages = job->main_showErrorMessages;
    st->collectWarnings = 0;
    st->warnings = R_NilValue;
    st->evalDepth = 0;
    /* Reset worker-local options to the per-job base snapshot. */
    st->mtlOptions = st->mtlOptionsBase;
    st->expressions = st->expressions_keep;
    st->bcNodeStackTop = st->bcNodeStackBase;
    st->bcProtTop = st->bcNodeStackTop;
    st->bcProtCommitted = st->bcNodeStackBase;
    st->bcintactive = 0;
    st->bcpc = NULL;
    st->bcbody = NULL;
    st->bcframe = NULL;
    st->inError = 0;
    st->inWarning = 0;
    st->inPrintWarnings = 0;
    st->immediateWarning = 0;
    st->noBreakWarning = 0;
#ifdef R_USE_SIGNALS
    st->pendingPromises = NULL;
    if (TYPEOF(eval_env) == ENVSXP) {
	R_Toplevel.cloenv = eval_env;
	R_Toplevel.sysparent = eval_env;
    } else {
	R_Toplevel.cloenv = R_BaseEnv;
	R_Toplevel.sysparent = R_BaseEnv;
    }
#endif
}

static Rboolean mtl_is_main_thread(void)
{
    if (!mtl_main_thread_inited) {
	mtl_main_thread = pthread_self();
	mtl_main_thread_inited = 1;
    }
    return pthread_equal(mtl_main_thread, pthread_self());
}

static int mtl_any_worker_in_shared_lookup(void)
{
    if (__builtin_expect(mtl_shared_stats_enabled(), 0)) {
	atomic_fetch_add_explicit(&mtl_shared_writer_scan_calls, 1, memory_order_relaxed);
	atomic_fetch_add_explicit(&mtl_shared_writer_scan_workers,
				  (unsigned long) mtl_pool.nthreads, memory_order_relaxed);
    }
    for (int i = 0; i < mtl_pool.nthreads; i++) {
	mtl_worker_t *w = mtl_pool.workers ? mtl_pool.workers[i] : NULL;
	if (w != NULL &&
	    atomic_load_explicit(&w->in_shared_lookup, memory_order_acquire))
	    return 1;
    }
    return 0;
}

attribute_hidden void R_mtl_shared_env_reader_enter(void)
{
    if (!R_MTL_THREADING_ACTIVE)
	return;
    mtl_worker_t *w = mtl_current_worker;
    if (w == NULL)
	return;
    if (mtl_shared_env_reader_depth++ > 0)
	return;
    if (__builtin_expect(mtl_shared_stats_enabled(), 0))
	atomic_fetch_add_explicit(&mtl_shared_reader_enter_calls, 1, memory_order_relaxed);

    for (;;) {
	if (!atomic_load_explicit(&mtl_shared_env_mutate_pending, memory_order_acquire)) {
	    atomic_store_explicit(&w->in_shared_lookup, 1, memory_order_release);
	    if (!atomic_load_explicit(&mtl_shared_env_mutate_pending, memory_order_acquire))
		break;
	    if (__builtin_expect(mtl_shared_stats_enabled(), 0))
		atomic_fetch_add_explicit(&mtl_shared_reader_retry_after_set, 1, memory_order_relaxed);
	    atomic_store_explicit(&w->in_shared_lookup, 0, memory_order_release);
	}

	if (__builtin_expect(mtl_shared_stats_enabled(), 0))
	    atomic_fetch_add_explicit(&mtl_shared_reader_wait_loops, 1, memory_order_relaxed);
	pthread_mutex_lock(&mtl_shared_env_mu);
	while (atomic_load_explicit(&mtl_shared_env_mutate_pending, memory_order_acquire)) {
	    if (__builtin_expect(mtl_shared_stats_enabled(), 0))
		atomic_fetch_add_explicit(&mtl_shared_reader_wait_condwait, 1, memory_order_relaxed);
	    pthread_cond_wait(&mtl_shared_env_cv, &mtl_shared_env_mu);
	}
	pthread_mutex_unlock(&mtl_shared_env_mu);
    }
}

attribute_hidden void R_mtl_shared_env_reader_exit(void)
{
    if (!R_MTL_THREADING_ACTIVE)
	return;
    mtl_worker_t *w = mtl_current_worker;
    if (w == NULL)
	return;
    if (mtl_shared_env_reader_depth <= 0)
	return;
    if (--mtl_shared_env_reader_depth > 0)
	return;
    if (__builtin_expect(mtl_shared_stats_enabled(), 0))
	atomic_fetch_add_explicit(&mtl_shared_reader_exit_calls, 1, memory_order_relaxed);

    atomic_store_explicit(&w->in_shared_lookup, 0, memory_order_release);
    if (atomic_load_explicit(&mtl_shared_env_mutate_pending, memory_order_acquire)) {
	pthread_mutex_lock(&mtl_shared_env_mu);
	pthread_cond_broadcast(&mtl_shared_env_cv);
	pthread_mutex_unlock(&mtl_shared_env_mu);
    }
}

attribute_hidden void R_mtl_shared_env_writer_begin(void)
{
    if (!R_MTL_THREADING_ACTIVE)
	return;
    if (!mtl_is_main_thread())
	return;
    if (mtl_shared_env_writer_depth++ > 0)
	return;
    if (__builtin_expect(mtl_shared_stats_enabled(), 0))
	atomic_fetch_add_explicit(&mtl_shared_writer_begin_calls, 1, memory_order_relaxed);

    atomic_store_explicit(&mtl_shared_env_mutate_pending, 1, memory_order_release);
    pthread_mutex_lock(&mtl_shared_env_mu);
    while (mtl_any_worker_in_shared_lookup()) {
	if (__builtin_expect(mtl_shared_stats_enabled(), 0)) {
	    atomic_fetch_add_explicit(&mtl_shared_writer_wait_loops, 1, memory_order_relaxed);
	    atomic_fetch_add_explicit(&mtl_shared_writer_wait_condwait, 1, memory_order_relaxed);
	}
	pthread_cond_wait(&mtl_shared_env_cv, &mtl_shared_env_mu);
    }
    pthread_mutex_unlock(&mtl_shared_env_mu);
}

attribute_hidden void R_mtl_shared_env_writer_end(void)
{
    if (!R_MTL_THREADING_ACTIVE)
	return;
    if (!mtl_is_main_thread())
	return;
    if (mtl_shared_env_writer_depth <= 0)
	return;
    if (--mtl_shared_env_writer_depth > 0)
	return;
    if (__builtin_expect(mtl_shared_stats_enabled(), 0))
	atomic_fetch_add_explicit(&mtl_shared_writer_end_calls, 1, memory_order_relaxed);

    atomic_store_explicit(&mtl_shared_env_mutate_pending, 0, memory_order_release);
    pthread_mutex_lock(&mtl_shared_env_mu);
    pthread_cond_broadcast(&mtl_shared_env_cv);
    pthread_mutex_unlock(&mtl_shared_env_mu);
}

attribute_hidden void R_mtl_shared_env_writer_unlock_all(void)
{
    if (!mtl_is_main_thread())
	return;
    if (mtl_shared_env_writer_depth <= 0)
	return;
    if (__builtin_expect(mtl_shared_stats_enabled(), 0))
	atomic_fetch_add_explicit(&mtl_shared_writer_unlock_all_calls, 1, memory_order_relaxed);
    mtl_shared_env_writer_depth = 0;
    atomic_store_explicit(&mtl_shared_env_mutate_pending, 0, memory_order_release);
    pthread_mutex_lock(&mtl_shared_env_mu);
    pthread_cond_broadcast(&mtl_shared_env_cv);
    pthread_mutex_unlock(&mtl_shared_env_mu);
}

static void mtl_notify_close_locked(void)
{
    if (mtl_pool.notify_fd_read >= 0) {
	close(mtl_pool.notify_fd_read);
	mtl_pool.notify_fd_read = -1;
    }
    if (mtl_pool.notify_fd_write >= 0) {
	close(mtl_pool.notify_fd_write);
	mtl_pool.notify_fd_write = -1;
    }
}

static int mtl_set_fd_nonblocking_cloexec(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
	return -1;
    if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
	return -1;

    int fdfl = fcntl(fd, F_GETFD, 0);
    if (fdfl < 0)
	return -1;
    if (fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC) < 0)
	return -1;
    return 0;
}

static void mtl_notify_init_locked(void)
{
    if (mtl_pool.notify_fd_read >= 0 && mtl_pool.notify_fd_write >= 0)
	return;

    int pfd[2] = { -1, -1 };
    if (pipe(pfd) != 0) {
	atomic_fetch_add_explicit(&mtl_notify_pipe_init_fail, 1, memory_order_relaxed);
	return;
    }

    if (mtl_set_fd_nonblocking_cloexec(pfd[0]) != 0 ||
	mtl_set_fd_nonblocking_cloexec(pfd[1]) != 0) {
	close(pfd[0]);
	close(pfd[1]);
	atomic_fetch_add_explicit(&mtl_notify_pipe_init_fail, 1, memory_order_relaxed);
	return;
    }

    mtl_pool.notify_fd_read = pfd[0];
    mtl_pool.notify_fd_write = pfd[1];
    atomic_fetch_add_explicit(&mtl_notify_pipe_init_ok, 1, memory_order_relaxed);
}

static void mtl_notify_signal_locked(void)
{
    atomic_fetch_add_explicit(&mtl_notify_signal_calls, 1, memory_order_relaxed);
    if (mtl_pool.notify_fd_write < 0)
	return;

    unsigned char b = 1;
    ssize_t n = write(mtl_pool.notify_fd_write, &b, 1);
    if (n == 1) {
	atomic_fetch_add_explicit(&mtl_notify_signal_write_ok, 1, memory_order_relaxed);
	return;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
	atomic_fetch_add_explicit(&mtl_notify_signal_write_eagain, 1, memory_order_relaxed);
	return;
    }
    atomic_fetch_add_explicit(&mtl_notify_signal_write_err, 1, memory_order_relaxed);
}

static int mtl_notify_drain_fd(int fd)
{
    atomic_fetch_add_explicit(&mtl_notify_drain_calls, 1, memory_order_relaxed);
    if (fd < 0)
	return 0;

    int total = 0;
    unsigned char buf[512];
    for (;;) {
	ssize_t n = read(fd, buf, sizeof(buf));
	if (n > 0) {
	    if (n > INT_MAX - total)
		return INT_MAX;
	    total += (int) n;
	    continue;
	}
	if (n == 0)
	    break;
	if (errno == EINTR)
	    continue;
	if (errno == EAGAIN || errno == EWOULDBLOCK) {
	    atomic_fetch_add_explicit(&mtl_notify_drain_eagain, 1, memory_order_relaxed);
	    break;
	}
	atomic_fetch_add_explicit(&mtl_notify_drain_err, 1, memory_order_relaxed);
	break;
    }
    if (total > 0)
	atomic_fetch_add_explicit(&mtl_notify_drain_bytes, (unsigned long) total, memory_order_relaxed);
    return total;
}

		static void mtl_pool_init_if_needed(void)
		{
		    if (mtl_pool.inited)
			return;

		    memset(&mtl_pool, 0, sizeof(mtl_pool));
		    mtl_pool.notify_fd_read = -1;
		    mtl_pool.notify_fd_write = -1;
		    pthread_mutex_init(&mtl_pool.mu, NULL);
		    pthread_cond_init(&mtl_pool.cv, NULL);
		    mtl_notify_init_locked();
		    mtl_dotOptionsSym = install(".Options");
		    mtl_futurePtrSym = install("ptr");
		    mtl_useFancyQuotesSym = install("useFancyQuotes");
		    mtl_pool.inited = 1;
		}

	/* Worker -> main request plumbing.
	 *
	 * This is intentionally minimal: workers enqueue a request and wait.
	 * The main thread services requests while waiting for workers to join.
	 *
	 * This avoids a global interpreter lock around .Call/.External while still
	 * allowing selected operations that must touch global process state to run
	 * on the main thread (e.g. symbol/CHARSXP interning).
	 */

	static void mtl_rpc_exec(void *vp)
	{
	    mtl_main_req_t *r = (mtl_main_req_t *) vp;
	    r->result = r->fun(r->data);
	}

static void mtl_rpc_complete(mtl_main_req_t *r, int ok, const char *msg)
{
    if (ok) {
	/* Keep result rooted until requester thread has consumed it. */
	R_PreserveObject(r->result);
	r->result_preserved = 1;
    }
    pthread_mutex_lock(&r->mu);
    r->ok = ok;
	    if (!ok) {
		const char *m = (msg && msg[0]) ? msg : "error";
		snprintf(r->errmsg, sizeof(r->errmsg), "%s", m);
	    }
	    r->done = 1;
	    pthread_cond_signal(&r->cv);
	    pthread_mutex_unlock(&r->mu);
	}

	static void mtl_rpc_service_locked(void)
	{
	    for (;;) {
		mtl_main_req_t *list = mtl_pool.rpc_head;
		if (list == NULL)
		    return;
		int aborted = mtl_pool.rpc_aborted;
		mtl_pool.rpc_head = NULL;
		mtl_pool.rpc_tail = NULL;

		/* Don't hold the pool mutex while executing R code. */
		pthread_mutex_unlock(&mtl_pool.mu);

		for (mtl_main_req_t *r = list; r != NULL;) {
		    mtl_main_req_t *next = r->next;
		    r->next = NULL;

		    if (aborted) {
			mtl_rpc_complete(r, 0, "mtlapply aborted");
			r = next;
			continue;
		    }

		    Rboolean ok = R_ToplevelExec(mtl_rpc_exec, r);
		    if (ok)
			mtl_rpc_complete(r, 1, NULL);
		    else {
			const char *msg = R_curErrorBuf();
			mtl_rpc_complete(r, 0, msg ? msg : "error");
		    }
		    r = next;
		}

		pthread_mutex_lock(&mtl_pool.mu);
	    }
	}

static void mtl_rpc_abort_all(void)
{
    pthread_mutex_lock(&mtl_pool.mu);
    mtl_pool.rpc_aborted = 1;
	    mtl_main_req_t *list = mtl_pool.rpc_head;
	    mtl_pool.rpc_head = NULL;
	    mtl_pool.rpc_tail = NULL;
	    pthread_mutex_unlock(&mtl_pool.mu);

	    for (mtl_main_req_t *r = list; r != NULL;) {
		mtl_main_req_t *next = r->next;
		r->next = NULL;
	mtl_rpc_complete(r, 0, "mtlapply aborted");
	r = next;
    }
}

static void mtl_rpc_abort_all_locked(void)
{
    mtl_pool.rpc_aborted = 1;
    mtl_main_req_t *list = mtl_pool.rpc_head;
    mtl_pool.rpc_head = NULL;
    mtl_pool.rpc_tail = NULL;
    for (mtl_main_req_t *r = list; r != NULL;) {
	mtl_main_req_t *next = r->next;
	r->next = NULL;
	mtl_rpc_complete(r, 0, "mtlapply aborted");
	r = next;
    }
}

static void mtl_job_set_error(mtl_job_t *job, const char *msg)
{
    if (atomic_exchange_explicit(&job->error, 1, memory_order_relaxed) == 0) {
	pthread_mutex_lock(&job->err_mutex);
	if (msg == NULL || msg[0] == '\0')
	    msg = "error";
	snprintf(job->errmsg, sizeof(job->errmsg), "%s", msg);
	pthread_mutex_unlock(&job->err_mutex);
    }
    atomic_store_explicit(&job->cancel_requested, 1, memory_order_relaxed);
}

static void mtl_job_release(mtl_job_t *job)
{
    if (job == NULL)
	return;
    if (atomic_fetch_sub_explicit(&job->refcount, 1, memory_order_acq_rel) == 1) {
	pthread_mutex_destroy(&job->err_mutex);
	free(job->results);
	free(job->range_next);
	free(job->range_end);
	free(job);
    }
}

static void mtl_job_init_ranges(mtl_job_t *job)
{
    if (job == NULL || job->bg_threads <= 0)
	return;

    job->range_next = (atomic_long *) calloc((size_t) job->bg_threads, sizeof(atomic_long));
    job->range_end = (long *) calloc((size_t) job->bg_threads, sizeof(long));
    if (job->range_next == NULL || job->range_end == NULL)
	error(_("cannot allocate memory"));

    R_xlen_t base = job->n / job->bg_threads;
    R_xlen_t rem = job->n % job->bg_threads;
    R_xlen_t start = 0;
    for (int t = 0; t < job->bg_threads; t++) {
	R_xlen_t len = base + ((R_xlen_t) t < rem ? 1 : 0);
	R_xlen_t end = start + len;
	atomic_init(&job->range_next[t], (long) start);
	job->range_end[t] = (long) end;
	start = end;
    }
}

static R_INLINE int mtl_job_take_from_range(mtl_job_t *job, int rid, R_xlen_t *out)
{
    long idx = atomic_fetch_add_explicit(&job->range_next[rid], 1, memory_order_relaxed);
    if (idx < job->range_end[rid]) {
	*out = (R_xlen_t) idx;
	return 1;
    }
    return 0;
}

static int mtl_job_claim_index(mtl_job_t *job, int wid, R_xlen_t *out)
{
    if (job == NULL || out == NULL || wid < 0 || wid >= job->bg_threads)
	return 0;

    if (mtl_job_take_from_range(job, wid, out))
	return 1;

    for (int off = 1; off < job->bg_threads; off++) {
	int victim = wid + off;
	if (victim >= job->bg_threads)
	    victim -= job->bg_threads;
	if (mtl_job_take_from_range(job, victim, out))
	    return 1;
    }
    return 0;
}

static void mtl_set_threading_active_locked(void);

static R_INLINE int mtl_future_is_terminal(mtl_future_t *f)
{
    return (f->status == MTL_FUTURE_FULFILLED ||
	    f->status == MTL_FUTURE_REJECTED ||
	    f->status == MTL_FUTURE_CANCELLED);
}

static void mtl_future_queue_push_locked(mtl_future_t *f);
static void mtl_future_complete_locked(mtl_future_t *f, mtl_future_status_t status,
				       SEXP value, const char *errmsg);

static void mtl_future_activate_dependents_locked(mtl_future_t *parent)
{
    mtl_future_t *child = parent->deps_head;
    parent->deps_head = NULL;
    while (child != NULL) {
	mtl_future_t *next = child->next_dep;
	child->next_dep = NULL;
	if (child->status != MTL_FUTURE_PENDING) {
	    child = next;
	    continue;
	}
	if (child->cancel_requested) {
	    mtl_future_complete_locked(child, MTL_FUTURE_CANCELLED, R_NilValue, NULL);
	} else if (parent->status == MTL_FUTURE_FULFILLED) {
	    mtl_future_queue_push_locked(child);
	} else if (parent->status == MTL_FUTURE_CANCELLED) {
	    mtl_future_complete_locked(child, MTL_FUTURE_CANCELLED, R_NilValue, NULL);
	} else {
	    mtl_future_complete_locked(child, MTL_FUTURE_REJECTED, R_NilValue,
				      parent->errmsg[0] ? parent->errmsg : "background error");
	}
	child = next;
    }
}

static void mtl_future_queue_push_locked(mtl_future_t *f)
{
    f->next_q = NULL;
    if (mtl_pool.future_q_tail != NULL)
	mtl_pool.future_q_tail->next_q = f;
    else
	mtl_pool.future_q_head = f;
    mtl_pool.future_q_tail = f;
    f->enqueued = 1;
}

static void mtl_future_complete_locked(mtl_future_t *f, mtl_future_status_t status,
				       SEXP value, const char *errmsg)
{
    if (f == NULL || mtl_future_is_terminal(f))
	return;

    f->status = status;
    if (status == MTL_FUTURE_FULFILLED) {
	f->value = value;
    } else if (status == MTL_FUTURE_REJECTED) {
	snprintf(f->errmsg, sizeof(f->errmsg), "%s",
		 (errmsg && errmsg[0]) ? errmsg : "background error");
    }

    if (f->parent != NULL && f->parent->dep_count > 0)
	f->parent->dep_count--;

    mtl_future_activate_dependents_locked(f);
    mtl_notify_signal_locked();
    mtl_set_threading_active_locked();
    pthread_cond_broadcast(&mtl_pool.cv);
}

static mtl_future_t *mtl_future_queue_pop_locked(void)
{
    mtl_future_t *f = mtl_pool.future_q_head;
    if (f == NULL)
	return NULL;
    mtl_pool.future_q_head = f->next_q;
    if (mtl_pool.future_q_head == NULL)
	mtl_pool.future_q_tail = NULL;
    f->next_q = NULL;
    f->enqueued = 0;
    return f;
}

static void mtl_future_queue_remove_locked(mtl_future_t *target)
{
    mtl_future_t *prev = NULL, *cur = mtl_pool.future_q_head;
    while (cur != NULL) {
	if (cur == target) {
	    if (prev != NULL)
		prev->next_q = cur->next_q;
	    else
		mtl_pool.future_q_head = cur->next_q;
	    if (mtl_pool.future_q_tail == cur)
		mtl_pool.future_q_tail = prev;
	    cur->next_q = NULL;
	    cur->enqueued = 0;
	    return;
	}
	prev = cur;
	cur = cur->next_q;
    }
}

static void mtl_future_release_storage(mtl_future_t *f)
{
    if (f == NULL)
	return;
    if (f->value != R_NilValue)
	R_ReleaseObject(f->value);
    if (f->cont_fun != R_NilValue)
	R_ReleaseObject(f->cont_fun);
    if (f->cont_args != R_NilValue)
	R_ReleaseObject(f->cont_args);
    if (f->expr != R_NilValue)
	R_ReleaseObject(f->expr);
    if (f->env != R_NilValue)
	R_ReleaseObject(f->env);
    free(f);
}

static void mtl_options_set_logical(SEXP opts, SEXP sym, int value)
{
    if (TYPEOF(opts) != LISTSXP || TYPEOF(sym) != SYMSXP)
	return;
    for (SEXP node = opts; node != R_NilValue; node = CDR(node)) {
	if (TAG(node) == sym) {
	    SETCAR(node, ScalarLogical(value ? TRUE : FALSE));
	    return;
	}
    }
}

static void mtl_future_sweep_locked(void)
{
    mtl_future_t *prev = NULL, *cur = mtl_pool.future_all;
    while (cur != NULL) {
	mtl_future_t *next = cur->next_all;
	if (cur->detached && mtl_future_is_terminal(cur) &&
	    !cur->enqueued && cur->dep_count == 0 && cur->deps_head == NULL) {
	    if (prev != NULL)
		prev->next_all = next;
	    else
		mtl_pool.future_all = next;
	    mtl_future_release_storage(cur);
	} else {
	    prev = cur;
	}
	cur = next;
    }
}

static R_INLINE int mtl_pool_has_active_tasks_locked(void)
{
    /* "Active tasks" means work that can execute on the pool:
       - an in-flight mtlapply() job, or
       - queued/running background futures. */
    return (mtl_pool.job_depth > 0 ||
	    mtl_pool.future_q_head != NULL ||
	    mtl_pool.future_running > 0);
}

static void mtl_set_threading_active_locked(void)
{
    if (!mtl_pool_has_active_tasks_locked())
	R_mtl_set_threading_active(0);
    else
	R_mtl_set_threading_active(1);
}

static mtl_future_t *mtl_future_from_sexp(SEXP fut)
{
    if (TYPEOF(fut) != VECSXP)
	error(_("'%s' must be an mt_future object"), "future");
    if (mtl_futurePtrSym == NULL)
	mtl_futurePtrSym = install("ptr");
    SEXP ptr = getAttrib(fut, mtl_futurePtrSym);
    if (TYPEOF(ptr) != EXTPTRSXP || R_ExternalPtrAddr(ptr) == NULL)
	error(_("invalid mt_future handle"));
    return (mtl_future_t *) R_ExternalPtrAddr(ptr);
}

static void mtl_future_finalizer(SEXP ext)
{
    mtl_future_t *f = (mtl_future_t *) R_ExternalPtrAddr(ext);
    if (f == NULL)
	return;
    if (!mtl_pool.inited || mtl_pool.shutdown) {
	R_ClearExternalPtr(ext);
	return;
    }
    pthread_mutex_lock(&mtl_pool.mu);
    f->detached = 1;
    if (f->status == MTL_FUTURE_PENDING) {
	if (f->dep_count == 0) {
	    if (f->enqueued)
		mtl_future_queue_remove_locked(f);
	    f->cancel_requested = 1;
	    mtl_future_complete_locked(f, MTL_FUTURE_CANCELLED, R_NilValue, NULL);
	}
    } else if (f->status == MTL_FUTURE_RUNNING) {
	if (f->dep_count == 0)
	    f->cancel_requested = 1;
    }
    mtl_set_threading_active_locked();
    pthread_cond_broadcast(&mtl_pool.cv);
    if (mtl_is_main_thread())
	mtl_future_sweep_locked();
    pthread_mutex_unlock(&mtl_pool.mu);
    R_ClearExternalPtr(ext);
}

static int mtl_timeout_to_deadline(double timeout, struct timespec *out)
{
    if (!R_FINITE(timeout) || timeout < 0)
	return 0;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    long sec = (long) timeout;
    long nsec = (long) ((timeout - (double) sec) * 1e9);
    out->tv_sec = now.tv_sec + sec;
    out->tv_nsec = now.tv_nsec + nsec;
    if (out->tv_nsec >= 1000000000L) {
	out->tv_sec += 1;
	out->tv_nsec -= 1000000000L;
    }
    return 1;
}

			attribute_hidden SEXP R_mtl_invoke_on_main(SEXP (*fun)(void *), void *data)
			{
			    return R_mtl_invoke_on_main_reason(fun, data, R_MTL_RPC_OTHER);
			}

			attribute_hidden SEXP R_mtl_invoke_on_main_reason(SEXP (*fun)(void *), void *data,
									  int reason)
		{
		    if (fun == NULL)
			error("R_mtl_invoke_on_main: NULL fun");

		    reason = mtl_rpc_sanitize_reason(reason);

		    if (R_Interpreter == NULL || !R_Interpreter->isMTLWorker)
			return fun(data);

	    if (!R_MTL_THREADING_ACTIVE)
		error("R_mtl_invoke_on_main: called outside mtlapply()");

		    mtl_main_req_t *r = (mtl_main_req_t *) calloc(1, sizeof(mtl_main_req_t));
		    if (r == NULL)
			error("cannot allocate memory");
		    r->fun = fun;
		    r->data = data;
	    r->reason = reason;
	    r->result = R_NilValue;
	    r->result_preserved = 0;
	    r->ok = 0;
	    r->done = 0;
	    r->next = NULL;
	    pthread_mutex_init(&r->mu, NULL);
	    pthread_cond_init(&r->cv, NULL);

		    pthread_mutex_lock(&mtl_pool.mu);
		    if (mtl_pool.shutdown ||
			((mtl_pool.job_top == NULL) &&
			 (mtl_pool.future_q_head == NULL) &&
			 (mtl_pool.future_running == 0)) ||
			mtl_pool.rpc_aborted) {
		pthread_mutex_unlock(&mtl_pool.mu);
		pthread_mutex_destroy(&r->mu);
		pthread_cond_destroy(&r->cv);
		free(r);
		error("R_mtl_invoke_on_main: no active threaded job");
	    }
	    if (mtl_pool.rpc_tail)
		mtl_pool.rpc_tail->next = r;
	    else
		mtl_pool.rpc_head = r;
	    mtl_pool.rpc_tail = r;
	    atomic_fetch_add_explicit(&mtl_rpc_calls[reason], 1, memory_order_relaxed);
	    mtl_notify_signal_locked();
	    pthread_cond_broadcast(&mtl_pool.cv);
	    pthread_mutex_unlock(&mtl_pool.mu);

	    pthread_mutex_lock(&r->mu);
	    while (!r->done)
		pthread_cond_wait(&r->cv, &r->mu);
	    pthread_mutex_unlock(&r->mu);

	    SEXP res = r->result;
	    int ok = r->ok;
	    char msg[1024];
	    msg[0] = '\0';
	    if (!ok)
		snprintf(msg, sizeof(msg), "%s", r->errmsg);

	    if (ok && r->result_preserved) {
		PROTECT(res);
		R_ReleaseObject(res);
		UNPROTECT(1);
		r->result_preserved = 0;
	    }

	    pthread_mutex_destroy(&r->mu);
	    pthread_cond_destroy(&r->cv);
	    free(r);

		    if (!ok)
			atomic_fetch_add_explicit(&mtl_rpc_errors[reason], 1, memory_order_relaxed);
		    if (!ok)
			error("%s", msg[0] ? msg : "error");
			    return res;
			}

	typedef struct {
	    R_InterpreterState *interp;
	    SEXP value;
	    SEXP out;
	} mtl_future_copy_t;

	static SEXP mtl_future_copy_main(void *vp)
	{
	    mtl_future_copy_t *d = (mtl_future_copy_t *) vp;
	    SEXP out = duplicate(d->value);
	    R_PreserveObject(out);
	    d->out = out;
	    if (d->interp != NULL) {
		R_mtl_adopt_worker_heap(d->interp);
		d->interp->preciousList = R_NilValue;
	    }
	    return out;
	}

static void mtl_future_copy_main_exec(void *vp)
{
    mtl_future_copy_t *d = (mtl_future_copy_t *) vp;
    d->out = R_mtl_invoke_on_main_reason(mtl_future_copy_main, d, R_MTL_RPC_OTHER);
}

static SEXP mtl_future_adopt_main(void *vp)
{
    mtl_future_copy_t *d = (mtl_future_copy_t *) vp;
    if (d->interp != NULL) {
	R_mtl_adopt_worker_heap(d->interp);
	d->interp->preciousList = R_NilValue;
    }
    d->out = R_NilValue;
    return R_NilValue;
}

static void mtl_future_adopt_main_exec(void *vp)
{
    mtl_future_copy_t *d = (mtl_future_copy_t *) vp;
    R_mtl_invoke_on_main_reason(mtl_future_adopt_main, d, R_MTL_RPC_OTHER);
}

static void *mtl_pool_worker_main(void *vp)
{
    mtl_worker_t *w = (mtl_worker_t *) vp;
    mtl_pool_t *p = w->pool;
    mtl_current_worker = w;
    mtl_shared_env_reader_depth = 0;

    R_RegisterInterpreterState(&w->interp);

    R_InterpreterState *saved_interp = R_InterpreterTLS;
    R_InterpreterTLS = &w->interp;
    R_InterpreterState *saved_compat = R_mtl_set_compat_interpreter(&w->interp);

#ifdef R_USE_SIGNALS
    /* begincontext() assumes R_GlobalContext is non-NULL. Install a
       per-thread dummy toplevel context as the base of the chain. */
    R_Toplevel.nextcontext = NULL;
    R_Toplevel.callflag = CTXT_TOPLEVEL;
    R_Toplevel.cstacktop = R_PPStackTop;
    R_Toplevel.gcenabled = R_GCEnabled;
    R_Toplevel.promargs = R_NilValue;
    R_Toplevel.callfun = R_NilValue;
    R_Toplevel.call = R_NilValue;
    R_Toplevel.cloenv = R_BaseEnv;
    R_Toplevel.sysparent = R_BaseEnv;
    R_Toplevel.conexit = R_NilValue;
    R_Toplevel.vmax = vmaxget();
    R_Toplevel.nodestack = w->interp.bcNodeStackTop;
    R_Toplevel.bcprottop = w->interp.bcProtTop;
    R_Toplevel.cend = NULL;
    R_Toplevel.cenddata = NULL;
    R_Toplevel.intsusp = FALSE;
    R_Toplevel.handlerstack = w->interp.handlerStack;
    R_Toplevel.restartstack = w->interp.restartStack;
    R_Toplevel.srcref = R_NilValue;
    R_Toplevel.prstack = NULL;
    R_Toplevel.returnValue = SEXP_TO_STACKVAL(NULL);
    R_Toplevel.evaldepth = 0;
    R_Toplevel.browserfinish = 0;
    w->interp.globalContext = w->interp.toplevelContext = w->interp.sessionContext = &R_Toplevel;
    w->interp.exitContext = NULL;
#endif

    /* Disable stack checks in this thread; main's limits are unrelated. */
    w->interp.cStackStart = (uintptr_t) -1;
    w->interp.cStackLimit = (uintptr_t) -1;
    w->interp.oldCStackLimit = (uintptr_t) 0;

	    for (;;) {
		mtl_job_t *job = NULL;
		mtl_future_t *future = NULL;
		int run_job = 0;

		pthread_mutex_lock(&p->mu);
		for (;;) {
		    int job_ready = (p->job_top != NULL &&
				     w->id < p->job_top->bg_threads &&
				     w->seen_gen != p->gen);
		    if (p->shutdown || job_ready || p->future_q_head != NULL) {
			run_job = job_ready;
			break;
		    }
		    pthread_cond_wait(&p->cv, &p->mu);
		}
		if (p->shutdown) {
		    pthread_mutex_unlock(&p->mu);
		    break;
		}

		if (run_job) {
		    job = p->job_top;
		    w->seen_gen = p->gen;
		    atomic_fetch_add_explicit(&job->refcount, 1, memory_order_relaxed);
		} else {
		    future = mtl_future_queue_pop_locked();
		    if (future != NULL && future->status == MTL_FUTURE_PENDING) {
			future->status = MTL_FUTURE_RUNNING;
			p->future_running++;
		    } else
			future = NULL;
		}
		pthread_mutex_unlock(&p->mu);

		if (job != NULL) {
			/* Snapshot global options into the worker heap for this job.
			   The worker sees/sets options against this snapshot (copy-on-write),
			   so packages using withr::with_options() work without mutating global
			   process state. */
				{
				    R_mtl_global_lock();
				    SEXP glob = SYMVALUE(mtl_dotOptionsSym);
				    w->interp.mtlOptionsBase = R_mtl_shallow_duplicate_pairlist(glob);
				    /* Keep worker-side condition formatting ASCII-only.
				       This avoids multibyte quote corruption when multiple
				       workers signal errors concurrently. */
				    mtl_options_set_logical(w->interp.mtlOptionsBase,
							    mtl_useFancyQuotesSym, 0);
				    w->interp.mtlOptions = w->interp.mtlOptionsBase;
				    R_mtl_global_unlock();
				}

			/* Build per-job call objects in the worker heap to avoid mutating
			   main-heap call structures from worker threads. */
			SEXP tail = PROTECT(duplicate(job->tail0));
			SEXP argcell = PROTECT(CONS(R_NilValue, tail));
			SEXP fcall = PROTECT(LCONS(job->FUN, argcell));
		    MARK_NOT_MUTABLE(fcall);

		    for (;;) {
			if (atomic_load_explicit(&job->cancel_requested, memory_order_relaxed))
			    break;
			R_xlen_t i = 0;
			if (!mtl_job_claim_index(job, w->id, &i))
			    break;
			if (atomic_load_explicit(&job->cancel_requested, memory_order_relaxed))
			    break;

			/* Reset per-interpreter stacks/slots for this evaluation. */
			mtl_interp_reset_for_eval(&w->interp, job, job->eval_env);

			/* job->XX is normalized to VECSXP by do_mtlapply(). */
			SETCAR(argcell, VECTOR_ELT(job->XX, i));
			if (atomic_load_explicit(&job->cancel_requested, memory_order_relaxed))
			    break;
			atomic_fetch_add_explicit(&job->active_eval_workers, 1, memory_order_relaxed);
			int err = 0;
#ifdef R_USE_SIGNALS
			/* R_tryEvalSilent() errors unwind to this thread's toplevel
			   context. Refresh the baseline snapshots before each eval so
			   unwind restores the current protection/vmax/node-stack state
			   (including job-scoped PROTECTs like fcall/argcell/tail). */
			R_Toplevel.cstacktop = R_PPStackTop;
			R_Toplevel.gcenabled = R_GCEnabled;
			R_Toplevel.bcintactive = R_BCIntActive;
			R_Toplevel.bcpc = R_BCpc;
			R_Toplevel.bcbody = R_BCbody;
			R_Toplevel.bcframe = R_BCFrame;
			R_Toplevel.vmax = vmaxget();
			R_Toplevel.intsusp = R_interrupts_suspended;
			R_Toplevel.nodestack = R_BCNodeStackTop;
			R_Toplevel.bcprottop = R_BCProtTop;
			R_Toplevel.handlerstack = R_HandlerStack;
			R_Toplevel.restartstack = R_RestartStack;
			R_Toplevel.prstack = R_PendingPromises;
			R_Toplevel.evaldepth = R_EvalDepth;
			R_Toplevel.srcref = R_Srcref;
#endif
			mtl_parallel_begin();
			int lock_eval = 0;
			const char *lock_eval_env = getenv("R_MTL_LOCK_EVAL");
			if (lock_eval_env != NULL && *lock_eval_env != '\0' &&
			    strcmp(lock_eval_env, "0") != 0) {
			    R_mtl_global_lock();
			    lock_eval = 1;
			}
			if (mtl_trace_enabled()) {
			    fprintf(stderr, "[mtl] worker=%p eval i=%lld begin\n",
				    (void *)w, (long long)i);
			    fflush(stderr);
			}
			SEXP val = R_tryEvalSilent(fcall, job->eval_env, &err);
			if (mtl_trace_enabled()) {
			    fprintf(stderr, "[mtl] worker=%p eval i=%lld end err=%d\n",
				    (void *)w, (long long)i, err);
			    fflush(stderr);
			}
			if (lock_eval)
			    R_mtl_global_unlock();
			mtl_parallel_end();
			if (err || val == NULL) {
			    const char *msg = R_curErrorBuf();
			    mtl_job_set_error(job, msg);
			    atomic_fetch_sub_explicit(&job->active_eval_workers, 1, memory_order_relaxed);
			    break;
			}

			PROTECT(val);
			if (MAYBE_REFERENCED(val)) {
			    SEXP dup = lazy_duplicate(val);
			    UNPROTECT(1);
			    val = dup;
			    PROTECT(val);
			}
			job->results[i] = val;
			/* Keep worker results reachable via the worker interpreter's
			   precious list until main-thread adoption/collection. This avoids
			   per-result R_PreserveObject/R_ReleaseObject traffic. */
			SEXP keep = CONS(val, w->interp.preciousList);
			w->interp.preciousList = keep;
			UNPROTECT(1);
			atomic_fetch_sub_explicit(&job->active_eval_workers, 1, memory_order_relaxed);
			}

			if (mtl_trace_enabled()) {
			    fprintf(stderr,
				    "[mtl] worker=%p post-loop err=%d ppTop=%d interp.ppTop=%d handler=%p restart=%p\n",
				    (void *)w,
				    (int) atomic_load_explicit(&job->error, memory_order_relaxed),
				    (int) R_PPStackTop,
				    (int) w->interp.ppStackTop,
				    (void *) R_HandlerStack,
				    (void *) R_RestartStack);
			    fflush(stderr);
			}

			UNPROTECT(3); /* tail, argcell, fcall */

			/* Drop worker-local options before GC/adoption: keep them job-local. */
			w->interp.mtlOptions = R_NilValue;
			w->interp.mtlOptionsBase = R_NilValue;

			/* Worker GC is disabled; adoption/reset happens on main thread. */
			pthread_mutex_lock(&p->mu);
			atomic_fetch_add_explicit(&job->workers_done, 1, memory_order_relaxed);
			pthread_cond_broadcast(&p->cv);
			pthread_mutex_unlock(&p->mu);
			mtl_job_release(job);
			continue;
		    }

		if (future != NULL) {
		    mtl_job_t fake_job;
		    memset(&fake_job, 0, sizeof(fake_job));
		    fake_job.main_showErrorMessages = future->main_showErrorMessages;
		    mtl_interp_reset_for_eval(&w->interp, &fake_job, future->env);

		    int err = 0;
		    const char *errmsg = NULL;
		    SEXP out = R_NilValue;

#ifdef R_USE_SIGNALS
		    R_Toplevel.cstacktop = R_PPStackTop;
		    R_Toplevel.gcenabled = R_GCEnabled;
		    R_Toplevel.bcintactive = R_BCIntActive;
		    R_Toplevel.bcpc = R_BCpc;
		    R_Toplevel.bcbody = R_BCbody;
		    R_Toplevel.bcframe = R_BCFrame;
		    R_Toplevel.vmax = vmaxget();
		    R_Toplevel.intsusp = R_interrupts_suspended;
		    R_Toplevel.nodestack = R_BCNodeStackTop;
		    R_Toplevel.bcprottop = R_BCProtTop;
		    R_Toplevel.handlerstack = R_HandlerStack;
		    R_Toplevel.restartstack = R_RestartStack;
		    R_Toplevel.prstack = R_PendingPromises;
		    R_Toplevel.evaldepth = R_EvalDepth;
		    R_Toplevel.srcref = R_Srcref;
#endif
		    mtl_parallel_begin();
		    int lock_eval = 0;
		    const char *lock_eval_env = getenv("R_MTL_LOCK_EVAL");
		    if (lock_eval_env != NULL && *lock_eval_env != '\0' &&
			strcmp(lock_eval_env, "0") != 0) {
			R_mtl_global_lock();
			lock_eval = 1;
		    }
		    SEXP val = R_NilValue;
		    if (future->parent != NULL) {
			SEXP tail = PROTECT(VectorToPairList(future->cont_args));
			SEXP argcell = PROTECT(CONS(future->parent->value, tail));
			SEXP fcall = PROTECT(LCONS(future->cont_fun, argcell));
			MARK_NOT_MUTABLE(fcall);
			val = R_tryEvalSilent(fcall, future->env, &err);
			UNPROTECT(3);
		    } else {
			val = R_tryEvalSilent(future->expr, future->env, &err);
		    }
		    if (lock_eval)
			R_mtl_global_unlock();
		    mtl_parallel_end();
		    int cancelled = 0;
		    pthread_mutex_lock(&p->mu);
		    cancelled = future->cancel_requested;
		    pthread_mutex_unlock(&p->mu);
		    if (!err && val != NULL && !cancelled) {
			mtl_future_copy_t cp;
			cp.interp = &w->interp;
			cp.value = val;
			cp.out = R_NilValue;
			Rboolean copy_ok = R_ToplevelExec(mtl_future_copy_main_exec, &cp);
			if (copy_ok && cp.out != R_NilValue)
			    out = cp.out;
			else {
			    err = 1;
			    errmsg = R_curErrorBuf();
			}
		    } else {
			mtl_future_copy_t cp;
			cp.interp = &w->interp;
			cp.value = R_NilValue;
			cp.out = R_NilValue;
			R_ToplevelExec(mtl_future_adopt_main_exec, &cp);
			if (err)
			    errmsg = R_curErrorBuf();
		    }

		    pthread_mutex_lock(&p->mu);
		    if (future->cancel_requested) {
			mtl_future_complete_locked(future, MTL_FUTURE_CANCELLED, R_NilValue, NULL);
		    } else if (err || out == R_NilValue) {
			mtl_future_complete_locked(future, MTL_FUTURE_REJECTED, R_NilValue,
						  (errmsg && errmsg[0]) ? errmsg : "background error");
		    } else {
			mtl_future_complete_locked(future, MTL_FUTURE_FULFILLED, out, NULL);
		    }
		    if (p->future_running > 0)
			p->future_running--;
		    pthread_mutex_unlock(&p->mu);
		}
	    }

    R_InterpreterTLS = saved_interp;
    R_mtl_set_compat_interpreter(saved_compat);
    atomic_store_explicit(&w->in_shared_lookup, 0, memory_order_release);
    mtl_current_worker = NULL;
    mtl_shared_env_reader_depth = 0;

	    /* Worker teardown after error-unwind paths can leave transient
	       allocator metadata inconsistent for explicit frees. Keep shutdown
	       robust by unregistering interpreter state but letting OS reclaim
	       worker-local memory at process exit. */
	    R_UnregisterInterpreterState(&w->interp);

    return NULL;
}

static void mtl_pool_ensure_threads(int nthreads)
{
    if (nthreads <= mtl_pool.nthreads)
	return;

    int old = mtl_pool.nthreads;
    pthread_t *new_threads = (pthread_t *) calloc((size_t) nthreads, sizeof(pthread_t));
    mtl_worker_t **new_workers = (mtl_worker_t **) calloc((size_t) nthreads, sizeof(mtl_worker_t *));
    if (new_threads == NULL || new_workers == NULL)
	error(_("cannot allocate memory"));
    if (mtl_pool.nthreads > 0) {
	memcpy(new_threads, mtl_pool.threads, (size_t) mtl_pool.nthreads * sizeof(pthread_t));
	memcpy(new_workers, mtl_pool.workers, (size_t) mtl_pool.nthreads * sizeof(mtl_worker_t *));
    }
    free(mtl_pool.threads);
    free(mtl_pool.workers);
    mtl_pool.threads = new_threads;
    mtl_pool.workers = new_workers;

    for (int i = old; i < nthreads; i++) {
	mtl_worker_t *w = (mtl_worker_t *) calloc(1, sizeof(mtl_worker_t));
	if (w == NULL)
	    error(_("cannot allocate memory"));
	w->id = i;
	w->pool = &mtl_pool;
	atomic_init(&w->in_shared_lookup, 0);
	mtl_interp_init_from_main(&w->interp);
	mtl_pool.workers[i] = w;
	int rc = pthread_create(&mtl_pool.threads[i], NULL, mtl_pool_worker_main, w);
	if (rc != 0)
	    error("pthread_create failed");
	atomic_fetch_add_explicit(&mtl_pool_threads_created, 1, memory_order_relaxed);
    }
    mtl_pool.nthreads = nthreads;
}

attribute_hidden void R_mtlpool_shutdown(void)
{
    if (!mtl_pool.inited)
	return;

    pthread_mutex_lock(&mtl_pool.mu);
    mtl_pool.shutdown = 1;
    mtl_rpc_abort_all_locked();
    pthread_cond_broadcast(&mtl_pool.cv);
    pthread_mutex_unlock(&mtl_pool.mu);

    for (int i = 0; i < mtl_pool.nthreads; i++)
	pthread_join(mtl_pool.threads[i], NULL);

    pthread_mutex_lock(&mtl_pool.mu);
    mtl_notify_close_locked();
    pthread_mutex_unlock(&mtl_pool.mu);

    for (int i = 0; i < mtl_pool.nthreads; i++)
	free(mtl_pool.workers[i]);
    free(mtl_pool.workers);
    free(mtl_pool.threads);
    pthread_mutex_destroy(&mtl_pool.mu);
    pthread_cond_destroy(&mtl_pool.cv);
    memset(&mtl_pool, 0, sizeof(mtl_pool));
}

	typedef struct {
	    mtl_job_t *job;
	    int n_bg_threads;
	    SEXP ans;
	    int mu_locked;
	} mtlapply_run_data_t;

static void mtl_main_eval_loop(mtl_job_t *job, SEXP env, SEXP ans)
{
    /* Build per-job call objects in the caller thread/interpreter heap. */
	    SEXP tail = PROTECT(duplicate(job->tail0));
	    SEXP argcell = PROTECT(CONS(R_NilValue, tail));
	    SEXP fcall = PROTECT(LCONS(job->FUN, argcell));
	    MARK_NOT_MUTABLE(fcall);

    for (R_xlen_t i = 0; i < job->n; i++) {
	if (atomic_load_explicit(&job->cancel_requested, memory_order_relaxed))
	    break;

	/* job->XX is normalized to VECSXP by do_mtlapply(). */
	SETCAR(argcell, VECTOR_ELT(job->XX, i));

	int err = 0;
	mtl_parallel_begin();
	SEXP val = R_tryEvalSilent(fcall, env, &err);
	mtl_parallel_end();
	if (err || val == NULL) {
	    const char *msg = R_curErrorBuf();
	    mtl_job_set_error(job, msg);
	    break;
	}

	PROTECT(val);
	if (MAYBE_REFERENCED(val)) {
	    SEXP dup = lazy_duplicate(val);
	    UNPROTECT(1);
	    val = dup;
	    PROTECT(val);
	}
	/* Root via the answer list (scanned by GC); no PreserveObject here. */
	SET_VECTOR_ELT(ans, i, val);
	UNPROTECT(1);
    }

	    UNPROTECT(3); /* tail, argcell, fcall */
	}

static SEXP mtl_box_vector_args(SEXP XX)
{
    if (TYPEOF(XX) == VECSXP || TYPEOF(XX) == EXPRSXP)
	return XX;
    return coerceVector(XX, VECSXP);
}

static SEXP mtl_serial_apply_no_pool(SEXP XX, SEXP FUN, SEXP dots, SEXP names, SEXP eval_env)
{
    R_xlen_t n = xlength(XX);
    int nprotect = 0;
    SEXP ans = PROTECT(allocVector(VECSXP, n)); nprotect++;
    if (!isNull(names)) setAttrib(ans, R_NamesSymbol, names);

    SEXP tail = PROTECT(VectorToPairList(dots)); nprotect++;
    SEXP argcell = PROTECT(CONS(R_NilValue, tail)); nprotect++;
    SEXP fcall = PROTECT(LCONS(FUN, argcell)); nprotect++;
    MARK_NOT_MUTABLE(fcall);

    for (R_xlen_t i = 0; i < n; i++) {
	if (TYPEOF(XX) == VECSXP || TYPEOF(XX) == EXPRSXP) {
	    SETCAR(argcell, VECTOR_ELT(XX, i));
	} else {
	    switch (TYPEOF(XX)) {
	    case LGLSXP:
		SETCAR(argcell, ScalarLogical(LOGICAL_ELT(XX, i)));
		break;
	    case INTSXP:
		SETCAR(argcell, ScalarInteger(INTEGER_ELT(XX, i)));
		break;
	    case REALSXP:
		SETCAR(argcell, ScalarReal(REAL_ELT(XX, i)));
		break;
	    case RAWSXP:
		{
		    SEXP s = allocVector(RAWSXP, 1);
		    RAW(s)[0] = RAW(XX)[i];
		    SETCAR(argcell, s);
		}
		break;
	    case STRSXP:
		SETCAR(argcell, ScalarString(STRING_ELT(XX, i)));
		break;
	    case CPLXSXP:
		{
		    SEXP s = allocVector(CPLXSXP, 1);
		    COMPLEX(s)[0] = COMPLEX_ELT(XX, i);
		    SETCAR(argcell, s);
		}
		break;
	    default:
		error(_("mtlapply: unsupported type '%s'"), R_typeToChar(XX));
	    }
	}

	int err = 0;
	SEXP val = R_tryEvalSilent(fcall, eval_env, &err);
	if (err || val == NULL) {
	    const char *msg = R_curErrorBuf();
	    error("%s", (msg && msg[0]) ? msg : "mtlapply error");
	}
	PROTECT(val);
	if (MAYBE_REFERENCED(val)) {
	    SEXP dup = lazy_duplicate(val);
	    UNPROTECT(1);
	    val = dup;
	    PROTECT(val);
	}
	SET_VECTOR_ELT(ans, i, val);
	UNPROTECT(1);
    }

    UNPROTECT(nprotect);
    return ans;
}

	static SEXP mtlapply_run(void *vp)
	{
	    mtlapply_run_data_t *d = (mtlapply_run_data_t *) vp;

	    if (d->n_bg_threads == 0) {
		/* threads=1: stay on the main thread (no pool). */
		mtl_main_eval_loop(d->job, d->job->eval_env, d->ans);
		return R_NilValue;
	    }

	    pthread_mutex_lock(&mtl_pool.mu);
	    d->mu_locked = 1;

	    if (mtl_pool.job_depth == 0) {
		/* New top-level job: clear any pending/aborted RPC state. */
		mtl_pool.rpc_aborted = 0;
		mtl_pool.rpc_head = NULL;
		mtl_pool.rpc_tail = NULL;
	    }

	    d->job->parent_job = mtl_pool.job_top;
	    mtl_pool.job_top = d->job;
	    mtl_pool.job_depth++;
	    mtl_set_threading_active_locked();
	    mtl_pool.gen++;
	    pthread_cond_broadcast(&mtl_pool.cv);

	    pthread_mutex_unlock(&mtl_pool.mu);
	    d->mu_locked = 0;

    pthread_mutex_lock(&mtl_pool.mu);
    d->mu_locked = 1;
	    while (atomic_load_explicit(&d->job->workers_done, memory_order_relaxed) < d->n_bg_threads) {
		/* Service worker->main requests while waiting from the main thread only. */
		if (mtl_is_main_thread())
		    mtl_rpc_service_locked();
		if (atomic_load_explicit(&d->job->workers_done, memory_order_relaxed) < d->n_bg_threads)
		    pthread_cond_wait(&mtl_pool.cv, &mtl_pool.mu);
	    }

	    /* Drain any remaining requests before tearing down the job. */
		    if (mtl_is_main_thread())
			mtl_rpc_service_locked();
		    if (mtl_pool.job_top == d->job) {
			mtl_pool.job_top = d->job->parent_job;
			if (mtl_pool.job_depth > 0)
			    mtl_pool.job_depth--;
			mtl_pool.gen++;
			pthread_cond_broadcast(&mtl_pool.cv);
		    }

	    if (mtl_pool.job_depth == 0)
		mtl_set_threading_active_locked();

	    pthread_mutex_unlock(&mtl_pool.mu);
	    d->mu_locked = 0;

	    return R_NilValue;
	}

static void mtlapply_run_cleanup(void *vp, Rboolean jump)
{
    mtlapply_run_data_t *d = (mtlapply_run_data_t *) vp;

    /* On unwind, force workers to stop touching this job and
       wait for them to acknowledge completion before leaving this frame.
       While waiting, keep servicing worker->main RPCs to avoid deadlock. */
    if (jump && d->job && d->n_bg_threads > 0) {
	atomic_store_explicit(&d->job->error, 1, memory_order_relaxed);
	atomic_store_explicit(&d->job->cancel_requested, 1, memory_order_relaxed);

	if (!d->mu_locked) {
	    pthread_mutex_lock(&mtl_pool.mu);
	    d->mu_locked = 1;
	}

	/* Abort pending worker->main requests while holding the pool mutex
	   to avoid races/deadlocks with concurrent cleanup. */
	mtl_rpc_abort_all_locked();

	/* Keep current top job valid until all workers have reported done. */
	if (mtl_pool.job_top == d->job) {
	    while (atomic_load_explicit(&d->job->workers_done, memory_order_relaxed) < d->n_bg_threads) {
		if (mtl_is_main_thread())
		    mtl_rpc_service_locked();
		if (atomic_load_explicit(&d->job->workers_done, memory_order_relaxed) < d->n_bg_threads)
		    pthread_cond_wait(&mtl_pool.cv, &mtl_pool.mu);
	    }
	    if (mtl_is_main_thread())
		mtl_rpc_service_locked();
	    if (mtl_pool.job_top == d->job) {
		mtl_pool.job_top = d->job->parent_job;
		if (mtl_pool.job_depth > 0)
		    mtl_pool.job_depth--;
		mtl_pool.gen++;
		pthread_cond_broadcast(&mtl_pool.cv);
	    }
	}
    } else if (jump) {
	/* No active workers for this job, but still abort pending RPCs. */
	if (d->mu_locked)
	    mtl_rpc_abort_all_locked();
	else
	    mtl_rpc_abort_all();
    }

    if (d->mu_locked) {
	pthread_mutex_unlock(&mtl_pool.mu);
	d->mu_locked = 0;
    }

    if (mtl_pool.job_depth == 0)
	mtl_set_threading_active_locked();
}
#endif /* HAVE_PTHREAD */

#ifndef HAVE_PTHREAD
attribute_hidden void R_mtl_shared_env_reader_enter(void) {}
attribute_hidden void R_mtl_shared_env_reader_exit(void) {}
attribute_hidden void R_mtl_shared_env_writer_begin(void) {}
attribute_hidden void R_mtl_shared_env_writer_end(void) {}
attribute_hidden void R_mtl_shared_env_writer_unlock_all(void) {}
#endif

/* .Internal(lapply(X, FUN)) */

/* This is a special .Internal, so has unevaluated arguments.  It is
   called from a closure wrapper, so X and FUN will be symbols that
   are bound to promises in rho.

   FUN must be unevaluated for use in e.g. bquote .
*/
attribute_hidden SEXP do_lapply(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    PROTECT_INDEX pidx, cidx;

    checkArity(op, args);
    SEXP X, XX, FUN;
    X = checkArgIsSymbol(CAR(args));
    XX = PROTECT(eval(CAR(args), rho));
    R_xlen_t n = xlength(XX);  // a vector, so will be valid.
    FUN = checkArgIsSymbol(CADR(args));
    bool realIndx = n > INT_MAX;

    SEXP ans = PROTECT(allocVector(VECSXP, n));
    SEXP names = getAttrib(XX, R_NamesSymbol);
    if(!isNull(names)) setAttrib(ans, R_NamesSymbol, names);

    /* Build call: FUN(X[[<ind>]], ...) */
    SEXP isym = install("i");
    SEXP tmp = PROTECT(lang3(R_Bracket2Symbol, X, isym));
    SEXP R_fcall = PROTECT(lang3(FUN, tmp, R_DotsSymbol));
    MARK_NOT_MUTABLE(R_fcall);

    /* Create the loop index variable and value */
    SEXP ind = allocVector(realIndx ? REALSXP : INTSXP, 1);
    PROTECT_WITH_INDEX(ind, &pidx);
    defineVar(isym, ind, rho);
    INCREMENT_NAMED(ind);
    R_varloc_t loc = R_findVarLocInFrame(rho, isym);
    PROTECT_WITH_INDEX(loc.cell, &cidx);


    for(R_xlen_t i = 0; i < n; i++) {
	if (realIndx) REAL(ind)[0] = (double)(i + 1);
	else INTEGER(ind)[0] = (int)(i + 1);
	tmp = R_forceAndCall(R_fcall, 1, rho);
	if (MAYBE_REFERENCED(tmp)) tmp = lazy_duplicate(tmp);
	SET_VECTOR_ELT(ans, i, tmp);
	if (ind != R_GetVarLocValue(loc) || MAYBE_SHARED(ind)) {
	    /* ind has been captured or removed by FUN so fix it up */
	    REPROTECT(ind = duplicate(ind), pidx);
	    defineVar(isym, ind, rho);
	    INCREMENT_NAMED(ind);
	    loc = R_findVarLocInFrame(rho, isym);
	    REPROTECT(loc.cell, cidx);
	}
    }

    UNPROTECT(6);
    return ans;
}

/* .Internal(mtlapply(X, FUN, DOTS, THREADS)) */
attribute_hidden SEXP do_mtlapply(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);

    SEXP XX = CAR(args);
    SEXP FUN = CADR(args);
	    SEXP dots = CADDR(args);
	    int nthreads = asInteger(CADDDR(args));
	    Rboolean is_main = mtl_is_main_thread();
	    Rboolean in_worker = (R_Interpreter != NULL && R_Interpreter->isMTLWorker);

		    if (nthreads == NA_INTEGER || nthreads < 1)
			error(_("invalid '%s' value"), "threads");

	    if (!isVector(XX))
		error(_("'%s' must be a vector"), "X");
	    if (!isFunction(FUN))
		error(_("'%s' must be a function"), "FUN");
	    if (TYPEOF(dots) != VECSXP)
		error(_("'%s' must be a list"), "DOTS");

#ifndef HAVE_PTHREAD
    error("mtlapply() requires pthreads support");
#else
    R_xlen_t n = xlength(XX);
    if (n == NA_INTEGER)
	error(_("invalid length"));

    SEXP names = getAttrib(XX, R_NamesSymbol);
    SEXP XX_work = XX;

    if (!is_main && !in_worker)
	error("mtlapply() may only be called from the main thread");

	    if (n == 0) {
		SEXP ans = allocVector(VECSXP, 0);
		if (!isNull(names)) setAttrib(ans, R_NamesSymbol, names);
		return ans;
	    }

	    /* 'threads' is the number of worker slots for this call. */
	    if (nthreads > n) nthreads = (int) n;
	    int n_bg_threads = nthreads;
	    if (n_bg_threads > 0 && n > (R_xlen_t) LONG_MAX)
		error("mtlapply: long-vector length is not supported in threaded mode");

	    /* gc.torture exercises collector paths that are not currently safe to
	       run concurrently across worker interpreters. Keep semantics by running
	       serially in this mode. */
	    if (n_bg_threads > 1 && R_gc_torture_is_active())
		n_bg_threads = 0;

	    int nprotect = 0;
	    if (TYPEOF(XX) != VECSXP && TYPEOF(XX) != EXPRSXP) {
		XX_work = PROTECT(mtl_box_vector_args(XX)); nprotect++;
	    } else {
		PROTECT(XX_work); nprotect++;
	    }
	    if (in_worker) {
		SEXP out = mtl_serial_apply_no_pool(XX_work, FUN, dots, names, rho);
		UNPROTECT(nprotect);
		return out;
	    }

	    if (n_bg_threads > 0) {
		mtl_pool_init_if_needed();
		mtl_pool_ensure_threads(n_bg_threads);
	    }

		    /* Convert DOTS list to a pairlist once; workers duplicate in their heaps. */
		    PROTECT(FUN); nprotect++;
		    PROTECT(dots); nprotect++;
		    SEXP tail0 = PROTECT(VectorToPairList(dots)); nprotect++;

	    /* Root the answer while workers are running. */
	    SEXP ans = PROTECT(allocVector(VECSXP, n)); nprotect++;
	    if (!isNull(names)) setAttrib(ans, R_NamesSymbol, names);

		    mtl_job_t *job = (mtl_job_t *) calloc(1, sizeof(mtl_job_t));
		    if (job == NULL)
			error(_("cannot allocate memory"));
		    job->XX = XX_work;
		    job->FUN = FUN;
		    job->eval_env = rho;
		    job->tail0 = tail0;
		    job->n = n;
		    job->results = NULL;
		    atomic_init(&job->next, 0);
		    atomic_init(&job->error, 0);
		    atomic_init(&job->cancel_requested, 0);
		    atomic_init(&job->active_eval_workers, 0);
		    atomic_init(&job->workers_done, 0);
		    job->bg_threads = n_bg_threads;
		    job->parent_job = NULL;
		    atomic_init(&job->refcount, 1); /* main owner; workers acquire when job starts */
		    job->errmsg[0] = '\0';
		    /* Worker-thread condition printing is not thread-safe and can
		       corrupt output/state under concurrent errors. Workers always
		       evaluate with printing disabled; the main thread rethrows one
		       consolidated error message for the caller. */
		    job->main_showErrorMessages = FALSE;
		    pthread_mutex_init(&job->err_mutex, NULL);

		    if (n_bg_threads > 0) {
			if (n > (R_xlen_t) (SIZE_MAX / sizeof(SEXP)))
			    error(_("invalid length"));
			job->results = (SEXP *) calloc((size_t) n, sizeof(SEXP));
			if (job->results == NULL)
			    error(_("cannot allocate memory"));
			mtl_job_init_ranges(job);
		    }

			    mtlapply_run_data_t run_data;
			    run_data.job = job;
			    run_data.n_bg_threads = n_bg_threads;
			    run_data.ans = ans;
			    run_data.mu_locked = 0;
				    /* R_UnwindProtect tells the cleanup whether we are unwinding due
				       to an error/non-local jump. We need that to safely stop worker
				       threads on error; R_ExecWithCleanup runs its cleanup
				       unconditionally and would mark every job as failed. */
				    R_UnwindProtect(mtlapply_run, &run_data,
						    mtlapply_run_cleanup, &run_data,
						    NULL);

			    const char *noadopt = getenv("R_MTL_NOADOPT");
	    if (atomic_load_explicit(&job->error, memory_order_relaxed)) {
		char msg[1024];
		snprintf(msg, sizeof(msg), "%s",
			 job->errmsg[0] ? job->errmsg : "mtlapply error");
		if (n_bg_threads > 0) {
		    for (int t = 0; t < n_bg_threads; t++)
			mtl_pool.workers[t]->interp.preciousList = R_NilValue;
		}
		mtl_job_release(job);
		UNPROTECT(nprotect);
		/* Keep the pool alive across errors. Worker eval state is reset per
		   task, and tearing down interpreters inside an active error unwind
		   has proven unstable on some paths. */
		error("%s", msg);
	    }

			    /* Adopt all worker heaps into main before touching the results. */
			    if (n_bg_threads > 0) {
				if (noadopt == NULL || *noadopt == '\0') {
				    for (int t = 0; t < n_bg_threads; t++)
					R_mtl_adopt_worker_heap(&mtl_pool.workers[t]->interp);
				}
				for (R_xlen_t i = 0; i < n; i++) {
				    if (job->results[i] != NULL) {
					SET_VECTOR_ELT(ans, i, job->results[i]);
					job->results[i] = NULL;
				    }
				}
				/* Worker results are rooted via worker precious lists while the
				   job is running. Once results are rooted by 'ans', clear those
				   lists to avoid stale cross-job roots. */
				if (noadopt == NULL || *noadopt == '\0') {
				    for (int t = 0; t < n_bg_threads; t++)
					mtl_pool.workers[t]->interp.preciousList = R_NilValue;
				}
			    }

			    /* Optional debugging guard: if enabled, do a few cheap checks to
			       fail-fast on common mtlapply() corruption modes. */
			    if (getenv("R_MTL_SANITY")) {
			if (R_GlobalEnv == NULL || TYPEOF(R_GlobalEnv) != ENVSXP)
			    R_Suicide("mtlapply: corrupted R_GlobalEnv");
			if (R_BaseEnv == NULL || TYPEOF(R_BaseEnv) != ENVSXP)
			    R_Suicide("mtlapply: corrupted R_BaseEnv");
			if (HASHTAB(R_GlobalEnv) != R_NilValue &&
			    TYPEOF(HASHTAB(R_GlobalEnv)) != VECSXP)
			    R_Suicide("mtlapply: corrupted R_GlobalEnv hashtab");
			if (HASHTAB(R_BaseEnv) != R_NilValue &&
			    TYPEOF(HASHTAB(R_BaseEnv)) != VECSXP)
			    R_Suicide("mtlapply: corrupted R_BaseEnv hashtab");
		    }

			    mtl_job_release(job);
			    UNPROTECT(nprotect);
			    return ans;
		#endif
		}

/* .Internal(mtlparallelmax()) : testing/debugging aid.
 *
 * Returns the max number of worker threads simultaneously evaluating
 * user code in mtlapply() since the last call, and resets the counter. */
attribute_hidden SEXP do_mtlparallelmax(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
#ifndef HAVE_PTHREAD
    return ScalarInteger(0);
#else
    return ScalarInteger(mtl_parallel_max_and_reset());
#endif
}

/* .Internal(mtlpoolstats(reset))
 *
 * Returns named integer stats for the worker pool:
 * - threads.created: cumulative threads spawned
 * - threads.current: current pool size
 * - job.active: whether a job is currently attached to the pool
 * - job.depth: active nested job depth
 */
attribute_hidden SEXP do_mtlpoolstats(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
    int reset = asLogical(CAR(args));
    if (reset == NA_LOGICAL)
	error(_("invalid '%s' value"), "reset");

    SEXP out, nms;
    PROTECT(out = allocVector(INTSXP, 4));
    PROTECT(nms = allocVector(STRSXP, 4));

#ifndef HAVE_PTHREAD
    INTEGER(out)[0] = 0;
    INTEGER(out)[1] = 0;
    INTEGER(out)[2] = 0;
    INTEGER(out)[3] = 0;
#else
    unsigned long created = reset
	? atomic_exchange_explicit(&mtl_pool_threads_created, 0, memory_order_relaxed)
	: atomic_load_explicit(&mtl_pool_threads_created, memory_order_relaxed);
    int current = 0;
    int active = 0;
    int depth = 0;
    if (mtl_pool.inited) {
	pthread_mutex_lock(&mtl_pool.mu);
	current = mtl_pool.nthreads;
	active = (mtl_pool.job_top != NULL);
	depth = mtl_pool.job_depth;
	pthread_mutex_unlock(&mtl_pool.mu);
    }
    if (created > INT_MAX) created = INT_MAX;
    INTEGER(out)[0] = (int) created;
    INTEGER(out)[1] = current;
    INTEGER(out)[2] = active;
    INTEGER(out)[3] = depth;
#endif

    SET_STRING_ELT(nms, 0, mkChar("threads.created"));
    SET_STRING_ELT(nms, 1, mkChar("threads.current"));
    SET_STRING_ELT(nms, 2, mkChar("job.active"));
    SET_STRING_ELT(nms, 3, mkChar("job.depth"));
    setAttrib(out, R_NamesSymbol, nms);
    UNPROTECT(2);
    return out;
}

/* .Internal(mtlrpcstats(reset))
 *
 * Returns named integer stats for worker->main RPC traffic, optionally
 * resetting counters when reset is TRUE. */
attribute_hidden SEXP do_mtlrpcstats(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
    int reset = asLogical(CAR(args));
    if (reset == NA_LOGICAL)
	error(_("invalid '%s' value"), "reset");

    const int nreason = R_MTL_RPC_REASON_COUNT;
    const int nout = 2 * nreason + 2;
    SEXP out, nms;
    PROTECT(out = allocVector(INTSXP, nout));
    PROTECT(nms = allocVector(STRSXP, nout));
    int k = 0;
    unsigned long total_calls = 0, total_errors = 0;
    for (int i = 0; i < nreason; i++) {
	char nm[64];
	snprintf(nm, sizeof(nm), "calls.%s", mtl_rpc_reason_name_lookup(i));
#ifdef HAVE_PTHREAD
	unsigned long v = mtl_rpc_counter_read(&mtl_rpc_calls[i], reset);
#else
	unsigned long v = 0;
#endif
	total_calls += v;
	if (v > INT_MAX) v = INT_MAX;
	INTEGER(out)[k] = (int) v;
	SET_STRING_ELT(nms, k, mkChar(nm));
	k++;
    }
    for (int i = 0; i < nreason; i++) {
	char nm[64];
	snprintf(nm, sizeof(nm), "errors.%s", mtl_rpc_reason_name_lookup(i));
#ifdef HAVE_PTHREAD
	unsigned long v = mtl_rpc_counter_read(&mtl_rpc_errors[i], reset);
#else
	unsigned long v = 0;
#endif
	total_errors += v;
	if (v > INT_MAX) v = INT_MAX;
	INTEGER(out)[k] = (int) v;
	SET_STRING_ELT(nms, k, mkChar(nm));
	k++;
    }
    if (total_calls > INT_MAX) total_calls = INT_MAX;
    if (total_errors > INT_MAX) total_errors = INT_MAX;
    INTEGER(out)[k] = (int) total_calls;
    SET_STRING_ELT(nms, k, mkChar("calls.total"));
    k++;
    INTEGER(out)[k] = (int) total_errors;
    SET_STRING_ELT(nms, k, mkChar("errors.total"));
    setAttrib(out, R_NamesSymbol, nms);
    UNPROTECT(2);
    return out;
}

/* .Internal(mtlsharedenvstats(reset))
 *
 * Returns named integer counters for shared-environment coordination paths,
 * plus lightweight environment lookup/mutation classification counters. */
attribute_hidden SEXP do_mtlsharedenvstats(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
    int reset = asLogical(CAR(args));
    if (reset == NA_LOGICAL)
	error(_("invalid '%s' value"), "reset");

    unsigned long v[20];
#ifdef HAVE_PTHREAD
    v[0] = mtl_rpc_counter_read(&mtl_shared_reader_enter_calls, reset);
    v[1] = mtl_rpc_counter_read(&mtl_shared_reader_retry_after_set, reset);
    v[2] = mtl_rpc_counter_read(&mtl_shared_reader_wait_loops, reset);
    v[3] = mtl_rpc_counter_read(&mtl_shared_reader_wait_condwait, reset);
    v[4] = mtl_rpc_counter_read(&mtl_shared_reader_exit_calls, reset);
    v[5] = mtl_rpc_counter_read(&mtl_shared_writer_begin_calls, reset);
    v[6] = mtl_rpc_counter_read(&mtl_shared_writer_wait_loops, reset);
    v[7] = mtl_rpc_counter_read(&mtl_shared_writer_wait_condwait, reset);
    v[8] = mtl_rpc_counter_read(&mtl_shared_writer_end_calls, reset);
    v[9] = mtl_rpc_counter_read(&mtl_shared_writer_unlock_all_calls, reset);
    v[10] = mtl_rpc_counter_read(&mtl_shared_writer_scan_calls, reset);
    v[11] = mtl_rpc_counter_read(&mtl_shared_writer_scan_workers, reset);
#else
    for (int i = 0; i < 12; i++)
	v[i] = 0;
#endif
    unsigned long ev[R_MTL_ENVSTAT_COUNT];
    R_mtl_envirstats_get(ev, reset);
    for (int i = 0; i < R_MTL_ENVSTAT_COUNT; i++)
	v[12 + i] = ev[i];

    SEXP out, nms;
    PROTECT(out = allocVector(INTSXP, 20));
    PROTECT(nms = allocVector(STRSXP, 20));
    const char *names[20] = {
	"reader.enter.calls",
	"reader.retry_after_set",
	"reader.wait.loops",
	"reader.wait.condwait",
	"reader.exit.calls",
	"writer.begin.calls",
	"writer.wait.loops",
	"writer.wait.condwait",
	"writer.end.calls",
	"writer.unlock_all.calls",
	"writer.scan.calls",
	"writer.scan.workers",
	"env.mutcheck.calls",
	"env.mutcheck.inactive",
	"env.mutcheck.worker",
	"env.mutcheck.nonenv",
	"env.mutcheck.shared_true",
	"env.searchpath.calls",
	"env.searchpath.true",
	"env.worker_access.true"
    };
    for (int i = 0; i < 20; i++) {
	unsigned long x = v[i];
	if (x > INT_MAX) x = INT_MAX;
	INTEGER(out)[i] = (int) x;
	SET_STRING_ELT(nms, i, mkChar(names[i]));
    }
    setAttrib(out, R_NamesSymbol, nms);
    UNPROTECT(2);
    return out;
}

/* .Internal(mtnotifystats(reset))
 *
 * Returns named integer counters for notify fd signaling/draining. */
attribute_hidden SEXP do_mtnotifystats(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
    int reset = asLogical(CAR(args));
    if (reset == NA_LOGICAL)
	error(_("invalid '%s' value"), "reset");

    SEXP out, nms;
    PROTECT(out = allocVector(INTSXP, 10));
    PROTECT(nms = allocVector(STRSXP, 10));

    unsigned long vals[10];
#ifdef HAVE_PTHREAD
    vals[0] = mtl_rpc_counter_read(&mtl_notify_signal_calls, reset);
    vals[1] = mtl_rpc_counter_read(&mtl_notify_signal_write_ok, reset);
    vals[2] = mtl_rpc_counter_read(&mtl_notify_signal_write_eagain, reset);
    vals[3] = mtl_rpc_counter_read(&mtl_notify_signal_write_err, reset);
    vals[4] = mtl_rpc_counter_read(&mtl_notify_drain_calls, reset);
    vals[5] = mtl_rpc_counter_read(&mtl_notify_drain_bytes, reset);
    vals[6] = mtl_rpc_counter_read(&mtl_notify_drain_eagain, reset);
    vals[7] = mtl_rpc_counter_read(&mtl_notify_drain_err, reset);
    vals[8] = mtl_rpc_counter_read(&mtl_notify_pipe_init_ok, reset);
    vals[9] = mtl_rpc_counter_read(&mtl_notify_pipe_init_fail, reset);
#else
    for (int i = 0; i < 10; i++)
	vals[i] = 0;
#endif

    const char *names[10] = {
	"signal.calls",
	"signal.write_ok",
	"signal.write_eagain",
	"signal.write_err",
	"drain.calls",
	"drain.bytes",
	"drain.eagain",
	"drain.err",
	"pipe.init_ok",
	"pipe.init_fail"
    };
    for (int i = 0; i < 10; i++) {
	if (vals[i] > INT_MAX) vals[i] = INT_MAX;
	INTEGER(out)[i] = (int) vals[i];
	SET_STRING_ELT(nms, i, mkChar(names[i]));
    }
    setAttrib(out, R_NamesSymbol, nms);
    UNPROTECT(2);
    return out;
}

/* .Internal(mtlpoolreset())
 *
 * Force a teardown of the worker pool so the next mtlapply() call starts
 * from fresh worker interpreters. */
attribute_hidden SEXP do_mtlpoolreset(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
#ifdef HAVE_PTHREAD
    if (mtl_pool.inited)
	R_mtlpool_shutdown();
#endif
    return ScalarLogical(1);
}

/* .Internal(mtlisworker()) */
attribute_hidden SEXP do_mtlisworker(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
    return ScalarLogical((R_Interpreter != NULL && R_Interpreter->isMTLWorker) ? TRUE : FALSE);
}

typedef struct {
    SEXP expr;
    SEXP env;
} mtl_onmain_eval_t;

static SEXP mtl_onmain_eval(void *vp)
{
    mtl_onmain_eval_t *d = (mtl_onmain_eval_t *) vp;
    if (TYPEOF(d->env) != ENVSXP)
	error(_("'%s' must be an environment"), "env");
    /* Duplicate into the main heap before evaluation. The incoming call may
       originate from a worker heap and must not be mutated in place here. */
    SEXP expr = PROTECT(duplicate(d->expr));
    SEXP out = eval(expr, d->env);
    UNPROTECT(1);
    return out;
}

/* .Internal(mtonmain(EXPR, ENV)) */
attribute_hidden SEXP do_mtonmain(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
    SEXP expr = CAR(args);
    SEXP env = CADR(args);
    if (TYPEOF(env) != ENVSXP)
	error(_("'%s' must be an environment"), "env");

    if (R_Interpreter != NULL && R_Interpreter->isMTLWorker) {
	mtl_onmain_eval_t d = { .expr = expr, .env = env };
	return R_mtl_invoke_on_main_reason(mtl_onmain_eval, &d, R_MTL_RPC_OTHER);
    }
    return eval(expr, env);
}

/* .Internal(mtbackground(EXPR, ENV)) */
attribute_hidden SEXP do_mtbackground(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
#ifndef HAVE_PTHREAD
    error("background() requires pthreads support");
#else
    if (R_Interpreter != NULL && R_Interpreter->isMTLWorker)
	error("background() may only be called from the main thread");

    SEXP expr = CAR(args);
    SEXP env = CADR(args);
    if (TYPEOF(env) != ENVSXP)
	error(_("'%s' must be an environment"), "env");

    int nthreads = 2;
    SEXP opt = GetOption1(install("mtlapply.threads"));
    if (opt != R_NilValue && XLENGTH(opt) > 0)
	nthreads = asInteger(opt);
    if (nthreads == NA_INTEGER || nthreads < 1)
	error("invalid value in options(\"mtlapply.threads\"): must be >= 1");

    mtl_pool_init_if_needed();
    mtl_pool_ensure_threads(nthreads);

    mtl_future_t *f = (mtl_future_t *) calloc(1, sizeof(mtl_future_t));
    if (f == NULL)
	error(_("cannot allocate memory"));
    f->expr = expr;
    f->env = env;
    f->cont_fun = R_NilValue;
    f->cont_args = R_NilValue;
    f->value = R_NilValue;
    f->parent = NULL;
    f->deps_head = NULL;
    f->next_dep = NULL;
    f->dep_count = 0;
    f->status = MTL_FUTURE_PENDING;
    f->cancel_requested = 0;
    f->detached = 0;
    f->enqueued = 0;
    f->main_showErrorMessages = R_ShowErrorMessages;
    f->errmsg[0] = '\0';
    f->next_q = NULL;
    f->next_all = NULL;
    R_PreserveObject(f->expr);
    R_PreserveObject(f->env);

    pthread_mutex_lock(&mtl_pool.mu);
    f->next_all = mtl_pool.future_all;
    mtl_pool.future_all = f;
    mtl_future_queue_push_locked(f);
    mtl_pool.rpc_aborted = 0;
    mtl_set_threading_active_locked();
    pthread_cond_broadcast(&mtl_pool.cv);
    pthread_mutex_unlock(&mtl_pool.mu);

    SEXP ext = PROTECT(R_MakeExternalPtr(f, R_NilValue, R_NilValue));
    R_RegisterCFinalizerEx(ext, mtl_future_finalizer, TRUE);
    UNPROTECT(1);
    return ext;
#endif
}

/* .Internal(mtthen(FUTURE, FUN, DOTS)) */
attribute_hidden SEXP do_mtthen(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
#ifndef HAVE_PTHREAD
    error("then() requires pthreads support");
#else
    if (R_Interpreter != NULL && R_Interpreter->isMTLWorker)
	error("then() may only be called from the main thread");

    SEXP parent_fut = CAR(args);
    SEXP fun = CADR(args);
    SEXP dots = CADDR(args);
    if (!isFunction(fun))
	error(_("'%s' must be a function"), "fn");
    if (TYPEOF(dots) != VECSXP)
	error(_("'%s' must be a list"), "DOTS");

    mtl_future_t *parent = mtl_future_from_sexp(parent_fut);

    int nthreads = 2;
    SEXP opt = GetOption1(install("mtlapply.threads"));
    if (opt != R_NilValue && XLENGTH(opt) > 0)
	nthreads = asInteger(opt);
    if (nthreads == NA_INTEGER || nthreads < 1)
	error("invalid value in options(\"mtlapply.threads\"): must be >= 1");

    mtl_pool_init_if_needed();
    mtl_pool_ensure_threads(nthreads);

    mtl_future_t *f = (mtl_future_t *) calloc(1, sizeof(mtl_future_t));
    if (f == NULL)
	error(_("cannot allocate memory"));

    f->expr = R_NilValue;
    f->env = R_BaseEnv;
    f->cont_fun = fun;
    f->cont_args = dots;
    f->value = R_NilValue;
    f->parent = parent;
    f->deps_head = NULL;
    f->next_dep = NULL;
    f->dep_count = 0;
    f->status = MTL_FUTURE_PENDING;
    f->cancel_requested = 0;
    f->detached = 0;
    f->enqueued = 0;
    f->main_showErrorMessages = R_ShowErrorMessages;
    f->errmsg[0] = '\0';
    f->next_q = NULL;
    f->next_all = NULL;
    R_PreserveObject(f->env);
    R_PreserveObject(f->cont_fun);
    R_PreserveObject(f->cont_args);

    pthread_mutex_lock(&mtl_pool.mu);
    f->next_all = mtl_pool.future_all;
    mtl_pool.future_all = f;

    parent->dep_count++;
    if (mtl_future_is_terminal(parent)) {
	if (parent->status == MTL_FUTURE_FULFILLED) {
	    mtl_future_queue_push_locked(f);
	} else if (parent->status == MTL_FUTURE_CANCELLED) {
	    mtl_future_complete_locked(f, MTL_FUTURE_CANCELLED, R_NilValue, NULL);
	} else {
	    mtl_future_complete_locked(f, MTL_FUTURE_REJECTED, R_NilValue,
				      parent->errmsg[0] ? parent->errmsg : "background error");
	}
    } else {
	f->next_dep = parent->deps_head;
	parent->deps_head = f;
    }
    mtl_pool.rpc_aborted = 0;
    mtl_set_threading_active_locked();
    pthread_cond_broadcast(&mtl_pool.cv);
    pthread_mutex_unlock(&mtl_pool.mu);

    SEXP ext = PROTECT(R_MakeExternalPtr(f, R_NilValue, R_NilValue));
    R_RegisterCFinalizerEx(ext, mtl_future_finalizer, TRUE);
    UNPROTECT(1);
    return ext;
#endif
}

/* .Internal(mtwait(FUTURES, TIMEOUT)) */
attribute_hidden SEXP do_mtwait(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
#ifndef HAVE_PTHREAD
    return R_NilValue;
#else
    SEXP futures = CAR(args);
    double timeout = asReal(CADR(args));
    if (ISNAN(timeout))
	error(_("invalid '%s' value"), "timeout");
    if (!isVectorList(futures))
	error(_("'%s' must be a list of mt_future objects"), "futures");

    R_xlen_t n = XLENGTH(futures);
    if (n == 0)
	return R_NilValue;

    mtl_future_t **ptrs = (mtl_future_t **) R_alloc((size_t) n, sizeof(mtl_future_t *));
    for (R_xlen_t i = 0; i < n; i++)
	ptrs[i] = mtl_future_from_sexp(VECTOR_ELT(futures, i));

    int have_deadline = 0;
    struct timespec deadline;
    if (timeout == 0)
	have_deadline = 1, deadline.tv_sec = 0, deadline.tv_nsec = 0;
    else if (R_FINITE(timeout))
	have_deadline = mtl_timeout_to_deadline(timeout, &deadline);

    int found_idx = -1;
    mtl_future_status_t found_status = MTL_FUTURE_PENDING;
    SEXP found_value = R_NilValue;
    char found_err[1024];
    found_err[0] = '\0';

    pthread_mutex_lock(&mtl_pool.mu);
    for (;;) {
	if (mtl_is_main_thread())
	    mtl_rpc_service_locked();
	mtl_future_sweep_locked();
	for (R_xlen_t i = 0; i < n; i++) {
	    mtl_future_t *f = ptrs[i];
	    if (f != NULL && mtl_future_is_terminal(f)) {
		found_idx = (int) i + 1;
		found_status = f->status;
		found_value = f->value;
		if (found_status == MTL_FUTURE_REJECTED)
		    snprintf(found_err, sizeof(found_err), "%s", f->errmsg);
		break;
	    }
	}
	if (found_idx >= 0)
	    break;
	if (timeout == 0)
	    break;
	int rc = 0;
	if (have_deadline)
	    rc = pthread_cond_timedwait(&mtl_pool.cv, &mtl_pool.mu, &deadline);
	else
	    rc = pthread_cond_wait(&mtl_pool.cv, &mtl_pool.mu);
	if (rc == ETIMEDOUT)
	    break;
    }
    mtl_set_threading_active_locked();
    pthread_mutex_unlock(&mtl_pool.mu);

    if (found_idx < 0)
	return R_NilValue;

    SEXP out = PROTECT(allocVector(VECSXP, 5));
    SEXP nms = PROTECT(allocVector(STRSXP, 5));
    SET_VECTOR_ELT(out, 0, ScalarInteger(found_idx));
    SET_VECTOR_ELT(out, 1, ScalarLogical(found_status == MTL_FUTURE_FULFILLED));
    SET_VECTOR_ELT(out, 2, (found_status == MTL_FUTURE_FULFILLED) ? found_value : R_NilValue);
    if (found_status == MTL_FUTURE_REJECTED)
	SET_VECTOR_ELT(out, 3, mkString(found_err[0] ? found_err : "background error"));
    else
	SET_VECTOR_ELT(out, 3, R_NilValue);
    SET_VECTOR_ELT(out, 4, ScalarLogical(found_status == MTL_FUTURE_CANCELLED));
    SET_STRING_ELT(nms, 0, mkChar("index"));
    SET_STRING_ELT(nms, 1, mkChar("ok"));
    SET_STRING_ELT(nms, 2, mkChar("value"));
    SET_STRING_ELT(nms, 3, mkChar("error"));
    SET_STRING_ELT(nms, 4, mkChar("cancelled"));
    setAttrib(out, R_NamesSymbol, nms);
    UNPROTECT(2);
    return out;
#endif
}

/* .Internal(mtnotifyfd()) */
attribute_hidden SEXP do_mtnotifyfd(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
#ifndef HAVE_PTHREAD
    return ScalarInteger(-1);
#else
    mtl_pool_init_if_needed();
    int fd = -1;
    pthread_mutex_lock(&mtl_pool.mu);
    if (mtl_pool.notify_fd_read < 0 || mtl_pool.notify_fd_write < 0)
	mtl_notify_init_locked();
    fd = mtl_pool.notify_fd_read;
    pthread_mutex_unlock(&mtl_pool.mu);
    return ScalarInteger(fd);
#endif
}

/* .Internal(mtnotifydrain()) */
attribute_hidden SEXP do_mtnotifydrain(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
#ifndef HAVE_PTHREAD
    return ScalarInteger(0);
#else
    int fd = -1;
    pthread_mutex_lock(&mtl_pool.mu);
    if (mtl_pool.inited)
	fd = mtl_pool.notify_fd_read;
    pthread_mutex_unlock(&mtl_pool.mu);
    return ScalarInteger(mtl_notify_drain_fd(fd));
#endif
}

/* .Internal(mtnotifysignal(N)) -- testing/debug aid */
attribute_hidden SEXP do_mtnotifysignal(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
#ifndef HAVE_PTHREAD
    return ScalarInteger(0);
#else
    int n = asInteger(CAR(args));
    if (n == NA_INTEGER || n < 0)
	error(_("invalid '%s' value"), "n");
    mtl_pool_init_if_needed();
    pthread_mutex_lock(&mtl_pool.mu);
    for (int i = 0; i < n; i++)
	mtl_notify_signal_locked();
    pthread_mutex_unlock(&mtl_pool.mu);
    return ScalarInteger(n);
#endif
}

/* .Internal(mtcancel(FUTURE)) */
attribute_hidden SEXP do_mtcancel(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
#ifndef HAVE_PTHREAD
    return ScalarLogical(0);
#else
    SEXP fut = CAR(args);
    mtl_future_t *f = mtl_future_from_sexp(fut);
    int did = 0;
    pthread_mutex_lock(&mtl_pool.mu);
    if (f->status == MTL_FUTURE_PENDING) {
	if (f->enqueued)
	    mtl_future_queue_remove_locked(f);
	f->cancel_requested = 1;
	mtl_future_complete_locked(f, MTL_FUTURE_CANCELLED, R_NilValue, NULL);
	did = 1;
    } else if (f->status == MTL_FUTURE_RUNNING) {
	f->cancel_requested = 1;
	did = 1;
	while (f->status == MTL_FUTURE_RUNNING) {
	    if (mtl_is_main_thread())
		mtl_rpc_service_locked();
	    if (f->status != MTL_FUTURE_RUNNING)
		break;
	    pthread_cond_wait(&mtl_pool.cv, &mtl_pool.mu);
	}
    }
    mtl_set_threading_active_locked();
    if (mtl_is_main_thread())
	mtl_future_sweep_locked();
    pthread_mutex_unlock(&mtl_pool.mu);
    return ScalarLogical(did);
#endif
}

/* .Internal(vapply(X, FUN, FUN.VALUE, USE.NAMES)) */

/* This is a special .Internal */
attribute_hidden SEXP do_vapply(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP R_fcall, ans, names = R_NilValue, rowNames = R_NilValue,
	X, XX, FUN, value, dim_v;
    R_xlen_t i, n;
    int commonLen;
    int useNames, rnk_v = -1; // = array_rank(value) := length(dim(value))
    bool array_value;
    SEXPTYPE commonType;
    PROTECT_INDEX index = 0; /* initialize to avoid a warning */

    checkArity(op, args);
    PROTECT(X = CAR(args));
    PROTECT(XX = eval(CAR(args), rho));
    FUN = CADR(args);  /* must be unevaluated for use in e.g. bquote */
    PROTECT(value = eval(CADDR(args), rho));
    if (!isVector(value)) error(_("'FUN.VALUE' must be a vector"));
    useNames = asLogical(PROTECT(eval(CADDDR(args), rho)));
    UNPROTECT(1);
    // FIXME: does not protect against length > 1
    if (useNames == NA_LOGICAL) error(_("invalid '%s' value"), "USE.NAMES");

    n = xlength(XX);
    if (n == NA_INTEGER) error(_("invalid length"));
    bool realIndx = n > INT_MAX;

    commonLen = length(value);
    if (commonLen > 1 && n > INT_MAX)
	error(_("long vectors are not supported for matrix/array results"));
    commonType = TYPEOF(value);
    // check once here
    if (commonType != CPLXSXP && commonType != REALSXP &&
	commonType != INTSXP  && commonType != LGLSXP &&
	commonType != RAWSXP  && commonType != STRSXP &&
	commonType != VECSXP)
	error(_("type '%s' is not supported"), R_typeToChar(value));
    dim_v = getAttrib(value, R_DimSymbol);
    array_value = (TYPEOF(dim_v) == INTSXP && LENGTH(dim_v) >= 1);
    PROTECT(ans = allocVector(commonType, n*commonLen));
    if (useNames) {
	PROTECT(names = getAttrib(XX, R_NamesSymbol));
	if (isNull(names) && TYPEOF(XX) == STRSXP) {
	    UNPROTECT(1);
	    PROTECT(names = XX);
	}
	PROTECT_WITH_INDEX(rowNames = getAttrib(value,
						array_value ? R_DimNamesSymbol
						: R_NamesSymbol),
			   &index);
    }
    /* The R level code has ensured that XX is a vector.
       If it is atomic we can speed things up slightly by
       using the evaluated version.
    */
    {
	SEXP ind, tmp;
	/* Build call: FUN(XX[[<ind>]], ...) */

	SEXP isym = install("i");
	PROTECT(ind = allocVector(realIndx ? REALSXP : INTSXP, 1));
	defineVar(isym, ind, rho);
	INCREMENT_NAMED(ind);

	/* Notice that it is OK to have one arg to LCONS do memory
	   allocation and not PROTECT the result (LCONS does memory
	   protection of its args internally), but not both of them,
	   since the computation of one may destroy the other */
	PROTECT(tmp = LCONS(R_Bracket2Symbol,
			    LCONS(X, LCONS(isym, R_NilValue))));
	PROTECT(R_fcall = LCONS(FUN,
				LCONS(tmp, LCONS(R_DotsSymbol, R_NilValue))));

	int common_len_offset = 0;
	for(i = 0; i < n; i++) {
	    SEXP val; SEXPTYPE valType;
	    PROTECT_INDEX indx;
	    if (realIndx) REAL(ind)[0] = (double)(i + 1);
	    else INTEGER(ind)[0] = (int)(i + 1);
	    val = R_forceAndCall(R_fcall, 1, rho);
	    if (MAYBE_REFERENCED(val))
		val = lazy_duplicate(val); // Need to duplicate? Copying again anyway
	    PROTECT_WITH_INDEX(val, &indx);
	    if (length(val) != commonLen)
		error(_("values must be length %d,\n but FUN(X[[%lld]]) result is length %d"),
		       commonLen, (long long)i+1, length(val));
	    valType = TYPEOF(val);
	    if (valType != commonType) {
		bool okay = false;
		switch (commonType) {
		case CPLXSXP: okay = (valType == REALSXP) || (valType == INTSXP)
				    || (valType == LGLSXP); break;
		case REALSXP: okay = (valType == INTSXP) || (valType == LGLSXP); break;
		case INTSXP:  okay = (valType == LGLSXP); break;
		}
		if (!okay)
		    error(_("values must be type '%s',\n but FUN(X[[%lld]]) result is type '%s'"),
			  R_typeToChar(value), (long long)i+1, R_typeToChar(val));
		REPROTECT(val = coerceVector(val, commonType), indx);
	    }
	    /* Take row names from the first result only */
	    if (i == 0 && useNames && isNull(rowNames))
		REPROTECT(rowNames = getAttrib(val,
					       array_value ? R_DimNamesSymbol : R_NamesSymbol),
			  index);
	    // two cases - only for efficiency
	    if(commonLen == 1) { // common case
		switch (commonType) {
		case CPLXSXP: COMPLEX(ans)[i] = COMPLEX(val)[0]; break;
		case REALSXP: REAL(ans)   [i] = REAL   (val)[0]; break;
		case INTSXP:  INTEGER(ans)[i] = INTEGER(val)[0]; break;
		case LGLSXP:  LOGICAL(ans)[i] = LOGICAL(val)[0]; break;
		case RAWSXP:  RAW(ans)    [i] = RAW    (val)[0]; break;
		case STRSXP:  SET_STRING_ELT(ans, i, STRING_ELT(val, 0)); break;
		case VECSXP:  SET_VECTOR_ELT(ans, i, VECTOR_ELT(val, 0)); break;
		}
	    } else if (commonLen) { // commonLen > 1
		switch (commonType) {
		case REALSXP:
		    memcpy(REAL(ans) + common_len_offset,
			   REAL(val), commonLen * sizeof(double)); break;
		case INTSXP:
		    memcpy(INTEGER(ans) + common_len_offset,
			   INTEGER(val), commonLen * sizeof(int)); break;
		case LGLSXP:
		    memcpy(LOGICAL(ans) + common_len_offset,
			   LOGICAL(val), commonLen * sizeof(int)); break;
		case RAWSXP:
		    memcpy(RAW(ans) + common_len_offset,
			   RAW(val), commonLen * sizeof(Rbyte)); break;
		case CPLXSXP:
		    memcpy(COMPLEX(ans) + common_len_offset,
			   COMPLEX(val), commonLen * sizeof(Rcomplex)); break;
		case STRSXP:
		    for (int j = 0; j < commonLen; j++)
			SET_STRING_ELT(ans, common_len_offset + j, STRING_ELT(val, j));
		    break;
		case VECSXP:
		    for (int j = 0; j < commonLen; j++)
			SET_VECTOR_ELT(ans, common_len_offset + j, VECTOR_ELT(val, j));
		    break;
		}
		common_len_offset += commonLen;
	    }
	    UNPROTECT(1);
	}
	UNPROTECT(3);
    }

    if (commonLen != 1) {
	SEXP dim;
	rnk_v = array_value ? LENGTH(dim_v) : 1;
	PROTECT(dim = allocVector(INTSXP, rnk_v+1));
	if(array_value)
	    for(int j = 0; j < rnk_v; j++)
		INTEGER(dim)[j] = INTEGER(dim_v)[j];
	else
	    INTEGER(dim)[0] = commonLen;
	INTEGER(dim)[rnk_v] = (int) n;  // checked above
	setAttrib(ans, R_DimSymbol, dim);
	UNPROTECT(1);
    }

    if (useNames) {
	if (commonLen == 1) {
	    if(!isNull(names)) setAttrib(ans, R_NamesSymbol, names);
	} else {
	    if (!isNull(names) || !isNull(rowNames)) {
		SEXP dimnames;
		PROTECT(dimnames = allocVector(VECSXP, rnk_v+1));
		if(array_value && !isNull(rowNames)) {
		    if(TYPEOF(rowNames) != VECSXP || LENGTH(rowNames) != rnk_v)
			// should never happen ..
			error(_("dimnames(<value>) is neither NULL nor list of length %d"),
			      rnk_v);
		    for(int j = 0; j < rnk_v; j++)
			SET_VECTOR_ELT(dimnames, j, VECTOR_ELT(rowNames, j));
		} else
		    SET_VECTOR_ELT(dimnames, 0, rowNames);

		SET_VECTOR_ELT(dimnames, rnk_v, names);
		setAttrib(ans, R_DimNamesSymbol, dimnames);
		UNPROTECT(1);
	    }
	}
	UNPROTECT(2); /* names and rowNames */
    }
    UNPROTECT(4); /* X, XX, value, ans */
    return ans;
}

//  Apply FUN() to X recursively;  workhorse of rapply()
static SEXP do_one(SEXP X, SEXP FUN, SEXP classes, SEXP deflt,
		   bool replace, SEXP rho)
{
    SEXP ans, names, klass;
    bool matched = false;

    /* if X is a list, recurse.  Otherwise if it matches classes call f */
    if(X == R_NilValue || isVectorList(X)) {
	R_xlen_t n = xlength(X);
	if (replace) {
	    PROTECT(ans = shallow_duplicate(X));
	} else {
	    PROTECT(ans = allocVector(VECSXP, n));
	    names = getAttrib(X, R_NamesSymbol);
	    if(!isNull(names)) setAttrib(ans, R_NamesSymbol, names);
	}
	for(R_xlen_t i = 0; i < n; i++)
	    SET_VECTOR_ELT(ans, i, do_one(VECTOR_ELT(X, i), FUN, classes,
					  deflt, replace, rho));
	UNPROTECT(1);
	return ans;
    }
    if(strcmp(CHAR(STRING_ELT(classes, 0)), "ANY") == 0) /* ASCII */
	matched = true;
    else {
	PROTECT(klass = R_data_class(X, false));
	for(int i = 0; i < LENGTH(klass); i++)
	    for(int j = 0; j < length(classes); j++)
		if(Seql(STRING_ELT(klass, i), STRING_ELT(classes, j)))
		    matched = true;
	UNPROTECT(1);
    }
    if(matched) {
	/* This stores value to which the function is to be applied in
	   a variable X in the environment of the rapply closure call
	   that calls into the rapply .Internal. */
	SEXP R_fcall; /* could allocate once and preserve for re-use */
	SEXP Xsym = install("X");
	defineVar(Xsym, X, rho);
	INCREMENT_NAMED(X);
	/* PROTECT(R_fcall = lang2(FUN, Xsym)); */
	PROTECT(R_fcall = lang3(FUN, Xsym, R_DotsSymbol));
	ans = R_forceAndCall(R_fcall, 1, rho);
	if (MAYBE_REFERENCED(ans))
	    ans = lazy_duplicate(ans);
	UNPROTECT(1);
	return(ans);
    } else if(replace) return lazy_duplicate(X);
    else return lazy_duplicate(deflt);
}

attribute_hidden SEXP do_rapply(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP X, FUN, classes, deflt, how, ans;

    checkArity(op, args);
    X = CAR(args); args = CDR(args);
    if(!isVectorList(X))
	error(_("'%s' must be a list or expression"), "object");
    FUN = CAR(args); args = CDR(args);
    if(!isFunction(FUN)) error(_("invalid '%s' argument"), "f");
    classes = CAR(args); args = CDR(args);
    if(!isString(classes)) error(_("invalid '%s' argument"), "classes");
    deflt = CAR(args); args = CDR(args);
    how = CAR(args);
    if(!isString(how)) error(_("invalid '%s' argument"), "how");
    bool replace = strcmp(CHAR(STRING_ELT(how, 0)), "replace") == 0; /* ASCII */
    R_xlen_t n = xlength(X);
    if (replace) {
      PROTECT(ans = shallow_duplicate(X));
    } else {
      PROTECT(ans = allocVector(VECSXP, n));
      SEXP names = getAttrib(X, R_NamesSymbol);
      if(!isNull(names)) setAttrib(ans, R_NamesSymbol, names);
    }
    for(R_xlen_t i = 0; i < n; i++)
	SET_VECTOR_ELT(ans, i, do_one(VECTOR_ELT(X, i), FUN, classes, deflt,
				      replace, rho));
    UNPROTECT(1);
    return ans;
}

/**
 * Recursively check if  X  is a tree with only factor leaves;
 *   the workhorse for do_islistfactor()
 * @param X  list or expression
 * @return TRUE(1), FALSE(0) or NA_LOGICAL
 */
static int islistfactor(SEXP X)
{
    switch(TYPEOF(X)) {
    case VECSXP:
    case EXPRSXP: {
	int n = LENGTH(X), ans = NA_LOGICAL;
	for(int i = 0; i < n; i++) {
	    int isLF = islistfactor(VECTOR_ELT(X, i));
	    if(!isLF)
		return false;
	    else if(isLF == true)
		ans = true;
	    // else isLF is NA
	}
	return ans;
    }
    default:
	return isFactor(X);
    }
}


/* is this a tree with only factor leaves? */
// currently only called from unlist()
attribute_hidden SEXP do_islistfactor(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
    SEXP X = CAR(args);
    bool recursive = asBool2(CADR(args), call);
    int n = length(X);
    if(n == 0 || !isVectorList(X))
	return ScalarLogical(false);

    if(!recursive) {
	for(int i = 0; i < n; i++)
	    if(!isFactor(VECTOR_ELT(X, i)))
		return ScalarLogical(false);

	return ScalarLogical(true);
    }
    else { // recursive:  isVectorList(X) <==> X is VECSXP or EXPRSXP
	return ScalarLogical((islistfactor(X) == true) ? true : false);
    }
}
