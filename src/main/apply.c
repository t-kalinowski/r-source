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
	typedef struct {
	    SEXP XX;
	    SEXP FUN;
	    SEXP eval_env;           /* call-site environment used for evaluation */
	    SEXP tail0;              /* DOTS as pairlist (main heap, duplicated per worker) */
	    R_xlen_t n;
	    SEXP *results;            /* C array of worker-owned SEXPs (adopted after join) */
	    atomic_long next;         /* next index to claim */
	    atomic_int error;         /* 0/1 */
	    atomic_int cancel_requested; /* 0/1 cooperative cancellation */
	    atomic_int active_eval_workers; /* workers currently evaluating/writing one item */
	    atomic_int workers_done;  /* workers that have fully exited this job */
	    int bg_threads;           /* number of worker threads assigned to this job */
	    atomic_int refcount;      /* heap lifetime across main + workers */
	    char errmsg[1024];
	    pthread_mutex_t err_mutex; /* protects errmsg */
	    int main_showErrorMessages;
	} mtl_job_t;

		typedef struct {
		    int id;
		    struct mtl_pool_t *pool;
		    unsigned long seen_gen;
		    R_InterpreterState interp;
		} mtl_worker_t;

	typedef struct mtl_main_req_t {
	    /* Function to execute on the main thread (must not escape data). */
	    SEXP (*fun)(void *);
	    void *data;
	    int reason;

	    SEXP result;          /* valid when ok==1 */
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
	    int job_nthreads;
	    mtl_job_t *job; /* owned by the mtlapply() caller thread */

	    /* Worker->main requests (e.g. global caches that must be mutated by main). */
	    mtl_main_req_t *rpc_head;
	    mtl_main_req_t *rpc_tail;
	    int rpc_aborted;
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

static mtl_pool_t mtl_pool;
static atomic_int mtl_pool_threads_created = 0;
static SEXP mtl_dotOptionsSym = NULL;

			static void mtl_interp_init_from_main(R_InterpreterState *st)
			{
		    st->heap = NULL;
		    st->gcEnabled = 1;
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

static void mtl_interp_reset_for_eval(R_InterpreterState *st, const mtl_job_t *job)
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

		static void mtl_pool_init_if_needed(void)
		{
		    if (mtl_pool.inited)
			return;

		    memset(&mtl_pool, 0, sizeof(mtl_pool));
		    pthread_mutex_init(&mtl_pool.mu, NULL);
		    pthread_cond_init(&mtl_pool.cv, NULL);
		    mtl_dotOptionsSym = install(".Options");
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
	free(job);
    }
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
	    r->ok = 0;
	    r->done = 0;
	    r->next = NULL;
	    pthread_mutex_init(&r->mu, NULL);
	    pthread_cond_init(&r->cv, NULL);

		    pthread_mutex_lock(&mtl_pool.mu);
		    if (mtl_pool.job == NULL || mtl_pool.rpc_aborted) {
		pthread_mutex_unlock(&mtl_pool.mu);
		pthread_mutex_destroy(&r->mu);
		pthread_cond_destroy(&r->cv);
		free(r);
		error("R_mtl_invoke_on_main: no active mtlapply() job");
	    }
		    if (mtl_pool.rpc_tail)
			mtl_pool.rpc_tail->next = r;
		    else
			mtl_pool.rpc_head = r;
		    mtl_pool.rpc_tail = r;
		    atomic_fetch_add_explicit(&mtl_rpc_calls[reason], 1, memory_order_relaxed);
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

	    pthread_mutex_destroy(&r->mu);
	    pthread_cond_destroy(&r->cv);
	    free(r);

		    if (!ok)
			atomic_fetch_add_explicit(&mtl_rpc_errors[reason], 1, memory_order_relaxed);
		    if (!ok)
			error("%s", msg[0] ? msg : "error");
		    return res;
		}

	static void *mtl_pool_worker_main(void *vp)
	{
	    mtl_worker_t *w = (mtl_worker_t *) vp;
	    mtl_pool_t *p = w->pool;

    R_RegisterInterpreterState(&w->interp);

    R_InterpreterState *saved_interp = R_InterpreterTLS;
    R_InterpreterTLS = &w->interp;
    R_InterpreterState *saved_compat = R_mtl_set_compat_interpreter(&w->interp);

#ifdef R_USE_SIGNALS
    /* begincontext() assumes R_GlobalContext is non-NULL. Install a
       per-thread dummy toplevel context as the base of the chain. */
    R_Toplevel.nextcontext = NULL;
    R_Toplevel.callflag = CTXT_TOPLEVEL;
    R_Toplevel.cstacktop = 0;
    R_Toplevel.gcenabled = R_GCEnabled;
    R_Toplevel.promargs = R_NilValue;
    R_Toplevel.callfun = R_NilValue;
    R_Toplevel.call = R_NilValue;
    R_Toplevel.cloenv = R_BaseEnv;
    R_Toplevel.sysparent = R_BaseEnv;
    R_Toplevel.conexit = R_NilValue;
    R_Toplevel.vmax = NULL;
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
		pthread_mutex_lock(&p->mu);
		while (!p->shutdown &&
		       (p->job == NULL || w->id >= p->job_nthreads || w->seen_gen == p->gen)) {
	    pthread_cond_wait(&p->cv, &p->mu);
	}
	if (p->shutdown) {
	    pthread_mutex_unlock(&p->mu);
	    break;
	}

		mtl_job_t *job = p->job;
		unsigned long mygen = p->gen;
		w->seen_gen = mygen;
		atomic_fetch_add_explicit(&job->refcount, 1, memory_order_relaxed);
		pthread_mutex_unlock(&p->mu);

			/* Snapshot global options into the worker heap for this job.
			   The worker sees/sets options against this snapshot (copy-on-write),
			   so packages using withr::with_options() work without mutating global
			   process state. */
			{
			    R_mtl_global_lock();
			    SEXP glob = SYMVALUE(mtl_dotOptionsSym);
			    w->interp.mtlOptionsBase = R_mtl_shallow_duplicate_pairlist(glob);
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
		    long idx = atomic_fetch_add_explicit(&job->next, 1, memory_order_relaxed);
		    if (idx < 0 || (R_xlen_t) idx >= job->n)
		break;
	    R_xlen_t i = (R_xlen_t) idx;

		    /* Reset per-interpreter stacks/slots for this evaluation. */
		    mtl_interp_reset_for_eval(&w->interp, job);

		    /* Set the function's first argument for this iteration. */
		    if (TYPEOF(job->XX) == VECSXP || TYPEOF(job->XX) == EXPRSXP) {
			SETCAR(argcell, VECTOR_ELT(job->XX, i));
		    } else {
			switch (TYPEOF(job->XX)) {
			case LGLSXP:
			    SETCAR(argcell, ScalarLogical(LOGICAL_ELT(job->XX, i)));
			    break;
			case INTSXP:
			    SETCAR(argcell, ScalarInteger(INTEGER_ELT(job->XX, i)));
			    break;
			case REALSXP:
			    SETCAR(argcell, ScalarReal(REAL_ELT(job->XX, i)));
			    break;
			case RAWSXP:
			    {
				SEXP s = allocVector(RAWSXP, 1);
				RAW(s)[0] = RAW(job->XX)[i];
				SETCAR(argcell, s);
			    }
			    break;
			case STRSXP:
			    SETCAR(argcell, ScalarString(STRING_ELT(job->XX, i)));
			    break;
			case CPLXSXP:
			    {
				SEXP s = allocVector(CPLXSXP, 1);
				COMPLEX(s)[0] = COMPLEX_ELT(job->XX, i);
				SETCAR(argcell, s);
			    }
			    break;
			default:
			    {
				char msg[128];
				snprintf(msg, sizeof(msg),
					 "mtlapply: unsupported type '%s'", R_typeToChar(job->XX));
				mtl_job_set_error(job, msg);
			    }
			    break;
			}
			if (atomic_load_explicit(&job->cancel_requested, memory_order_relaxed))
			    break;
		    }
		    if (atomic_load_explicit(&job->cancel_requested, memory_order_relaxed))
			break;
		    atomic_fetch_add_explicit(&job->active_eval_workers, 1, memory_order_relaxed);
		    int err = 0;
		    mtl_parallel_begin();
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
		    R_PreserveObject(val);
		    job->results[i] = val;
		    UNPROTECT(1);
		    atomic_fetch_sub_explicit(&job->active_eval_workers, 1, memory_order_relaxed);
		}

			UNPROTECT(3); /* tail, argcell, fcall */

				/* Drop worker-local options before GC/adoption: keep them job-local. */
				w->interp.mtlOptions = R_NilValue;
				w->interp.mtlOptionsBase = R_NilValue;

			/* Only on successful jobs: before heap adoption, move live worker
			   nodes out of New space. On error paths we do not adopt worker
			   heaps, and forcing GC here can deadlock teardown. */
			if (!atomic_load_explicit(&job->error, memory_order_relaxed))
			    R_gc();

			pthread_mutex_lock(&p->mu);
			atomic_fetch_add_explicit(&job->workers_done, 1, memory_order_relaxed);
			pthread_cond_broadcast(&p->cv);
			pthread_mutex_unlock(&p->mu);
		mtl_job_release(job);
	    }

    R_InterpreterTLS = saved_interp;
    R_mtl_set_compat_interpreter(saved_compat);

	    /* Worker interpreter stacks are not reused; free its protection stack. */
	    R_UnregisterInterpreterState(&w->interp);
	    R_DestroyInterpreterHeap(&w->interp);
	    free(w->interp.ppStack);
	    w->interp.ppStack = NULL;
	    w->interp.ppStackTop = 0;
    free(w->interp.bcNodeStackBase);
    w->interp.bcNodeStackBase = NULL;
    w->interp.bcNodeStackTop = NULL;
    w->interp.bcNodeStackEnd = NULL;
    w->interp.bcProtTop = NULL;
    w->interp.bcProtCommitted = NULL;

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
    pthread_cond_broadcast(&mtl_pool.cv);
    pthread_mutex_unlock(&mtl_pool.mu);

    for (int i = 0; i < mtl_pool.nthreads; i++)
	pthread_join(mtl_pool.threads[i], NULL);
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

		for (;;) {
		    if (atomic_load_explicit(&job->cancel_requested, memory_order_relaxed))
			break;
		long idx = atomic_fetch_add_explicit(&job->next, 1, memory_order_relaxed);
		if (idx < 0 || (R_xlen_t) idx >= job->n)
		    break;
		R_xlen_t i = (R_xlen_t) idx;

		/* Set the function's first argument for this iteration. */
		if (TYPEOF(job->XX) == VECSXP || TYPEOF(job->XX) == EXPRSXP) {
		    SETCAR(argcell, VECTOR_ELT(job->XX, i));
		} else {
		    switch (TYPEOF(job->XX)) {
		    case LGLSXP:
			SETCAR(argcell, ScalarLogical(LOGICAL_ELT(job->XX, i)));
			break;
		    case INTSXP:
			SETCAR(argcell, ScalarInteger(INTEGER_ELT(job->XX, i)));
			break;
		    case REALSXP:
			SETCAR(argcell, ScalarReal(REAL_ELT(job->XX, i)));
			break;
		    case RAWSXP:
			{
			    SEXP s = allocVector(RAWSXP, 1);
			    RAW(s)[0] = RAW(job->XX)[i];
			    SETCAR(argcell, s);
			}
			break;
		    case STRSXP:
			SETCAR(argcell, ScalarString(STRING_ELT(job->XX, i)));
			break;
		    case CPLXSXP:
			{
			    SEXP s = allocVector(CPLXSXP, 1);
			    COMPLEX(s)[0] = COMPLEX_ELT(job->XX, i);
			    SETCAR(argcell, s);
			}
			break;
		    default:
			{
			    char msg[128];
			    snprintf(msg, sizeof(msg),
				     "mtlapply: unsupported type '%s'", R_typeToChar(job->XX));
			    mtl_job_set_error(job, msg);
			}
			break;
		    }
		    if (atomic_load_explicit(&job->cancel_requested, memory_order_relaxed))
			break;
		}

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

    /* mtlapply() is restricted to the main thread; concurrent jobs are fatal. */
    if (mtl_pool.job != NULL) {
	pthread_mutex_unlock(&mtl_pool.mu);
	d->mu_locked = 0;
	R_Suicide("mtlapply internal error: concurrent job");
    }

	    /* Enable threaded allocator/GC fast paths only while workers may run. */
	    R_mtl_set_threading_active(1);

	    /* New job: clear any pending/aborted RPC state. */
	    mtl_pool.rpc_aborted = 0;
	    mtl_pool.rpc_head = NULL;
	    mtl_pool.rpc_tail = NULL;

	    mtl_pool.job = d->job;
	    mtl_pool.job_nthreads = d->n_bg_threads;
	    mtl_pool.gen++;
	    pthread_cond_broadcast(&mtl_pool.cv);

	    pthread_mutex_unlock(&mtl_pool.mu);
	    d->mu_locked = 0;

    /* For threaded jobs, keep the main thread as coordinator only.
       Running FUN concurrently on the main thread and worker threads
       can corrupt error/unwind state when one branch raises an error. */

	    pthread_mutex_lock(&mtl_pool.mu);
	    d->mu_locked = 1;
	    while (atomic_load_explicit(&d->job->workers_done, memory_order_relaxed) < d->n_bg_threads) {
		/* Service any worker->main requests (e.g. install/mkChar) while waiting. */
		mtl_rpc_service_locked();
		/* Fail-fast on worker errors: once no worker is actively evaluating,
		   detach this job and return immediately on the main thread. */
		if (atomic_load_explicit(&d->job->cancel_requested, memory_order_relaxed) &&
		    atomic_load_explicit(&d->job->active_eval_workers, memory_order_relaxed) == 0) {
		    mtl_rpc_abort_all_locked();
		    mtl_pool.job = NULL;
		    pthread_cond_broadcast(&mtl_pool.cv);
		    break;
		}
		if (atomic_load_explicit(&d->job->workers_done, memory_order_relaxed) < d->n_bg_threads)
		    pthread_cond_wait(&mtl_pool.cv, &mtl_pool.mu);
	    }

	    /* Drain any remaining requests before tearing down the job. */
	    mtl_rpc_service_locked();
	    if (mtl_pool.job == d->job) {
		mtl_pool.job = NULL;
		pthread_cond_broadcast(&mtl_pool.cv);
	    }

	    R_mtl_set_threading_active(0);

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

	/* Keep mtl_pool.job valid until all workers have reported done. */
	if (mtl_pool.job == d->job) {
	    while (atomic_load_explicit(&d->job->workers_done, memory_order_relaxed) < d->n_bg_threads) {
		mtl_rpc_service_locked();
		if (atomic_load_explicit(&d->job->active_eval_workers, memory_order_relaxed) == 0) {
		    mtl_pool.job = NULL;
		    pthread_cond_broadcast(&mtl_pool.cv);
		    break;
		}
		if (atomic_load_explicit(&d->job->workers_done, memory_order_relaxed) < d->n_bg_threads)
		    pthread_cond_wait(&mtl_pool.cv, &mtl_pool.mu);
	    }
	    mtl_rpc_service_locked();
	    if (mtl_pool.job == d->job) {
		mtl_pool.job = NULL;
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

    R_mtl_set_threading_active(0);
}
#endif /* HAVE_PTHREAD */

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

    if (!is_main && !in_worker)
	error("mtlapply() may only be called from the main thread");

    if (in_worker)
	return mtl_serial_apply_no_pool(XX, FUN, dots, names, rho);

	    if (n == 0) {
		SEXP ans = allocVector(VECSXP, 0);
		if (!isNull(names)) setAttrib(ans, R_NamesSymbol, names);
		return ans;
	    }

	    /* 'threads' is the number of worker threads (main thread coordinates). */
	    if (nthreads > n) nthreads = (int) n;
	    int n_bg_threads = nthreads;

	    if (n_bg_threads > 0) {
		mtl_pool_init_if_needed();
		mtl_pool_ensure_threads(n_bg_threads);
	    }

		    /* Convert DOTS list to a pairlist once; workers duplicate in their heaps. */
		    int nprotect = 0;
		    PROTECT(XX); nprotect++;
		    PROTECT(FUN); nprotect++;
		    PROTECT(dots); nprotect++;
		    SEXP tail0 = PROTECT(VectorToPairList(dots)); nprotect++;

	    /* Root the answer while workers are running. */
	    SEXP ans = PROTECT(allocVector(VECSXP, n)); nprotect++;
	    if (!isNull(names)) setAttrib(ans, R_NamesSymbol, names);

		    mtl_job_t *job = (mtl_job_t *) calloc(1, sizeof(mtl_job_t));
		    if (job == NULL)
			error(_("cannot allocate memory"));
		    job->XX = XX;
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
		    atomic_init(&job->refcount, 1); /* main owner; workers acquire when job starts */
		    job->errmsg[0] = '\0';
		    job->main_showErrorMessages = R_ShowErrorMessages;
		    pthread_mutex_init(&job->err_mutex, NULL);

		    if (n_bg_threads > 0) {
			if (n > (R_xlen_t) (SIZE_MAX / sizeof(SEXP)))
			    error(_("invalid length"));
			job->results = (SEXP *) calloc((size_t) n, sizeof(SEXP));
			if (job->results == NULL)
			    error(_("cannot allocate memory"));
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

			    if (atomic_load_explicit(&job->error, memory_order_relaxed)) {
				char msg[1024];
				snprintf(msg, sizeof(msg), "%s",
					 job->errmsg[0] ? job->errmsg : "mtlapply error");
			/* Ensure worker heaps don't keep partial results rooted. */
			for (int t = 0; t < n_bg_threads; t++)
			    mtl_pool.workers[t]->interp.preciousList = R_NilValue;
			mtl_job_release(job);
			UNPROTECT(nprotect);
			error("%s", msg);
		    }

			    /* Adopt all worker heaps into main before touching the results. */
			    const char *noadopt = getenv("R_MTL_NOADOPT");
			    if (n_bg_threads > 0) {
				if (noadopt == NULL || *noadopt == '\0') {
				    for (int t = 0; t < n_bg_threads; t++)
					R_mtl_adopt_worker_heap(&mtl_pool.workers[t]->interp);
				}
				for (R_xlen_t i = 0; i < n; i++) {
				    if (job->results[i] != NULL)
					SET_VECTOR_ELT(ans, i, job->results[i]);
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
 */
attribute_hidden SEXP do_mtlpoolstats(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
    int reset = asLogical(CAR(args));
    if (reset == NA_LOGICAL)
	error(_("invalid '%s' value"), "reset");

    SEXP out, nms;
    PROTECT(out = allocVector(INTSXP, 3));
    PROTECT(nms = allocVector(STRSXP, 3));

#ifndef HAVE_PTHREAD
    INTEGER(out)[0] = 0;
    INTEGER(out)[1] = 0;
    INTEGER(out)[2] = 0;
#else
    unsigned long created = reset
	? atomic_exchange_explicit(&mtl_pool_threads_created, 0, memory_order_relaxed)
	: atomic_load_explicit(&mtl_pool_threads_created, memory_order_relaxed);
    int current = 0;
    int active = 0;
    if (mtl_pool.inited) {
	pthread_mutex_lock(&mtl_pool.mu);
	current = mtl_pool.nthreads;
	active = (mtl_pool.job != NULL);
	pthread_mutex_unlock(&mtl_pool.mu);
    }
    if (created > INT_MAX) created = INT_MAX;
    INTEGER(out)[0] = (int) created;
    INTEGER(out)[1] = current;
    INTEGER(out)[2] = active;
#endif

    SET_STRING_ELT(nms, 0, mkChar("threads.created"));
    SET_STRING_ELT(nms, 1, mkChar("threads.current"));
    SET_STRING_ELT(nms, 2, mkChar("job.active"));
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
