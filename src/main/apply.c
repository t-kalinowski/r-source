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

#ifdef HAVE_PTHREAD
	typedef struct {
	    SEXP XX;
	    SEXP FUN;
	    SEXP tail0;              /* DOTS as pairlist (main heap, duplicated per worker) */
	    R_xlen_t n;
	    SEXP *results;            /* C array of worker-owned SEXPs (adopted after join) */
	    atomic_long next;         /* next index to claim */
	    atomic_int error;         /* 0/1 */
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
    int job_done;
    mtl_job_t *job; /* owned by the mtlapply() caller thread */
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
static SEXP mtl_dotGlobalEnvSym = NULL;

	static void mtl_interp_init_from_main(R_InterpreterState *st)
	{
	    st->heap = NULL;
	    st->currentExpr = NULL;
	    st->returnedValue = R_NilValue;
	    st->handlerStack = R_NilValue;
	    st->restartStack = R_NilValue;
	    st->visible = TRUE;
	    st->showErrorMessages = 1;
	    st->allowOptionsSet = 0;
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

static void mtl_ensure_main_thread(void)
{
    if (!mtl_main_thread_inited) {
	mtl_main_thread = pthread_self();
	mtl_main_thread_inited = 1;
    }
    if (!pthread_equal(mtl_main_thread, pthread_self()))
	error("mtlapply() may only be called from the main thread");
}

static void mtl_pool_init_if_needed(void)
{
    if (mtl_pool.inited)
	return;

    memset(&mtl_pool, 0, sizeof(mtl_pool));
    pthread_mutex_init(&mtl_pool.mu, NULL);
    pthread_cond_init(&mtl_pool.cv, NULL);
    mtl_dotGlobalEnvSym = install(".GlobalEnv");
    mtl_pool.inited = 1;
}

	static void *mtl_pool_worker_main(void *vp)
	{
	    mtl_worker_t *w = (mtl_worker_t *) vp;
	    mtl_pool_t *p = w->pool;

    R_RegisterInterpreterState(&w->interp);

    R_InterpreterState *saved_interp = R_Interpreter;
    R_Interpreter = &w->interp;

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
    R_Toplevel.nodestack = R_BCNodeStackTop;
    R_Toplevel.bcprottop = R_BCProtTop;
    R_Toplevel.cend = NULL;
    R_Toplevel.cenddata = NULL;
    R_Toplevel.intsusp = FALSE;
    R_Toplevel.handlerstack = R_HandlerStack;
    R_Toplevel.restartstack = R_RestartStack;
    R_Toplevel.srcref = R_NilValue;
    R_Toplevel.prstack = NULL;
    R_Toplevel.returnValue = SEXP_TO_STACKVAL(NULL);
    R_Toplevel.evaldepth = 0;
    R_Toplevel.browserfinish = 0;
    R_GlobalContext = R_ToplevelContext = R_SessionContext = &R_Toplevel;
    R_ExitContext = NULL;
#endif

	    /* Disable stack checks in this thread; main's limits are unrelated. */
	    R_CStackStart = (uintptr_t) -1;
	    R_CStackLimit = (uintptr_t) -1;
	    R_OldCStackLimit = (uintptr_t) 0;

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
		pthread_mutex_unlock(&p->mu);

		/* Create a fresh worker "global" environment for this job.
		   Parent is the real global env, so reads see main-session bindings. */
		{
		    SEXP wenv = PROTECT(NewEnvironment(R_NilValue, R_NilValue, R_GlobalEnv));
		    w->interp.workerGlobalEnv = wenv;
		    defineVar(mtl_dotGlobalEnvSym, wenv, wenv); /* shadow .GlobalEnv */
		    UNPROTECT(1);
		}

		/* Build per-job call objects in the worker heap to avoid mutating
		   main-heap call structures from worker threads. */
		SEXP tail = PROTECT(duplicate(job->tail0));
		SEXP argcell = PROTECT(CONS(R_NilValue, tail));
		SEXP fcall = PROTECT(LCONS(job->FUN, argcell));
		MARK_NOT_MUTABLE(fcall);

		for (;;) {
		    if (atomic_load_explicit(&job->error, memory_order_relaxed))
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
			    if (atomic_exchange_explicit(&job->error, 1, memory_order_relaxed) == 0) {
				pthread_mutex_lock(&job->err_mutex);
				snprintf(job->errmsg, sizeof(job->errmsg),
					 "mtlapply: unsupported type '%s'", R_typeToChar(job->XX));
				pthread_mutex_unlock(&job->err_mutex);
			    }
			    break;
			}
			if (atomic_load_explicit(&job->error, memory_order_relaxed))
			    break;
		    }
		    int err = 0;
		    mtl_parallel_begin();
		    if (mtl_trace_enabled()) {
			fprintf(stderr, "[mtl] worker=%p eval i=%lld begin\n",
				(void *)w, (long long)i);
		fflush(stderr);
	    }
		    SEXP val = R_tryEvalSilent(fcall, w->interp.workerGlobalEnv, &err);
		    if (mtl_trace_enabled()) {
			fprintf(stderr, "[mtl] worker=%p eval i=%lld end err=%d\n",
				(void *)w, (long long)i, err);
			fflush(stderr);
		    }
	    mtl_parallel_end();
		    if (err || val == NULL) {
			if (atomic_exchange_explicit(&job->error, 1, memory_order_relaxed) == 0) {
			    pthread_mutex_lock(&job->err_mutex);
			    const char *msg = R_curErrorBuf();
		    if (msg == NULL) msg = "error";
		    snprintf(job->errmsg, sizeof(job->errmsg), "%s", msg);
		    pthread_mutex_unlock(&job->err_mutex);
		}
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
		}

			UNPROTECT(3); /* tail, argcell, fcall */

			/* Drop the job-global env so it won't be kept alive across adoption. */
			w->interp.workerGlobalEnv = NULL;

			/* Ensure all live worker nodes are moved out of New space before the
			   main thread adopts this heap. Without this, adoption can move
			   still-live New-space nodes into the main heap where they may be
			   overwritten by subsequent allocations before a main GC runs. */
			R_gc();

			pthread_mutex_lock(&p->mu);
			if (p->job == job && p->gen == mygen) {
			    p->job_done++;
			    pthread_cond_broadcast(&p->cv);
	}
	pthread_mutex_unlock(&p->mu);
    }

    R_Interpreter = saved_interp;

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

    /* From this point on the runtime may have multiple OS threads executing.
       Enable heavier heap synchronization needed for worker/main-heap interop. */
    R_mtl_threading_active = 1;

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

    mtl_ensure_main_thread();

    SEXP names = getAttrib(XX, R_NamesSymbol);

    if (n == 0) {
	SEXP ans = allocVector(VECSXP, 0);
	if (!isNull(names)) setAttrib(ans, R_NamesSymbol, names);
	return ans;
    }

    if (nthreads > n) nthreads = (int) n;

    mtl_pool_init_if_needed();
    mtl_pool_ensure_threads(nthreads);

	    /* Convert DOTS list to a pairlist once; workers duplicate in their heaps. */
	    int nprotect = 0;
	    PROTECT(XX); nprotect++;
	    PROTECT(FUN); nprotect++;
	    PROTECT(dots); nprotect++;
	    SEXP tail0 = PROTECT(VectorToPairList(dots)); nprotect++;

    /* Root the answer while workers are running. */
    SEXP ans = PROTECT(allocVector(VECSXP, n)); nprotect++;
    if (!isNull(names)) setAttrib(ans, R_NamesSymbol, names);

	    mtl_job_t job;
	    memset(&job, 0, sizeof(job));
	    job.XX = XX;
	    job.FUN = FUN;
	    job.tail0 = tail0;
	    job.n = n;
	    job.results = NULL;
	    atomic_init(&job.next, 0);
	    atomic_init(&job.error, 0);
	    job.errmsg[0] = '\0';
	    job.main_showErrorMessages = R_ShowErrorMessages;
	    pthread_mutex_init(&job.err_mutex, NULL);

	    if (n > (R_xlen_t) (SIZE_MAX / sizeof(SEXP)))
		error(_("invalid length"));
	    job.results = (SEXP *) calloc((size_t) n, sizeof(SEXP));
	    if (job.results == NULL)
		error(_("cannot allocate memory"));

	    pthread_mutex_lock(&mtl_pool.mu);
	    if (mtl_pool.job != NULL) {
		pthread_mutex_unlock(&mtl_pool.mu);
		error("mtlapply internal error: concurrent job");
	    }
    mtl_pool.job = &job;
	    mtl_pool.job_nthreads = nthreads;
	    mtl_pool.job_done = 0;
	    mtl_pool.gen++;
	    pthread_cond_broadcast(&mtl_pool.cv);
	    while (mtl_pool.job_done < nthreads)
		pthread_cond_wait(&mtl_pool.cv, &mtl_pool.mu);
	    mtl_pool.job = NULL;
	    pthread_cond_broadcast(&mtl_pool.cv);
	    pthread_mutex_unlock(&mtl_pool.mu);

	    pthread_mutex_destroy(&job.err_mutex);

	    if (atomic_load_explicit(&job.error, memory_order_relaxed)) {
		/* Ensure worker heaps don't keep partial results rooted. */
		for (int t = 0; t < nthreads; t++)
		    mtl_pool.workers[t]->interp.preciousList = R_NilValue;
		free(job.results);
		UNPROTECT(nprotect);
		error("%s", job.errmsg[0] ? job.errmsg : "mtlapply error");
	    }

		    /* Adopt all worker heaps into main before touching the results. */
		    const char *noadopt = getenv("R_MTL_NOADOPT");
		    if (noadopt == NULL || *noadopt == '\0') {
			for (int t = 0; t < nthreads; t++)
			    R_mtl_adopt_worker_heap(&mtl_pool.workers[t]->interp);
		    }

		    for (R_xlen_t i = 0; i < n; i++)
			SET_VECTOR_ELT(ans, i, job.results[i] ? job.results[i] : R_NilValue);
		    free(job.results);

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
