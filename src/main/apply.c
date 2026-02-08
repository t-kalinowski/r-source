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
#endif

static SEXP checkArgIsSymbol(SEXP x) {
    if (TYPEOF(x) != SYMSXP)
	error("argument must be a symbol");
    return x;
}

#ifdef HAVE_PTHREAD
typedef struct {
    SEXP XX;
    R_xlen_t n;
    SEXP *results;
    R_xlen_t next;
    pthread_mutex_t next_mutex;
    int error;
    char errmsg[1024];
    pthread_mutex_t err_mutex;
    uintptr_t main_CStackStart;
    uintptr_t main_CStackLimit;
    uintptr_t main_OldCStackLimit;
    int main_showErrorMessages;
} mtl_shared_t;

typedef struct {
    mtl_shared_t *sh;
    SEXP argcell;
    SEXP fcall;
    R_InterpreterState interp;
} mtl_worker_t;

static pthread_mutex_t mtl_gil = PTHREAD_MUTEX_INITIALIZER;
static R_THREAD_LOCAL int mtl_gil_held = 0;
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

/* These are used by arithmetic.c to temporarily release the lock while doing
 * long-running, allocation-free compute loops (e.g. cos on a REALSXP). */
attribute_hidden int R_mtl_parallel_region_begin(void)
{
    if (!mtl_gil_held)
	return 0;
    mtl_gil_held = 0;
    pthread_mutex_unlock(&mtl_gil);
    int cur = atomic_fetch_add_explicit(&mtl_parallel_active, 1, memory_order_relaxed) + 1;
    mtl_parallel_update_max(cur);
    return 1;
}

attribute_hidden void R_mtl_parallel_region_end(int token)
{
    if (!token)
	return;
    atomic_fetch_sub_explicit(&mtl_parallel_active, 1, memory_order_relaxed);
    pthread_mutex_lock(&mtl_gil);
    mtl_gil_held = 1;
}

static int mtl_parallel_max_and_reset(void)
{
    return atomic_exchange_explicit(&mtl_parallel_max, 0, memory_order_relaxed);
}
static int mtl_main_thread_inited = 0;
static pthread_t mtl_main_thread;

static void mtl_interp_init_from_main(R_InterpreterState *st)
{
    st->currentExpr = NULL;
    st->returnedValue = R_NilValue;
    st->handlerStack = R_NilValue;
    st->restartStack = R_NilValue;
    st->visible = TRUE;
    st->showErrorMessages = 1;
    st->allowOptionsSet = 0;
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
    st->expressions_keep = R_Expressions_keep;
    st->expressions = st->expressions_keep;
    st->bcNodeStackBase = R_BCNodeStackBase;
    st->bcNodeStackEnd = R_BCNodeStackEnd;
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
    st->next = NULL;
#ifdef R_USE_SIGNALS
    st->pendingPromises = NULL;
    st->toplevelContext = R_ToplevelContext;
    st->globalContext = R_GlobalContext;
    st->sessionContext = R_SessionContext;
    st->exitContext = R_ExitContext;
#endif

    /* Allocate per-interpreter protection stack for this worker. */
    R_InitInterpreterProtectStack(st);
}

static void mtl_interp_reset_for_eval(R_InterpreterState *st, const mtl_shared_t *sh)
{
    st->currentExpr = NULL;
    st->returnedValue = R_NilValue;
    st->handlerStack = R_NilValue;
    st->restartStack = R_NilValue;
    st->visible = TRUE;
    st->showErrorMessages = sh->main_showErrorMessages;
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

static void *mtl_worker_main(void *vp)
{
    mtl_worker_t *w = (mtl_worker_t *) vp;
    mtl_shared_t *s = w->sh;

    R_RegisterInterpreterState(&w->interp);

    for (;;) {
	pthread_mutex_lock(&s->next_mutex);
	if (s->error || s->next >= s->n) {
	    pthread_mutex_unlock(&s->next_mutex);
	    break;
	}
	R_xlen_t i = s->next++;
	pthread_mutex_unlock(&s->next_mutex);

	pthread_mutex_lock(&mtl_gil);
	mtl_gil_held = 1;

	R_InterpreterState *saved_interp = R_Interpreter;
	R_Interpreter = &w->interp;

	/* Disable stack checks in this thread; our saved start is for main. */
	R_CStackStart = (uintptr_t) -1;
	R_CStackLimit = (uintptr_t) -1;
	R_OldCStackLimit = (uintptr_t) 0;

	/* Reset per-interpreter stacks/slots for this evaluation. */
	mtl_interp_reset_for_eval(&w->interp, s);

	SETCAR(w->argcell, VECTOR_ELT(s->XX, i));
	int err = 0;
	SEXP val = R_tryEvalSilent(w->fcall, R_GlobalEnv, &err);
	if (err || val == NULL) {
	    pthread_mutex_lock(&s->err_mutex);
	    if (!s->error) {
		s->error = 1;
		const char *msg = R_curErrorBuf();
		if (msg == NULL) msg = "error";
		snprintf(s->errmsg, sizeof(s->errmsg), "%s", msg);
		    }
		    pthread_mutex_unlock(&s->err_mutex);
	    R_Interpreter = saved_interp;
	    mtl_gil_held = 0;
	    pthread_mutex_unlock(&mtl_gil);
	    break;
	}

	PROTECT(val);
	if (MAYBE_REFERENCED(val))
	    val = lazy_duplicate(val);
	R_PreserveObject(val);
	UNPROTECT(1);

	s->results[i] = val;
	R_Interpreter = saved_interp;
	mtl_gil_held = 0;
	pthread_mutex_unlock(&mtl_gil);
    }

    /* Worker interpreter stacks are not reused; free its protection stack. */
    R_UnregisterInterpreterState(&w->interp);
    free(w->interp.ppStack);
    w->interp.ppStack = NULL;
    w->interp.ppStackTop = 0;

    return NULL;
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

    if (TYPEOF(XX) != VECSXP)
	error(_("'%s' must be a list"), "X");
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

    if (nthreads > n) nthreads = (int) n;

    SEXP names = getAttrib(XX, R_NamesSymbol);

    /* Convert DOTS list to a pairlist once, then share it across threads. */
    int nprotect = 0;
    PROTECT(XX); nprotect++;
    PROTECT(FUN); nprotect++;
    PROTECT(dots); nprotect++;
    SEXP tail0 = PROTECT(VectorToPairList(dots)); nprotect++;

    /* Results are preserved while threads are running to keep them GC-safe. */
    SEXP *results = (SEXP *) calloc((size_t) n, sizeof(SEXP));
    if (results == NULL)
	error(_("cannot allocate memory"));

    /* Save main thread's stack checks; worker threads will disable them. */
    mtl_shared_t sh;
    sh.XX = XX;
    sh.n = n;
    sh.results = results;
    sh.next = 0;
    sh.error = 0;
    sh.errmsg[0] = '\0';
    sh.main_CStackStart = R_CStackStart;
    sh.main_CStackLimit = R_CStackLimit;
    sh.main_OldCStackLimit = R_OldCStackLimit;
    sh.main_showErrorMessages = R_ShowErrorMessages;

    pthread_mutex_init(&sh.next_mutex, NULL);
    pthread_mutex_init(&sh.err_mutex, NULL);

    pthread_t *threads = (pthread_t *) calloc((size_t) nthreads, sizeof(pthread_t));
    mtl_worker_t *workers = (mtl_worker_t *) calloc((size_t) nthreads, sizeof(mtl_worker_t));
    if (threads == NULL || workers == NULL)
	error(_("cannot allocate memory"));

    /* Per-thread call objects (each thread gets its own DOTS pairlist). */
    for (int t = 0; t < nthreads; t++) {
	workers[t].sh = &sh;
	SEXP tail = PROTECT(duplicate(tail0)); nprotect++;
	workers[t].argcell = PROTECT(CONS(R_NilValue, tail)); nprotect++;
	workers[t].fcall = PROTECT(LCONS(FUN, workers[t].argcell)); nprotect++;
	mtl_interp_init_from_main(&workers[t].interp);
    }

    for (int t = 0; t < nthreads; t++) {
	int rc = pthread_create(&threads[t], NULL, mtl_worker_main, &workers[t]);
	if (rc != 0)
	    error("pthread_create failed");
    }
    for (int t = 0; t < nthreads; t++)
	pthread_join(threads[t], NULL);

    /* Restore main thread stack check globals. */
    R_CStackStart = sh.main_CStackStart;
    R_CStackLimit = sh.main_CStackLimit;
    R_OldCStackLimit = sh.main_OldCStackLimit;

    pthread_mutex_destroy(&sh.next_mutex);
    pthread_mutex_destroy(&sh.err_mutex);
    free(threads);
    free(workers);

    if (sh.error) {
	for (R_xlen_t i = 0; i < n; i++)
	    if (results[i] != NULL)
		R_ReleaseObject(results[i]);
	free(results);
	UNPROTECT(nprotect);
	error("%s", sh.errmsg[0] ? sh.errmsg : "mtlapply error");
    }

    SEXP ans = PROTECT(allocVector(VECSXP, n)); nprotect++;
    if (!isNull(names)) setAttrib(ans, R_NamesSymbol, names);
    for (R_xlen_t i = 0; i < n; i++) {
	SEXP val = results[i];
	if (val == NULL)
	    val = R_NilValue;
	SET_VECTOR_ELT(ans, i, val);
	if (results[i] != NULL)
	    R_ReleaseObject(results[i]);
    }

    free(results);
    UNPROTECT(nprotect);
    return ans;
#endif
}

/* .Internal(mtlparallelmax()) : testing/debugging aid.
 *
 * Returns the max number of threads simultaneously executing a released
 * "parallel region" since the last call, and resets the counter. */
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
