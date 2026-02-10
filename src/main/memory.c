/*
 *  R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1998--2025  The R Core Team.
 *  Copyright (C) 1995, 1996  Robert Gentleman and Ross Ihaka
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

/*
 *	This code implements a non-moving generational collector
 *      with two or three generations.
 *
 *	Memory allocated by R_alloc is maintained in a stack.  Code
 *	that R_allocs memory must use vmaxget and vmaxset to obtain
 *	and reset the stack pointer.
 */

#define USE_RINTERNALS
#define COMPILING_MEMORY_C

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>

#ifdef HAVE_PTHREAD
# include <pthread.h>
# include <stdatomic.h>
#endif

#include <R_ext/RS.h> /* for S4 allocation */
#include <R_ext/Print.h>

/* Declarations for Valgrind.

   These are controlled by the
     --with-valgrind-instrumentation=
   option to configure, which sets VALGRIND_LEVEL to the
   supplied value (default 0) and defines NVALGRIND if
   the value is 0.

   level 0 is no additional instrumentation
   level 1 marks uninitialized numeric, logical, integer, raw,
	   complex vectors and R_alloc memory
   level 2 marks the data section of vector nodes as inaccessible
	   when they are freed.

   level 3 was withdrawn in R 3.2.0.

   It may be necessary to define NVALGRIND for a non-gcc
   compiler on a supported architecture if it has different
   syntax for inline assembly language from gcc.

   For Win32, Valgrind is useful only if running under Wine.
*/
#ifdef Win32
# ifndef USE_VALGRIND_FOR_WINE
# define NVALGRIND 1
#endif
#endif


#ifndef VALGRIND_LEVEL
# define VALGRIND_LEVEL 0
#endif

#ifndef NVALGRIND
# include "valgrind/memcheck.h"
#endif

/* For speed in cases when the argument is known to not be an ALTREP list. */
#define VECTOR_ELT_0(x,i)        ((SEXP *) STDVEC_DATAPTR(x))[i]
#define SET_VECTOR_ELT_0(x,i, v) (((SEXP *) STDVEC_DATAPTR(x))[i] = (v))

#define R_USE_SIGNALS 1
#include <Defn.h>
#include <Internal.h>
#include <R_ext/GraphicsEngine.h> /* GEDevDesc, GEgetDevice */

/* Registry of interpreter states (main + worker subinterpreters).
 *
 * Needed so the global GC can scan all protection stacks and per-interpreter
 * roots even when GC is triggered from a different interpreter/thread. */
static R_InterpreterState *R_InterpreterRegistry = NULL;
#ifdef HAVE_PTHREAD
static pthread_mutex_t R_InterpreterRegistryMutex = PTHREAD_MUTEX_INITIALIZER;
# define LOCK_INTERP_REGISTRY() pthread_mutex_lock(&R_InterpreterRegistryMutex)
# define UNLOCK_INTERP_REGISTRY() pthread_mutex_unlock(&R_InterpreterRegistryMutex)
#else
# define LOCK_INTERP_REGISTRY() ((void) 0)
# define UNLOCK_INTERP_REGISTRY() ((void) 0)
#endif
#include <R_ext/Rdynload.h>
#include <R_ext/Rallocators.h> /* for R_allocator_t structure */
#include <Rmath.h> // R_pow_di
#include <Print.h> // R_print

/* malloc uses size_t.  We are assuming here that size_t is at least
   as large as unsigned long.  Changed from int at 1.6.0 to (i) allow
   2-4Gb objects on 32-bit system and (ii) objects limited only by
   length on a 64-bit system.
*/

static int gc_reporting = 0;
static int gc_count = 0;

/* Report error encountered during garbage collection where for detecting
   problems it is better to abort, but for debugging (or some production runs,
   where external validation of results is possible) it may be preferred to
   continue. Configurable via _R_GC_FAIL_ON_ERROR_. Typically these problems
   are due to memory corruption.
*/
static Rboolean gc_fail_on_error = FALSE;
static void gc_error(const char *msg)
{
    if (gc_fail_on_error)
	R_Suicide(msg);
    else if (R_in_gc)
	REprintf("%s", msg);
    else
	error("%s", msg);
}

/* These are used in profiling to separate out time in GC */
attribute_hidden int R_gc_running(void) { return R_in_gc; }

#ifdef TESTING_WRITE_BARRIER
# define PROTECTCHECK
#endif

/* Heap synchronization for internal multi-threading experiments.
 *
 * Goal: allow concurrent allocation in multiple threads while still running
 * a stop-the-world GC.
 *
 * - A thread enters the "allocation region" before manipulating shared heap
 *   allocation cursors/counters.
 * - The GC (and a few other heap-structure mutations) enters an exclusive
 *   region that waits for in-flight allocators to drain.
 *
 * Unwind safety: if an error longjmp occurs while holding heap state, the
 * toplevel boundary must call R_mtl_heap_unlock_all() to avoid deadlocks and
 * stuck allocator counters (context.c does this).
 */
#ifdef HAVE_PTHREAD
# include <stdatomic.h>

/* Global: enabled only while mtlapply() workers are evaluating. */
/* Must have default visibility: internal bundled shared objects include
 * Defn.h and reference this flag via the R_Interpreter macro. */
attribute_visible int R_mtl_threading_active = 0;

static pthread_mutex_t R_heap_excl_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  R_heap_excl_cond  = PTHREAD_COND_INITIALIZER;
static atomic_int      R_heap_exclusive  = 0; /* set while in exclusive region */
static atomic_ulong    R_heap_inflight   = 0; /* active allocator threads */

static R_THREAD_LOCAL int R_heap_excl_depth = 0;
static R_THREAD_LOCAL int R_heap_alloc_depth = 0;
static R_THREAD_LOCAL int R_heap_inflight_held = 0;
static R_THREAD_LOCAL int R_heap_inflight_suspended = 0;

static R_INLINE void heap_alloc_suspend(void)
{
    if (__builtin_expect(!R_mtl_threading_active, 1))
	return;
    /* Worker heaps are private: they do not participate in main-heap GC sync.
       Workers only switch to the main heap while holding the heap lock, so
       main-heap allocation there is already excluded from concurrent GC. */
    if (R_Interpreter != NULL && R_Interpreter->isMTLWorker)
	return;
    if (R_heap_inflight_held && !R_heap_inflight_suspended) {
	unsigned long prev = atomic_fetch_sub_explicit(&R_heap_inflight, 1, memory_order_relaxed);
	R_heap_inflight_suspended = 1;
	/* If we were the last inflight allocator, wake any exclusive waiter.
	   Use the mutex to avoid lost wakeups between check and wait. */
	if (prev == 1) {
	    pthread_mutex_lock(&R_heap_excl_mutex);
	    pthread_cond_broadcast(&R_heap_excl_cond);
	    pthread_mutex_unlock(&R_heap_excl_mutex);
	}
    }
}

static R_INLINE void heap_alloc_resume(void)
{
    if (__builtin_expect(!R_mtl_threading_active, 1))
	return;
    /* See heap_alloc_suspend(). */
    if (R_Interpreter != NULL && R_Interpreter->isMTLWorker)
	return;
    if (R_heap_inflight_held && R_heap_inflight_suspended) {
	atomic_fetch_add_explicit(&R_heap_inflight, 1, memory_order_relaxed);
	R_heap_inflight_suspended = 0;
    }
}

static R_INLINE void heap_alloc_enter(void)
{
    if (__builtin_expect(!R_mtl_threading_active, 1))
	return;
    /* See heap_alloc_suspend(). */
    if (R_Interpreter != NULL && R_Interpreter->isMTLWorker)
	return;
    if (R_heap_excl_depth > 0) {
	R_heap_alloc_depth++;
	return;
    }
    if (R_heap_alloc_depth++ != 0)
	return;

    for (;;) {
	/* Wait if another thread is in an exclusive heap region. */
	if (atomic_load_explicit(&R_heap_exclusive, memory_order_acquire)) {
	    pthread_mutex_lock(&R_heap_excl_mutex);
	    while (atomic_load_explicit(&R_heap_exclusive, memory_order_relaxed))
		pthread_cond_wait(&R_heap_excl_cond, &R_heap_excl_mutex);
	    pthread_mutex_unlock(&R_heap_excl_mutex);
	    continue;
	}
	atomic_fetch_add_explicit(&R_heap_inflight, 1, memory_order_acq_rel);
	/* Re-check: if exclusivity raced with us, back out and retry. */
	if (atomic_load_explicit(&R_heap_exclusive, memory_order_acquire)) {
	    unsigned long prev = atomic_fetch_sub_explicit(&R_heap_inflight, 1, memory_order_relaxed);
	    pthread_mutex_lock(&R_heap_excl_mutex);
	    if (prev == 1)
		pthread_cond_broadcast(&R_heap_excl_cond);
	    while (atomic_load_explicit(&R_heap_exclusive, memory_order_relaxed))
		pthread_cond_wait(&R_heap_excl_cond, &R_heap_excl_mutex);
	    pthread_mutex_unlock(&R_heap_excl_mutex);
	    continue;
	}
	R_heap_inflight_held = 1;
	R_heap_inflight_suspended = 0;
	break;
    }
}

static R_INLINE void heap_alloc_exit(void)
{
    if (__builtin_expect(!R_mtl_threading_active, 1))
	return;
    /* See heap_alloc_suspend(). */
    if (R_Interpreter != NULL && R_Interpreter->isMTLWorker)
	return;
    if (R_heap_excl_depth > 0) {
	R_heap_alloc_depth--;
	return;
    }
    if (--R_heap_alloc_depth != 0)
	return;
    if (!R_heap_inflight_held)
	return;

    if (!R_heap_inflight_suspended)
    {
	unsigned long prev = atomic_fetch_sub_explicit(&R_heap_inflight, 1, memory_order_relaxed);
	/* If we were the last inflight allocator, wake any exclusive waiter. */
	if (prev == 1) {
	    pthread_mutex_lock(&R_heap_excl_mutex);
	    pthread_cond_broadcast(&R_heap_excl_cond);
	    pthread_mutex_unlock(&R_heap_excl_mutex);
	}
    }
    R_heap_inflight_held = 0;
    R_heap_inflight_suspended = 0;
}

attribute_hidden void R_mtl_heap_lock(void)
{
    if (!R_mtl_threading_active)
	return;
    if (R_heap_excl_depth++ > 0)
	return;

    /* If we're in an allocation region, temporarily drop our inflight count
       to avoid deadlocking waiting for ourselves. */
    heap_alloc_suspend();

    pthread_mutex_lock(&R_heap_excl_mutex);
    atomic_store_explicit(&R_heap_exclusive, 1, memory_order_release);
    while (atomic_load_explicit(&R_heap_inflight, memory_order_acquire) != 0)
	pthread_cond_wait(&R_heap_excl_cond, &R_heap_excl_mutex);
    /* Keep mutex held until unlock. */
}

attribute_hidden void R_mtl_heap_unlock(void)
{
    if (!R_mtl_threading_active)
	return;
    if (--R_heap_excl_depth > 0)
	return;

    atomic_store_explicit(&R_heap_exclusive, 0, memory_order_release);
    pthread_cond_broadcast(&R_heap_excl_cond);
    pthread_mutex_unlock(&R_heap_excl_mutex);
    heap_alloc_resume();
}

attribute_hidden void R_mtl_heap_unlock_all(void)
{
    if (!R_mtl_threading_active)
	return;
    /* Drop allocator inflight accounting if we are unwinding mid-allocation. */
    if (R_heap_inflight_held) {
	heap_alloc_suspend();
	R_heap_inflight_held = 0;
	R_heap_inflight_suspended = 0;
	R_heap_alloc_depth = 0;
    }

    if (R_heap_excl_depth > 0) {
	R_heap_excl_depth = 0;
	atomic_store_explicit(&R_heap_exclusive, 0, memory_order_release);
	pthread_cond_broadcast(&R_heap_excl_cond);
	pthread_mutex_unlock(&R_heap_excl_mutex);
    }
}
#else
static R_INLINE void heap_alloc_enter(void) {}
static R_INLINE void heap_alloc_exit(void) {}
static R_INLINE void heap_alloc_suspend(void) {}
static R_INLINE void heap_alloc_resume(void) {}

attribute_hidden void R_mtl_heap_lock(void) {}
attribute_hidden void R_mtl_heap_unlock(void) {}
attribute_hidden void R_mtl_heap_unlock_all(void) {}
#endif

/* Global lock for operations that still mutate shared, process-wide state.
 *
 * This is distinct from the heap lock: it is intended to serialize access to
 * global configuration/state that has not yet been moved into interpreter
 * state. Use sparingly and in small critical sections. */
#ifdef HAVE_PTHREAD
static pthread_mutex_t R_global_mutex = PTHREAD_MUTEX_INITIALIZER;
static R_THREAD_LOCAL int R_global_lock_depth = 0;

attribute_hidden void R_mtl_global_lock(void)
{
    if (R_global_lock_depth++ == 0)
	pthread_mutex_lock(&R_global_mutex);
}

attribute_hidden void R_mtl_global_unlock(void)
{
    if (--R_global_lock_depth == 0)
	pthread_mutex_unlock(&R_global_mutex);
}

attribute_hidden int R_mtl_global_is_locked(void)
{
    return R_global_lock_depth > 0;
}

attribute_hidden void R_mtl_global_unlock_all(void)
{
    if (R_global_lock_depth > 0) {
	R_global_lock_depth = 0;
	pthread_mutex_unlock(&R_global_mutex);
    }
}
#else
attribute_hidden void R_mtl_global_lock(void) {}
attribute_hidden void R_mtl_global_unlock(void) {}
attribute_hidden void R_mtl_global_unlock_all(void) {}
attribute_hidden int R_mtl_global_is_locked(void) { return 0; }
#endif

#ifdef PROTECTCHECK
/* This is used to help detect unprotected SEXP values.  It is most
   useful if the strict barrier is enabled as well. The strategy is:

       All GCs are full GCs

       New nodes are marked as NEWSXP

       After a GC all free nodes that are not of type NEWSXP are
       marked as type FREESXP

       Most calls to accessor functions check their SEXP inputs and
       SEXP outputs with CHK() to see if a reachable node is a
       FREESXP and signal an error if a FREESXP is found.

   Combined with GC torture this can help locate where an unprotected
   SEXP is being used.

   This approach will miss cases where an unprotected node has been
   re-allocated.  For these cases it is possible to set
   gc_inhibit_release to TRUE.  FREESXP nodes will not be reallocated,
   or large ones released, until gc_inhibit_release is set to FALSE
   again.  This will of course result in memory growth and should be
   used with care and typically in combination with OS mechanisms to
   limit process memory usage.  LT */

/* Before a node is marked as a FREESXP by the collector the previous
   type is recorded.  For now using the LEVELS field seems
   reasonable.  */
#define OLDTYPE(s) LEVELS(s)
#define SETOLDTYPE(s, t) SETLEVELS(s, t)

static R_INLINE SEXP CHK(SEXP x)
{
    /* **** NULL check because of R_CurrentExpr */
    if (x != NULL && TYPEOF(x) == FREESXP)
	error("unprotected object (%p) encountered (was %s)",
	      (void *)x, sexptype2char(OLDTYPE(x)));
    return x;
}
#else
#define CHK(x) x
#endif

/* The following three variables definitions are used to record the
   address and type of the first bad type seen during a collection,
   and for FREESXP nodes they record the old type as well. */
static SEXPTYPE bad_sexp_type_seen = 0;
static SEXP bad_sexp_type_sexp = NULL;
#ifdef PROTECTCHECK
static SEXPTYPE bad_sexp_type_old_type = 0;
#endif
static int bad_sexp_type_line = 0;

static R_INLINE void register_bad_sexp_type(SEXP s, int line)
{
    if (bad_sexp_type_seen == 0) {
	bad_sexp_type_seen = TYPEOF(s);
	bad_sexp_type_sexp = s;
	bad_sexp_type_line = line;
#ifdef PROTECTCHECK
	if (TYPEOF(s) == FREESXP)
	    bad_sexp_type_old_type = OLDTYPE(s);
#endif
    }
}

/* also called from typename() in inspect.c */
attribute_hidden
const char *sexptype2char(SEXPTYPE type) {
    switch (type) {
    case NILSXP:	return "NILSXP";
    case SYMSXP:	return "SYMSXP";
    case LISTSXP:	return "LISTSXP";
    case CLOSXP:	return "CLOSXP";
    case ENVSXP:	return "ENVSXP";
    case PROMSXP:	return "PROMSXP";
    case LANGSXP:	return "LANGSXP";
    case SPECIALSXP:	return "SPECIALSXP";
    case BUILTINSXP:	return "BUILTINSXP";
    case CHARSXP:	return "CHARSXP";
    case LGLSXP:	return "LGLSXP";
    case INTSXP:	return "INTSXP";
    case REALSXP:	return "REALSXP";
    case CPLXSXP:	return "CPLXSXP";
    case STRSXP:	return "STRSXP";
    case DOTSXP:	return "DOTSXP";
    case ANYSXP:	return "ANYSXP";
    case VECSXP:	return "VECSXP";
    case EXPRSXP:	return "EXPRSXP";
    case BCODESXP:	return "BCODESXP";
    case EXTPTRSXP:	return "EXTPTRSXP";
    case WEAKREFSXP:	return "WEAKREFSXP";
    case OBJSXP:	return "OBJSXP"; /* was S4SXP */
    case RAWSXP:	return "RAWSXP";
    case NEWSXP:	return "NEWSXP"; /* should never happen */
    case FREESXP:	return "FREESXP";
    default:		return "<unknown>";
    }
}

#define GC_TORTURE

/* These are per-interpreter state: worker threads must not be able to
   interfere with main-thread GC scheduling decisions. */
static R_THREAD_LOCAL int gc_pending = 0;
#ifdef GC_TORTURE
/* **** if the user specified a wait before starting to force
   **** collections it might make sense to also wait before starting
   **** to inhibit releases */
static R_THREAD_LOCAL int gc_force_wait = 0;
static R_THREAD_LOCAL int gc_force_gap = 0;
static R_THREAD_LOCAL Rboolean gc_inhibit_release = FALSE;
#define FORCE_GC (gc_pending || (gc_force_wait > 0 ? (--gc_force_wait > 0 ? 0 : (gc_force_wait = gc_force_gap, 1)) : 0))
#else
# define FORCE_GC gc_pending
#endif

#ifdef R_MEMORY_PROFILING
static void R_ReportAllocation(R_size_t);
static void R_ReportNewPage(void);
#endif

#define GC_PROT(X) do { \
    int __wait__ = gc_force_wait; \
    int __gap__ = gc_force_gap;			   \
    Rboolean __release__ = gc_inhibit_release;	   \
    X;						   \
    gc_force_wait = __wait__;			   \
    gc_force_gap = __gap__;			   \
    gc_inhibit_release = __release__;		   \
}  while(0)

static void R_gc_internal(R_size_t size_needed);
static void R_gc_no_finalizers(R_size_t size_needed);
static void R_gc_lite(void);
static void mem_err_heap(R_size_t size);
static void mem_err_malloc(R_size_t size);

static SEXPREC UnmarkedNodeTemplate;
#define NODE_IS_MARKED(s) (MARK(s)==1)
#define MARK_NODE(s) (MARK(s)=1)
#define UNMARK_NODE(s) (MARK(s)=0)


/* Tuning Constants. Most of these could be made settable from R,
   within some reasonable constraints at least.  Since there are quite
   a lot of constants it would probably make sense to put together
   several "packages" representing different space/speed tradeoffs
   (e.g. very aggressive freeing and small increments to conserve
   memory; much less frequent releasing and larger increments to
   increase speed). */

/* There are three levels of collections.  Level 0 collects only the
   youngest generation, level 1 collects the two youngest generations,
   and level 2 collects all generations.  Higher level collections
   occur at least after specified numbers of lower level ones.  After
   LEVEL_0_FREQ level zero collections a level 1 collection is done;
   after every LEVEL_1_FREQ level 1 collections a level 2 collection
   occurs.  Thus, roughly, every LEVEL_0_FREQ-th collection is a level
   1 collection and every (LEVEL_0_FREQ * LEVEL_1_FREQ)-th collection
   is a level 2 collection.  */
#define LEVEL_0_FREQ 20
#define LEVEL_1_FREQ 5
static int collect_counts_max[] = { LEVEL_0_FREQ, LEVEL_1_FREQ };

/* When a level N collection fails to produce at least MinFreeFrac *
   R_NSize free nodes and MinFreeFrac * R_VSize free vector space, the
   next collection will be a level N + 1 collection.

   This constant is also used in heap size adjustment as a minimal
   fraction of the minimal heap size levels that should be available
   for allocation. */
static double R_MinFreeFrac = 0.2;

/* When pages are released, a number of free nodes equal to
   R_MaxKeepFrac times the number of allocated nodes for each class is
   retained.  Pages not needed to meet this requirement are released.
   An attempt to release pages is made every R_PageReleaseFreq level 1
   or level 2 collections. */
static double R_MaxKeepFrac = 0.5;
static int R_PageReleaseFreq = 1;

/* The heap size constants R_NSize and R_VSize are used for triggering
   collections.  The initial values set by defaults or command line
   arguments are used as minimal values.  After full collections these
   levels are adjusted up or down, though not below the minimal values
   or above the maximum values, towards maintain heap occupancy within
   a specified range.  When the number of nodes in use reaches
   R_NGrowFrac * R_NSize, the value of R_NSize is incremented by
   R_NGrowIncrMin + R_NGrowIncrFrac * R_NSize.  When the number of
   nodes in use falls below R_NShrinkFrac, R_NSize is decremented by
   R_NShrinkIncrMin + R_NShrinkFrac * R_NSize.  Analogous adjustments
   are made to R_VSize.

   This mechanism for adjusting the heap size constants is very
   primitive but hopefully adequate for now.  Some modeling and
   experimentation would be useful.  We want the heap sizes to get set
   at levels adequate for the current computations.  The present
   mechanism uses only the size of the current live heap to provide
   information about the current needs; since the current live heap
   size can be very volatile, the adjustment mechanism only makes
   gradual adjustments.  A more sophisticated strategy would use more
   of the live heap history.

   Some of the settings can now be adjusted by environment variables.
*/
static double R_NGrowFrac = 0.70;
static double R_NShrinkFrac = 0.30;

static double R_VGrowFrac = 0.70;
static double R_VShrinkFrac = 0.30;

#ifdef SMALL_MEMORY
/* On machines with only 32M of memory (or on a classic Mac OS port)
   it might be a good idea to use settings like these that are more
   aggressive at keeping memory usage down. */
static double R_NGrowIncrFrac = 0.0, R_NShrinkIncrFrac = 0.2;
static int R_NGrowIncrMin = 50000, R_NShrinkIncrMin = 0;
static double R_VGrowIncrFrac = 0.0, R_VShrinkIncrFrac = 0.2;
static int R_VGrowIncrMin = 100000, R_VShrinkIncrMin = 0;
#else
static double R_NGrowIncrFrac = 0.2, R_NShrinkIncrFrac = 0.2;
static int R_NGrowIncrMin = 40000, R_NShrinkIncrMin = 0;
static double R_VGrowIncrFrac = 0.2, R_VShrinkIncrFrac = 0.2;
static int R_VGrowIncrMin = 80000, R_VShrinkIncrMin = 0;
#endif

static void init_gc_grow_settings(void)
{
    char *arg;

    arg = getenv("R_GC_MEM_GROW");
    if (arg != NULL) {
	int which = (int) atof(arg);
	switch (which) {
	case 0: /* very conservative -- the SMALL_MEMORY settings */
	    R_NGrowIncrFrac = 0.0;
	    R_VGrowIncrFrac = 0.0;
	    break;
	case 1: /* default */
	    break;
	case 2: /* somewhat aggressive */
	    R_NGrowIncrFrac = 0.3;
	    R_VGrowIncrFrac = 0.3;
	    break;
	case 3: /* more aggressive */
	    R_NGrowIncrFrac = 0.4;
	    R_VGrowIncrFrac = 0.4;
	    R_NGrowFrac = 0.5;
	    R_VGrowFrac = 0.5;
	    break;
	}
    }
    arg = getenv("R_GC_GROWFRAC");
    if (arg != NULL) {
	double frac = atof(arg);
	if (0.35 <= frac && frac <= 0.75) {
	    R_NGrowFrac = frac;
	    R_VGrowFrac = frac;
	}
    }
    arg = getenv("R_GC_GROWINCRFRAC");
    if (arg != NULL) {
	double frac = atof(arg);
	if (0.05 <= frac && frac <= 0.80) {
	    R_NGrowIncrFrac = frac;
	    R_VGrowIncrFrac = frac;
	}
    }
    arg = getenv("R_GC_NGROWINCRFRAC");
    if (arg != NULL) {
	double frac = atof(arg);
	if (0.05 <= frac && frac <= 0.80)
	    R_NGrowIncrFrac = frac;
    }
    arg = getenv("R_GC_VGROWINCRFRAC");
    if (arg != NULL) {
	double frac = atof(arg);
	if (0.05 <= frac && frac <= 0.80)
	    R_VGrowIncrFrac = frac;
    }
}

/* Maximal Heap Limits.  These variables contain upper limits on the
   heap sizes.  They could be made adjustable from the R level,
   perhaps by a handler for a recoverable error.

   Access to these values is provided with reader and writer
   functions; the writer function insures that the maximal values are
   never set below the current ones. */
static R_size_t R_MaxVSize = R_SIZE_T_MAX;
static R_size_t R_MaxNSize = R_SIZE_T_MAX;
static int vsfac = 1; /* current units for vsize: changes at initialization */

attribute_hidden R_size_t R_GetMaxVSize(void)
{
    if (R_MaxVSize == R_SIZE_T_MAX) return R_SIZE_T_MAX;
    return R_MaxVSize * vsfac;
}

attribute_hidden Rboolean R_SetMaxVSize(R_size_t size)
{
    if (size == R_SIZE_T_MAX) {
	R_MaxVSize = R_SIZE_T_MAX;
	return TRUE;
    }
    if (vsfac == 1) {
	if (size >= R_VSize) {
	    R_MaxVSize = size;
	    return TRUE;
	}
    } else 
	if (size / vsfac >= R_VSize) {
	    R_MaxVSize = (size + 1) / vsfac;
	    return TRUE;
	}
    return FALSE;
}

attribute_hidden R_size_t R_GetMaxNSize(void)
{
    return R_MaxNSize;
}

attribute_hidden Rboolean R_SetMaxNSize(R_size_t size)
{
    if (size >= R_NSize) {
	R_MaxNSize = size;
	return TRUE;
    }
    return FALSE;
}

attribute_hidden void R_SetPPSize(R_size_t size)
{
    R_PPStackSize = (int) size;
}

attribute_hidden SEXP do_maxVSize(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    const double MB = 1048576.0;
    double newval = asReal(CAR(args));

    if (newval > 0) {
	if (newval == R_PosInf)
	    R_MaxVSize = R_SIZE_T_MAX;
	else {
	    double newbytes = newval * MB;
	    if (newbytes >= (double) R_SIZE_T_MAX)
		R_MaxVSize = R_SIZE_T_MAX;
	    else if (!R_SetMaxVSize((R_size_t) newbytes))
		warning(_("a limit lower than current usage, so ignored"));
	}
    }

    if (R_MaxVSize == R_SIZE_T_MAX)
	return ScalarReal(R_PosInf);
    else
	return ScalarReal(R_GetMaxVSize() / MB);
}

attribute_hidden SEXP do_maxNSize(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    double newval = asReal(CAR(args));

    if (newval > 0) {
	if (newval == R_PosInf)
	    R_MaxNSize = R_SIZE_T_MAX;
	else {
	    if (newval >= (double) R_SIZE_T_MAX) 
		R_MaxNSize = R_SIZE_T_MAX;
	    else if (!R_SetMaxNSize((R_size_t) newval))
		warning(_("a limit lower than current usage, so ignored"));
	}
    }

    if (R_MaxNSize == R_SIZE_T_MAX)
	return ScalarReal(R_PosInf);
    else
	return ScalarReal(R_GetMaxNSize());
}


/* Miscellaneous Globals. */

/* R_VStack and R_PreciousList are per-interpreter (see R_InterpreterState). */
static R_size_t orig_R_NSize;
static R_size_t orig_R_VSize;

static R_size_t R_N_maxused=0;
static R_size_t R_V_maxused=0;

/* Node Classes.  Non-vector nodes are of class zero. Small vector
   nodes are in classes 1, ..., NUM_SMALL_NODE_CLASSES, and large
   vector nodes are in class LARGE_NODE_CLASS. Vectors with
   custom allocators are in CUSTOM_NODE_CLASS. For vector nodes the
   node header is followed in memory by the vector data, offset from
   the header by SEXPREC_ALIGN. */

#define NUM_NODE_CLASSES 8

/* sxpinfo allocates 3 bits for the node class, so at most 8 are allowed */
#if NUM_NODE_CLASSES > 8
# error NUM_NODE_CLASSES must be at most 8
#endif

#define LARGE_NODE_CLASS  (NUM_NODE_CLASSES - 1)
#define CUSTOM_NODE_CLASS (NUM_NODE_CLASSES - 2)
#define NUM_SMALL_NODE_CLASSES (NUM_NODE_CLASSES - 2)

/* the number of VECREC's in nodes of the small node classes */
static int NodeClassSize[NUM_SMALL_NODE_CLASSES] = { 0, 1, 2, 4, 8, 16 };

#define NODE_CLASS(s) ((s)->sxpinfo.gccls)
#define SET_NODE_CLASS(s,v) (((s)->sxpinfo.gccls) = (v))


/* Node Generations. */

#define NUM_OLD_GENERATIONS 2

/* sxpinfo allocates one bit for the old generation count, so only 1
   or 2 is allowed */
#if NUM_OLD_GENERATIONS > 2 || NUM_OLD_GENERATIONS < 1
# error number of old generations must be 1 or 2
#endif

#define NODE_GENERATION(s) ((s)->sxpinfo.gcgen)
#define SET_NODE_GENERATION(s,g) ((s)->sxpinfo.gcgen=(g))

#define NODE_GEN_IS_YOUNGER(s,g) \
  (! NODE_IS_MARKED(s) || NODE_GENERATION(s) < (g))
#define NODE_IS_OLDER(x, y) \
    (NODE_IS_MARKED(x) && (y) && \
   (! NODE_IS_MARKED(y) || NODE_GENERATION(x) > NODE_GENERATION(y)))

static int num_old_gens_to_collect = 0;
static int gen_gc_counts[NUM_OLD_GENERATIONS + 1];
static int collect_counts[NUM_OLD_GENERATIONS];


/* Node Pages.  Non-vector nodes and small vector nodes are allocated
   from fixed size pages.  The pages for each node class are kept in a
   linked list. */

typedef struct R_mtl_heap_state_ R_mtl_heap_state;

#define R_MTL_PAGE_MAGIC UINT64_C(0x525F4D544C504147) /* "R_MTLPAG" */

typedef struct PAGE_HEADER {
  uint64_t magic;
  struct PAGE_HEADER *next;
  R_mtl_heap_state *owner;
  /* Ensure PAGE_DATA(p) is suitably aligned for SEXPREC writes.
     On aarch64, the compiler can use paired stores (stp) for adjacent
     pointer fields in SEXPREC, which faults on misaligned addresses. */
  uintptr_t pad;
} PAGE_HEADER;
_Static_assert(sizeof(PAGE_HEADER) % 16 == 0, "PAGE_HEADER size must preserve 16-byte alignment");

#if ( SIZEOF_SIZE_T > 4 )
# define BASE_PAGE_SIZE 8000
#else
# define BASE_PAGE_SIZE 2000
#endif
#define R_PAGE_SIZE \
  (((BASE_PAGE_SIZE - sizeof(PAGE_HEADER)) / sizeof(SEXPREC)) \
   * sizeof(SEXPREC) \
   + sizeof(PAGE_HEADER))
#define NODE_SIZE(c) \
  ((c) == 0 ? sizeof(SEXPREC) : \
   sizeof(SEXPREC_ALIGN) + NodeClassSize[c] * sizeof(VECREC))

#define PAGE_DATA(p) ((void *) (p + 1))
/* Per-interpreter heap/GC state.
 *
 * The long-term goal is fully independent heaps+GCs for subinterpreters.
 * For now we at least parameterize the core heap bookkeeping by the current
 * interpreter state (R_Interpreter->heap).
 */
struct R_mtl_heap_state_ {
    struct {
	SEXP Old[NUM_OLD_GENERATIONS], New;
	SEXP Free;
	SEXPREC OldPeg[NUM_OLD_GENERATIONS], NewPeg;
#ifndef EXPEL_OLD_TO_NEW
	SEXP OldToNew[NUM_OLD_GENERATIONS];
	SEXPREC OldToNewPeg[NUM_OLD_GENERATIONS];
#endif
	int OldCount[NUM_OLD_GENERATIONS], AllocCount, PageCount;
	PAGE_HEADER *pages;
    } GenHeap[NUM_NODE_CLASSES];

    R_size_t NodesInUse;
    R_size_t LargeVallocSize;
    R_size_t SmallVallocSize;

    R_size_t NSize; /* node limit for this heap (cons cells) */
    R_size_t VSize; /* vector heap limit for this heap (in VECRECs) */
    int isWorker;
};

static R_mtl_heap_state R_MainHeapState;

/* Some global intern tables (symbols, CHARSXP cache) must only contain
 * main-heap nodes, since the main GC traces them. For worker threads we
 * temporarily switch the current interpreter heap pointer to the main heap
 * while holding the heap lock, so any nodes allocated for those tables are
 * main-owned. */
attribute_hidden struct R_mtl_heap_state_ *R_mtl_switch_to_main_heap(R_InterpreterState *st)
{
    if (st == NULL)
	return NULL;
    struct R_mtl_heap_state_ *saved = st->heap;
    st->heap = &R_MainHeapState;
    return saved;
}

attribute_hidden void R_mtl_restore_heap(R_InterpreterState *st, struct R_mtl_heap_state_ *saved)
{
    if (st == NULL)
	return;
    st->heap = saved;
}

#define R_HEAP (R_Interpreter->heap)
#define R_GenHeap (R_HEAP->GenHeap)
#define R_NodesInUse (R_HEAP->NodesInUse)
#define R_LargeVallocSize (R_HEAP->LargeVallocSize)
#define R_SmallVallocSize (R_HEAP->SmallVallocSize)
#define R_NSize_heap (R_HEAP->NSize)
#define R_VSize_heap (R_HEAP->VSize)

#define VHEAP_FREE() (R_VSize_heap - R_LargeVallocSize - R_SmallVallocSize)

static size_t R_mtl_pagesize = 0;
static uintptr_t R_mtl_pagesize_mask = 0;

static R_INLINE PAGE_HEADER *mtl_page_header_from_ptr(const void *p)
{
    uintptr_t a = (uintptr_t) p;
    return (PAGE_HEADER *) (a & ~R_mtl_pagesize_mask);
}

/* Owner map for "large" nodes allocated via malloc/custom allocator
 * (i.e. not within a PAGE_HEADER-managed node page). */
#define MTL_LARGE_OWNER_SIZE (1u << 20) /* must be power of two */
static _Atomic(uintptr_t) *mtl_large_owner_keys = NULL;
static _Atomic(uintptr_t) *mtl_large_owner_vals = NULL;
static size_t mtl_large_owner_mask = 0;

static void mtl_large_owner_init(void)
{
    if (mtl_large_owner_keys != NULL)
	return;
    mtl_large_owner_keys =
	(_Atomic(uintptr_t) *) calloc(MTL_LARGE_OWNER_SIZE, sizeof(_Atomic(uintptr_t)));
    mtl_large_owner_vals =
	(_Atomic(uintptr_t) *) calloc(MTL_LARGE_OWNER_SIZE, sizeof(_Atomic(uintptr_t)));
    if (mtl_large_owner_keys == NULL || mtl_large_owner_vals == NULL)
	R_Suicide("InitMemory: failed to allocate mtl large owner map");
    mtl_large_owner_mask = MTL_LARGE_OWNER_SIZE - 1;
}

static R_INLINE size_t mtl_large_owner_hash(uintptr_t k)
{
    /* 64-bit mix; on 32-bit this still does something reasonable. */
    k ^= k >> 33;
    k *= UINT64_C(0xff51afd7ed558ccd);
    k ^= k >> 33;
    return (size_t) k;
}

static void mtl_large_owner_set(SEXP s, R_mtl_heap_state *owner)
{
    if (mtl_large_owner_keys == NULL)
	mtl_large_owner_init();

    uintptr_t k = (uintptr_t) s;
    uintptr_t v = (uintptr_t) owner;
    size_t idx = mtl_large_owner_hash(k) & mtl_large_owner_mask;

    for (size_t probes = 0; probes <= mtl_large_owner_mask; probes++) {
	uintptr_t cur = atomic_load_explicit(&mtl_large_owner_keys[idx], memory_order_acquire);
	if (cur == k || cur == ~k) {
	    atomic_store_explicit(&mtl_large_owner_vals[idx], v, memory_order_release);
	    atomic_store_explicit(&mtl_large_owner_keys[idx], k, memory_order_release);
	    return;
	}
	if (cur == 0) {
	    uintptr_t expected = 0;
	    if (atomic_compare_exchange_weak_explicit(&mtl_large_owner_keys[idx],
						      &expected, ~k,
						      memory_order_acq_rel,
						      memory_order_acquire)) {
		atomic_store_explicit(&mtl_large_owner_vals[idx], v, memory_order_release);
		atomic_store_explicit(&mtl_large_owner_keys[idx], k, memory_order_release);
		return;
	    }
	}
	idx = (idx + 1) & mtl_large_owner_mask;
    }
    R_Suicide("mtl large owner map full");
}

static R_INLINE R_mtl_heap_state *mtl_large_owner_get(SEXP s)
{
    if (mtl_large_owner_keys == NULL)
	return NULL;
    uintptr_t k = (uintptr_t) s;
    size_t idx = mtl_large_owner_hash(k) & mtl_large_owner_mask;
    for (size_t probes = 0; probes <= mtl_large_owner_mask; probes++) {
	uintptr_t cur = atomic_load_explicit(&mtl_large_owner_keys[idx], memory_order_acquire);
	if (cur == 0)
	    return NULL;
	if (cur == k) {
	    uintptr_t v = atomic_load_explicit(&mtl_large_owner_vals[idx], memory_order_acquire);
	    return (R_mtl_heap_state *) v;
	}
	idx = (idx + 1) & mtl_large_owner_mask;
    }
    return NULL;
}

static R_INLINE R_mtl_heap_state *mtl_sexp_owner(SEXP s)
{
    if (s == NULL)
	return NULL;
    PAGE_HEADER *page = mtl_page_header_from_ptr((const void *) s);
    if (page->magic == R_MTL_PAGE_MAGIC)
	return page->owner;
    return mtl_large_owner_get(s);
}

/* The Heap Structure.  Nodes for each class/generation combination
   are arranged in circular doubly-linked lists.  The double linking
   allows nodes to be removed in constant time; this is used by the
   collector to move reachable nodes out of free space and into the
   appropriate generation.  The circularity eliminates the need for
   end checks.  In addition, each link is anchored at an artificial
   node, the Peg SEXPREC's in the structure below, which simplifies
   pointer maintenance.  The circular doubly-linked arrangement is
   taken from Baker's in-place incremental collector design; see
   ftp://ftp.netcom.com/pub/hb/hbaker/NoMotionGC.html or the Jones and
   Lins GC book.  The linked lists are implemented by adding two
   pointer fields to the SEXPREC structure, which increases its size
   from 5 to 7 words. Other approaches are possible but don't seem
   worth pursuing for R.

   There are two options for dealing with old-to-new pointers.  The
   first option is to make sure they never occur by transferring all
   referenced younger objects to the generation of the referrer when a
   reference to a newer object is assigned to an older one.  This is
   enabled by defining EXPEL_OLD_TO_NEW.  The second alternative is to
   keep track of all nodes that may contain references to newer nodes
   and to "age" the nodes they refer to at the beginning of each
   collection.  This is the default.  The first option is simpler in
   some ways, but will create more floating garbage and add a bit to
   the execution time, though the difference is probably marginal on
   both counts.*/
/*#define EXPEL_OLD_TO_NEW*/

#define NEXT_NODE(s) (s)->gengc_next_node
#define PREV_NODE(s) (s)->gengc_prev_node
#define SET_NEXT_NODE(s,t) (NEXT_NODE(s) = (t))
#define SET_PREV_NODE(s,t) (PREV_NODE(s) = (t))


/* Node List Manipulation */

/* unsnap node s from its list */
#define UNSNAP_NODE(s) do { \
  SEXP un__n__ = (s); \
  SEXP next = NEXT_NODE(un__n__); \
  SEXP prev = PREV_NODE(un__n__); \
  SET_NEXT_NODE(prev, next); \
  SET_PREV_NODE(next, prev); \
} while(0)

/* snap in node s before node t */
#define SNAP_NODE(s,t) do { \
  SEXP sn__n__ = (s); \
  SEXP next = (t); \
  SEXP prev = PREV_NODE(next); \
  SET_NEXT_NODE(sn__n__, next); \
  SET_PREV_NODE(next, sn__n__); \
  SET_NEXT_NODE(prev, sn__n__); \
  SET_PREV_NODE(sn__n__, prev); \
} while (0)

/* move all nodes on from_peg to to_peg */
#define BULK_MOVE(from_peg,to_peg) do { \
  SEXP __from__ = (from_peg); \
  SEXP __to__ = (to_peg); \
  SEXP first_old = NEXT_NODE(__from__); \
  SEXP last_old = PREV_NODE(__from__); \
  SEXP first_new = NEXT_NODE(__to__); \
  SET_PREV_NODE(first_old, __to__); \
  SET_NEXT_NODE(__to__, first_old); \
  SET_PREV_NODE(first_new, last_old); \
  SET_NEXT_NODE(last_old, first_new); \
  SET_NEXT_NODE(__from__, __from__); \
  SET_PREV_NODE(__from__, __from__); \
} while (0);


/* Processing Node Children */

/* This macro calls dc__action__ for each child of __n__, passing
   dc__extra__ as a second argument for each call. */
/* When the CHARSXP hash chains are maintained through the ATTRIB
   field it is important that we NOT trace those fields otherwise too
   many CHARSXPs will be kept alive artificially. As a safety we don't
   ignore all non-NULL ATTRIB values for CHARSXPs but only those that
   are themselves CHARSXPs, which is what they will be if they are
   part of a hash chain.  Theoretically, for CHARSXPs the ATTRIB field
   should always be either R_NilValue or a CHARSXP. */
#ifdef PROTECTCHECK
# define HAS_GENUINE_ATTRIB(x) \
    (TYPEOF(x) != FREESXP && ATTRIB(x) != R_NilValue && \
     (TYPEOF(x) != CHARSXP || TYPEOF(ATTRIB(x)) != CHARSXP))
#else
# define HAS_GENUINE_ATTRIB(x) \
    (ATTRIB(x) != R_NilValue && \
     (TYPEOF(x) != CHARSXP || TYPEOF(ATTRIB(x)) != CHARSXP))
#endif

#ifdef PROTECTCHECK
#define FREE_FORWARD_CASE case FREESXP: if (gc_inhibit_release) break;
#else
#define FREE_FORWARD_CASE
#endif
/*** assume for now all ALTREP nodes are based on CONS nodes */
#define DO_CHILDREN4(__n__,dc__action__,dc__str__action__,dc__extra__) do { \
  if (HAS_GENUINE_ATTRIB(__n__)) \
    dc__action__(ATTRIB(__n__), dc__extra__); \
  if (ALTREP(__n__)) {					\
	  dc__action__(TAG(__n__), dc__extra__);	\
	  dc__action__(CAR(__n__), dc__extra__);	\
	  dc__action__(CDR(__n__), dc__extra__);	\
      }							\
  else \
  switch (TYPEOF(__n__)) { \
  case NILSXP: \
  case BUILTINSXP: \
  case SPECIALSXP: \
  case CHARSXP: \
  case LGLSXP: \
  case INTSXP: \
  case REALSXP: \
  case CPLXSXP: \
  case WEAKREFSXP: \
  case RAWSXP: \
  case OBJSXP: \
    break; \
  case STRSXP: \
    { \
      R_xlen_t i; \
      for (i = 0; i < XLENGTH(__n__); i++) \
	dc__str__action__(VECTOR_ELT_0(__n__, i), dc__extra__); \
    } \
    break; \
  case EXPRSXP: \
  case VECSXP: \
    { \
      R_xlen_t i; \
      for (i = 0; i < XLENGTH(__n__); i++) \
	dc__action__(VECTOR_ELT_0(__n__, i), dc__extra__); \
    } \
    break; \
  case ENVSXP: \
    dc__action__(FRAME(__n__), dc__extra__); \
    dc__action__(ENCLOS(__n__), dc__extra__); \
    dc__action__(HASHTAB(__n__), dc__extra__); \
    break; \
  case LISTSXP: \
  case PROMSXP: \
    dc__action__(TAG(__n__), dc__extra__); \
    if (BOXED_BINDING_CELLS || BNDCELL_TAG(__n__) == 0) \
      dc__action__(CAR0(__n__), dc__extra__); \
    dc__action__(CDR(__n__), dc__extra__); \
    break; \
  case CLOSXP: \
  case LANGSXP: \
  case DOTSXP: \
  case SYMSXP: \
  case BCODESXP: \
    dc__action__(TAG(__n__), dc__extra__); \
    dc__action__(CAR0(__n__), dc__extra__); \
    dc__action__(CDR(__n__), dc__extra__); \
    break; \
  case EXTPTRSXP: \
    dc__action__(EXTPTR_PROT(__n__), dc__extra__); \
    dc__action__(EXTPTR_TAG(__n__), dc__extra__); \
    break; \
  FREE_FORWARD_CASE \
  default: \
    register_bad_sexp_type(__n__, __LINE__);		\
  } \
} while(0)

#define DO_CHILDREN(__n__,dc__action__,dc__extra__) \
    DO_CHILDREN4(__n__,dc__action__,dc__action__,dc__extra__)


/* Forwarding Nodes.  These macros mark nodes or children of nodes and
   place them on the forwarding list.  The forwarding list is assumed
   to be in a local variable of the caller named named
   forwarded_nodes. */

static R_INLINE int mtl_gc_owns_node(SEXP n)
{
    /* In the multi-heap (mtlapply) experiment, a thread can hold pointers to
       nodes from other heaps (e.g. a worker referencing main-heap nodes read-
       only, or temporarily switching to the main heap to intern CHARSXPs).

       The collector must never treat a node from another heap as collectible
       state for the current heap.

       For the main heap (isWorker==0), nodes not managed by the page-owner
       mechanism (owner==NULL) are assumed to be main-heap owned.

       For worker heaps, owner==NULL must be treated as "not owned": workers
       may reference main-heap nodes that do not have an owner mapping. */
    R_mtl_heap_state *owner = mtl_sexp_owner(n);
    if (R_HEAP->isWorker)
	return owner == R_HEAP;
    return owner == NULL || owner == R_HEAP;
}

#define MTL_GC_OWNS_NODE(n) (mtl_gc_owns_node(n))

#define MARK_AND_UNSNAP_NODE(s) do {		\
	SEXP mu__n__ = (s);			\
	CHECK_FOR_FREE_NODE(mu__n__);		\
	MARK_NODE(mu__n__);			\
	UNSNAP_NODE(mu__n__);			\
    } while (0)

#define FORWARD_NODE(s) do { \
  SEXP fn__n__ = (s); \
  if (fn__n__ && MTL_GC_OWNS_NODE(fn__n__) && ! NODE_IS_MARKED(fn__n__)) { \
    MARK_AND_UNSNAP_NODE(fn__n__); \
    SET_NEXT_NODE(fn__n__, forwarded_nodes); \
    forwarded_nodes = fn__n__; \
  } \
} while (0)

/* When scanning roots on the main heap, worker interpreter stacks may still
   contain worker-owned nodes (e.g. regular evaluation temporaries) even if a
   worker temporarily switches its heap pointer to the main heap in order to
   intern into global tables (CHARSXP cache, symbol table).

   Forwarding a node from the wrong heap is unsafe. Use this wrapper for
   interpreter-local stacks/contexts so we only forward nodes owned by the
   heap currently being collected. */
#define FORWARD_NODE_IN_CURRENT_HEAP(s) do {			\
    SEXP fnh__n__ = (s);					\
    if (fnh__n__) {						\
	R_mtl_heap_state *fnh__owner__ = mtl_sexp_owner(fnh__n__);	\
	if (fnh__owner__ == NULL || fnh__owner__ == R_HEAP)	\
	    FORWARD_NODE(fnh__n__);				\
    }								\
} while (0)

#define PROCESS_ONE_NODE(s) do {				\
		SEXP pn__n__ = (s);					\
		int __cls__ = NODE_CLASS(pn__n__);			\
		int __gen__ = NODE_GENERATION(pn__n__);			\
		SNAP_NODE(pn__n__, R_GenHeap[__cls__].Old[__gen__]);	\
	R_GenHeap[__cls__].OldCount[__gen__]++;			\
    } while (0)

/* avoid pushing on the forwarding stack when possible */
#define FORWARD_AND_PROCESS_ONE_NODE(s, tp) do {	\
	SEXP fpn__n__ = (s);				\
	int __tp__ = (tp);				\
	if (fpn__n__ && MTL_GC_OWNS_NODE(fpn__n__) && ! NODE_IS_MARKED(fpn__n__)) { \
	    if (TYPEOF(fpn__n__) == __tp__ &&		\
		! HAS_GENUINE_ATTRIB(fpn__n__)) {	\
		MARK_AND_UNSNAP_NODE(fpn__n__);		\
		PROCESS_ONE_NODE(fpn__n__);		\
	    }						\
	    else FORWARD_NODE(fpn__n__);		\
	}						\
    } while (0)

#define PROCESS_CHARSXP(__n__) FORWARD_AND_PROCESS_ONE_NODE(__n__, CHARSXP)
#define FC_PROCESS_CHARSXP(__n__,__dummy__) PROCESS_CHARSXP(__n__)
#define FC_FORWARD_NODE(__n__,__dummy__) FORWARD_NODE(__n__)
#define FORWARD_CHILDREN(__n__) \
    DO_CHILDREN4(__n__, FC_FORWARD_NODE, FC_PROCESS_CHARSXP, 0)

/* This macro should help localize where a FREESXP node is encountered
   in the GC */
#ifdef PROTECTCHECK
#define CHECK_FOR_FREE_NODE(s) { \
    SEXP cf__n__ = (s); \
    if (TYPEOF(cf__n__) == FREESXP && ! gc_inhibit_release) \
	register_bad_sexp_type(cf__n__, __LINE__); \
}
#else
#define CHECK_FOR_FREE_NODE(s)
#endif


/* Node Allocation. */

static void GetNewPage(int node_class);
static void mtl_worker_gc(R_size_t size_needed);

#ifdef HAVE_PTHREAD
static R_INLINE SEXP mtl_genheap_free_load(int c)
{
    if (__builtin_expect(!R_mtl_threading_active || R_HEAP->isWorker, 1))
	return R_GenHeap[c].Free;
    return __atomic_load_n(&R_GenHeap[c].Free, __ATOMIC_ACQUIRE);
}

static R_INLINE void mtl_genheap_free_store(int c, SEXP v)
{
    if (__builtin_expect(!R_mtl_threading_active || R_HEAP->isWorker, 1))
	R_GenHeap[c].Free = v;
    else
	__atomic_store_n(&R_GenHeap[c].Free, v, __ATOMIC_RELEASE);
}

# define GENHEAP_FREE_LOAD(c) mtl_genheap_free_load((c))
# define GENHEAP_FREE_STORE(c, v) mtl_genheap_free_store((c), (v))

static R_INLINE R_size_t mtl_r_size_t_load(R_size_t *p)
{
    if (__builtin_expect(!R_mtl_threading_active || R_HEAP->isWorker, 1))
	return *p;
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

static R_INLINE void mtl_r_size_t_store(R_size_t *p, R_size_t v)
{
    if (__builtin_expect(!R_mtl_threading_active || R_HEAP->isWorker, 1))
	*p = v;
    else
	__atomic_store_n(p, v, __ATOMIC_RELAXED);
}

static R_INLINE void mtl_r_size_t_add(R_size_t *p, R_size_t n)
{
    if (__builtin_expect(!R_mtl_threading_active || R_HEAP->isWorker, 1))
	*p += n;
    else
	__atomic_fetch_add(p, n, __ATOMIC_RELAXED);
}

# define NODES_IN_USE_LOAD() mtl_r_size_t_load(&R_NodesInUse)
# define NODES_IN_USE_STORE(v) mtl_r_size_t_store(&R_NodesInUse, (v))
# define NODES_IN_USE_ADD(n) mtl_r_size_t_add(&R_NodesInUse, (R_size_t) (n))

# define SMALL_VALLOC_LOAD() mtl_r_size_t_load(&R_SmallVallocSize)
# define LARGE_VALLOC_LOAD() mtl_r_size_t_load(&R_LargeVallocSize)
# define SMALL_VALLOC_ADD(n) mtl_r_size_t_add(&R_SmallVallocSize, (R_size_t) (n))
# define LARGE_VALLOC_ADD(n) mtl_r_size_t_add(&R_LargeVallocSize, (R_size_t) (n))
# define SMALL_VALLOC_STORE(v) mtl_r_size_t_store(&R_SmallVallocSize, (v))
# define LARGE_VALLOC_STORE(v) mtl_r_size_t_store(&R_LargeVallocSize, (v))
#else
# define GENHEAP_FREE_LOAD(c) (R_GenHeap[c].Free)
# define GENHEAP_FREE_STORE(c, v) (R_GenHeap[c].Free = (v))
# define NODES_IN_USE_LOAD() (R_NodesInUse)
# define NODES_IN_USE_STORE(v) (R_NodesInUse = (v))
# define NODES_IN_USE_ADD(n) (R_NodesInUse += (n))
# define SMALL_VALLOC_LOAD() (R_SmallVallocSize)
# define LARGE_VALLOC_LOAD() (R_LargeVallocSize)
# define SMALL_VALLOC_ADD(n) (R_SmallVallocSize += (n))
# define LARGE_VALLOC_ADD(n) (R_LargeVallocSize += (n))
# define SMALL_VALLOC_STORE(v) (R_SmallVallocSize = (v))
# define LARGE_VALLOC_STORE(v) (R_LargeVallocSize = (v))
#endif

/* The allocation fast paths must use the current heap's thresholds:
   mtlapply() worker threads have independent heaps with their own NSize/VSize. */
#define NO_FREE_NODES() (NODES_IN_USE_LOAD() >= R_NSize_heap)
#define CLASS_NEED_NEW_PAGE(c) (GENHEAP_FREE_LOAD(c) == R_GenHeap[c].New)
#define NEED_NEW_PAGE() CLASS_NEED_NEW_PAGE(0)

static R_INLINE R_size_t VHEAP_FREE_MTL(void)
{
    return R_VSize - LARGE_VALLOC_LOAD() - SMALL_VALLOC_LOAD();
}

static R_INLINE SEXP try_get_free_node(int node_class)
{
#ifdef HAVE_PTHREAD
    if (!R_mtl_threading_active || R_HEAP->isWorker) {
	/* Single-threaded heap fast path: avoid CAS loops and atomic RMW ops. */
	SEXP s = R_GenHeap[node_class].Free;
	if (s == R_GenHeap[node_class].New)
	    return NULL;
	R_GenHeap[node_class].Free = NEXT_NODE(s);
	R_NodesInUse += 1;
	return s;
    }

    for (;;) {
	SEXP expected = GENHEAP_FREE_LOAD(node_class);
	if (expected == R_GenHeap[node_class].New)
	    return NULL;
	SEXP desired = NEXT_NODE(expected);
	if (__atomic_compare_exchange_n(&R_GenHeap[node_class].Free,
				       &expected, desired,
				       1,
				       __ATOMIC_ACQ_REL,
				       __ATOMIC_ACQUIRE)) {
	    NODES_IN_USE_ADD(1);
	    return expected;
	}
    }
#else
    SEXP s = GENHEAP_FREE_LOAD(node_class);
    if (s == R_GenHeap[node_class].New)
	return NULL;
    GENHEAP_FREE_STORE(node_class, NEXT_NODE(s));
    NODES_IN_USE_ADD(1);
    return s;
#endif
}

static R_INLINE void mtl_gc(R_size_t size_needed)
{
    heap_alloc_suspend();
    if (R_HEAP->isWorker) {
	mtl_worker_gc(size_needed);
	/* Worker heaps do not currently run the main AdjustHeapSize() logic.
	   Ensure the per-worker GC trigger sizes can accommodate the live set. */
	{
	    R_size_t ninuse = NODES_IN_USE_LOAD();
	    if (ninuse >= R_NSize_heap) {
		/* Keep some headroom to reduce thrash. */
		R_size_t grow = (R_size_t) (R_NSize_heap * R_MinFreeFrac);
		if (grow < 1000) grow = 1000;
		R_size_t target = ninuse + grow;
		if (target < ninuse + 1) target = ninuse + 1; /* overflow paranoia */
		if (R_MaxNSize < R_SIZE_T_MAX && target > R_MaxNSize)
		    target = R_MaxNSize;
		R_NSize_heap = target;
	    }
	    if (size_needed > 0) {
		R_size_t used = SMALL_VALLOC_LOAD() + LARGE_VALLOC_LOAD();
		R_size_t minfree = (R_size_t) (R_VSize_heap * R_MinFreeFrac);
		R_size_t target = used + size_needed + minfree;
		if (target < used + size_needed) target = used + size_needed; /* overflow paranoia */
		if (R_MaxVSize < R_SIZE_T_MAX && target > R_MaxVSize)
		    target = R_MaxVSize;
		if (R_VSize_heap < target)
		    R_VSize_heap = target;
	    }
	}
    } else {
	if (R_mtl_threading_active) {
	    R_mtl_heap_lock();
	    R_gc_internal(size_needed);
	    R_mtl_heap_unlock();
	} else {
	    R_gc_internal(size_needed);
	}
    }
    heap_alloc_resume();
}

static R_INLINE void mtl_get_new_page(int node_class)
{
    heap_alloc_suspend();
    if (R_HEAP->isWorker) {
	if (CLASS_NEED_NEW_PAGE(node_class))
	    GetNewPage(node_class);
    } else {
	if (R_mtl_threading_active) {
	    R_mtl_heap_lock();
	    if (CLASS_NEED_NEW_PAGE(node_class))
		GetNewPage(node_class);
	    R_mtl_heap_unlock();
	} else {
	    if (CLASS_NEED_NEW_PAGE(node_class))
		GetNewPage(node_class);
	}
    }
    heap_alloc_resume();
}


/* Debugging Routines. */

#ifdef DEBUG_GC
static void CheckNodeGeneration(SEXP x, int g)
{
    if (x && NODE_GENERATION(x) < g) {
	gc_error("untraced old-to-new reference\n");
    }
}

static void DEBUG_CHECK_NODE_COUNTS(char *where)
{
    int i, OldCount, NewCount, OldToNewCount, gen;
    SEXP s;

    REprintf("Node counts %s:\n", where);
    for (i = 0; i < NUM_NODE_CLASSES; i++) {
	for (s = NEXT_NODE(R_GenHeap[i].New), NewCount = 0;
	     s != R_GenHeap[i].New;
	     s = NEXT_NODE(s)) {
	    NewCount++;
	    if (i != NODE_CLASS(s))
		gc_error("Inconsistent class assignment for node!\n");
	}
	for (gen = 0, OldCount = 0, OldToNewCount = 0;
	     gen < NUM_OLD_GENERATIONS;
	     gen++) {
	    for (s = NEXT_NODE(R_GenHeap[i].Old[gen]);
		 s != R_GenHeap[i].Old[gen];
		 s = NEXT_NODE(s)) {
		OldCount++;
		if (i != NODE_CLASS(s))
		    gc_error("Inconsistent class assignment for node!\n");
		if (gen != NODE_GENERATION(s))
		    gc_error("Inconsistent node generation\n");
		DO_CHILDREN(s, CheckNodeGeneration, gen);
	    }
	    for (s = NEXT_NODE(R_GenHeap[i].OldToNew[gen]);
		 s != R_GenHeap[i].OldToNew[gen];
		 s = NEXT_NODE(s)) {
		OldToNewCount++;
		if (i != NODE_CLASS(s))
		    gc_error("Inconsistent class assignment for node!\n");
		if (gen != NODE_GENERATION(s))
		    gc_error("Inconsistent node generation\n");
	    }
	}
	REprintf("Class: %d, New = %d, Old = %d, OldToNew = %d, Total = %d\n",
		 i,
		 NewCount, OldCount, OldToNewCount,
		 NewCount + OldCount + OldToNewCount);
    }
}

static void DEBUG_GC_SUMMARY(int full_gc)
{
    int i, gen, OldCount;
    REprintf("\n%s, VSize = %lu", full_gc ? "Full" : "Minor",
	     R_SmallVallocSize + R_LargeVallocSize);
    for (i = 1; i < NUM_NODE_CLASSES; i++) {
	for (gen = 0, OldCount = 0; gen < NUM_OLD_GENERATIONS; gen++)
	    OldCount += R_GenHeap[i].OldCount[gen];
	REprintf(", class %d: %d", i, OldCount);
    }
}
#else
#define DEBUG_CHECK_NODE_COUNTS(s)
#define DEBUG_GC_SUMMARY(x)
#endif /* DEBUG_GC */

#ifdef DEBUG_ADJUST_HEAP
static void DEBUG_ADJUST_HEAP_PRINT(double node_occup, double vect_occup)
{
    int i;
    R_size_t alloc;
    REprintf("Node occupancy: %.0f%%\nVector occupancy: %.0f%%\n",
	     100.0 * node_occup, 100.0 * vect_occup);
    alloc = R_LargeVallocSize +
	sizeof(SEXPREC_ALIGN) * R_GenHeap[LARGE_NODE_CLASS].AllocCount;
    for (i = 0; i < NUM_SMALL_NODE_CLASSES; i++)
	alloc += R_PAGE_SIZE * R_GenHeap[i].PageCount;
    REprintf("Total allocation: %lu\n", alloc);
    REprintf("Ncells %lu\nVcells %lu\n", R_NSize, R_VSize);
}
#else
#define DEBUG_ADJUST_HEAP_PRINT(node_occup, vect_occup)
#endif /* DEBUG_ADJUST_HEAP */

#ifdef DEBUG_RELEASE_MEM
static void DEBUG_RELEASE_PRINT(int rel_pages, int maxrel_pages, int i)
{
    if (maxrel_pages > 0) {
	int gen, n;
	REprintf("Class: %d, pages = %d, maxrel = %d, released = %d\n", i,
		 R_GenHeap[i].PageCount, maxrel_pages, rel_pages);
	for (gen = 0, n = 0; gen < NUM_OLD_GENERATIONS; gen++)
	    n += R_GenHeap[i].OldCount[gen];
	REprintf("Allocated = %d, in use = %d\n", R_GenHeap[i].AllocCount, n);
    }
}
#else
#define DEBUG_RELEASE_PRINT(rel_pages, maxrel_pages, i)
#endif /* DEBUG_RELEASE_MEM */

#ifdef COMPUTE_REFCNT_VALUES
#define INIT_REFCNT(x) do {			\
	SEXP __x__ = (x);			\
	SET_REFCNT(__x__, 0);			\
	SET_TRACKREFS(__x__, TRUE);		\
    } while (0)
#else
#define INIT_REFCNT(x) do {} while (0)
#endif

/* Page Allocation and Release. */

static void GetNewPage(int node_class)
{
    SEXP s, base;
    char *data;
    PAGE_HEADER *page;
    int node_size, page_count, i;  // FIXME: longer type?

    node_size = NODE_SIZE(node_class);
    page_count = (R_PAGE_SIZE - sizeof(PAGE_HEADER)) / node_size;

    void *pmem = NULL;
    if (posix_memalign(&pmem, R_mtl_pagesize, (size_t) R_PAGE_SIZE) != 0)
	pmem = NULL;
    page = (PAGE_HEADER *) pmem;
    if (page == NULL) {
	R_gc_no_finalizers(0);
	pmem = NULL;
	if (posix_memalign(&pmem, R_mtl_pagesize, (size_t) R_PAGE_SIZE) != 0)
	    pmem = NULL;
	page = (PAGE_HEADER *) pmem;
	if (page == NULL)
	    mem_err_malloc((R_size_t) R_PAGE_SIZE);
    }
#ifdef R_MEMORY_PROFILING
    R_ReportNewPage();
#endif
    page->magic = R_MTL_PAGE_MAGIC;
    page->owner = R_HEAP;
    page->next = R_GenHeap[node_class].pages;
    R_GenHeap[node_class].pages = page;
    R_GenHeap[node_class].PageCount++;

    data = PAGE_DATA(page);
    base = R_GenHeap[node_class].New;
    for (i = 0; i < page_count; i++, data += node_size) {
	s = (SEXP) data;
	R_GenHeap[node_class].AllocCount++;
	SNAP_NODE(s, base);
#if  VALGRIND_LEVEL > 1
	if (NodeClassSize[node_class] > 0)
	    VALGRIND_MAKE_MEM_NOACCESS(STDVEC_DATAPTR(s), NodeClassSize[node_class]*sizeof(VECREC));
#endif
	s->sxpinfo = UnmarkedNodeTemplate.sxpinfo;
	INIT_REFCNT(s);
	SET_NODE_CLASS(s, node_class);
#ifdef PROTECTCHECK
	SET_TYPEOF(s, NEWSXP);
#endif
	base = s;
	R_GenHeap[node_class].Free = s;
    }
}

static void ReleasePage(PAGE_HEADER *page, int node_class)
{
    SEXP s;
    char *data;
    int node_size, page_count, i;

    node_size = NODE_SIZE(node_class);
    page_count = (R_PAGE_SIZE - sizeof(PAGE_HEADER)) / node_size;
    data = PAGE_DATA(page);

    for (i = 0; i < page_count; i++, data += node_size) {
	s = (SEXP) data;
	UNSNAP_NODE(s);
	R_GenHeap[node_class].AllocCount--;
    }
    R_GenHeap[node_class].PageCount--;
    free(page);
}

static void TryToReleasePages(void)
{
    SEXP s;
    int i;
    static int release_count = 0;

    if (release_count == 0) {
	release_count = R_PageReleaseFreq;
	for (i = 0; i < NUM_SMALL_NODE_CLASSES; i++) {
	    PAGE_HEADER *page, *last, *next;
	    int node_size = NODE_SIZE(i);
	    int page_count = (R_PAGE_SIZE - sizeof(PAGE_HEADER)) / node_size;
	    int maxrel, maxrel_pages, rel_pages, gen;

	    maxrel = R_GenHeap[i].AllocCount;
	    for (gen = 0; gen < NUM_OLD_GENERATIONS; gen++)
		maxrel -= (int)((1.0 + R_MaxKeepFrac) *
				R_GenHeap[i].OldCount[gen]);
	    maxrel_pages = maxrel > 0 ? maxrel / page_count : 0;

	    /* all nodes in New space should be both free and unmarked */
	    for (page = R_GenHeap[i].pages, rel_pages = 0, last = NULL;
		 rel_pages < maxrel_pages && page != NULL;) {
		int j, in_use;
		char *data = PAGE_DATA(page);

		next = page->next;
		for (in_use = 0, j = 0; j < page_count;
		     j++, data += node_size) {
		    s = (SEXP) data;
		    if (NODE_IS_MARKED(s)) {
			in_use = 1;
			break;
		    }
		}
		if (! in_use) {
		    ReleasePage(page, i);
		    if (last == NULL)
			R_GenHeap[i].pages = next;
		    else
			last->next = next;
		    rel_pages++;
		}
		else last = page;
		page = next;
	    }
	    DEBUG_RELEASE_PRINT(rel_pages, maxrel_pages, i);
	    R_GenHeap[i].Free = NEXT_NODE(R_GenHeap[i].New);
	}
    }
    else release_count--;
}

/* compute size in VEC units so result will fit in LENGTH field for FREESXPs */
static R_INLINE R_size_t getVecSizeInVEC(SEXP s)
{
    if (IS_GROWABLE(s))
	SET_STDVEC_LENGTH(s, XTRUELENGTH(s));

    R_size_t size;
    switch (TYPEOF(s)) {	/* get size in bytes */
    case CHARSXP:
	size = XLENGTH(s) + 1;
	break;
    case RAWSXP:
	size = XLENGTH(s);
	break;
    case LGLSXP:
    case INTSXP:
	size = XLENGTH(s) * sizeof(int);
	break;
    case REALSXP:
	size = XLENGTH(s) * sizeof(double);
	break;
    case CPLXSXP:
	size = XLENGTH(s) * sizeof(Rcomplex);
	break;
    case STRSXP:
    case EXPRSXP:
    case VECSXP:
	size = XLENGTH(s) * sizeof(SEXP);
	break;
    default:
	register_bad_sexp_type(s, __LINE__);
	size = 0;
    }
    return BYTE2VEC(size);
}

static void custom_node_free(void *ptr);

static void ReleaseLargeFreeVectors(void)
{
    for (int node_class = CUSTOM_NODE_CLASS; node_class <= LARGE_NODE_CLASS; node_class++) {
	SEXP s = NEXT_NODE(R_GenHeap[node_class].New);
	while (s != R_GenHeap[node_class].New) {
	    SEXP next = NEXT_NODE(s);
	    if (1 /* CHAR(s) != NULL*/) {
		/* Consecutive representation of large vectors with header followed
		   by data. An alternative representation (currently not implemented)
		   could have CHAR(s) == NULL. */
		R_size_t size;
#ifdef PROTECTCHECK
		if (TYPEOF(s) == FREESXP)
		    size = STDVEC_LENGTH(s);
		else
		    /* should not get here -- arrange for a warning/error? */
		    size = getVecSizeInVEC(s);
#else
		size = getVecSizeInVEC(s);
#endif
		UNSNAP_NODE(s);
		R_GenHeap[node_class].AllocCount--;
		mtl_large_owner_set(s, NULL);
		if (node_class == LARGE_NODE_CLASS) {
		    R_LargeVallocSize -= size;
		    free(s);
		} else {
		    custom_node_free(s);
		}
	    }
	    s = next;
	}
    }
}

/* Heap Size Adjustment. */

static void AdjustHeapSize(R_size_t size_needed)
{
    R_size_t R_MinNFree = (R_size_t)(orig_R_NSize * R_MinFreeFrac);
    R_size_t R_MinVFree = (R_size_t)(orig_R_VSize * R_MinFreeFrac);
    R_size_t NNeeded = R_NodesInUse + R_MinNFree;
    R_size_t VNeeded = R_SmallVallocSize + R_LargeVallocSize
	+ size_needed + R_MinVFree;
    double node_occup = ((double) NNeeded) / R_NSize;
    double vect_occup =	((double) VNeeded) / R_VSize;

    if (node_occup > R_NGrowFrac) {
	R_size_t change =
	    (R_size_t)(R_NGrowIncrMin + R_NGrowIncrFrac * R_NSize);

	/* for early adjustments grow more aggressively */
	static R_size_t last_in_use = 0;
	static int adjust_count = 1;
	if (adjust_count < 50) {
	    adjust_count++;

	    /* estimate next in-use count by assuming linear growth */
	    R_size_t next_in_use = R_NodesInUse + (R_NodesInUse - last_in_use);
	    last_in_use = R_NodesInUse;

	    /* try to achieve and occupancy rate of R_NGrowFrac */
	    R_size_t next_nsize = (R_size_t) (next_in_use / R_NGrowFrac);
	    if (next_nsize > R_NSize + change)
		change = next_nsize - R_NSize;
	}

	if (R_MaxNSize >= R_NSize + change)
	    R_NSize += change;
    }
    else if (node_occup < R_NShrinkFrac) {
	R_NSize -= (R_size_t)(R_NShrinkIncrMin + R_NShrinkIncrFrac * R_NSize);
	if (R_NSize < NNeeded)
	    R_NSize = (NNeeded < R_MaxNSize) ? NNeeded: R_MaxNSize;
	if (R_NSize < orig_R_NSize)
	    R_NSize = orig_R_NSize;
    }

    if (vect_occup > 1.0 && VNeeded < R_MaxVSize)
	R_VSize = VNeeded;
    if (vect_occup > R_VGrowFrac) {
	R_size_t change = (R_size_t)(R_VGrowIncrMin + R_VGrowIncrFrac * R_VSize);
	if (R_MaxVSize - R_VSize >= change)
	    R_VSize += change;
    }
    else if (vect_occup < R_VShrinkFrac) {
	R_VSize -= (R_size_t)(R_VShrinkIncrMin + R_VShrinkIncrFrac * R_VSize);
	if (R_VSize < VNeeded)
	    R_VSize = VNeeded;
	if (R_VSize < orig_R_VSize)
	    R_VSize = orig_R_VSize;
    }

    DEBUG_ADJUST_HEAP_PRINT(node_occup, vect_occup);
}


/* Managing Old-to-New References. */

#define AGE_NODE(s,g) do { \
  SEXP an__n__ = (s); \
  int an__g__ = (g); \
  if (an__n__ && NODE_GEN_IS_YOUNGER(an__n__, an__g__)) { \
    if (NODE_IS_MARKED(an__n__)) \
       R_GenHeap[NODE_CLASS(an__n__)].OldCount[NODE_GENERATION(an__n__)]--; \
    else \
      MARK_NODE(an__n__); \
    SET_NODE_GENERATION(an__n__, an__g__); \
    UNSNAP_NODE(an__n__); \
    SET_NEXT_NODE(an__n__, forwarded_nodes); \
    forwarded_nodes = an__n__; \
  } \
} while (0)

static void AgeNodeAndChildren(SEXP s, int gen)
{
    SEXP forwarded_nodes = NULL;
    AGE_NODE(s, gen);
    while (forwarded_nodes != NULL) {
	s = forwarded_nodes;
	forwarded_nodes = NEXT_NODE(forwarded_nodes);
	if (NODE_GENERATION(s) != gen)
	    gc_error("****snapping into wrong generation\n");
	SNAP_NODE(s, R_GenHeap[NODE_CLASS(s)].Old[gen]);
	R_GenHeap[NODE_CLASS(s)].OldCount[gen]++;
	DO_CHILDREN(s, AGE_NODE, gen);
    }
}

static void old_to_new(SEXP x, SEXP y)
{
#ifdef EXPEL_OLD_TO_NEW
    AgeNodeAndChildren(y, NODE_GENERATION(x));
#else
    UNSNAP_NODE(x);
    SNAP_NODE(x, R_GenHeap[NODE_CLASS(x)].OldToNew[NODE_GENERATION(x)]);
#endif
}

#ifdef COMPUTE_REFCNT_VALUES
#define FIX_REFCNT_EX(x, old, new, chkpnd) do {				\
	SEXP __x__ = (x);						\
	if (TRACKREFS(__x__)) {						\
	    SEXP __old__ = (old);					\
	    SEXP __new__ = (new);					\
	    if (__old__ != __new__) {					\
		if (__old__) {						\
		    if ((chkpnd) && ASSIGNMENT_PENDING(__x__))		\
			SET_ASSIGNMENT_PENDING(__x__, FALSE);		\
		    else						\
			DECREMENT_REFCNT(__old__);			\
		}							\
		if (__new__) INCREMENT_REFCNT(__new__);			\
	    }								\
	}								\
    } while (0)
#define FIX_REFCNT(x, old, new) FIX_REFCNT_EX(x, old, new, FALSE)
#define FIX_BINDING_REFCNT(x, old, new)		\
    FIX_REFCNT_EX(x, old, new, TRUE)
#else
#define FIX_REFCNT(x, old, new) do {} while (0)
#define FIX_BINDING_REFCNT(x, old, new) do {\
	SEXP __x__ = (x);						\
	SEXP __old__ = (old);						\
	SEXP __new__ = (new);						\
	if (ASSIGNMENT_PENDING(__x__) && __old__ &&			\
	    __old__ != __new__)						\
	    SET_ASSIGNMENT_PENDING(__x__, FALSE);			\
    } while (0)
#endif

#define CHECK_OLD_TO_NEW(x,y) do { \
  if (NODE_IS_OLDER(CHK(x), CHK(y))) old_to_new(x,y);  } while (0)


/* Node Sorting.  SortNodes attempts to improve locality of reference
   by rearranging the free list to place nodes on the same place page
   together and order nodes within pages.  This involves a sweep of the
   heap, so it should not be done too often, but doing it at least
   occasionally does seem essential.  Sorting on each full colllection is
   probably sufficient.
*/

#define SORT_NODES
#ifdef SORT_NODES
static void SortNodes(void)
{
    SEXP s;
    int i;

    for (i = 0; i < NUM_SMALL_NODE_CLASSES; i++) {
	PAGE_HEADER *page;
	int node_size = NODE_SIZE(i);
	int page_count = (R_PAGE_SIZE - sizeof(PAGE_HEADER)) / node_size;

	SET_NEXT_NODE(R_GenHeap[i].New, R_GenHeap[i].New);
	SET_PREV_NODE(R_GenHeap[i].New, R_GenHeap[i].New);
	for (page = R_GenHeap[i].pages; page != NULL; page = page->next) {
	    int j;
	    char *data = PAGE_DATA(page);

	    for (j = 0; j < page_count; j++, data += node_size) {
		s = (SEXP) data;
		if (! NODE_IS_MARKED(s))
		    SNAP_NODE(s, R_GenHeap[i].New);
	    }
	}
	R_GenHeap[i].Free = NEXT_NODE(R_GenHeap[i].New);
    }
}
#endif


/* Finalization and Weak References */

/* The design of this mechanism is very close to the one described in
   "Stretching the storage manager: weak pointers and stable names in
   Haskell" by Peyton Jones, Marlow, and Elliott (at
   www.research.microsoft.com/Users/simonpj/papers/weak.ps.gz). --LT */

static SEXP R_weak_refs = NULL;

#define READY_TO_FINALIZE_MASK 1

#define SET_READY_TO_FINALIZE(s) ((s)->sxpinfo.gp |= READY_TO_FINALIZE_MASK)
#define CLEAR_READY_TO_FINALIZE(s) ((s)->sxpinfo.gp &= ~READY_TO_FINALIZE_MASK)
#define IS_READY_TO_FINALIZE(s) ((s)->sxpinfo.gp & READY_TO_FINALIZE_MASK)

#define FINALIZE_ON_EXIT_MASK 2

#define SET_FINALIZE_ON_EXIT(s) ((s)->sxpinfo.gp |= FINALIZE_ON_EXIT_MASK)
#define CLEAR_FINALIZE_ON_EXIT(s) ((s)->sxpinfo.gp &= ~FINALIZE_ON_EXIT_MASK)
#define FINALIZE_ON_EXIT(s) ((s)->sxpinfo.gp & FINALIZE_ON_EXIT_MASK)

#define WEAKREF_SIZE 4
#define WEAKREF_KEY(w) VECTOR_ELT_0(w, 0)
#define SET_WEAKREF_KEY(w, k) SET_VECTOR_ELT(w, 0, k)
#define WEAKREF_VALUE(w) VECTOR_ELT_0(w, 1)
#define SET_WEAKREF_VALUE(w, v) SET_VECTOR_ELT(w, 1, v)
#define WEAKREF_FINALIZER(w) VECTOR_ELT_0(w, 2)
#define SET_WEAKREF_FINALIZER(w, f) SET_VECTOR_ELT(w, 2, f)
#define WEAKREF_NEXT(w) VECTOR_ELT_0(w, 3)
#define SET_WEAKREF_NEXT(w, n) SET_VECTOR_ELT(w, 3, n)

static SEXP MakeCFinalizer(R_CFinalizer_t cfun);

static SEXP NewWeakRef(SEXP key, SEXP val, SEXP fin, Rboolean onexit)
{
    SEXP w;

    if (R_Interpreter != NULL && R_Interpreter->isMTLWorker)
	error(_("weak references/finalizers are not supported on mtlapply() worker threads"));

    switch (TYPEOF(key)) {
    case NILSXP:
    case ENVSXP:
    case EXTPTRSXP:
    case BCODESXP:
	break;
    default: error(_("can only weakly reference/finalize reference objects"));
    }

    PROTECT(key);
    PROTECT(val = MAYBE_REFERENCED(val) ? duplicate(val) : val);
    PROTECT(fin);
    w = allocVector(VECSXP, WEAKREF_SIZE);
    SET_TYPEOF(w, WEAKREFSXP);
    if (key != R_NilValue) {
	/* If the key is R_NilValue we don't register the weak reference.
	   This is used in loading saved images. */
	SET_WEAKREF_KEY(w, key);
	SET_WEAKREF_VALUE(w, val);
	SET_WEAKREF_FINALIZER(w, fin);
	SET_WEAKREF_NEXT(w, R_weak_refs);
	CLEAR_READY_TO_FINALIZE(w);
	if (onexit)
	    SET_FINALIZE_ON_EXIT(w);
	else
	    CLEAR_FINALIZE_ON_EXIT(w);
	R_weak_refs = w;
    }
    UNPROTECT(3);
    return w;
}

SEXP R_MakeWeakRef(SEXP key, SEXP val, SEXP fin, Rboolean onexit)
{
    switch (TYPEOF(fin)) {
    case NILSXP:
    case CLOSXP:
    case BUILTINSXP:
    case SPECIALSXP:
	break;
    default: error(_("finalizer must be a function or NULL"));
    }
    return NewWeakRef(key, val, fin, onexit);
}

SEXP R_MakeWeakRefC(SEXP key, SEXP val, R_CFinalizer_t fin, Rboolean onexit)
{
    SEXP w;
    PROTECT(key);
    PROTECT(val);
    w = NewWeakRef(key, val, MakeCFinalizer(fin), onexit);
    UNPROTECT(2);
    return w;
}

static Rboolean R_finalizers_pending = FALSE;
static void CheckFinalizers(void)
{
    SEXP s;
    R_finalizers_pending = FALSE;
    for (s = R_weak_refs; s != R_NilValue; s = WEAKREF_NEXT(s)) {
	if (! NODE_IS_MARKED(WEAKREF_KEY(s)) && ! IS_READY_TO_FINALIZE(s))
	    SET_READY_TO_FINALIZE(s);
	if (IS_READY_TO_FINALIZE(s))
	    R_finalizers_pending = TRUE;
    }
}

/* C finalizers are stored in a CHARSXP.  It would be nice if we could
   use EXTPTRSXP's but these only hold a void *, and function pointers
   are not guaranteed to be compatible with a void *.  There should be
   a cleaner way of doing this, but this will do for now. --LT */
/* Changed to RAWSXP in 2.8.0 */
static Rboolean isCFinalizer(SEXP fun)
{
    return TYPEOF(fun) == RAWSXP;
    /*return TYPEOF(fun) == EXTPTRSXP;*/
}

static SEXP MakeCFinalizer(R_CFinalizer_t cfun)
{
    SEXP s = allocVector(RAWSXP, sizeof(R_CFinalizer_t));
    *((R_CFinalizer_t *) RAW(s)) = cfun;
    return s;
    /*return R_MakeExternalPtr((void *) cfun, R_NilValue, R_NilValue);*/
}

static R_CFinalizer_t GetCFinalizer(SEXP fun)
{
    return *((R_CFinalizer_t *) RAW(fun));
    /*return (R_CFinalizer_t) R_ExternalPtrAddr(fun);*/
}

SEXP R_WeakRefKey(SEXP w)
{
    if (TYPEOF(w) != WEAKREFSXP)
	error(_("not a weak reference"));
    return WEAKREF_KEY(w);
}

SEXP R_WeakRefValue(SEXP w)
{
    SEXP v;
    if (TYPEOF(w) != WEAKREFSXP)
	error(_("not a weak reference"));
    v = WEAKREF_VALUE(w);
    if (v != R_NilValue)
	ENSURE_NAMEDMAX(v);
    return v;
}

void R_RunWeakRefFinalizer(SEXP w)
{
    SEXP key, fun, e;
    if (TYPEOF(w) != WEAKREFSXP)
	error(_("not a weak reference"));
    key = WEAKREF_KEY(w);
    fun = WEAKREF_FINALIZER(w);
    SET_WEAKREF_KEY(w, R_NilValue);
    SET_WEAKREF_VALUE(w, R_NilValue);
    SET_WEAKREF_FINALIZER(w, R_NilValue);
    if (! IS_READY_TO_FINALIZE(w))
	SET_READY_TO_FINALIZE(w); /* insures removal from list on next gc */
    PROTECT(key);
    PROTECT(fun);
    Rboolean oldintrsusp = R_interrupts_suspended;
    R_interrupts_suspended = TRUE;
    if (isCFinalizer(fun)) {
	/* Must be a C finalizer. */
	R_CFinalizer_t cfun = GetCFinalizer(fun);
	cfun(key);
    }
    else if (fun != R_NilValue) {
	/* An R finalizer. */
	PROTECT(e = LCONS(fun, LCONS(key, R_NilValue)));
	eval(e, R_GlobalEnv);
	UNPROTECT(1);
    }
    R_interrupts_suspended = oldintrsusp;
    UNPROTECT(2);
}

static Rboolean RunFinalizers(void)
{
    R_CHECK_THREAD;
    /* Prevent this function from running again when already in
       progress. Jumps can only occur inside the top level context
       where they will be caught, so the flag is guaranteed to be
       reset at the end. */
    static Rboolean running = FALSE;
    if (running) return FALSE;
    running = TRUE;

    volatile SEXP s, last;
    volatile Rboolean finalizer_run = FALSE;

    for (s = R_weak_refs, last = R_NilValue; s != R_NilValue;) {
	SEXP next = WEAKREF_NEXT(s);
	if (IS_READY_TO_FINALIZE(s)) {
	    /**** use R_ToplevelExec here? */
	    RCNTXT thiscontext;
	    RCNTXT * volatile saveToplevelContext;
	    volatile int savestack;
	    volatile SEXP topExp, oldHStack, oldRStack, oldRVal;
	    volatile Rboolean oldvis;
	    PROTECT(oldHStack = R_HandlerStack);
	    PROTECT(oldRStack = R_RestartStack);
	    PROTECT(oldRVal = R_ReturnedValue);
	    oldvis = R_Visible;
	    R_HandlerStack = R_NilValue;
	    R_RestartStack = R_NilValue;

	    finalizer_run = TRUE;

	    /* A top level context is established for the finalizer to
	       insure that any errors that might occur do not spill
	       into the call that triggered the collection. */
	    begincontext(&thiscontext, CTXT_TOPLEVEL, R_NilValue, R_GlobalEnv,
			 R_BaseEnv, R_NilValue, R_NilValue);
	    saveToplevelContext = R_ToplevelContext;
	    PROTECT(topExp = R_CurrentExpr);
	    savestack = R_PPStackTop;
	    /* The value of 'next' is protected to make it safe
	       for this routine to be called recursively from a
	       gc triggered by a finalizer. */
	    PROTECT(next);
	    if (! SETJMP(thiscontext.cjmpbuf)) {
		R_GlobalContext = R_ToplevelContext = &thiscontext;

		/* The entry in the weak reference list is removed
		   before running the finalizer.  This insures that a
		   finalizer is run only once, even if running it
		   raises an error. */
		if (last == R_NilValue)
		    R_weak_refs = next;
		else
		    SET_WEAKREF_NEXT(last, next);
		R_RunWeakRefFinalizer(s);
	    }
	    endcontext(&thiscontext);
	    UNPROTECT(1); /* next */
	    R_ToplevelContext = saveToplevelContext;
	    R_PPStackTop = savestack;
	    R_CurrentExpr = topExp;
	    R_HandlerStack = oldHStack;
	    R_RestartStack = oldRStack;
	    R_ReturnedValue = oldRVal;
	    R_Visible = oldvis;
	    UNPROTECT(4);/* topExp, oldRVal, oldRStack, oldHStack */
	}
	else last = s;
	s = next;
    }
    running = FALSE;
    R_finalizers_pending = FALSE;
    return finalizer_run;
}

void R_RunExitFinalizers(void)
{
    SEXP s;

    R_checkConstants(TRUE);

    for (s = R_weak_refs; s != R_NilValue; s = WEAKREF_NEXT(s))
	if (FINALIZE_ON_EXIT(s))
	    SET_READY_TO_FINALIZE(s);
    RunFinalizers();
}

void R_RunPendingFinalizers(void)
{
    if (R_finalizers_pending)
	RunFinalizers();
}

void R_RegisterFinalizerEx(SEXP s, SEXP fun, Rboolean onexit)
{
    R_MakeWeakRef(s, R_NilValue, fun, onexit);
}

void R_RegisterFinalizer(SEXP s, SEXP fun)
{
    R_RegisterFinalizerEx(s, fun, FALSE);
}

void R_RegisterCFinalizerEx(SEXP s, R_CFinalizer_t fun, Rboolean onexit)
{
    R_MakeWeakRefC(s, R_NilValue, fun, onexit);
}

void R_RegisterCFinalizer(SEXP s, R_CFinalizer_t fun)
{
    R_RegisterCFinalizerEx(s, fun, FALSE);
}

/* R interface function */

attribute_hidden SEXP do_regFinaliz(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    int onexit;

    checkArity(op, args);

    if (TYPEOF(CAR(args)) != ENVSXP && TYPEOF(CAR(args)) != EXTPTRSXP)
	error(_("first argument must be environment or external pointer"));
    if (TYPEOF(CADR(args)) != CLOSXP)
	error(_("second argument must be a function"));

    onexit = asLogical(CADDR(args));
    if(onexit == NA_LOGICAL)
	error(_("third argument must be 'TRUE' or 'FALSE'"));

    R_RegisterFinalizerEx(CAR(args), CADR(args), (Rboolean) onexit);
    return R_NilValue;
}


/* The Generational Collector. */

#define PROCESS_NODES() do { \
    while (forwarded_nodes != NULL) { \
	SEXP s = forwarded_nodes; \
	forwarded_nodes = NEXT_NODE(forwarded_nodes); \
	PROCESS_ONE_NODE(s); \
	FORWARD_CHILDREN(s); \
    } \
} while (0)

static int RunGenCollect(R_size_t size_needed)
{
    int i, gen, gens_collected;
    RCNTXT *ctxt;
    SEXP s;
    SEXP forwarded_nodes;

    bad_sexp_type_seen = 0;

    /* determine number of generations to collect */
    while (num_old_gens_to_collect < NUM_OLD_GENERATIONS) {
	if (collect_counts[num_old_gens_to_collect]-- <= 0) {
	    collect_counts[num_old_gens_to_collect] =
		collect_counts_max[num_old_gens_to_collect];
	    num_old_gens_to_collect++;
	}
	else break;
    }

#ifdef PROTECTCHECK
    num_old_gens_to_collect = NUM_OLD_GENERATIONS;
#endif

 again:
    gens_collected = num_old_gens_to_collect;

#ifndef EXPEL_OLD_TO_NEW
    /* eliminate old-to-new references in generations to collect by
       transferring referenced nodes to referring generation */
    for (gen = 0; gen < num_old_gens_to_collect; gen++) {
	for (i = 0; i < NUM_NODE_CLASSES; i++) {
	    s = NEXT_NODE(R_GenHeap[i].OldToNew[gen]);
	    while (s != R_GenHeap[i].OldToNew[gen]) {
		SEXP next = NEXT_NODE(s);
		DO_CHILDREN(s, AgeNodeAndChildren, gen);
		UNSNAP_NODE(s);
		if (NODE_GENERATION(s) != gen)
		    gc_error("****snapping into wrong generation\n");
		SNAP_NODE(s, R_GenHeap[i].Old[gen]);
		s = next;
	    }
	}
    }
#endif

    DEBUG_CHECK_NODE_COUNTS("at start");

    /* unmark all marked nodes in old generations to be collected and
       move to New space */
    for (gen = 0; gen < num_old_gens_to_collect; gen++) {
	for (i = 0; i < NUM_NODE_CLASSES; i++) {
	    R_GenHeap[i].OldCount[gen] = 0;
	    s = NEXT_NODE(R_GenHeap[i].Old[gen]);
	    while (s != R_GenHeap[i].Old[gen]) {
		SEXP next = NEXT_NODE(s);
		if (gen < NUM_OLD_GENERATIONS - 1)
		    SET_NODE_GENERATION(s, gen + 1);
		UNMARK_NODE(s);
		s = next;
	    }
	    if (NEXT_NODE(R_GenHeap[i].Old[gen]) != R_GenHeap[i].Old[gen])
		BULK_MOVE(R_GenHeap[i].Old[gen], R_GenHeap[i].New);
	}
    }

    forwarded_nodes = NULL;

#ifndef EXPEL_OLD_TO_NEW
    /* scan nodes in uncollected old generations with old-to-new pointers */
    for (gen = num_old_gens_to_collect; gen < NUM_OLD_GENERATIONS; gen++)
	for (i = 0; i < NUM_NODE_CLASSES; i++)
	    for (s = NEXT_NODE(R_GenHeap[i].OldToNew[gen]);
		 s != R_GenHeap[i].OldToNew[gen];
		 s = NEXT_NODE(s))
		FORWARD_CHILDREN(s);
#endif

    /* forward all roots */
    FORWARD_NODE(R_NilValue);	           /* Builtin constants */
    FORWARD_NODE(NA_STRING);
    FORWARD_NODE(R_BlankString);
    FORWARD_NODE(R_BlankScalarString);
    FORWARD_NODE(R_CurrentExpression);
    FORWARD_NODE(R_UnboundValue);
    FORWARD_NODE(R_RestartToken);
    FORWARD_NODE(R_MissingArg);
    FORWARD_NODE(R_InBCInterpreter);

	    FORWARD_NODE(R_GlobalEnv);	           /* Global environment */
	    FORWARD_NODE(R_BaseEnv);
	    FORWARD_NODE(R_EmptyEnv);
	    FORWARD_NODE(R_Srcref);                /* Current source reference */

	    FORWARD_NODE(R_TrueValue);
	    FORWARD_NODE(R_FalseValue);
	    FORWARD_NODE(R_LogicalNAValue);

    FORWARD_NODE(R_print.na_string);
    FORWARD_NODE(R_print.na_string_noquote);

    if (R_SymbolTable != NULL)             /* in case of GC during startup */
	for (i = 0; i < HSIZE; i++) {      /* Symbol table */
	    FORWARD_NODE(R_SymbolTable[i]);
	    SEXP s;
	    for (s = R_SymbolTable[i]; s != R_NilValue; s = CDR(s))
		if (ATTRIB(CAR(s)) != R_NilValue)
		    gc_error("****found a symbol with attributes\n");
	}

	    /* Per-interpreter roots (main + workers). */
	    LOCK_INTERP_REGISTRY();
	    for (R_InterpreterState *ist = R_InterpreterRegistry;
		 ist != NULL;
		 ist = ist->next) {
		/* Only forward roots for interpreters sharing the current heap.
		   Independent mtlapply() worker heaps are collected separately. */
		if (ist->heap != R_HEAP)
		    continue;
		FORWARD_NODE_IN_CURRENT_HEAP(ist->warnings);          /* Warnings, if any */
		FORWARD_NODE_IN_CURRENT_HEAP(ist->returnedValue);
		FORWARD_NODE_IN_CURRENT_HEAP(ist->handlerStack);      /* Condition handler stack */
		FORWARD_NODE_IN_CURRENT_HEAP(ist->restartStack);      /* Available restarts stack */
		FORWARD_NODE_IN_CURRENT_HEAP(ist->workerGlobalEnv);   /* Worker global env (may be NULL) */
		FORWARD_NODE_IN_CURRENT_HEAP(ist->bcbody);            /* Current byte code object */
		FORWARD_NODE_IN_CURRENT_HEAP(ist->parseErrorFile);    /* Parse error source file (may be NULL) */
		if (ist->currentExpr)                 /* Current expression */
		    FORWARD_NODE_IN_CURRENT_HEAP(ist->currentExpr);
	    }
	    UNLOCK_INTERP_REGISTRY();

	    for (i = 0; i < R_MaxDevices; i++) {   /* Device display lists */
		pGEDevDesc gdd = GEgetDevice(i);
		if (gdd) {
		    FORWARD_NODE(gdd->displayList);
	    FORWARD_NODE(gdd->savedSnapshot);
	    if (gdd->dev)
		FORWARD_NODE(gdd->dev->eventEnv);
	}
    }

	    /* R_PreciousList and R_VStack are per-interpreter and scanned below. */
	    /* Per-interpreter stacks/contexts that can hold live references. */
	    LOCK_INTERP_REGISTRY();
	    for (R_InterpreterState *ist = R_InterpreterRegistry;
		 ist != NULL;
		 ist = ist->next) {
		/* Only scan stacks for interpreters sharing the current heap.
		   Independent worker heaps are handled by worker-local GC. */
		if (ist->heap != R_HEAP)
		    continue;
#ifdef R_USE_SIGNALS
		for (ctxt = ist->globalContext; ctxt != NULL; ctxt = ctxt->nextcontext) {
		    FORWARD_NODE_IN_CURRENT_HEAP(ctxt->conexit);       /* on.exit expressions */
		    FORWARD_NODE_IN_CURRENT_HEAP(ctxt->promargs);	   /* promises supplied to closure */
		    FORWARD_NODE_IN_CURRENT_HEAP(ctxt->callfun);       /* the closure called */
		    FORWARD_NODE_IN_CURRENT_HEAP(ctxt->sysparent);     /* calling environment */
		    FORWARD_NODE_IN_CURRENT_HEAP(ctxt->call);          /* the call */
		    FORWARD_NODE_IN_CURRENT_HEAP(ctxt->cloenv);        /* the closure environment */
		    FORWARD_NODE_IN_CURRENT_HEAP(ctxt->bcbody);        /* the current byte code object */
		    FORWARD_NODE_IN_CURRENT_HEAP(ctxt->handlerstack);  /* the condition handler stack */
		    FORWARD_NODE_IN_CURRENT_HEAP(ctxt->restartstack);  /* the available restarts stack */
		    FORWARD_NODE_IN_CURRENT_HEAP(ctxt->srcref);	   /* the current source reference */
		    if (ctxt->returnValue.tag == 0)    /* For on.exit calls */
			FORWARD_NODE_IN_CURRENT_HEAP(ctxt->returnValue.u.sxpval);
		}
#endif

		for (int j = 0; j < ist->ppStackTop; j++) /* Protected pointers */
		    FORWARD_NODE_IN_CURRENT_HEAP(ist->ppStack[j]);

		FORWARD_NODE_IN_CURRENT_HEAP(ist->preciousList);
		FORWARD_NODE_IN_CURRENT_HEAP(ist->vStack);	   /* R_alloc stack */

		if (ist->bcNodeStackBase && ist->bcNodeStackTop) {
		    for (R_bcstack_t *sp = ist->bcNodeStackBase;
			 sp < ist->bcNodeStackTop;
			 sp++) {
			if (sp->tag == RAWMEM_TAG)
			    sp += sp->u.ival;
			else if (sp->tag == 0 || IS_PARTIAL_SXP_TAG(sp->tag))
			    FORWARD_NODE_IN_CURRENT_HEAP(sp->u.sxpval);
		    }
		}
	    }
	    UNLOCK_INTERP_REGISTRY();

	    /* main processing loop */
	    PROCESS_NODES();

    /* identify weakly reachable nodes */
    {
	Rboolean recheck_weak_refs;
	do {
	    recheck_weak_refs = FALSE;
	    for (s = R_weak_refs; s != R_NilValue; s = WEAKREF_NEXT(s)) {
		if (NODE_IS_MARKED(WEAKREF_KEY(s))) {
		    if (! NODE_IS_MARKED(WEAKREF_VALUE(s))) {
			recheck_weak_refs = TRUE;
			FORWARD_NODE(WEAKREF_VALUE(s));
		    }
		    if (! NODE_IS_MARKED(WEAKREF_FINALIZER(s))) {
			recheck_weak_refs = TRUE;
			FORWARD_NODE(WEAKREF_FINALIZER(s));
		    }
		}
	    }
	    PROCESS_NODES();
	} while (recheck_weak_refs);
    }

    /* mark nodes ready for finalizing */
    CheckFinalizers();

    /* process the weak reference chain */
    for (s = R_weak_refs; s != R_NilValue; s = WEAKREF_NEXT(s)) {
	FORWARD_NODE(s);
	FORWARD_NODE(WEAKREF_KEY(s));
	FORWARD_NODE(WEAKREF_VALUE(s));
	FORWARD_NODE(WEAKREF_FINALIZER(s));
    }
    PROCESS_NODES();

    DEBUG_CHECK_NODE_COUNTS("after processing forwarded list");

    /* process CHARSXP cache */
    if (R_StringHash != NULL) /* in case of GC during initialization */
    {
	SEXP t;
	int nc = 0;
	for (i = 0; i < LENGTH(R_StringHash); i++) {
	    s = VECTOR_ELT_0(R_StringHash, i);
	    t = R_NilValue;
	    while (s != R_NilValue) {
		if (! NODE_IS_MARKED(CXHEAD(s))) { /* remove unused CHARSXP and cons cell */
		    if (t == R_NilValue) /* head of list */
			VECTOR_ELT_0(R_StringHash, i) = CXTAIL(s);
		    else
			CXTAIL(t) = CXTAIL(s);
		    s = CXTAIL(s);
		    continue;
		}
		FORWARD_NODE(s);
		FORWARD_NODE(CXHEAD(s));
		t = s;
		s = CXTAIL(s);
	    }
	    if(VECTOR_ELT_0(R_StringHash, i) != R_NilValue) nc++;
	}
	SET_TRUELENGTH(R_StringHash, nc); /* SET_HASHPRI, really */
    }
    /* chains are known to be marked so don't need to scan again */
    FORWARD_AND_PROCESS_ONE_NODE(R_StringHash, VECSXP);
    PROCESS_NODES(); /* probably nothing to process, but just in case ... */

#ifdef PROTECTCHECK
    for(i=0; i< NUM_SMALL_NODE_CLASSES;i++){
	s = NEXT_NODE(R_GenHeap[i].New);
	while (s != R_GenHeap[i].New) {
	    SEXP next = NEXT_NODE(s);
	    if (TYPEOF(s) != NEWSXP) {
		if (TYPEOF(s) != FREESXP) {
		    SETOLDTYPE(s, TYPEOF(s));
		    SET_TYPEOF(s, FREESXP);
		}
		if (gc_inhibit_release)
		    FORWARD_NODE(s);
	    }
	    s = next;
	}
    }
    for (i = CUSTOM_NODE_CLASS; i <= LARGE_NODE_CLASS; i++) {
	s = NEXT_NODE(R_GenHeap[i].New);
	while (s != R_GenHeap[i].New) {
	    SEXP next = NEXT_NODE(s);
	    if (TYPEOF(s) != NEWSXP) {
		if (TYPEOF(s) != FREESXP) {
		    /**** could also leave this alone and restore the old
			  node type in ReleaseLargeFreeVectors before
			  calculating size */
		    if (1 /* CHAR(s) != NULL*/) {
			/* see comment in ReleaseLargeFreeVectors */
			R_size_t size = getVecSizeInVEC(s);
			SET_STDVEC_LENGTH(s, size);
		    }
		    SETOLDTYPE(s, TYPEOF(s));
		    SET_TYPEOF(s, FREESXP);
		}
		if (gc_inhibit_release)
		    FORWARD_NODE(s);
	    }
	    s = next;
	}
    }
    if (gc_inhibit_release)
	PROCESS_NODES();
#endif

    /* release large vector allocations */
    ReleaseLargeFreeVectors();

    DEBUG_CHECK_NODE_COUNTS("after releasing large allocated nodes");

    /* tell Valgrind about free nodes */
#if VALGRIND_LEVEL > 1
    for(i = 1; i< NUM_NODE_CLASSES; i++) {
	for(s = NEXT_NODE(R_GenHeap[i].New);
	    s != R_GenHeap[i].Free;
	    s = NEXT_NODE(s)) {
	    VALGRIND_MAKE_MEM_NOACCESS(STDVEC_DATAPTR(s),
				       NodeClassSize[i]*sizeof(VECREC));
	}
    }
#endif

    /* reset Free pointers */
    for (i = 0; i < NUM_NODE_CLASSES; i++)
	R_GenHeap[i].Free = NEXT_NODE(R_GenHeap[i].New);


    /* update heap statistics */
    R_Collected = R_NSize;
    R_SmallVallocSize = 0;
    for (gen = 0; gen < NUM_OLD_GENERATIONS; gen++) {
	for (i = 1; i < NUM_SMALL_NODE_CLASSES; i++)
	    R_SmallVallocSize += R_GenHeap[i].OldCount[gen] * NodeClassSize[i];
	for (i = 0; i < NUM_NODE_CLASSES; i++)
	    R_Collected -= R_GenHeap[i].OldCount[gen];
    }
    R_NodesInUse = R_NSize - R_Collected;

    if (num_old_gens_to_collect < NUM_OLD_GENERATIONS) {
	if (R_Collected < R_MinFreeFrac * R_NSize ||
	    VHEAP_FREE() < size_needed + R_MinFreeFrac * R_VSize) {
	    num_old_gens_to_collect++;
	    if (R_Collected <= 0 || VHEAP_FREE() < size_needed)
		goto again;
	}
	else num_old_gens_to_collect = 0;
    }
    else num_old_gens_to_collect = 0;

    gen_gc_counts[gens_collected]++;

    if (gens_collected == NUM_OLD_GENERATIONS) {
	/**** do some adjustment for intermediate collections? */
	AdjustHeapSize(size_needed);
	TryToReleasePages();
	DEBUG_CHECK_NODE_COUNTS("after heap adjustment");
    }
    else if (gens_collected > 0) {
	TryToReleasePages();
	DEBUG_CHECK_NODE_COUNTS("after heap adjustment");
    }
#ifdef SORT_NODES
    if (gens_collected == NUM_OLD_GENERATIONS)
	SortNodes();
#endif

    /* Keep the main heap's per-interpreter trigger sizes in sync with the
       global GC tuning parameters (R_NSize/R_VSize). Allocation fast paths
       use R_NSize_heap/R_VSize_heap via the current heap. */
    R_NSize_heap = R_NSize;
    R_VSize_heap = R_VSize;

    return gens_collected;
}

/* Minimal worker-local collection.
 *
 * This is used for mtlapply() worker heaps. It intentionally avoids global
 * process state such as weak refs/finalizers and the global CHARSXP cache.
 * It also only traces the current interpreter's roots/stacks/contexts.
 */
static void mtl_worker_gc(R_size_t size_needed)
{
    int i, gen;
    RCNTXT *ctxt;
    SEXP s;
    SEXP forwarded_nodes = NULL;

    bad_sexp_type_seen = 0;

    /* Full collection: move everything out of old generations into New and
       unmark, then forward reachable nodes back to Old lists. */
    for (gen = 0; gen < NUM_OLD_GENERATIONS; gen++) {
	for (i = 0; i < NUM_NODE_CLASSES; i++) {
	    R_GenHeap[i].OldCount[gen] = 0;
	    s = NEXT_NODE(R_GenHeap[i].Old[gen]);
	    while (s != R_GenHeap[i].Old[gen]) {
		SEXP next = NEXT_NODE(s);
		UNMARK_NODE(s);
		s = next;
	    }
	    if (NEXT_NODE(R_GenHeap[i].Old[gen]) != R_GenHeap[i].Old[gen])
		BULK_MOVE(R_GenHeap[i].Old[gen], R_GenHeap[i].New);
#ifndef EXPEL_OLD_TO_NEW
	    s = NEXT_NODE(R_GenHeap[i].OldToNew[gen]);
	    while (s != R_GenHeap[i].OldToNew[gen]) {
		SEXP next = NEXT_NODE(s);
		UNMARK_NODE(s);
		s = next;
	    }
	    if (NEXT_NODE(R_GenHeap[i].OldToNew[gen]) != R_GenHeap[i].OldToNew[gen])
		BULK_MOVE(R_GenHeap[i].OldToNew[gen], R_GenHeap[i].New);
#endif
	}
    }

    /* Forward interpreter roots. */
    FORWARD_NODE(R_Warnings);
    FORWARD_NODE(R_ReturnedValue);
    FORWARD_NODE(R_HandlerStack);
    FORWARD_NODE(R_RestartStack);
    FORWARD_NODE(R_Interpreter->workerGlobalEnv);
    FORWARD_NODE(R_BCbody);
    FORWARD_NODE(R_ParseErrorFile);
    if (R_CurrentExpr)
	FORWARD_NODE(R_CurrentExpr);

#ifdef R_USE_SIGNALS
    for (ctxt = R_GlobalContext; ctxt != NULL; ctxt = ctxt->nextcontext) {
	FORWARD_NODE(ctxt->conexit);
	FORWARD_NODE(ctxt->promargs);
	FORWARD_NODE(ctxt->callfun);
	FORWARD_NODE(ctxt->sysparent);
	FORWARD_NODE(ctxt->call);
	FORWARD_NODE(ctxt->cloenv);
	FORWARD_NODE(ctxt->bcbody);
	FORWARD_NODE(ctxt->handlerstack);
	FORWARD_NODE(ctxt->restartstack);
	FORWARD_NODE(ctxt->srcref);
	if (ctxt->returnValue.tag == 0)
	    FORWARD_NODE(ctxt->returnValue.u.sxpval);
    }
#endif

    for (int j = 0; j < R_PPStackTop; j++)
	FORWARD_NODE(R_PPStack[j]);
    FORWARD_NODE(R_PreciousList);
    FORWARD_NODE(R_VStack);

    if (R_BCNodeStackBase && R_BCNodeStackTop) {
	for (R_bcstack_t *sp = R_BCNodeStackBase; sp < R_BCNodeStackTop; sp++) {
	    if (sp->tag == RAWMEM_TAG)
		sp += sp->u.ival;
	    else if (sp->tag == 0 || IS_PARTIAL_SXP_TAG(sp->tag))
		FORWARD_NODE(sp->u.sxpval);
	}
    }

    PROCESS_NODES();

    /* Release large vector allocations that ended up in New space. */
    ReleaseLargeFreeVectors();

    /* Reset Free pointers. */
    for (i = 0; i < NUM_NODE_CLASSES; i++)
	R_GenHeap[i].Free = NEXT_NODE(R_GenHeap[i].New);

    /* Update heap statistics used by allocation fast paths. */
    R_size_t nodes_in_use = 0;
    R_size_t small_valloc = 0;
    for (gen = 0; gen < NUM_OLD_GENERATIONS; gen++) {
	for (i = 1; i < NUM_SMALL_NODE_CLASSES; i++)
	    small_valloc += (R_size_t) R_GenHeap[i].OldCount[gen] * (R_size_t) NodeClassSize[i];
	for (i = 0; i < NUM_NODE_CLASSES; i++)
	    nodes_in_use += (R_size_t) R_GenHeap[i].OldCount[gen];
    }
    NODES_IN_USE_STORE(nodes_in_use);
    SMALL_VALLOC_STORE(small_valloc);

    (void) size_needed; /* currently ignored */
}


/* public interface for controlling GC torture settings */
/* maybe, but in no header, and now hidden */
attribute_hidden
void R_gc_torture(int gap, int wait, Rboolean inhibit)
{
    if (gap != NA_INTEGER && gap >= 0)
	gc_force_wait = gc_force_gap = gap;
    if (gap > 0) {
	if (wait != NA_INTEGER && wait > 0)
	    gc_force_wait = wait;
    }
#ifdef PROTECTCHECK
    if (gap > 0) {
	if (inhibit != NA_LOGICAL)
	    gc_inhibit_release = inhibit;
    }
    else gc_inhibit_release = FALSE;
#endif
}

attribute_hidden SEXP do_gctorture(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    int gap;
    SEXP old = ScalarLogical(gc_force_wait > 0);

    checkArity(op, args);

    if (isLogical(CAR(args))) {
	Rboolean on = asRbool(CAR(args), call);
	if (on == NA_LOGICAL) gap = NA_INTEGER;
	else if (on) gap = 1;
	else gap = 0;
    }
    else gap = asInteger(CAR(args));

    R_gc_torture(gap, 0, FALSE);

    return old;
}

attribute_hidden SEXP do_gctorture2(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    int gap, wait;
    Rboolean inhibit;
    int old = gc_force_gap;

    checkArity(op, args);
    gap = asInteger(CAR(args));
    wait = asInteger(CADR(args));
    inhibit = asRbool(CADDR(args), call);
    R_gc_torture(gap, wait, inhibit);

    return ScalarInteger(old);
}

/* initialize gctorture settings from environment variables */
static void init_gctorture(void)
{
    char *arg = getenv("R_GCTORTURE");
    if (arg != NULL) {
	int gap = atoi(arg);
	if (gap > 0) {
	    gc_force_wait = gc_force_gap = gap;
	    arg = getenv("R_GCTORTURE_WAIT");
	    if (arg != NULL) {
		int wait = atoi(arg);
		if (wait > 0)
		    gc_force_wait = wait;
	    }
#ifdef PROTECTCHECK
	    arg = getenv("R_GCTORTURE_INHIBIT_RELEASE");
	    if (arg != NULL) {
		int inhibit = atoi(arg);
		if (inhibit > 0) gc_inhibit_release = TRUE;
		else gc_inhibit_release = FALSE;
	    }
#endif
	}
    }
}

attribute_hidden SEXP do_gcinfo(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    int i;
    SEXP old = ScalarLogical(gc_reporting);
    checkArity(op, args);
    i = asLogical(CAR(args));
    if (i != NA_LOGICAL)
	gc_reporting = i;
    return old;
}

/* reports memory use to profiler in eval.c */

attribute_hidden void get_current_mem(size_t *smallvsize,
				      size_t *largevsize,
				      size_t *nodes)
{
    *smallvsize = R_SmallVallocSize;
    *largevsize = R_LargeVallocSize;
    *nodes = R_NodesInUse * sizeof(SEXPREC);
    return;
}

attribute_hidden SEXP do_gc(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP value;
    int ogc, reset_max, full;
    R_size_t onsize = R_NSize /* can change during collection */;

    checkArity(op, args);
    ogc = gc_reporting;
    gc_reporting = asLogical(CAR(args));
    reset_max = asLogical(CADR(args));
    full = asLogical(CADDR(args));
    if (full)
	R_gc();
    else
	R_gc_lite();

    gc_reporting = ogc;
    /*- now return the [used , gc trigger size] for cells and heap */
    PROTECT(value = allocVector(REALSXP, 14));
    REAL(value)[0] = onsize - R_Collected;
    REAL(value)[1] = R_VSize - VHEAP_FREE();
    REAL(value)[4] = R_NSize;
    REAL(value)[5] = R_VSize;
    /* next four are in 0.1Mb, rounded up */
    REAL(value)[2] = 0.1*ceil(10. * (onsize - R_Collected)/Mega * sizeof(SEXPREC));
    REAL(value)[3] = 0.1*ceil(10. * (R_VSize - VHEAP_FREE())/Mega * vsfac);
    REAL(value)[6] = 0.1*ceil(10. * R_NSize/Mega * sizeof(SEXPREC));
    REAL(value)[7] = 0.1*ceil(10. * R_VSize/Mega * vsfac);
    REAL(value)[8] = (R_MaxNSize < R_SIZE_T_MAX) ?
	0.1*ceil(10. * R_MaxNSize/Mega * sizeof(SEXPREC)) : NA_REAL;
    REAL(value)[9] = (R_MaxVSize < R_SIZE_T_MAX) ?
	0.1*ceil(10. * R_MaxVSize/Mega * vsfac) : NA_REAL;
    if (reset_max){
	    R_N_maxused = onsize - R_Collected;
	    R_V_maxused = R_VSize - VHEAP_FREE();
    }
    REAL(value)[10] = R_N_maxused;
    REAL(value)[11] = R_V_maxused;
    REAL(value)[12] = 0.1*ceil(10. * R_N_maxused/Mega*sizeof(SEXPREC));
    REAL(value)[13] = 0.1*ceil(10. * R_V_maxused/Mega*vsfac);
    UNPROTECT(1);
    return value;
}

NORET static void mem_err_heap(R_size_t size)
{
    if (R_MaxVSize == R_SIZE_T_MAX)
	errorcall(R_NilValue, _("vector memory exhausted"));
    else {
	double l = R_GetMaxVSize() / 1024.0;
	const char *unit = "Kb";

	if (l > 1024.0*1024.0) {
	    l /= 1024.0*1024.0;
	    unit = "Gb";
	} else if (l > 1024.0) {
	    l /= 1024.0;
	    unit = "Mb";
	}
	errorcall(R_NilValue,
	          _("vector memory limit of %0.1f %s reached, see mem.maxVSize()"),
	          l, unit);
    }
}

NORET static void mem_err_cons(void)
{
    if (R_MaxNSize == R_SIZE_T_MAX)
        errorcall(R_NilValue, _("cons memory exhausted"));
    else
        errorcall(R_NilValue,
	          _("cons memory limit of %llu nodes reached, see mem.maxNSize()"),
	          (unsigned long long)R_MaxNSize);
}

NORET static void mem_err_malloc(R_size_t size)
{
    errorcall(R_NilValue, _("memory exhausted"));
}

/* InitMemory : Initialise the memory to be used in R. */
/* This includes: stack space, node space and vector space */

#define PP_REDZONE_SIZE 1000L
static int R_StandardPPStackSize, R_RealPPStackSize;

attribute_hidden void R_RegisterInterpreterState(R_InterpreterState *st)
{
    LOCK_INTERP_REGISTRY();
    st->next = R_InterpreterRegistry;
    R_InterpreterRegistry = st;
    UNLOCK_INTERP_REGISTRY();
}

attribute_hidden void R_UnregisterInterpreterState(R_InterpreterState *st)
{
    LOCK_INTERP_REGISTRY();
    R_InterpreterState **p = &R_InterpreterRegistry;
    while (*p && *p != st)
	p = &(*p)->next;
    if (*p == NULL) {
	UNLOCK_INTERP_REGISTRY();
	R_Suicide("R_UnregisterInterpreterState: interpreter not registered");
    }
    *p = st->next;
    st->next = NULL;
    UNLOCK_INTERP_REGISTRY();
}

attribute_hidden void R_InitInterpreterProtectStack(R_InterpreterState *st)
{
    if (st->ppStack != NULL)
	return;
    if (R_RealPPStackSize <= 0)
	R_Suicide("R_InitInterpreterProtectStack called before InitMemory");
    if (!(st->ppStack = (SEXP *) malloc(R_RealPPStackSize * sizeof(SEXP))))
	R_Suicide("couldn't allocate memory for pointer stack");
    st->ppStackTop = 0;
#if VALGRIND_LEVEL > 1
    VALGRIND_MAKE_MEM_NOACCESS(st->ppStack+R_PPStackSize, PP_REDZONE_SIZE);
#endif
}

attribute_hidden void R_InitInterpreterBCNodeStack(R_InterpreterState *st)
{
    if (st->bcNodeStackBase != NULL)
	return;
    st->bcNodeStackBase =
	(R_bcstack_t *) malloc(R_BCNODESTACKSIZE * sizeof(R_bcstack_t));
    if (st->bcNodeStackBase == NULL)
	R_Suicide("couldn't allocate node stack");
    st->bcNodeStackTop = st->bcNodeStackBase;
    st->bcNodeStackEnd = st->bcNodeStackBase + R_BCNODESTACKSIZE;
    st->bcProtTop = st->bcNodeStackTop;
    st->bcProtCommitted = st->bcNodeStackBase;
}

static void mtl_heap_init(R_mtl_heap_state *h)
{
    for (int i = 0; i < NUM_NODE_CLASSES; i++) {
	for (int gen = 0; gen < NUM_OLD_GENERATIONS; gen++) {
	    h->GenHeap[i].Old[gen] = &h->GenHeap[i].OldPeg[gen];
	    SET_PREV_NODE(h->GenHeap[i].Old[gen], h->GenHeap[i].Old[gen]);
	    SET_NEXT_NODE(h->GenHeap[i].Old[gen], h->GenHeap[i].Old[gen]);
#ifndef EXPEL_OLD_TO_NEW
	    h->GenHeap[i].OldToNew[gen] = &h->GenHeap[i].OldToNewPeg[gen];
	    SET_PREV_NODE(h->GenHeap[i].OldToNew[gen], h->GenHeap[i].OldToNew[gen]);
	    SET_NEXT_NODE(h->GenHeap[i].OldToNew[gen], h->GenHeap[i].OldToNew[gen]);
#endif
	    h->GenHeap[i].OldCount[gen] = 0;
	}
	h->GenHeap[i].New = &h->GenHeap[i].NewPeg;
	SET_PREV_NODE(h->GenHeap[i].New, h->GenHeap[i].New);
	SET_NEXT_NODE(h->GenHeap[i].New, h->GenHeap[i].New);
	h->GenHeap[i].pages = NULL;
	h->GenHeap[i].AllocCount = 0;
	h->GenHeap[i].PageCount = 0;
    }
    for (int i = 0; i < NUM_NODE_CLASSES; i++)
	h->GenHeap[i].Free = NEXT_NODE(h->GenHeap[i].New);
}

attribute_hidden void R_InitInterpreterHeap(R_InterpreterState *st)
{
    if (st->heap != NULL)
	return;

    R_mtl_heap_state *h = (R_mtl_heap_state *) calloc(1, sizeof(R_mtl_heap_state));
    if (h == NULL)
	R_Suicide("couldn't allocate interpreter heap state");

    h->isWorker = st->isMTLWorker ? 1 : 0;

    /* Default worker heaps are smaller; they can still grow by allocating pages,
       but will run GC sooner based on these thresholds. */
    if (h->isWorker) {
	h->NSize = R_NSize / 8;
	if (h->NSize < 10000) h->NSize = 10000;
	h->VSize = R_VSize / 8;
	if (h->VSize < 10000) h->VSize = 10000;
    } else {
	h->NSize = R_NSize;
	h->VSize = R_VSize;
    }

    mtl_heap_init(h);
    st->heap = h;
}

attribute_hidden void R_DestroyInterpreterHeap(R_InterpreterState *st)
{
    if (st->heap == NULL || st->heap == &R_MainHeapState)
	return;

    R_mtl_heap_state *h = st->heap;

    for (int i = 0; i < NUM_NODE_CLASSES; i++) {
	PAGE_HEADER *page = h->GenHeap[i].pages;
	while (page != NULL) {
	    PAGE_HEADER *next = page->next;
	    free(page);
	    page = next;
	}
	h->GenHeap[i].pages = NULL;
    }

	    /* Large/custom nodes are malloc-allocated; free anything still on the lists.
	       (This should only matter at shutdown.) */
	    for (int node_class = CUSTOM_NODE_CLASS; node_class <= LARGE_NODE_CLASS; node_class++) {
		for (int gen = 0; gen < NUM_OLD_GENERATIONS; gen++) {
		    SEXP s = NEXT_NODE(h->GenHeap[node_class].Old[gen]);
		    while (s != h->GenHeap[node_class].Old[gen]) {
			SEXP next = NEXT_NODE(s);
			UNSNAP_NODE(s);
			mtl_large_owner_set(s, NULL);
			if (node_class == LARGE_NODE_CLASS) free(s); else custom_node_free(s);
			s = next;
		    }
#ifndef EXPEL_OLD_TO_NEW
		    s = NEXT_NODE(h->GenHeap[node_class].OldToNew[gen]);
		    while (s != h->GenHeap[node_class].OldToNew[gen]) {
			SEXP next = NEXT_NODE(s);
			UNSNAP_NODE(s);
			mtl_large_owner_set(s, NULL);
			if (node_class == LARGE_NODE_CLASS) free(s); else custom_node_free(s);
			s = next;
		    }
#endif
		}
		SEXP s = NEXT_NODE(h->GenHeap[node_class].New);
		while (s != h->GenHeap[node_class].New) {
		    SEXP next = NEXT_NODE(s);
		    UNSNAP_NODE(s);
		    mtl_large_owner_set(s, NULL);
		    if (node_class == LARGE_NODE_CLASS) free(s); else custom_node_free(s);
		    s = next;
		}
	    }

    free(h);
    st->heap = NULL;
}

/* Adopt an mtlapply() worker heap into the main heap.
 *
 * This is a coarse "heap transfer" mechanism: after first running a worker-local
 * GC to drop garbage, we splice the worker heap's node/page lists into the main
 * heap and reset the worker heap to an empty state.
 *
 * The main heap lock is taken while mutating main heap lists. Worker evaluation
 * should not be concurrent with adoption (mtlapply() only adopts between jobs).
 */
attribute_hidden void R_mtl_adopt_worker_heap(R_InterpreterState *st)
{
    if (st == NULL || st->heap == NULL || st->heap == &R_MainHeapState)
	return;

    R_mtl_heap_state *src = st->heap;
    if (!src->isWorker)
	return;

    /* The worker thread runs a worker-local GC at the end of each job to move
       all live nodes out of New space before adoption. */

    /* Now splice lists into the main heap. */
    R_mtl_heap_lock();

    R_mtl_heap_state *dst = &R_MainHeapState;

    /* Transfer page-managed node pages for each node class. */
    for (int i = 0; i < NUM_NODE_CLASSES; i++) {
	PAGE_HEADER *wpages = src->GenHeap[i].pages;
	if (wpages != NULL) {
	    /* Retag pages as owned by the main heap. */
	    PAGE_HEADER *tail = NULL;
	    for (PAGE_HEADER *p = wpages; p != NULL; p = p->next) {
		p->owner = dst;
		tail = p;
	    }
	    /* Splice worker pages list into main pages list. */
	    tail->next = dst->GenHeap[i].pages;
	    dst->GenHeap[i].pages = wpages;
	    src->GenHeap[i].pages = NULL;

	    dst->GenHeap[i].PageCount += src->GenHeap[i].PageCount;
	    src->GenHeap[i].PageCount = 0;
	}

	/* Move node lists generation-by-generation. */
	for (int gen = 0; gen < NUM_OLD_GENERATIONS; gen++) {
	    if (NEXT_NODE(src->GenHeap[i].Old[gen]) != src->GenHeap[i].Old[gen]) {
		/* Update owner map for malloc-allocated large/custom nodes. */
		if (i >= CUSTOM_NODE_CLASS) {
		    for (SEXP s = NEXT_NODE(src->GenHeap[i].Old[gen]);
			 s != src->GenHeap[i].Old[gen];
			 s = NEXT_NODE(s))
			mtl_large_owner_set(s, dst);
		}
		BULK_MOVE(src->GenHeap[i].Old[gen], dst->GenHeap[i].Old[gen]);
		dst->GenHeap[i].OldCount[gen] += src->GenHeap[i].OldCount[gen];
		src->GenHeap[i].OldCount[gen] = 0;
	    }
#ifndef EXPEL_OLD_TO_NEW
	    if (NEXT_NODE(src->GenHeap[i].OldToNew[gen]) != src->GenHeap[i].OldToNew[gen]) {
		if (i >= CUSTOM_NODE_CLASS) {
		    for (SEXP s = NEXT_NODE(src->GenHeap[i].OldToNew[gen]);
			 s != src->GenHeap[i].OldToNew[gen];
			 s = NEXT_NODE(s))
			mtl_large_owner_set(s, dst);
		}
		BULK_MOVE(src->GenHeap[i].OldToNew[gen], dst->GenHeap[i].OldToNew[gen]);
		/* OldToNew is not separately counted; OldCount is updated as part of GC. */
	    }
#endif
	}

		/* Move any nodes still linked in New space (may include both allocated
		   and free nodes, depending on whether a GC has run in the worker).
		   We intentionally do not try to merge Free cursors here: the New-space
		   free boundary is defined relative to the heap's peg node, and mixing
		   cursors across pegs is subtle. Free nodes transferred this way will
		   become available after the next main-heap GC resets the Free cursor. */
		if (NEXT_NODE(src->GenHeap[i].New) != src->GenHeap[i].New) {
		    if (i >= CUSTOM_NODE_CLASS) {
			for (SEXP s = NEXT_NODE(src->GenHeap[i].New);
			     s != src->GenHeap[i].New;
			     s = NEXT_NODE(s))
			    mtl_large_owner_set(s, dst);
		    }
		    BULK_MOVE(src->GenHeap[i].New, dst->GenHeap[i].New);
		}

	/* AllocCount is a heuristic; keep the larger of the two. */
	if (dst->GenHeap[i].AllocCount < src->GenHeap[i].AllocCount)
	    dst->GenHeap[i].AllocCount = src->GenHeap[i].AllocCount;
	src->GenHeap[i].AllocCount = 0;
    }

    /* Transfer heap usage counters. */
    dst->NodesInUse += src->NodesInUse;
    dst->SmallVallocSize += src->SmallVallocSize;
    dst->LargeVallocSize += src->LargeVallocSize;
    src->NodesInUse = 0;
    src->SmallVallocSize = 0;
    src->LargeVallocSize = 0;

    R_mtl_heap_unlock();

    /* Drop worker roots to adopted objects and reset heap lists. */
    st->preciousList = R_NilValue;
    st->vStack = R_NilValue;
    st->workerGlobalEnv = NULL;
    mtl_heap_init(src);
}

attribute_hidden void InitMemory(void)
{
    int i;
    int gen;
    char *arg;

    R_mtl_heap_lock();

    if (R_Interpreter->heap == NULL) {
	R_Interpreter->heap = &R_MainHeapState;
	R_HEAP->isWorker = 0;
	R_HEAP->NSize = R_NSize;
	/* VSize is converted to VECRECs below, then stored in R_HEAP->VSize. */
    }
    if (R_mtl_pagesize == 0) {
	long ps = sysconf(_SC_PAGESIZE);
	if (ps <= 0)
	    R_Suicide("InitMemory: sysconf(_SC_PAGESIZE) failed");
	R_mtl_pagesize = (size_t) ps;
	if ((R_mtl_pagesize & (R_mtl_pagesize - 1)) != 0)
	    R_Suicide("InitMemory: page size is not a power of two");
	R_mtl_pagesize_mask = (uintptr_t) (R_mtl_pagesize - 1);
    }
    mtl_large_owner_init();

    init_gctorture();
    init_gc_grow_settings();

    arg = getenv("_R_GC_FAIL_ON_ERROR_");
    if (arg != NULL && StringTrue(arg))
	gc_fail_on_error = TRUE;
    else if (arg != NULL && StringFalse(arg))
	gc_fail_on_error = FALSE;

    gc_reporting = R_Verbose;
    R_StandardPPStackSize = R_PPStackSize;
    R_RealPPStackSize = R_PPStackSize + PP_REDZONE_SIZE;
    R_InitInterpreterProtectStack(R_Interpreter);
    R_RegisterInterpreterState(R_Interpreter);
    vsfac = sizeof(VECREC);
    R_VSize = (R_VSize + 1)/vsfac;
    if (R_MaxVSize < R_SIZE_T_MAX) R_MaxVSize = (R_MaxVSize + 1)/vsfac;
    R_HEAP->VSize = R_VSize;

    UNMARK_NODE(&UnmarkedNodeTemplate);

    for (i = 0; i < NUM_NODE_CLASSES; i++) {
      for (gen = 0; gen < NUM_OLD_GENERATIONS; gen++) {
	R_GenHeap[i].Old[gen] = &R_GenHeap[i].OldPeg[gen];
	SET_PREV_NODE(R_GenHeap[i].Old[gen], R_GenHeap[i].Old[gen]);
	SET_NEXT_NODE(R_GenHeap[i].Old[gen], R_GenHeap[i].Old[gen]);

#ifndef EXPEL_OLD_TO_NEW
	R_GenHeap[i].OldToNew[gen] = &R_GenHeap[i].OldToNewPeg[gen];
	SET_PREV_NODE(R_GenHeap[i].OldToNew[gen], R_GenHeap[i].OldToNew[gen]);
	SET_NEXT_NODE(R_GenHeap[i].OldToNew[gen], R_GenHeap[i].OldToNew[gen]);
#endif

	R_GenHeap[i].OldCount[gen] = 0;
      }
      R_GenHeap[i].New = &R_GenHeap[i].NewPeg;
      SET_PREV_NODE(R_GenHeap[i].New, R_GenHeap[i].New);
      SET_NEXT_NODE(R_GenHeap[i].New, R_GenHeap[i].New);
    }

    for (i = 0; i < NUM_NODE_CLASSES; i++)
	R_GenHeap[i].Free = NEXT_NODE(R_GenHeap[i].New);

    SET_NODE_CLASS(&UnmarkedNodeTemplate, 0);
    orig_R_NSize = R_NSize;
    orig_R_VSize = R_VSize;

    /* R_NilValue */
    /* THIS MUST BE THE FIRST CONS CELL ALLOCATED */
    /* OR ARMAGEDDON HAPPENS. */
    /* Field assignments for R_NilValue must not go through write barrier
       since the write barrier prevents assignments to R_NilValue's fields.
       because of checks for nil */
    if (CLASS_NEED_NEW_PAGE(0))
	GetNewPage(0);
    R_NilValue = try_get_free_node(0);
    if (R_NilValue == NULL)
	R_Suicide("InitMemory: failed to allocate R_NilValue");
    R_NilValue->sxpinfo = UnmarkedNodeTemplate.sxpinfo;
    INIT_REFCNT(R_NilValue);
    SET_REFCNT(R_NilValue, REFCNTMAX);
    SET_TYPEOF(R_NilValue, NILSXP);
    CAR0(R_NilValue) = R_NilValue;
    CDR(R_NilValue) = R_NilValue;
    TAG(R_NilValue) = R_NilValue;
    ATTRIB(R_NilValue) = R_NilValue;
    MARK_NOT_MUTABLE(R_NilValue);

    R_InitInterpreterBCNodeStack(R_Interpreter);

    R_weak_refs = R_NilValue;

    R_HandlerStack = R_RestartStack = R_NilValue;

    /*  The current source line */
    R_Srcref = R_NilValue;

    /* Unbound values which are to be preserved through GCs (per interpreter). */
    R_PreciousList = R_NilValue;
    R_VStack = R_NilValue;

    /* R_TrueValue and R_FalseValue */
    R_TrueValue = mkTrue();
    MARK_NOT_MUTABLE(R_TrueValue);
    R_FalseValue = mkFalse();
    MARK_NOT_MUTABLE(R_FalseValue);
    R_LogicalNAValue = allocVector(LGLSXP, 1);
    LOGICAL(R_LogicalNAValue)[0] = NA_LOGICAL;
    MARK_NOT_MUTABLE(R_LogicalNAValue);

    R_mtl_heap_unlock();
}

/* Since memory allocated from the heap is non-moving, R_alloc just
   allocates off the heap as RAWSXP/REALSXP and maintains the stack of
   allocations through the ATTRIB pointer.  The stack pointer R_VStack
   is traced by the collector. */
void *vmaxget(void)
{
    return (void *) R_VStack;
}

void vmaxset(const void *ovmax)
{
    R_VStack = (SEXP) ovmax;
}

char *R_alloc(size_t nelem, int eltsize)
{
    R_size_t size = nelem * eltsize;
    /* doubles are a precaution against integer overflow on 32-bit */
    double dsize = (double) nelem * eltsize;
    if (dsize > 0) {
	SEXP s;
#ifdef LONG_VECTOR_SUPPORT
	/* 64-bit platform: previous version used REALSXPs */
	if(dsize > R_XLEN_T_MAX)  /* currently 4096 TB */
	    error(_("cannot allocate memory block of size %0.f %s"),
		  dsize/R_pow_di(1024.0, 4), "Tb");
	s = allocVector(RAWSXP, size + 1);
#else
	if(dsize > R_LEN_T_MAX) /* must be in the Gb range */
	    error(_("cannot allocate memory block of size %0.1f %s"),
		  dsize/R_pow_di(1024.0, 3), "Gb");
	s = allocVector(RAWSXP, size + 1);
#endif
	ATTRIB(s) = R_VStack;
	R_VStack = s;
	return (char *) STDVEC_DATAPTR(s);
    }
    /* One programmer has relied on this, but it is undocumented! */
    else return NULL;
}

#ifdef HAVE_STDALIGN_H
# include <stdalign.h>
#endif

long double *R_allocLD(size_t nelem)
{
#if __alignof_is_defined
    // This is C11: picky compilers may warn.
    size_t ld_align = alignof(long double);
#elif __GNUC__
    // This is C99, but do not rely on it.
    // Apple clang warns this is gnu extension.
    #ifdef __clang__
    # pragma clang diagnostic ignored "-Wgnu-offsetof-extensions"
    #endif
    size_t ld_align = offsetof(struct { char __a; long double __b; }, __b);
#else
    size_t ld_align = 0x0F; // value of x86_64, known others are 4 or 8
#endif
    if (ld_align > 8) {
	uintptr_t tmp = (uintptr_t) R_alloc(nelem + 1, sizeof(long double));
	tmp = (tmp + ld_align - 1) & ~((uintptr_t)ld_align - 1);
	return (long double *) tmp;
    } else {
	return (long double *) R_alloc(nelem, sizeof(long double));
    }
}


/* S COMPATIBILITY */

char *S_alloc(long nelem, int eltsize)
{
    R_size_t size  = nelem * eltsize;
    char *p = R_alloc(nelem, eltsize);

    if(p) memset(p, 0, size);
    return p;
}


char *S_realloc(char *p, long new, long old, int size)
{
    size_t nold;
    char *q;
    /* shrinking is a no-op */
    if(new <= old) return p; // so new > 0 below
    q = R_alloc((size_t)new, size);
    nold = (size_t)old * size;
    if (nold)
	memcpy(q, p, nold);
    memset(q + nold, 0, (size_t)new*size - nold);
    return q;
}


/* Allocation functions that GC on initial failure */

void *R_malloc_gc(size_t n)
{
    void *np = malloc(n);
    if (np == NULL) {
	R_gc();
	np = malloc(n);
    }
    return np;
}

void *R_calloc_gc(size_t n, size_t s)
{
    void *np = calloc(n, s);
    if (np == NULL) {
	R_gc();
	np = calloc(n, s);
    }
    return np;
}

void *R_realloc_gc(void *p, size_t n)
{
    void *np = realloc(p, n);
    if (np == NULL) {
	R_gc();
	np = realloc(p, n);
    }
    return np;
}


/* "allocSExp" allocate a SEXPREC */
/* call gc if necessary */

SEXP allocSExp(SEXPTYPE t)
{
    if (t == NILSXP)
	/* R_NilValue should be the only NILSXP object */
	return R_NilValue;
    heap_alloc_enter();
    for (;;) {
	if (FORCE_GC || NO_FREE_NODES()) {
	    mtl_gc(0);
	    if (NO_FREE_NODES())
		mem_err_cons();
	    continue;
	}
	SEXP s = try_get_free_node(0);
	if (s == NULL) {
	    mtl_get_new_page(0);
	    continue;
	}
	s->sxpinfo = UnmarkedNodeTemplate.sxpinfo;
	INIT_REFCNT(s);
	SET_TYPEOF(s, t);
	CAR0(s) = R_NilValue;
	CDR(s) = R_NilValue;
	TAG(s) = R_NilValue;
	ATTRIB(s) = R_NilValue;
	heap_alloc_exit();
	return s;
    }
}

static SEXP allocSExpNonCons(SEXPTYPE t)
{
    heap_alloc_enter();
    for (;;) {
	if (FORCE_GC || NO_FREE_NODES()) {
	    mtl_gc(0);
	    if (NO_FREE_NODES())
		mem_err_cons();
	    continue;
	}
	SEXP s = try_get_free_node(0);
	if (s == NULL) {
	    mtl_get_new_page(0);
	    continue;
	}
	s->sxpinfo = UnmarkedNodeTemplate.sxpinfo;
	INIT_REFCNT(s);
	SET_TYPEOF(s, t);
	TAG(s) = R_NilValue;
	ATTRIB(s) = R_NilValue;
	heap_alloc_exit();
	return s;
    }
}

/* cons is defined directly to avoid the need to protect its arguments
   unless a GC will actually occur. */
SEXP cons(SEXP car, SEXP cdr)
{
    heap_alloc_enter();
    for (;;) {
	if (FORCE_GC || NO_FREE_NODES()) {
	    PROTECT(car);
	    PROTECT(cdr);
	    mtl_gc(0);
	    UNPROTECT(2);
	    if (NO_FREE_NODES())
		mem_err_cons();
	    continue;
	}

	SEXP s = try_get_free_node(0);
	if (s == NULL) {
	    PROTECT(car);
	    PROTECT(cdr);
	    mtl_get_new_page(0);
	    UNPROTECT(2);
	    continue;
	}

	s->sxpinfo = UnmarkedNodeTemplate.sxpinfo;
	INIT_REFCNT(s);
	SET_TYPEOF(s, LISTSXP);
	CAR0(s) = CHK(car); if (car) INCREMENT_REFCNT(car);
	CDR(s) = CHK(cdr); if (cdr) INCREMENT_REFCNT(cdr);
	TAG(s) = R_NilValue;
	ATTRIB(s) = R_NilValue;
	heap_alloc_exit();
	return s;
    }
}

attribute_hidden SEXP CONS_NR(SEXP car, SEXP cdr)
{
    heap_alloc_enter();
    for (;;) {
	if (FORCE_GC || NO_FREE_NODES()) {
	    PROTECT(car);
	    PROTECT(cdr);
	    mtl_gc(0);
	    UNPROTECT(2);
	    if (NO_FREE_NODES())
		mem_err_cons();
	    continue;
	}

	SEXP s = try_get_free_node(0);
	if (s == NULL) {
	    PROTECT(car);
	    PROTECT(cdr);
	    mtl_get_new_page(0);
	    UNPROTECT(2);
	    continue;
	}

	s->sxpinfo = UnmarkedNodeTemplate.sxpinfo;
	INIT_REFCNT(s);
	DISABLE_REFCNT(s);
	SET_TYPEOF(s, LISTSXP);
	CAR0(s) = CHK(car);
	CDR(s) = CHK(cdr);
	TAG(s) = R_NilValue;
	ATTRIB(s) = R_NilValue;
	heap_alloc_exit();
	return s;
    }
}

/*----------------------------------------------------------------------

  NewEnvironment

  Create an environment by extending "rho" with a frame obtained by
  pairing the variable names given by the tags on "namelist" with
  the values given by the elements of "valuelist".

  NewEnvironment is defined directly to avoid the need to protect its
  arguments unless a GC will actually occur.  This definition allows
  the namelist argument to be shorter than the valuelist; in this
  case the remaining values must be named already.  (This is useful
  in cases where the entire valuelist is already named--namelist can
  then be R_NilValue.)

  The valuelist is destructively modified and used as the
  environment's frame.
*/
SEXP NewEnvironment(SEXP namelist, SEXP valuelist, SEXP rho)
{
    heap_alloc_enter();
    for (;;) {
	if (FORCE_GC || NO_FREE_NODES()) {
	    PROTECT(namelist);
	    PROTECT(valuelist);
	    PROTECT(rho);
	    mtl_gc(0);
	    UNPROTECT(3);
	    if (NO_FREE_NODES())
		mem_err_cons();
	    continue;
	}

	SEXP newrho = try_get_free_node(0);
	if (newrho == NULL) {
	    PROTECT(namelist);
	    PROTECT(valuelist);
	    PROTECT(rho);
	    mtl_get_new_page(0);
	    UNPROTECT(3);
	    continue;
	}

	newrho->sxpinfo = UnmarkedNodeTemplate.sxpinfo;
	INIT_REFCNT(newrho);
	SET_TYPEOF(newrho, ENVSXP);
	FRAME(newrho) = valuelist; INCREMENT_REFCNT(valuelist);
	ENCLOS(newrho) = CHK(rho); if (rho != NULL) INCREMENT_REFCNT(rho);
	HASHTAB(newrho) = R_NilValue;
	ATTRIB(newrho) = R_NilValue;

	SEXP v = CHK(valuelist);
	SEXP n = CHK(namelist);
	while (v != R_NilValue && n != R_NilValue) {
	    SET_TAG(v, TAG(n));
	    v = CDR(v);
	    n = CDR(n);
	}

	heap_alloc_exit();
	return newrho;
    }
}

/* mkPROMISE is defined directly do avoid the need to protect its arguments
   unless a GC will actually occur. */
attribute_hidden SEXP mkPROMISE(SEXP expr, SEXP rho)
{
    heap_alloc_enter();
    for (;;) {
	if (FORCE_GC || NO_FREE_NODES()) {
	    PROTECT(expr);
	    PROTECT(rho);
	    mtl_gc(0);
	    UNPROTECT(2);
	    if (NO_FREE_NODES())
		mem_err_cons();
	    continue;
	}

	SEXP s = try_get_free_node(0);
	if (s == NULL) {
	    PROTECT(expr);
	    PROTECT(rho);
	    mtl_get_new_page(0);
	    UNPROTECT(2);
	    continue;
	}

	/* precaution to ensure code does not get modified via
	   substitute() and the like */
	ENSURE_NAMEDMAX(expr);

	s->sxpinfo = UnmarkedNodeTemplate.sxpinfo;
	INIT_REFCNT(s);
	SET_TYPEOF(s, PROMSXP);
	PRCODE(s) = CHK(expr); INCREMENT_REFCNT(expr);
	PRENV(s) = CHK(rho); INCREMENT_REFCNT(rho);
	PRVALUE0(s) = R_UnboundValue;
	PRSEEN(s) = 0;
	ATTRIB(s) = R_NilValue;
	heap_alloc_exit();
	return s;
    }
}

attribute_hidden /* would need to be in an installed header if not hidden */
SEXP R_mkEVPROMISE(SEXP expr, SEXP val)
{
    SEXP prom = mkPROMISE(expr, R_NilValue);
    SET_PRVALUE(prom, val);
    return prom;
}

attribute_hidden SEXP R_mkEVPROMISE_NR(SEXP expr, SEXP val)
{
    SEXP prom = mkPROMISE(expr, R_NilValue);
    DISABLE_REFCNT(prom);
    SET_PRVALUE(prom, val);
    return prom;
}

/* support for custom allocators that allow vectors to be allocated
   using non-standard means such as COW mmap() */

static void *custom_node_alloc(R_allocator_t *allocator, size_t size) {
    if (!allocator || !allocator->mem_alloc) return NULL;
    void *ptr = allocator->mem_alloc(allocator, size + sizeof(R_allocator_t));
    if (ptr) {
	R_allocator_t *ca = (R_allocator_t*) ptr;
	*ca = *allocator;
	return (void*) (ca + 1);
    }
    return NULL;
}

static void custom_node_free(void *ptr) {
    if (ptr) {
	R_allocator_t *allocator = ((R_allocator_t*) ptr) - 1;
	allocator->mem_free(allocator, (void*)allocator);
    }
}

/* All vector objects must be a multiple of sizeof(SEXPREC_ALIGN)
   bytes so that alignment is preserved for all objects */

/* Allocate a vector object (and also list-like objects).
   This ensures only validity of list-like (LISTSXP, VECSXP, EXPRSXP),
   STRSXP and CHARSXP types;  e.g., atomic types remain un-initialized
   and must be initialized upstream, e.g., in do_makevector().
*/
#define intCHARSXP 73

SEXP allocVector3(SEXPTYPE type, R_xlen_t length, R_allocator_t *allocator)
{
    if (type == NILSXP)
	return R_NilValue;

    heap_alloc_enter();

    SEXP s = NULL;  /* See comment in original code about VECSXP casts. */
    R_size_t size = 0, alloc_size = 0, old_R_VSize = 0;
    int node_class = 0;
#if VALGRIND_LEVEL > 0
    R_size_t actual_size = 0;
#endif

    /* Handle some scalars directly to improve speed. */
    if (length == 1) {
	switch(type) {
	case REALSXP:
	case INTSXP:
	case LGLSXP:
	    node_class = 1;
	    alloc_size = NodeClassSize[1];
	    for (;;) {
		if (FORCE_GC || NO_FREE_NODES() || VHEAP_FREE() < alloc_size) {
		    mtl_gc(alloc_size);
		    if (NO_FREE_NODES())
			mem_err_cons();
		    if (VHEAP_FREE() < alloc_size)
			mem_err_heap(size);
		    continue;
		}
		s = try_get_free_node(node_class);
		if (s == NULL) {
		    mtl_get_new_page(node_class);
		    continue;
		}
		break;
	    }
#if VALGRIND_LEVEL > 1
	    switch(type) {
	    case REALSXP: actual_size = sizeof(double); break;
	    case INTSXP: actual_size = sizeof(int); break;
	    case LGLSXP: actual_size = sizeof(int); break;
	    }
	    VALGRIND_MAKE_MEM_UNDEFINED(STDVEC_DATAPTR(s), actual_size);
#endif
	    s->sxpinfo = UnmarkedNodeTemplate.sxpinfo;
	    SETSCALAR(s, 1);
	    SET_NODE_CLASS(s, node_class);
	    SMALL_VALLOC_ADD(alloc_size);
	    /* Note that we do not include the header size into VallocSize,
	       but it is counted into memory usage via R_NodesInUse. */
	    ATTRIB(s) = R_NilValue;
	    SET_TYPEOF(s, type);
	    SET_STDVEC_LENGTH(s, (R_len_t) length); /* is 1 */
	    SET_STDVEC_TRUELENGTH(s, 0);
	    INIT_REFCNT(s);
	    heap_alloc_exit();
	    return s;
	default:
	    break;
	}
    }

    if (length > R_XLEN_T_MAX)
	error(_("cannot allocate vector of length %lld"), (long long)length);
    else if (length < 0 )
	error(_("negative length vectors are not allowed"));
    /* number of vector cells to allocate */
    switch (type) {
    case RAWSXP:
	size = BYTE2VEC(length);
#if VALGRIND_LEVEL > 0
	actual_size = length;
#endif
	break;
    case CHARSXP:
	error("use of allocVector(CHARSXP ...) is defunct\n");
    case intCHARSXP:
	type = CHARSXP;
	size = BYTE2VEC(length + 1);
#if VALGRIND_LEVEL > 0
	actual_size = length + 1;
#endif
	break;
    case LGLSXP:
    case INTSXP:
	if (length <= 0)
	    size = 0;
	else {
	    if (length > R_SIZE_T_MAX / sizeof(int))
		error(_("cannot allocate vector of length %lld"),
		      (long long)length);
	    size = INT2VEC(length);
#if VALGRIND_LEVEL > 0
	    actual_size = length*sizeof(int);
#endif
	}
	break;
    case REALSXP:
	if (length <= 0)
	    size = 0;
	else {
	    if (length > R_SIZE_T_MAX / sizeof(double))
		error(_("cannot allocate vector of length %lld"),
		      (long long)length);
	    size = FLOAT2VEC(length);
#if VALGRIND_LEVEL > 0
	    actual_size = length * sizeof(double);
#endif
	}
	break;
    case CPLXSXP:
	if (length <= 0)
	    size = 0;
	else {
	    if (length > R_SIZE_T_MAX / sizeof(Rcomplex))
		error(_("cannot allocate vector of length %lld"),
		      (long long)length);
	    size = COMPLEX2VEC(length);
#if VALGRIND_LEVEL > 0
	    actual_size = length * sizeof(Rcomplex);
#endif
	}
	break;
    case STRSXP:
    case EXPRSXP:
    case VECSXP:
	if (length <= 0)
	    size = 0;
	else {
	    if (length > R_SIZE_T_MAX / sizeof(SEXP))
		error(_("cannot allocate vector of length %lld"),
		      (long long)length);
	    size = PTR2VEC(length);
#if VALGRIND_LEVEL > 0
	    actual_size = length * sizeof(SEXP);
#endif
	}
	break;
    case LANGSXP:
	if (length == 0) {
	    heap_alloc_exit();
	    return R_NilValue;
	}
#ifdef LONG_VECTOR_SUPPORT
	if (length > R_SHORT_LEN_MAX) error("invalid length for pairlist");
#endif
	s = allocList((int) length);
	SET_TYPEOF(s, LANGSXP);
	heap_alloc_exit();
	return s;
    case LISTSXP:
#ifdef LONG_VECTOR_SUPPORT
	if (length > R_SHORT_LEN_MAX) error("invalid length for pairlist");
#endif
	s = allocList((int) length);
	heap_alloc_exit();
	return s;
    default:
	error(_("invalid type/length (%s/%lld) in vector allocation"),
	      type2char(type), (long long)length);
    }

    if (allocator) {
	node_class = CUSTOM_NODE_CLASS;
	alloc_size = size;
    } else {
	if (size <= NodeClassSize[1]) {
	    node_class = 1;
	    alloc_size = NodeClassSize[1];
	}
	else {
	    node_class = LARGE_NODE_CLASS;
	    alloc_size = size;
	    for (int i = 2; i < NUM_SMALL_NODE_CLASSES; i++) {
		if (size <= NodeClassSize[i]) {
		    node_class = i;
		    alloc_size = NodeClassSize[i];
		    break;
		}
	    }
	}
    }

    /* save current R_VSize to roll back adjustment if malloc fails */
    old_R_VSize = R_VSize;

    /* we need to do the gc here so allocSExp doesn't! */
    if (FORCE_GC || NO_FREE_NODES() || VHEAP_FREE() < alloc_size) {
	mtl_gc(alloc_size);
	if (NO_FREE_NODES())
	    mem_err_cons();
	if (VHEAP_FREE() < alloc_size)
	    mem_err_heap(size);
    }

    if (size > 0) {
	if (node_class < NUM_SMALL_NODE_CLASSES) {
	    for (;;) {
		s = try_get_free_node(node_class);
		if (s != NULL)
		    break;
		mtl_get_new_page(node_class);
	    }
#if VALGRIND_LEVEL > 1
	    VALGRIND_MAKE_MEM_UNDEFINED(STDVEC_DATAPTR(s), actual_size);
#endif
	    s->sxpinfo = UnmarkedNodeTemplate.sxpinfo;
	    INIT_REFCNT(s);
	    SET_NODE_CLASS(s, node_class);
	    SMALL_VALLOC_ADD(alloc_size);
	    SET_STDVEC_LENGTH(s, (R_len_t) length);
	}
	else {
	    Rboolean success = FALSE;
	    R_size_t hdrsize = sizeof(SEXPREC_ALIGN);
	    void *mem = NULL; /* initialize to suppress warning */
	    if (size < (R_SIZE_T_MAX / sizeof(VECREC)) - hdrsize) { /*** not sure this test is quite right -- why subtract the header? LT */
		/* I think subtracting the header is fine, "size" (*VSize)
		   variables do not count the header, but the header is
		   included into memory usage via NodesInUse, instead.
		   We want the whole object including the header to be
		   indexable by size_t. - TK */
		mem = allocator ?
		    custom_node_alloc(allocator, hdrsize + size * sizeof(VECREC)) :
		    malloc(hdrsize + size * sizeof(VECREC));
		if (mem == NULL) {
		    /* If we are near the address space limit, we
		       might be short of address space.  So return
		       all unused objects to malloc and try again. */
		    R_gc_no_finalizers(alloc_size);
		    mem = allocator ?
			custom_node_alloc(allocator, hdrsize + size * sizeof(VECREC)) :
			malloc(hdrsize + size * sizeof(VECREC));
		}
		if (mem != NULL) {
		    s = mem;
		    SET_STDVEC_LENGTH(s, length);
		    success = TRUE;
		}
		else s = NULL;
#ifdef R_MEMORY_PROFILING
		R_ReportAllocation(hdrsize + size * sizeof(VECREC));
#endif
	    } else s = NULL; /* suppress warning */
	    if (! success) {
		double dsize = (double)size * sizeof(VECREC)/1024.0;
		/* reset the vector heap limit */
		R_VSize = old_R_VSize;
		if(dsize > 1024.0*1024.0)
		    errorcall(R_NilValue,
			      _("cannot allocate vector of size %0.1f %s"),
			      dsize/1024.0/1024.0, "Gb");
		if(dsize > 1024.0)
		    errorcall(R_NilValue,
			      _("cannot allocate vector of size %0.1f %s"),
			      dsize/1024.0, "Mb");
		else
		    errorcall(R_NilValue,
			      _("cannot allocate vector of size %0.f %s"),
			      dsize, "Kb");
	    }
		    s->sxpinfo = UnmarkedNodeTemplate.sxpinfo;
		    INIT_REFCNT(s);
			    SET_NODE_CLASS(s, node_class);
			    /* Large vector nodes mutate the New list, so serialize that part. */
			    if (R_HEAP->isWorker) {
				if (!allocator) LARGE_VALLOC_ADD(size);
				R_GenHeap[node_class].AllocCount++;
				NODES_IN_USE_ADD(1);
				SNAP_NODE(s, R_GenHeap[node_class].New);
			    } else if (R_mtl_threading_active) {
				heap_alloc_suspend();
				R_mtl_heap_lock();
				if (!allocator) LARGE_VALLOC_ADD(size);
				R_GenHeap[node_class].AllocCount++;
				NODES_IN_USE_ADD(1);
				SNAP_NODE(s, R_GenHeap[node_class].New);
				R_mtl_heap_unlock();
				heap_alloc_resume();
			    } else {
				/* Serial main-thread fast path: no locks, no atomics. */
				if (!allocator) R_LargeVallocSize += size;
				R_GenHeap[node_class].AllocCount++;
				R_NodesInUse += 1;
				SNAP_NODE(s, R_GenHeap[node_class].New);
			    }
			    /* Only worker heaps need explicit owner tracking for malloc nodes.
			       Main-heap ownership is the default (owner == NULL). */
			    if (R_HEAP->isWorker)
				mtl_large_owner_set(s, R_HEAP);
			}
		ATTRIB(s) = R_NilValue;
		SET_TYPEOF(s, type);
	    }
    else {
	GC_PROT(s = allocSExpNonCons(type));
	SET_STDVEC_LENGTH(s, (R_len_t) length);
    }
    SETALTREP(s, 0);
    SET_STDVEC_TRUELENGTH(s, 0);
    INIT_REFCNT(s);

    /* The following prevents disaster in the case */
    /* that an uninitialised string vector is marked */
    /* Direct assignment is OK since the node was just allocated and */
    /* so is at least as new as R_NilValue and R_BlankString */
    if (type == EXPRSXP || type == VECSXP) {
	SEXP *data = STRING_PTR(s);
#if VALGRIND_LEVEL > 1
	VALGRIND_MAKE_MEM_DEFINED(STRING_PTR(s), actual_size);
#endif
	for (R_xlen_t i = 0; i < length; i++)
	    data[i] = R_NilValue;
    }
    else if(type == STRSXP) {
	SEXP *data = STRING_PTR(s);
#if VALGRIND_LEVEL > 1
	VALGRIND_MAKE_MEM_DEFINED(STRING_PTR(s), actual_size);
#endif
	for (R_xlen_t i = 0; i < length; i++)
	    data[i] = R_BlankString;
    }
    else if (type == CHARSXP || type == intCHARSXP) {
#if VALGRIND_LEVEL > 0
	VALGRIND_MAKE_MEM_UNDEFINED(CHAR(s), actual_size);
#endif
	CHAR_RW(s)[length] = 0;
    }
#if VALGRIND_LEVEL > 0
    else if (type == REALSXP)
	VALGRIND_MAKE_MEM_UNDEFINED(REAL(s), actual_size);
    else if (type == INTSXP)
	VALGRIND_MAKE_MEM_UNDEFINED(INTEGER(s), actual_size);
    else if (type == LGLSXP)
	VALGRIND_MAKE_MEM_UNDEFINED(LOGICAL(s), actual_size);
    else if (type == CPLXSXP)
	VALGRIND_MAKE_MEM_UNDEFINED(COMPLEX(s), actual_size);
    else if (type == RAWSXP)
	VALGRIND_MAKE_MEM_UNDEFINED(RAW(s), actual_size);
#endif
    heap_alloc_exit();
    return s;
}

/* For future hiding of allocVector(CHARSXP) */
attribute_hidden SEXP allocCharsxp(R_len_t len)
{
    return allocVector(intCHARSXP, len);
}

SEXP allocList(int n)
{
    int i;
    SEXP result;
    result = R_NilValue;
    for (i = 0; i < n; i++)
	result = CONS(R_NilValue, result);
    return result;
}

SEXP allocLang(int n)
{
    if (n > 0)
	return LCONS(R_NilValue, allocList(n - 1));
    else
	return R_NilValue;
}

SEXP allocS4Object(void)
{
   SEXP s;
   GC_PROT(s = allocSExpNonCons(OBJSXP));
   SET_S4_OBJECT(s);
   return s;
}

attribute_hidden SEXP R_allocObject(void)
{
   SEXP s;
   GC_PROT(s = allocSExpNonCons(OBJSXP));
   return s;
}

static SEXP allocFormalsList(int nargs, ...)
{
    SEXP res = R_NilValue;
    SEXP n;
    int i;
    va_list(syms);
    va_start(syms, nargs);

    for(i = 0; i < nargs; i++) {
	res = CONS(R_NilValue, res);
    }
    R_PreserveObject(res);

    n = res;
    for(i = 0; i < nargs; i++) {
	SET_TAG(n, (SEXP) va_arg(syms, SEXP));
	MARK_NOT_MUTABLE(n);
	n = CDR(n);
    }
    va_end(syms);

    return res;
}


attribute_hidden /* would need to be in an installed header if not hidden */
SEXP allocFormalsList2(SEXP sym1, SEXP sym2)
{
    return allocFormalsList(2, sym1, sym2);
}

attribute_hidden /* would need to be in an installed header if not hidden */
SEXP allocFormalsList3(SEXP sym1, SEXP sym2, SEXP sym3)
{
    return allocFormalsList(3, sym1, sym2, sym3);
}

attribute_hidden /* would need to be in an installed header if not hidden */
SEXP allocFormalsList4(SEXP sym1, SEXP sym2, SEXP sym3, SEXP sym4)
{
    return allocFormalsList(4, sym1, sym2, sym3, sym4);
}

attribute_hidden /* would need to be in an installed header if not hidden */
SEXP allocFormalsList5(SEXP sym1, SEXP sym2, SEXP sym3, SEXP sym4, SEXP sym5)
{
    return allocFormalsList(5, sym1, sym2, sym3, sym4, sym5);
}

attribute_hidden /* would need to be in an installed header if not hidden */
SEXP allocFormalsList6(SEXP sym1, SEXP sym2, SEXP sym3, SEXP sym4,
		       SEXP sym5, SEXP sym6)
{
    return allocFormalsList(6, sym1, sym2, sym3, sym4, sym5, sym6);
}

/* "gc" a mark-sweep or in-place generational garbage collector */

void R_gc(void)
{
    /* Worker interpreters have independent heaps and run a worker-local GC. */
    if (R_Interpreter != NULL && R_Interpreter->heap != NULL && R_HEAP->isWorker) {
	mtl_worker_gc(0);
	return;
    }

    num_old_gens_to_collect = NUM_OLD_GENERATIONS;
    heap_alloc_suspend();
    R_mtl_heap_lock();
    R_gc_internal(0);
    R_mtl_heap_unlock();
    heap_alloc_resume();
#ifndef IMMEDIATE_FINALIZERS
    R_RunPendingFinalizers();
#endif
}

void R_gc_lite(void)
{
    /* Worker interpreters have independent heaps and run a worker-local GC. */
    if (R_Interpreter != NULL && R_Interpreter->heap != NULL && R_HEAP->isWorker) {
	mtl_worker_gc(0);
	return;
    }

    heap_alloc_suspend();
    R_mtl_heap_lock();
    R_gc_internal(0);
    R_mtl_heap_unlock();
    heap_alloc_resume();
#ifndef IMMEDIATE_FINALIZERS
    R_RunPendingFinalizers();
#endif
}

static void R_gc_no_finalizers(R_size_t size_needed)
{
    /* Worker interpreters have independent heaps and run a worker-local GC. */
    if (R_Interpreter != NULL && R_Interpreter->heap != NULL && R_HEAP->isWorker) {
	mtl_worker_gc(size_needed);
	return;
    }

    num_old_gens_to_collect = NUM_OLD_GENERATIONS;
    heap_alloc_suspend();
    R_mtl_heap_lock();
    R_gc_internal(size_needed);
    R_mtl_heap_unlock();
    heap_alloc_resume();
}

static double gctimes[5], gcstarttimes[5];
static Rboolean gctime_enabled = FALSE;

/* this is primitive */
attribute_hidden SEXP do_gctime(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP ans;

    if (args == R_NilValue)
	gctime_enabled = TRUE;
    else {
	check1arg(args, call, "on");
	gctime_enabled = asRbool(CAR(args), call);
    }
    ans = allocVector(REALSXP, 5);
    REAL(ans)[0] = gctimes[0];
    REAL(ans)[1] = gctimes[1];
    REAL(ans)[2] = gctimes[2];
    REAL(ans)[3] = gctimes[3];
    REAL(ans)[4] = gctimes[4];
    return ans;
}

static void gc_start_timing(void)
{
    if (gctime_enabled)
	R_getProcTime(gcstarttimes);
}

static void gc_end_timing(void)
{
    if (gctime_enabled) {
	double times[5], delta;
	R_getProcTime(times);

	/* add delta to compensate for timer resolution */
#if 0
	/* this seems to over-compensate too */
	delta = R_getClockIncrement();
#else
	delta = 0;
#endif

	gctimes[0] += times[0] - gcstarttimes[0] + delta;
	gctimes[1] += times[1] - gcstarttimes[1] + delta;
	gctimes[2] += times[2] - gcstarttimes[2];
	gctimes[3] += times[3] - gcstarttimes[3];
	gctimes[4] += times[4] - gcstarttimes[4];
    }
}

#define R_MAX(a,b) (a) < (b) ? (b) : (a)

#ifdef THREADCHECK
# if !defined(Win32) && defined(HAVE_PTHREAD)
#   include <pthread.h>
attribute_hidden void R_check_thread(const char *s)
{
    static Rboolean main_thread_inited = FALSE;
    static pthread_t main_thread;
    if (! main_thread_inited) {
        main_thread = pthread_self();
        main_thread_inited = TRUE;
    }
    if (! pthread_equal(main_thread, pthread_self())) {
        char buf[1024];
	size_t bsize = sizeof buf;
	memset(buf, 0, bsize);
        snprintf(buf, bsize - 1, "Wrong thread calling '%s'", s);
        R_Suicide(buf);
    }
}
# else
/* This could be implemented for Windows using their threading API */
attribute_hidden void R_check_thread(const char *s) {}
# endif
#endif

static void R_gc_internal(R_size_t size_needed)
{
    R_CHECK_THREAD;
    if (!R_GCEnabled || R_in_gc) {
      if (R_in_gc)
        gc_error("*** recursive gc invocation\n");
      if (NO_FREE_NODES()) {
	  /* GC is disabled (e.g. during R_expand_binding_value()) but an
	     allocation path still needs a few nodes.  Growing by 1 can lead
	     to pathological GC-thrash on workloads that expand many bindings
	     while close to the node limit (e.g. package installs).  */
	  R_size_t grow =
	      (R_size_t) (R_NGrowIncrMin + R_NGrowIncrFrac * (double) R_NSize);
	  if (grow < 1000) grow = 1000;
	  R_size_t target = R_NodesInUse + grow;
	  if (target < R_NodesInUse + 1) /* overflow paranoia */
	      target = R_NodesInUse + 1;
	  if (R_MaxNSize < R_SIZE_T_MAX && target > R_MaxNSize)
	      target = R_MaxNSize;
	  R_NSize = target;
      }

      if (num_old_gens_to_collect < NUM_OLD_GENERATIONS &&
	  VHEAP_FREE() < size_needed + R_MinFreeFrac * R_VSize)
	num_old_gens_to_collect++;

      if (size_needed > VHEAP_FREE()) {
	  R_size_t expand = size_needed - VHEAP_FREE();
	  if (R_VSize + expand > R_MaxVSize)
	      mem_err_heap(size_needed);
	  R_VSize += expand;
      }

      /* Keep the main heap's trigger sizes in sync if we adjusted them while
         GC was disabled/in progress. */
      if (R_Interpreter != NULL && R_Interpreter->heap != NULL && !R_HEAP->isWorker) {
	  R_NSize_heap = R_NSize;
	  R_VSize_heap = R_VSize;
      }

      gc_pending = TRUE;
      return;
    }
    gc_pending = FALSE;

    R_size_t onsize = R_NSize /* can change during collection */;
    double ncells, vcells, vfrac, nfrac;
    SEXPTYPE first_bad_sexp_type = 0;
#ifdef PROTECTCHECK
    SEXPTYPE first_bad_sexp_type_old_type = 0;
#endif
    SEXP first_bad_sexp_type_sexp = NULL;
    int first_bad_sexp_type_line = 0;
    int gens_collected = 0;

#ifdef IMMEDIATE_FINALIZERS
    Rboolean first = TRUE;
 again:
#endif

    gc_count++;

    R_N_maxused = R_MAX(R_N_maxused, R_NodesInUse);
    R_V_maxused = R_MAX(R_V_maxused, R_VSize - VHEAP_FREE());

    BEGIN_SUSPEND_INTERRUPTS {
	R_in_gc = TRUE;
	gc_start_timing();
	gens_collected = RunGenCollect(size_needed);
	gc_end_timing();
	R_in_gc = FALSE;
    } END_SUSPEND_INTERRUPTS;

    if (R_check_constants > 2 ||
	    (R_check_constants > 1 && gens_collected == NUM_OLD_GENERATIONS))
	R_checkConstants(TRUE);

    if (gc_reporting) {
	REprintf("Garbage collection %d = %d", gc_count, gen_gc_counts[0]);
	for (int i = 0; i < NUM_OLD_GENERATIONS; i++)
	    REprintf("+%d", gen_gc_counts[i + 1]);
	REprintf(" (level %d) ... ", gens_collected);
	DEBUG_GC_SUMMARY(gens_collected == NUM_OLD_GENERATIONS);
    }

    if (bad_sexp_type_seen != 0 && first_bad_sexp_type == 0) {
	first_bad_sexp_type = bad_sexp_type_seen;
#ifdef PROTECTCHECK
	first_bad_sexp_type_old_type = bad_sexp_type_old_type;
#endif
	first_bad_sexp_type_sexp = bad_sexp_type_sexp;
	first_bad_sexp_type_line = bad_sexp_type_line;
    }

    if (gc_reporting) {
	ncells = onsize - R_Collected;
	nfrac = (100.0 * ncells) / R_NSize;
	/* We try to make this consistent with the results returned by gc */
	ncells = 0.1*ceil(10*ncells * sizeof(SEXPREC)/Mega);
	REprintf("\n%.1f %s of cons cells used (%d%%)\n",
		 ncells, "Mbytes", (int) (nfrac + 0.5));
	vcells = R_VSize - VHEAP_FREE();
	vfrac = (100.0 * vcells) / R_VSize;
	vcells = 0.1*ceil(10*vcells * vsfac/Mega);
	REprintf("%.1f %s of vectors used (%d%%)\n",
		 vcells, "Mbytes", (int) (vfrac + 0.5));
    }

#ifdef IMMEDIATE_FINALIZERS
    if (first) {
	first = FALSE;
	/* Run any eligible finalizers.  The return result of
	   RunFinalizers is TRUE if any finalizers are actually run.
	   There is a small chance that running finalizers here may
	   chew up enough memory to make another immediate collection
	   necessary.  If so, we jump back to the beginning and run
	   the collection, but on this second pass we do not run
	   finalizers. */
	if (RunFinalizers() &&
	    (NO_FREE_NODES() || size_needed > VHEAP_FREE()))
	    goto again;
    }
#endif

    if (first_bad_sexp_type != 0) {
	char msg[256];
#ifdef PROTECTCHECK
	if (first_bad_sexp_type == FREESXP)
	    snprintf(msg, 256,
	          "GC encountered a node (%p) with type FREESXP (was %s)"
		  " at memory.c:%d",
		  (void *) first_bad_sexp_type_sexp,
		  sexptype2char(first_bad_sexp_type_old_type),
		  first_bad_sexp_type_line);
	else
	    snprintf(msg, 256,
		     "GC encountered a node (%p) with an unknown SEXP type: %d"
		     " at memory.c:%d",
		     (void *) first_bad_sexp_type_sexp,
		     first_bad_sexp_type,
		     first_bad_sexp_type_line);
#else
	snprintf(msg, 256,
		 "GC encountered a node (%p) with an unknown SEXP type: %d"
		 " at memory.c:%d",
		 (void *)first_bad_sexp_type_sexp,
		 first_bad_sexp_type,
		 first_bad_sexp_type_line);
	gc_error(msg);
#endif
    }

    /* sanity check on logical scalar values */
    if (R_TrueValue != NULL && LOGICAL(R_TrueValue)[0] != TRUE) {
	LOGICAL(R_TrueValue)[0] = TRUE;
	gc_error("internal TRUE value has been modified");
    }
    if (R_FalseValue != NULL && LOGICAL(R_FalseValue)[0] != FALSE) {
	LOGICAL(R_FalseValue)[0] = FALSE;
	gc_error("internal FALSE value has been modified");
    }
    if (R_LogicalNAValue != NULL &&
	LOGICAL(R_LogicalNAValue)[0] != NA_LOGICAL) {
	LOGICAL(R_LogicalNAValue)[0] = NA_LOGICAL;
	gc_error("internal logical NA value has been modified");
    }
}


attribute_hidden SEXP do_memoryprofile(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP ans, nms;
    int i, tmp;

    checkArity(op, args);
    PROTECT(ans = allocVector(INTSXP, 24));
    PROTECT(nms = allocVector(STRSXP, 24));
    for (i = 0; i < 24; i++) {
	INTEGER(ans)[i] = 0;
	SET_STRING_ELT(nms, i, type2str(i > LGLSXP? i+2 : i));
    }
    setAttrib(ans, R_NamesSymbol, nms);

    BEGIN_SUSPEND_INTERRUPTS {
      int gen;

      /* run a full GC to make sure that all stuff in use is in Old space */
      R_gc();
      for (gen = 0; gen < NUM_OLD_GENERATIONS; gen++) {
	for (i = 0; i < NUM_NODE_CLASSES; i++) {
	  SEXP s;
	  for (s = NEXT_NODE(R_GenHeap[i].Old[gen]);
	       s != R_GenHeap[i].Old[gen];
	       s = NEXT_NODE(s)) {
	      tmp = TYPEOF(s);
	      if(tmp > LGLSXP) tmp -= 2;
	      INTEGER(ans)[tmp]++;
	  }
	}
      }
    } END_SUSPEND_INTERRUPTS;
    UNPROTECT(2);
    return ans;
}

/* "protect" push a single argument onto R_PPStack */

/* In handling a stack overflow we have to be careful not to use
   PROTECT. error("protect(): stack overflow") would call deparse1,
   which uses PROTECT and segfaults.*/

/* However, the traceback creation in the normal error handler also
   does a PROTECT, as does the jumping code, at least if there are
   cleanup expressions to handle on the way out.  So for the moment
   we'll allocate a slightly larger PP stack and only enable the added
   red zone during handling of a stack overflow error.  LT */

static void reset_pp_stack(void *data)
{
    int *poldpps = data;
    R_PPStackSize =  *poldpps;
}

NORET void R_signal_protect_error(void)
{
    RCNTXT cntxt;
    int oldpps = R_PPStackSize;

    begincontext(&cntxt, CTXT_CCODE, R_NilValue, R_BaseEnv, R_BaseEnv,
		 R_NilValue, R_NilValue);
    cntxt.cend = &reset_pp_stack;
    cntxt.cenddata = &oldpps;

    /* condition is pre-allocated and protected with R_PreserveObject */
    SEXP cond = R_getProtectStackOverflowError();

    if (R_PPStackSize < R_RealPPStackSize) {
	R_PPStackSize = R_RealPPStackSize;
	/* allow calling handlers */
	R_signalErrorCondition(cond, R_NilValue);
    }

    /* calling handlers at this point might produce a C stack
       overflow/SEGFAULT so treat them as failed and skip them */
    R_signalErrorConditionEx(cond, R_NilValue, TRUE);

    endcontext(&cntxt); /* not reached */
}

NORET void R_signal_unprotect_error(void)
{
    error(ngettext("unprotect(): only %d protected item",
		   "unprotect(): only %d protected items", R_PPStackTop),
	  R_PPStackTop);
}

#ifndef INLINE_PROTECT
SEXP protect(SEXP s)
{
    R_CHECK_THREAD;
    if (R_PPStackTop >= R_PPStackSize)
	R_signal_protect_error();
    R_PPStack[R_PPStackTop++] = CHK(s);
    return s;
}


/* "unprotect" pop argument list from top of R_PPStack */

void unprotect(int l)
{
    R_CHECK_THREAD;
    if (R_PPStackTop >=  l)
	R_PPStackTop -= l;
    else R_signal_unprotect_error();
}
#endif

/* "unprotect_ptr" remove pointer from somewhere in R_PPStack */

void unprotect_ptr(SEXP s)
{
    R_CHECK_THREAD;
    int i = R_PPStackTop;

    /* go look for  s  in  R_PPStack */
    /* (should be among the top few items) */
    do {
	if (i == 0)
	    error(_("unprotect_ptr: pointer not found"));
    } while ( R_PPStack[--i] != s );

    /* OK, got it, and  i  is indexing its location */
    /* Now drop stack above it, if any */

    while (++i < R_PPStackTop) R_PPStack[i - 1] = R_PPStack[i];

    R_PPStackTop--;
}

/* Debugging function:  is s protected? */

attribute_hidden int Rf_isProtected(SEXP s)
{
    R_CHECK_THREAD;
    int i = R_PPStackTop;

    /* go look for  s  in  R_PPStack */
    do {
	if (i == 0)
	    return(i);
    } while ( R_PPStack[--i] != s );

    /* OK, got it, and  i  is indexing its location */
    return(i);
}


#ifndef INLINE_PROTECT
void R_ProtectWithIndex(SEXP s, PROTECT_INDEX *pi)
{
    protect(s);
    *pi = R_PPStackTop - 1;
}
#endif

NORET void R_signal_reprotect_error(PROTECT_INDEX i)
{
    error(ngettext("R_Reprotect: only %d protected item, can't reprotect index %d",
		   "R_Reprotect: only %d protected items, can't reprotect index %d",
		   R_PPStackTop),
	  R_PPStackTop, i);
}

#ifndef INLINE_PROTECT
void R_Reprotect(SEXP s, PROTECT_INDEX i)
{
    R_CHECK_THREAD;
    if (i >= R_PPStackTop || i < 0)
	R_signal_reprotect_error(i);
    R_PPStack[i] = s;
}
#endif

#ifdef UNUSED
/* remove all objects from the protection stack from index i upwards
   and return them in a vector. The order in the vector is from new
   to old. */
SEXP R_CollectFromIndex(PROTECT_INDEX i)
{
    R_CHECK_THREAD;
    SEXP res;
    int top = R_PPStackTop, j = 0;
    if (i > top) i = top;
    res = protect(allocVector(VECSXP, top - i));
    while (i < top)
	SET_VECTOR_ELT(res, j++, R_PPStack[--top]);
    R_PPStackTop = top; /* this includes the protect we used above */
    return res;
}
#endif

/* "initStack" initialize environment stack */
attribute_hidden
void initStack(void)
{
    R_PPStackTop = 0;
}


/* S-like wrappers for calloc, realloc and free that check for error
   conditions */

void *R_chk_calloc(size_t nelem, size_t elsize)
{
    void *p;
#ifndef HAVE_WORKING_CALLOC
    if(nelem == 0)
	return(NULL);
#endif
    p = calloc(nelem, elsize);
    if(!p)
	error(_("'R_Calloc' could not allocate memory (%llu of %llu bytes)"),
	      (unsigned long long)nelem, (unsigned long long)elsize);
    return(p);
}

void *R_chk_realloc(void *ptr, size_t size)
{
    void *p;
    /* Protect against broken realloc */
    if(ptr) p = realloc(ptr, size); else p = malloc(size);
    if(!p)
	error(_("'R_Realloc' could not re-allocate memory (%llu bytes)"),
	      (unsigned long long)size);
    return(p);
}

void R_chk_free(void *ptr)
{
    /* S-PLUS warns here, but there seems no reason to do so */
    /* if(!ptr) warning("attempt to free NULL pointer by Free"); */
    if(ptr) free(ptr); /* ANSI C says free has no effect on NULL, but
			  better to be safe here */
}

void *R_chk_memcpy(void *dest, const void *src, size_t n)
{
    if (n >= PTRDIFF_MAX)
	error(_("object is too large (%llu bytes)"), (unsigned long long)n);
    return n ? memcpy(dest, src, n) : dest;
}

void *R_chk_memset(void *s, int c, size_t n)
{
    if (n >= PTRDIFF_MAX)
	error(_("object is too large (%llu bytes)"), (unsigned long long)n);
    return n ? memset(s, c, n) : s;
}

/* This code keeps a list of objects which are not assigned to variables
   but which are required to persist across garbage collections.  The
   objects are registered with R_PreserveObject and deregistered with
   R_ReleaseObject. */

static SEXP DeleteFromList(SEXP object, SEXP list)
{
    if (CAR(list) == object)
	return CDR(list);
    else {
	SEXP last = list;
	for (SEXP head = CDR(list); head != R_NilValue; head = CDR(head)) {
	    if (CAR(head) == object) {
		SETCDR(last, CDR(head));
		return list;
	    }
	    else last = head;
	}
	return list;
    }
}

#define ALLOW_PRECIOUS_HASH
#ifdef ALLOW_PRECIOUS_HASH
/* This allows using a fixed size hash table. This makes deleting much
   more efficient for applications that don't follow the "sparing use"
   advice in R-exts.texi. Using the hash table is enabled by starting
   R with the environment variable R_HASH_PRECIOUS set.

   Pointer hashing as used here isn't entirely portable (we do it in
   at least one other place, in serialize.c) but it could be made so
   by computing a unique value based on the allocation page and
   position in the page. */

#define PHASH_SIZE 1069
#define PTRHASH(obj) (((R_size_t) (obj)) >> 3)

#ifdef HAVE_PTHREAD
/* R_PreserveObject/R_ReleaseObject can be called from mtlapply() worker
   threads, so initialization of the precious-hash mode must be thread-safe. */
static atomic_int precious_init_state = 0; /* 0=uninit, 1=initing, 2=done */
static atomic_int use_precious_hash = 0;

static R_INLINE void precious_hash_init_once(void)
{
    int st = atomic_load_explicit(&precious_init_state, memory_order_acquire);
    if (st == 2)
	return;
    if (st == 0) {
	int expected = 0;
	if (atomic_compare_exchange_strong_explicit(&precious_init_state, &expected, 1,
						   memory_order_acq_rel, memory_order_acquire)) {
	    if (getenv("R_HASH_PRECIOUS"))
		atomic_store_explicit(&use_precious_hash, 1, memory_order_release);
	    atomic_store_explicit(&precious_init_state, 2, memory_order_release);
	    return;
	}
    }
    while (atomic_load_explicit(&precious_init_state, memory_order_acquire) != 2) {
	/* spin: initialization is fast and happens at most once */
    }
}
#else
static int use_precious_hash = FALSE;
static int precious_inited = FALSE;
#endif

void R_PreserveObject(SEXP object)
{
    R_CHECK_THREAD;
#ifdef HAVE_PTHREAD
    precious_hash_init_once();
    int use_hash = atomic_load_explicit(&use_precious_hash, memory_order_acquire);
#else
    if (! precious_inited) {
	precious_inited = TRUE;
	if (getenv("R_HASH_PRECIOUS"))
	    use_precious_hash = TRUE;
    }
    int use_hash = use_precious_hash;
#endif
    if (use_hash) {
	if (R_PreciousList == R_NilValue)
	    R_PreciousList = allocVector(VECSXP, PHASH_SIZE);
	int bin = PTRHASH(object) % PHASH_SIZE;
	SET_VECTOR_ELT(R_PreciousList, bin,
		       CONS(object, VECTOR_ELT_0(R_PreciousList, bin)));
    }
    else
	R_PreciousList = CONS(object, R_PreciousList);
}

void R_ReleaseObject(SEXP object)
{
    R_CHECK_THREAD;
    /* When HASH mode is enabled it is process-wide, so init before checking. */
#ifdef HAVE_PTHREAD
    precious_hash_init_once();
    int use_hash = atomic_load_explicit(&use_precious_hash, memory_order_acquire);
#else
    int use_hash = use_precious_hash;
#endif
    if (R_PreciousList == R_NilValue)
	return; /* can't be anything to delete yet */
    if (use_hash) {
	int bin = PTRHASH(object) % PHASH_SIZE;
	SET_VECTOR_ELT(R_PreciousList, bin,
		       DeleteFromList(object,
				      VECTOR_ELT_0(R_PreciousList, bin)));
    }
    else
	R_PreciousList =  DeleteFromList(object, R_PreciousList);
}
#else
void R_PreserveObject(SEXP object)
{
    R_CHECK_THREAD;
    R_PreciousList = CONS(object, R_PreciousList);
}

void R_ReleaseObject(SEXP object)
{
    R_CHECK_THREAD;
    R_PreciousList =  DeleteFromList(object, R_PreciousList);
}
#endif


/* This code is similar to R_PreserveObject/R_ReleasObject, but objects are
   kept in a provided multi-set (which needs to be itself protected).
   When protected via PROTECT, the multi-set is automatically unprotected
   during long jump, and thus all its members are eventually reclaimed.
   These functions were introduced for parsers generated by bison, because
   one cannot instruct bison to use PROTECT/UNPROTECT when working with
   the stack of semantic values. */

/* Multi-set is defined by a triple (store, npreserved, initialSize)
     npreserved is the number of elements in the store (counting each instance
       of the same value)
     store is a VECSXP or R_NilValue
       when VECSXP, preserved values are stored at the beginning, filled up by
       R_NilValue
     initialSize is the size for the VECSXP to be allocated if preserving values
       while store is R_NilValue

    The representation is CONS(store, npreserved) with TAG()==initialSize
*/

/* Create new multi-set for protecting objects. initialSize may be zero
   (a hardcoded default is then used). */
SEXP R_NewPreciousMSet(int initialSize)
{
    SEXP npreserved, mset, isize;

    /* npreserved is modified in place */
    npreserved = allocVector(INTSXP, 1);
    SET_INTEGER_ELT(npreserved, 0, 0);
    PROTECT(mset = CONS(R_NilValue, npreserved));
    /* isize is not modified in place */
    if (initialSize < 0)
	error("'initialSize' must be non-negative");
    isize = ScalarInteger(initialSize);
    SET_TAG(mset, isize);
    UNPROTECT(1); /* mset */
    return mset;
}

static void checkMSet(SEXP mset)
{
    SEXP store = CAR(mset);
    SEXP npreserved = CDR(mset);
    SEXP isize = TAG(mset);
    if (/*MAYBE_REFERENCED(mset) ||*/
	((store != R_NilValue) &&
	 (TYPEOF(store) != VECSXP /*|| MAYBE_REFERENCED(store)*/)) ||
	(TYPEOF(npreserved) != INTSXP || XLENGTH(npreserved) != 1 /*||
	 MAYBE_REFERENCED(npreserved)*/) ||
	(TYPEOF(isize) != INTSXP || XLENGTH(isize) != 1))

	error("Invalid mset");
}

/* Add object to multi-set. The object will be protected as long as the
   multi-set is protected. */
void R_PreserveInMSet(SEXP x, SEXP mset)
{
    if (x == R_NilValue || isSymbol(x))
	return; /* no need to preserve */
    PROTECT(x);
    checkMSet(mset);
    SEXP store = CAR(mset);
    int *n = INTEGER(CDR(mset));
    if (store == R_NilValue) {
	R_xlen_t newsize = INTEGER_ELT(TAG(mset), 0);
	if (newsize == 0)
	    newsize = 4; /* default minimum size */
	store = allocVector(VECSXP, newsize);
	SETCAR(mset, store);
    }
    R_xlen_t size = XLENGTH(store);
    if (*n == size) {
	R_xlen_t newsize = 2 * size;
	if (newsize >= INT_MAX || newsize < size)
	    error("Multi-set overflow");
	SEXP newstore = PROTECT(allocVector(VECSXP, newsize));
	for(R_xlen_t i = 0; i < size; i++)
	    SET_VECTOR_ELT(newstore, i, VECTOR_ELT_0(store, i));
	SETCAR(mset, newstore);
	UNPROTECT(1); /* newstore */
	store = newstore;
    }
    UNPROTECT(1); /* x */
    SET_VECTOR_ELT(store, (*n)++, x);
}

/* Remove (one instance of) the object from the multi-set. If there is another
   instance of the object in the multi-set, it will still be protected. If there
   is no instance of the object, the function does nothing. */
void R_ReleaseFromMSet(SEXP x, SEXP mset)
{
    if (x == R_NilValue || isSymbol(x))
	return; /* not preserved */
    checkMSet(mset);
    SEXP store = CAR(mset);
    if (store == R_NilValue)
	return; /* not preserved */
    int *n = INTEGER(CDR(mset));
    for(R_xlen_t i = (*n) - 1; i >= 0; i--) {
	if (VECTOR_ELT_0(store, i) == x) {
	    for(;i < (*n) - 1; i++)
		SET_VECTOR_ELT(store, i, VECTOR_ELT_0(store, i + 1));
	    SET_VECTOR_ELT(store, i, R_NilValue);
	    (*n)--;
	    return;
	}
    }
    /* not preserved */
}

/* Release all objects from the multi-set, but the multi-set can be used for
   preserving more objects. */
attribute_hidden void R_ReleaseMSet(SEXP mset, int keepSize)
{
    checkMSet(mset);
    SEXP store = CAR(mset);
    if (store == R_NilValue)
	return; /* already empty */
    int *n = INTEGER(CDR(mset));
    if (XLENGTH(store) <= keepSize) {
	/* just free the entries */
	for(R_xlen_t i = 0; i < *n; i++)
	    SET_VECTOR_ELT(store, i, R_NilValue);
    } else
	SETCAR(mset, R_NilValue);
    *n = 0;
}

/* External Pointer Objects */
SEXP R_MakeExternalPtr(void *p, SEXP tag, SEXP prot)
{
    SEXP s = allocSExp(EXTPTRSXP);
    EXTPTR_PTR(s) = p;
    EXTPTR_PROT(s) = CHK(prot); if (prot) INCREMENT_REFCNT(prot);
    EXTPTR_TAG(s) = CHK(tag); if (tag) INCREMENT_REFCNT(tag);
    return s;
}

#define CHKEXTPTRSXP(x)							\
    if (TYPEOF(x) != EXTPTRSXP)						\
	error(_("%s: argument of type %s is not an external pointer"),	\
	      __func__, sexptype2char(TYPEOF(x)))

void *R_ExternalPtrAddr(SEXP s)
{
    CHKEXTPTRSXP(s);
    return EXTPTR_PTR(CHK(s));
}

SEXP R_ExternalPtrTag(SEXP s)
{
    CHKEXTPTRSXP(s);
    return CHK(EXTPTR_TAG(CHK(s)));
}

SEXP R_ExternalPtrProtected(SEXP s)
{
    CHKEXTPTRSXP(s);
    return CHK(EXTPTR_PROT(CHK(s)));
}

void R_ClearExternalPtr(SEXP s)
{
    CHKEXTPTRSXP(s);
    EXTPTR_PTR(s) = NULL;
}

void R_SetExternalPtrAddr(SEXP s, void *p)
{
    CHKEXTPTRSXP(s);
    EXTPTR_PTR(s) = p;
}

void R_SetExternalPtrTag(SEXP s, SEXP tag)
{
    CHKEXTPTRSXP(s);
    FIX_REFCNT(s, EXTPTR_TAG(s), tag);
    CHECK_OLD_TO_NEW(s, tag);
    EXTPTR_TAG(s) = tag;
}

void R_SetExternalPtrProtected(SEXP s, SEXP p)
{
    CHKEXTPTRSXP(s);
    FIX_REFCNT(s, EXTPTR_PROT(s), p);
    CHECK_OLD_TO_NEW(s, p);
    EXTPTR_PROT(s) = p;
}

/*
   Added to API in R 3.4.0.
   Work around casting issues: works where it is needed.
 */
typedef union {void *p; DL_FUNC fn;} fn_ptr;

SEXP R_MakeExternalPtrFn(DL_FUNC p, SEXP tag, SEXP prot)
{
    fn_ptr tmp;
    SEXP s = allocSExp(EXTPTRSXP);
    tmp.fn = p;
    EXTPTR_PTR(s) = tmp.p;
    EXTPTR_PROT(s) = CHK(prot); if (prot) INCREMENT_REFCNT(prot);
    EXTPTR_TAG(s) = CHK(tag); if (tag) INCREMENT_REFCNT(tag);
    return s;
}

DL_FUNC R_ExternalPtrAddrFn(SEXP s)
{
    CHKEXTPTRSXP(s);
    fn_ptr tmp;
    tmp.p =  EXTPTR_PTR(CHK(s));
    return tmp.fn;
}



/* The following functions are replacements for the accessor macros.
   They are used by code that does not have direct access to the
   internal representation of objects.  The replacement functions
   implement the write barrier. */

/* General Cons Cell Attributes */
SEXP (ATTRIB)(SEXP x) { return CHK(ATTRIB(CHK(x))); }
int (ANY_ATTRIB)(SEXP x) { return ANY_ATTRIB(CHK(x)); }
int (OBJECT)(SEXP x) { return OBJECT(CHK(x)); }
int (TYPEOF)(SEXP x) { return TYPEOF(CHK(x)); }
int (NAMED)(SEXP x) { return NAMED(CHK(x)); }
attribute_hidden int (RTRACE)(SEXP x) { return RTRACE(CHK(x)); }
int (LEVELS)(SEXP x) { return LEVELS(CHK(x)); }
int (REFCNT)(SEXP x) { return REFCNT(CHK(x)); }
attribute_hidden int (TRACKREFS)(SEXP x) { return TRACKREFS(CHK(x)); }
int (ALTREP)(SEXP x) { return ALTREP(CHK(x)); }
void (MARK_NOT_MUTABLE)(SEXP x) { MARK_NOT_MUTABLE(CHK(x)); }
int (MAYBE_SHARED)(SEXP x) { return MAYBE_SHARED(CHK(x)); }
int (NO_REFERENCES)(SEXP x) { return NO_REFERENCES(CHK(x)); }

// this is NOT a function version of the IS_SCALAR macro!
int (IS_SCALAR)(SEXP x, int type)
{
    return TYPEOF(CHK(x)) == type && XLENGTH(x) == 1;
}

attribute_hidden int (MARK)(SEXP x) { return MARK(CHK(x)); }
attribute_hidden
void (DECREMENT_REFCNT)(SEXP x) { DECREMENT_REFCNT(CHK(x)); }
attribute_hidden
void (INCREMENT_REFCNT)(SEXP x) { INCREMENT_REFCNT(CHK(x)); }
attribute_hidden
void (DISABLE_REFCNT)(SEXP x)  { DISABLE_REFCNT(CHK(x)); }
attribute_hidden
void (ENABLE_REFCNT)(SEXP x) { ENABLE_REFCNT(CHK(x)); }
attribute_hidden
int (ASSIGNMENT_PENDING)(SEXP x) { return ASSIGNMENT_PENDING(CHK(x)); }
attribute_hidden void (SET_ASSIGNMENT_PENDING)(SEXP x, int v)
{
    SET_ASSIGNMENT_PENDING(CHK(x), v);
}
attribute_hidden
int (IS_ASSIGNMENT_CALL)(SEXP x) { return IS_ASSIGNMENT_CALL(CHK(x)); }
attribute_hidden
void (MARK_ASSIGNMENT_CALL)(SEXP x) { MARK_ASSIGNMENT_CALL(CHK(x)); }

void (SET_ATTRIB)(SEXP x, SEXP v) {
    if(TYPEOF(v) != LISTSXP && TYPEOF(v) != NILSXP)
	error("value of 'SET_ATTRIB' must be a pairlist or NULL, not a '%s'",
	      R_typeToChar(v));
    FIX_REFCNT(x, ATTRIB(x), v);
    CHECK_OLD_TO_NEW(x, v);
    ATTRIB(x) = v;
}
void (SET_OBJECT)(SEXP x, int v) { SET_OBJECT(CHK(x), v); }
void (SET_NAMED)(SEXP x, int v)
{
#ifndef SWITCH_TO_REFCNT
    SET_NAMED(CHK(x), v);
#endif
}
attribute_hidden
void (SET_RTRACE)(SEXP x, int v) { SET_RTRACE(CHK(x), v); }
int (SETLEVELS)(SEXP x, int v) { return SETLEVELS(CHK(x), v); }
void DUPLICATE_ATTRIB(SEXP to, SEXP from) {
    SET_ATTRIB(CHK(to), duplicate(CHK(ATTRIB(CHK(from)))));
    SET_OBJECT(CHK(to), OBJECT(from));
    IS_S4_OBJECT(from) ?  SET_S4_OBJECT(to) : UNSET_S4_OBJECT(to);
}
void SHALLOW_DUPLICATE_ATTRIB(SEXP to, SEXP from) {
    SET_ATTRIB(CHK(to), shallow_duplicate(CHK(ATTRIB(CHK(from)))));
    SET_OBJECT(CHK(to), OBJECT(from));
    IS_S4_OBJECT(from) ?  SET_S4_OBJECT(to) : UNSET_S4_OBJECT(to);
}
void CLEAR_ATTRIB(SEXP x)
{
    SET_ATTRIB(CHK(x), R_NilValue);
    SET_OBJECT(x, 0);
    UNSET_S4_OBJECT(x);
}

NORET static void bad_SET_TYPEOF(int from, int to)
{
    error(_("can't change type from %s to %s"),
	  sexptype2char(from), sexptype2char(to));
}

static void check_SET_TYPEOF(SEXP x, int v)
{
    if (ALTREP(x))
	error(_("can't change the type of an ALTREP object from %s to %s"),
	      sexptype2char(TYPEOF(x)), sexptype2char(v));
    switch (TYPEOF(x)) {
    case LISTSXP:
    case LANGSXP:
    case DOTSXP:
	if (BNDCELL_TAG(x))
	    error(_("can't change the type of a binding cell"));
	switch (v) {
	case LISTSXP:
	case LANGSXP:
	case DOTSXP:
	case BCODESXP: return;
	default: bad_SET_TYPEOF(TYPEOF(x), v);
	}
    case INTSXP:
    case LGLSXP:
	switch (v) {
	case INTSXP:
	case LGLSXP: return;
	default: bad_SET_TYPEOF(TYPEOF(x), v);
	}
    case VECSXP:
    case EXPRSXP:
	switch (v) {
	case VECSXP:
	case EXPRSXP: return;
	default: bad_SET_TYPEOF(TYPEOF(x), v);
	}
    default: bad_SET_TYPEOF(TYPEOF(x), v);
    }
}

void (SET_TYPEOF)(SEXP x, int v)
{
    /* Ideally this should not exist as a function outsie of base, but
       it was shown in WRE and is used in a good number of packages.
       So try to make it a little safer by only allowing some type
       changes.
    */
    if (TYPEOF(CHK(x)) != v) {
	check_SET_TYPEOF(x, v);
	SET_TYPEOF(CHK(x), v);
    }
}

attribute_hidden
void (ALTREP_SET_TYPEOF)(SEXP x, int v) { SET_TYPEOF(CHK(x), v); }

void (ENSURE_NAMEDMAX)(SEXP x) { ENSURE_NAMEDMAX(CHK(x)); }
attribute_hidden void (ENSURE_NAMED)(SEXP x) { ENSURE_NAMED(CHK(x)); }
attribute_hidden
void (SETTER_CLEAR_NAMED)(SEXP x) { SETTER_CLEAR_NAMED(CHK(x)); }
attribute_hidden
void (RAISE_NAMED)(SEXP x, int n) { RAISE_NAMED(CHK(x), n); }

/* S4 object testing */
int (IS_S4_OBJECT)(SEXP x){ return IS_S4_OBJECT(CHK(x)); }
void (SET_S4_OBJECT)(SEXP x){ SET_S4_OBJECT(CHK(x)); }
void (UNSET_S4_OBJECT)(SEXP x){ UNSET_S4_OBJECT(CHK(x)); }

/* JIT optimization support */
attribute_hidden int (NOJIT)(SEXP x) { return NOJIT(CHK(x)); }
attribute_hidden int (MAYBEJIT)(SEXP x) { return MAYBEJIT(CHK(x)); }
attribute_hidden void (SET_NOJIT)(SEXP x) { SET_NOJIT(CHK(x)); }
attribute_hidden void (SET_MAYBEJIT)(SEXP x) { SET_MAYBEJIT(CHK(x)); }
attribute_hidden void (UNSET_MAYBEJIT)(SEXP x) { UNSET_MAYBEJIT(CHK(x)); }

/* Growable vector support */
int (IS_GROWABLE)(SEXP x) { return IS_GROWABLE(CHK(x)); }
int (GROWABLE_BIT_SET)(SEXP x) { return GROWABLE_BIT_SET(CHK(x)); }
void (SET_GROWABLE_BIT)(SEXP x) { SET_GROWABLE_BIT(CHK(x)); }

static int nvec[32] = {
    1,1,1,1,1,1,1,1,
    1,0,0,1,1,0,0,0,
    0,1,1,0,0,1,1,0,
    0,1,1,1,1,1,1,1
};

static R_INLINE SEXP CHK2(SEXP x)
{
    x = CHK(x);
    if(nvec[TYPEOF(x)])
	error("LENGTH or similar applied to %s object", R_typeToChar(x));
    return x;
}

/* Vector Accessors */
int (LENGTH)(SEXP x) { return x == R_NilValue ? 0 : LENGTH(CHK2(x)); }
R_xlen_t (XLENGTH)(SEXP x) { return XLENGTH(CHK2(x)); }
R_xlen_t (TRUELENGTH)(SEXP x) { return TRUELENGTH(CHK2(x)); }

void (SETLENGTH)(SEXP x, R_xlen_t v)
{
    if (ALTREP(x))
	error("SETLENGTH() cannot be applied to an ALTVEC object.");
    if (! isVector(x))
	error(_("SETLENGTH() can only be applied to a standard vector, "
		"not a '%s'"), R_typeToChar(x));
    SET_STDVEC_LENGTH(CHK2(x), v);
}

void (SET_TRUELENGTH)(SEXP x, R_xlen_t v) { SET_TRUELENGTH(CHK2(x), v); }
int  (IS_LONG_VEC)(SEXP x) { return IS_LONG_VEC(CHK2(x)); }
#ifdef TESTING_WRITE_BARRIER
attribute_hidden
R_xlen_t (STDVEC_LENGTH)(SEXP x) { return STDVEC_LENGTH(CHK2(x)); }
attribute_hidden
R_xlen_t (STDVEC_TRUELENGTH)(SEXP x) { return STDVEC_TRUELENGTH(CHK2(x)); }
attribute_hidden void (SETALTREP)(SEXP x, int v) { SETALTREP(x, v); }
#endif

/* temporary, to ease transition away from remapping */
R_xlen_t Rf_XLENGTH(SEXP x) { return XLENGTH(CHK2(x)); }

const char *(R_CHAR)(SEXP x) {
    if(TYPEOF(x) != CHARSXP) // Han-Tak proposes to prepend  'x && '
	error("%s() can only be applied to a '%s', not a '%s'",
	      "CHAR", "CHARSXP", R_typeToChar(x));
    return (const char *) CHAR(CHK(x));
}

SEXP (STRING_ELT)(SEXP x, R_xlen_t i) {
    if(TYPEOF(x) != STRSXP)
	error("%s() can only be applied to a '%s', not a '%s'",
	      "STRING_ELT", "character vector", R_typeToChar(x));
    if (i < 0 || i >= XLENGTH(x))
	error(_("attempt access index %lld/%lld in STRING_ELT"),
	      (long long)i, (long long)XLENGTH(x));
    if (ALTREP(x))
	return CHK(ALTSTRING_ELT(CHK(x), i));
    else {
	SEXP *ps = STDVEC_DATAPTR(CHK(x));
	return CHK(ps[i]);
    }
}

SEXP (VECTOR_ELT)(SEXP x, R_xlen_t i) {
    /* We need to allow vector-like types here */
    if(TYPEOF(x) != VECSXP &&
       TYPEOF(x) != EXPRSXP &&
       TYPEOF(x) != WEAKREFSXP)
	error("%s() can only be applied to a '%s', not a '%s'",
	      "VECTOR_ELT", "list", R_typeToChar(x));
    if (i < 0 || i >= XLENGTH(x))
	error(_("attempt access index %lld/%lld in VECTOR_ELT"),
	      (long long)i, (long long)XLENGTH(x));
    if (ALTREP(x)) {
	SEXP ans = CHK(ALTLIST_ELT(CHK(x), i));
	/* the element is marked as not mutable since complex
	   assignment can't see reference counts on any intermediate
	   containers in an ALTREP */
	MARK_NOT_MUTABLE(ans);
        return ans;
    }
    else
        return CHK(VECTOR_ELT_0(CHK(x), i));
}

#ifdef CATCH_ZERO_LENGTH_ACCESS
/* Attempts to read or write elements of a zero length vector will
   result in a segfault, rather than read and write random memory.
   Returning NULL would be more natural, but Matrix seems to assume
   that even zero-length vectors have non-NULL data pointers, so
   return (void *) 1 instead. Zero-length CHARSXP objects still have a
   trailing zero byte so they are not handled. */
# define CHKZLN(x) do {						\
	if (STDVEC_LENGTH(CHK(x)) == 0 && TYPEOF(x) != CHARSXP) \
	    return (void *) 1;					\
    } while (0)
#else
# define CHKZLN(x) do { } while (0)
#endif

void *(STDVEC_DATAPTR)(SEXP x)
{
    if (ALTREP(x))
	error("cannot get STDVEC_DATAPTR from ALTREP object");
    if (! isVector(x) && TYPEOF(x) != WEAKREFSXP)
	error("STDVEC_DATAPTR can only be applied to a vector, not a '%s'",
	      R_typeToChar(x));
    CHKZLN(x);
    return STDVEC_DATAPTR(x);
}

/* nedded for implementing Dataptr ALTREP methods */
void *DATAPTR_RW(SEXP x) { return DATAPTR(x); }

int *(LOGICAL)(SEXP x) {
    if(TYPEOF(x) != LGLSXP)
	error("%s() can only be applied to a '%s', not a '%s'",
	      "LOGICAL",  "logical", R_typeToChar(x));
    CHKZLN(x);
    return LOGICAL(x);
}

const int *(LOGICAL_RO)(SEXP x) {
    if(TYPEOF(x) != LGLSXP)
	error("%s() can only be applied to a '%s', not a '%s'",
	      "LOGICAL",  "logical", R_typeToChar(x));
    CHKZLN(x);
    return LOGICAL_RO(x);
}

/* Maybe this should exclude logicals, but it is widely used */
int *(INTEGER)(SEXP x) {
    if(TYPEOF(x) != INTSXP && TYPEOF(x) != LGLSXP)
	error("%s() can only be applied to a '%s', not a '%s'",
	      "INTEGER", "integer", R_typeToChar(x));
    CHKZLN(x);
    return INTEGER(x);
}

const int *(INTEGER_RO)(SEXP x) {
    if(TYPEOF(x) != INTSXP && TYPEOF(x) != LGLSXP)
	error("%s() can only be applied to a '%s', not a '%s'",
	      "INTEGER", "integer", R_typeToChar(x));
    CHKZLN(x);
    return INTEGER_RO(x);
}

Rbyte *(RAW)(SEXP x) {
    if(TYPEOF(x) != RAWSXP)
	error("%s() can only be applied to a '%s', not a '%s'",
	      "RAW", "raw", R_typeToChar(x));
    CHKZLN(x);
    return RAW(x);
}

const Rbyte *(RAW_RO)(SEXP x) {
    if(TYPEOF(x) != RAWSXP)
	error("%s() can only be applied to a '%s', not a '%s'",
	      "RAW", "raw", R_typeToChar(x));
    CHKZLN(x);
    return RAW(x);
}

double *(REAL)(SEXP x) {
    if(TYPEOF(x) != REALSXP)
	error("%s() can only be applied to a '%s', not a '%s'",
	      "REAL", "numeric", R_typeToChar(x));
    CHKZLN(x);
    return REAL(x);
}

const double *(REAL_RO)(SEXP x) {
    if(TYPEOF(x) != REALSXP)
	error("%s() can only be applied to a '%s', not a '%s'",
	      "REAL", "numeric", R_typeToChar(x));
    CHKZLN(x);
    return REAL_RO(x);
}

Rcomplex *(COMPLEX)(SEXP x) {
    if(TYPEOF(x) != CPLXSXP)
	error("%s() can only be applied to a '%s', not a '%s'",
	      "COMPLEX", "complex", R_typeToChar(x));
    CHKZLN(x);
    return COMPLEX(x);
}

const Rcomplex *(COMPLEX_RO)(SEXP x) {
    if(TYPEOF(x) != CPLXSXP)
	error("%s() can only be applied to a '%s', not a '%s'",
	      "COMPLEX", "complex", R_typeToChar(x));
    CHKZLN(x);
    return COMPLEX_RO(x);
}

SEXP *(STRING_PTR)(SEXP x) {
    if(TYPEOF(x) != STRSXP)
	error("%s() can only be applied to a '%s', not a '%s'",
	      "STRING_PTR", "character", R_typeToChar(x));
    CHKZLN(x);
    return STRING_PTR(x);
}

const SEXP *(STRING_PTR_RO)(SEXP x) {
    if(TYPEOF(x) != STRSXP)
	error("%s() can only be applied to a '%s', not a '%s'",
	      __func__, "character", R_typeToChar(x));
    CHKZLN(x);
    return STRING_PTR_RO(x);
}

NORET SEXP * (VECTOR_PTR)(SEXP x)
{
  error(_("not safe to return vector pointer"));
}

const SEXP *(VECTOR_PTR_RO)(SEXP x) {
    if(TYPEOF(x) != VECSXP)
	error("%s() can only be applied to a '%s', not a '%s'",
	      __func__, "list", R_typeToChar(x));
    CHKZLN(x);
    return VECTOR_PTR_RO(x);
}

void (SET_STRING_ELT)(SEXP x, R_xlen_t i, SEXP v) {
    if(TYPEOF(CHK(x)) != STRSXP)
	error("%s() can only be applied to a '%s', not a '%s'",
	      "SET_STRING_ELT", "character vector", R_typeToChar(x));
    if(TYPEOF(CHK(v)) != CHARSXP)
       error("Value of SET_STRING_ELT() must be a 'CHARSXP' not a '%s'",
	     R_typeToChar(v));
    if (i < 0 || i >= XLENGTH(x))
	error(_("attempt to set index %lld/%lld in SET_STRING_ELT"),
	      (long long)i, (long long)XLENGTH(x));
    CHECK_OLD_TO_NEW(x, v);
    if (ALTREP(x))
	ALTSTRING_SET_ELT(x, i, v);
    else {
	SEXP *ps = STDVEC_DATAPTR(x);
	FIX_REFCNT(x, ps[i], v);
	ps[i] = v;
    }
}

SEXP (SET_VECTOR_ELT)(SEXP x, R_xlen_t i, SEXP v) {
    /*  we need to allow vector-like types here */
    if(TYPEOF(x) != VECSXP &&
       TYPEOF(x) != EXPRSXP &&
       TYPEOF(x) != WEAKREFSXP) {
	error("%s() can only be applied to a '%s', not a '%s'",
	      "SET_VECTOR_ELT", "list", R_typeToChar(x));
    }
    if (i < 0 || i >= XLENGTH(x))
	error(_("attempt to set index %lld/%lld in SET_VECTOR_ELT"),
	      (long long)i, (long long)XLENGTH(x));
    if (ALTREP(x))
        ALTLIST_SET_ELT(x, i, v);
    else {
        FIX_REFCNT(x, VECTOR_ELT_0(x, i), v);
        CHECK_OLD_TO_NEW(x, v);
        SET_VECTOR_ELT_0(x, i, v);
    }
    return v;
}

/* check for a CONS-like object */
#ifdef TESTING_WRITE_BARRIER
static R_INLINE SEXP CHKCONS(SEXP e)
{
    if (ALTREP(e))
	return CHK(e);
    switch (TYPEOF(e)) {
    case LISTSXP:
    case LANGSXP:
    case NILSXP:
    case DOTSXP:
    case CLOSXP:    /**** use separate accessors? */
    case BCODESXP:  /**** use separate accessors? */
    case ENVSXP:    /**** use separate accessors? */
    case PROMSXP:   /**** use separate accessors? */
    case EXTPTRSXP: /**** use separate accessors? */
	return CHK(e);
    default:
	error("CAR/CDR/TAG or similar applied to %s object",
	      R_typeToChar(e));
    }
}
#else
#define CHKCONS(e) CHK(e)
#endif

attribute_hidden
int (BNDCELL_TAG)(SEXP cell) { return BNDCELL_TAG(cell); }
attribute_hidden
void (SET_BNDCELL_TAG)(SEXP cell, int val) { SET_BNDCELL_TAG(cell, val); }
attribute_hidden
double (BNDCELL_DVAL)(SEXP cell) { return BNDCELL_DVAL(cell); }
attribute_hidden
int (BNDCELL_IVAL)(SEXP cell) { return BNDCELL_IVAL(cell); }
attribute_hidden
int (BNDCELL_LVAL)(SEXP cell) { return BNDCELL_LVAL(cell); }
attribute_hidden
void (SET_BNDCELL_DVAL)(SEXP cell, double v) { SET_BNDCELL_DVAL(cell, v); }
attribute_hidden
void (SET_BNDCELL_IVAL)(SEXP cell, int v) { SET_BNDCELL_IVAL(cell, v); }
attribute_hidden
void (SET_BNDCELL_LVAL)(SEXP cell, int v) { SET_BNDCELL_LVAL(cell, v); }
attribute_hidden
void (INIT_BNDCELL)(SEXP cell, int type) { INIT_BNDCELL(cell, type); }
attribute_hidden
int (PROMISE_TAG)(SEXP cell) { return PROMISE_TAG(cell); }
attribute_hidden
void (SET_PROMISE_TAG)(SEXP cell, int val) { SET_PROMISE_TAG(cell, val); }

#define CLEAR_BNDCELL_TAG(cell) do {		\
	if (BNDCELL_TAG(cell)) {		\
	    CAR0(cell) = R_NilValue;		\
	    SET_BNDCELL_TAG(cell, 0);		\
	}					\
    } while (0)

attribute_hidden
void SET_BNDCELL(SEXP cell, SEXP val)
{
    CLEAR_BNDCELL_TAG(cell);
    SETCAR(cell, val);
}

attribute_hidden void R_expand_binding_value(SEXP b)
{
#if BOXED_BINDING_CELLS
    SET_BNDCELL_TAG(b, 0);
#else
    int enabled = R_GCEnabled;
    R_GCEnabled = FALSE;
    int typetag = BNDCELL_TAG(b);
    if (typetag) {
	union {
	    SEXP sxpval;
	    double dval;
	    int ival;
	} vv;
	SEXP val;
	vv.sxpval = CAR0(b);
	switch (typetag) {
	case REALSXP:
	    PROTECT(b);
	    val = ScalarReal(vv.dval);
	    SET_BNDCELL(b, val);
	    INCREMENT_NAMED(val);
	    UNPROTECT(1);
	    break;
	case INTSXP:
	    PROTECT(b);
	    val = ScalarInteger(vv.ival);
	    SET_BNDCELL(b, val);
	    INCREMENT_NAMED(val);
	    UNPROTECT(1);
	    break;
	case LGLSXP:
	    PROTECT(b);
	    val = ScalarLogical(vv.ival);
	    SET_BNDCELL(b, val);
	    INCREMENT_NAMED(val);
	    UNPROTECT(1);
	    break;
	}
    }
    R_GCEnabled = enabled;
#endif
}

#ifdef IMMEDIATE_PROMISE_VALUES
attribute_hidden SEXP R_expand_promise_value(SEXP x)
{
    if (PROMISE_TAG(x))
	R_expand_binding_value(x);
    return PRVALUE0(x);
}
#endif

attribute_hidden void R_args_enable_refcnt(SEXP args)
{
#ifdef SWITCH_TO_REFCNT
    /* args is escaping into user C code and might get captured, so
       make sure it is reference counting. Should be able to get rid
       of this function if we reduce use of CONS_NR. */
    for (SEXP a = args; a != R_NilValue; a = CDR(a))
	if (! TRACKREFS(a)) {
	    ENABLE_REFCNT(a);
	    INCREMENT_REFCNT(CAR(a));
	    INCREMENT_REFCNT(CDR(a));
#ifdef TESTING_WRITE_BARRIER
	    /* this should not see non-tracking arguments */
	    if (! TRACKREFS(CAR(a)))
		error("argument not tracking references");
#endif
	}
#endif
}

attribute_hidden void R_try_clear_args_refcnt(SEXP args)
{
#ifdef SWITCH_TO_REFCNT
    /* If args excapes properly its reference count will have been
       incremented. If it has no references, then it can be reverted
       to NR and the reference counts on its CAR and CDR can be
       decremented. */
    while (args != R_NilValue && NO_REFERENCES(args)) {
	SEXP next = CDR(args);
	DISABLE_REFCNT(args);
	DECREMENT_REFCNT(CAR(args));
	DECREMENT_REFCNT(CDR(args));
	args = next;
    }
#endif
}

/* List Accessors */
SEXP (TAG)(SEXP e) { return CHK(TAG(CHKCONS(e))); }
attribute_hidden SEXP (CAR0)(SEXP e) { return CHK(CAR0(CHKCONS(e))); }
SEXP (CDR)(SEXP e) { return CHK(CDR(CHKCONS(e))); }
SEXP (CAAR)(SEXP e) { return CHK(CAAR(CHKCONS(e))); }
SEXP (CDAR)(SEXP e) { return CHK(CDAR(CHKCONS(e))); }
SEXP (CADR)(SEXP e) { return CHK(CADR(CHKCONS(e))); }
SEXP (CDDR)(SEXP e) { return CHK(CDDR(CHKCONS(e))); }
SEXP (CDDDR)(SEXP e) { return CHK(CDDDR(CHKCONS(e))); }
SEXP (CADDR)(SEXP e) { return CHK(CADDR(CHKCONS(e))); }
SEXP (CADDDR)(SEXP e) { return CHK(CADDDR(CHKCONS(e))); }
SEXP (CAD4R)(SEXP e) { return CHK(CAD4R(CHKCONS(e))); }
SEXP (CAD5R)(SEXP e) { return CHK(CAD5R(CHKCONS(e))); }
attribute_hidden int (MISSING)(SEXP x) { return MISSING(CHKCONS(x)); }

void (SET_TAG)(SEXP x, SEXP v)
{
    if (CHKCONS(x) == NULL || x == R_NilValue)
	error(_("bad value"));
    FIX_REFCNT(x, TAG(x), v);
    CHECK_OLD_TO_NEW(x, v);
    TAG(x) = v;
}

SEXP (SETCAR)(SEXP x, SEXP y)
{
    if (CHKCONS(x) == NULL || x == R_NilValue)
	error(_("bad value"));
    CLEAR_BNDCELL_TAG(x);
    if (y == CAR(x))
	return y;
    FIX_BINDING_REFCNT(x, CAR(x), y);
    CHECK_OLD_TO_NEW(x, y);
    CAR0(x) = y;
    return y;
}

SEXP (SETCDR)(SEXP x, SEXP y)
{
    if (CHKCONS(x) == NULL || x == R_NilValue)
	error(_("bad value"));
    FIX_REFCNT(x, CDR(x), y);
#ifdef TESTING_WRITE_BARRIER
    /* this should not add a non-tracking CDR to a tracking cell */
    if (TRACKREFS(x) && y && ! TRACKREFS(y))
	error("inserting non-tracking CDR in tracking cell");
#endif
    CHECK_OLD_TO_NEW(x, y);
    CDR(x) = y;
    return y;
}

SEXP (SETCADR)(SEXP x, SEXP y)
{
    SEXP cell;
    if (CHKCONS(x) == NULL || x == R_NilValue ||
	CHKCONS(CDR(x)) == NULL || CDR(x) == R_NilValue)
	error(_("bad value"));
    cell = CDR(x);
    CLEAR_BNDCELL_TAG(cell);
    FIX_REFCNT(cell, CAR(cell), y);
    CHECK_OLD_TO_NEW(cell, y);
    CAR0(cell) = y;
    return y;
}

SEXP (SETCADDR)(SEXP x, SEXP y)
{
    SEXP cell;
    if (CHKCONS(x) == NULL || x == R_NilValue ||
	CHKCONS(CDR(x)) == NULL || CDR(x) == R_NilValue ||
	CHKCONS(CDDR(x)) == NULL || CDDR(x) == R_NilValue)
	error(_("bad value"));
    cell = CDDR(x);
    CLEAR_BNDCELL_TAG(cell);
    FIX_REFCNT(cell, CAR(cell), y);
    CHECK_OLD_TO_NEW(cell, y);
    CAR0(cell) = y;
    return y;
}

SEXP (SETCADDDR)(SEXP x, SEXP y)
{
    SEXP cell;
    if (CHKCONS(x) == NULL || x == R_NilValue ||
	CHKCONS(CDR(x)) == NULL || CDR(x) == R_NilValue ||
	CHKCONS(CDDR(x)) == NULL || CDDR(x) == R_NilValue ||
	CHKCONS(CDDDR(x)) == NULL || CDDDR(x) == R_NilValue)
	error(_("bad value"));
    cell = CDDDR(x);
    CLEAR_BNDCELL_TAG(cell);
    FIX_REFCNT(cell, CAR(cell), y);
    CHECK_OLD_TO_NEW(cell, y);
    CAR0(cell) = y;
    return y;
}

#define CD4R(x) CDR(CDR(CDR(CDR(x))))

SEXP (SETCAD4R)(SEXP x, SEXP y)
{
    SEXP cell;
    if (CHKCONS(x) == NULL || x == R_NilValue ||
	CHKCONS(CDR(x)) == NULL || CDR(x) == R_NilValue ||
	CHKCONS(CDDR(x)) == NULL || CDDR(x) == R_NilValue ||
	CHKCONS(CDDDR(x)) == NULL || CDDDR(x) == R_NilValue ||
	CHKCONS(CD4R(x)) == NULL || CD4R(x) == R_NilValue)
	error(_("bad value"));
    cell = CD4R(x);
    CLEAR_BNDCELL_TAG(cell);
    FIX_REFCNT(cell, CAR(cell), y);
    CHECK_OLD_TO_NEW(cell, y);
    CAR0(cell) = y;
    return y;
}

SEXP (EXTPTR_PROT)(SEXP x) { CHKEXTPTRSXP(x); return EXTPTR_PROT(CHK(x)); }
SEXP (EXTPTR_TAG)(SEXP x) { CHKEXTPTRSXP(x); return EXTPTR_TAG(CHK(x)); }
void *(EXTPTR_PTR)(SEXP x) { CHKEXTPTRSXP(x); return EXTPTR_PTR(CHK(x)); }

attribute_hidden
void (SET_MISSING)(SEXP x, int v) { SET_MISSING(CHKCONS(x), v); }

/* Closure Accessors */
/* some internals seem to depend on allowing a LISTSXP */
#define CHKCLOSXP(x) \
    if (TYPEOF(x) != CLOSXP && TYPEOF(x) != LISTSXP) \
	error(_("%s: argument of type %s is not a closure"), \
	      __func__, sexptype2char(TYPEOF(x)))
SEXP (FORMALS)(SEXP x) { CHKCLOSXP(x); return CHK(FORMALS(CHK(x))); }
SEXP (BODY)(SEXP x) { CHKCLOSXP(x); return CHK(BODY(CHK(x))); }
SEXP (CLOENV)(SEXP x) { CHKCLOSXP(x); return CHK(CLOENV(CHK(x))); }
int (RDEBUG)(SEXP x) { return RDEBUG(CHK(x)); }
attribute_hidden int (RSTEP)(SEXP x) { return RSTEP(CHK(x)); }
SEXP R_ClosureFormals(SEXP x) { return (FORMALS)(x); }
SEXP R_ClosureBody(SEXP x) { return (BODY)(x); }
SEXP R_ClosureEnv(SEXP x) { return (CLOENV)(x); }

void (SET_FORMALS)(SEXP x, SEXP v) { FIX_REFCNT(x, FORMALS(x), v); CHECK_OLD_TO_NEW(x, v); FORMALS(x) = v; }
void (SET_BODY)(SEXP x, SEXP v) { FIX_REFCNT(x, BODY(x), v); CHECK_OLD_TO_NEW(x, v); BODY(x) = v; }
void (SET_CLOENV)(SEXP x, SEXP v) { FIX_REFCNT(x, CLOENV(x), v); CHECK_OLD_TO_NEW(x, v); CLOENV(x) = v; }
void (SET_RDEBUG)(SEXP x, int v) { SET_RDEBUG(CHK(x), v); }
attribute_hidden
void (SET_RSTEP)(SEXP x, int v) { SET_RSTEP(CHK(x), v); }

/* These are only needed with the write barrier on */
#ifdef TESTING_WRITE_BARRIER
/* Primitive Accessors */
/* not hidden since needed in some base packages */
int (PRIMOFFSET)(SEXP x) { return PRIMOFFSET(CHK(x)); }
attribute_hidden
void (SET_PRIMOFFSET)(SEXP x, int v) { SET_PRIMOFFSET(CHK(x), v); }
#endif

/* Symbol Accessors */
/* looks like R_NilValue is also being passed to tome of these */
#define CHKSYMSXP(x) \
    if (x != R_NilValue && TYPEOF(x) != SYMSXP) \
	error(_("%s: argument of type %s is not a symbol or NULL"), \
	      __func__, sexptype2char(TYPEOF(x)))
SEXP (PRINTNAME)(SEXP x) { CHKSYMSXP(x); return CHK(PRINTNAME(CHK(x))); }
SEXP (SYMVALUE)(SEXP x) { CHKSYMSXP(x); return CHK(SYMVALUE(CHK(x))); }
SEXP (INTERNAL)(SEXP x) { CHKSYMSXP(x); return CHK(INTERNAL(CHK(x))); }
int (DDVAL)(SEXP x) { CHKSYMSXP(x); return DDVAL(CHK(x)); }

attribute_hidden
void (SET_PRINTNAME)(SEXP x, SEXP v) { FIX_REFCNT(x, PRINTNAME(x), v); CHECK_OLD_TO_NEW(x, v); PRINTNAME(x) = v; }

attribute_hidden
void (SET_SYMVALUE)(SEXP x, SEXP v)
{
    if (SYMVALUE(x) == v)
	return;
    FIX_BINDING_REFCNT(x, SYMVALUE(x), v);
    CHECK_OLD_TO_NEW(x, v);
    SYMVALUE(x) = v;
}

attribute_hidden
void (SET_INTERNAL)(SEXP x, SEXP v) {
    FIX_REFCNT(x, INTERNAL(x), v);
    CHECK_OLD_TO_NEW(x, v);
    INTERNAL(x) = v;
}
attribute_hidden void (SET_DDVAL)(SEXP x, int v) { SET_DDVAL(CHK(x), v); }

/* Environment Accessors */
/* looks like R_NilValue is still showing up in internals */
#define CHKENVSXP(x)						\
    if (TYPEOF(x) != ENVSXP && x != R_NilValue)				\
	error(_("%s: argument of type %s is not an environment or NULL"), \
	      __func__, sexptype2char(TYPEOF(x)))
SEXP (FRAME)(SEXP x) { CHKENVSXP(x); return CHK(FRAME(CHK(x))); }
SEXP (ENCLOS)(SEXP x) { CHKENVSXP(x); return CHK(ENCLOS(CHK(x))); }
SEXP (HASHTAB)(SEXP x) { CHKENVSXP(x); return CHK(HASHTAB(CHK(x))); }
int (ENVFLAGS)(SEXP x) { CHKENVSXP(x); return ENVFLAGS(CHK(x)); }
SEXP R_ParentEnv(SEXP x) { return (ENCLOS)(x); }

void (SET_FRAME)(SEXP x, SEXP v) { FIX_REFCNT(x, FRAME(x), v); CHECK_OLD_TO_NEW(x, v); FRAME(x) = v; }

void (SET_ENCLOS)(SEXP x, SEXP v)
{
    if (v == R_NilValue)
	/* mainly to handle unserializing old files */
	v = R_EmptyEnv;
    if (TYPEOF(v) != ENVSXP)
	error(_("'parent' is not an environment"));
    for (SEXP e = v; e != R_NilValue; e = ENCLOS(e))
	if (e == x)
	    error(_("cycles in parent chains are not allowed"));
    FIX_REFCNT(x, ENCLOS(x), v);
    CHECK_OLD_TO_NEW(x, v);
    ENCLOS(x) = v;
}

void (SET_HASHTAB)(SEXP x, SEXP v) { FIX_REFCNT(x, HASHTAB(x), v); CHECK_OLD_TO_NEW(x, v); HASHTAB(x) = v; }
void (SET_ENVFLAGS)(SEXP x, int v) { SET_ENVFLAGS(x, v); }

/* Promise Accessors */
SEXP (PRCODE)(SEXP x) { return CHK(PRCODE(CHK(x))); }
SEXP (PRENV)(SEXP x) { return CHK(PRENV(CHK(x))); }
SEXP (PRVALUE)(SEXP x) { return CHK(PRVALUE(CHK(x))); }
int (PRSEEN)(SEXP x) { return PRSEEN(CHK(x)); }
attribute_hidden
int (PROMISE_IS_EVALUATED)(SEXP x)
{
    x = CHK(x);
    return PROMISE_IS_EVALUATED(x);
}

void (SET_PRENV)(SEXP x, SEXP v){ FIX_REFCNT(x, PRENV(x), v); CHECK_OLD_TO_NEW(x, v); PRENV(x) = v; }
void (SET_PRCODE)(SEXP x, SEXP v) { FIX_REFCNT(x, PRCODE(x), v); CHECK_OLD_TO_NEW(x, v); PRCODE(x) = v; }
void (SET_PRSEEN)(SEXP x, int v) { SET_PRSEEN(CHK(x), v); }

void (SET_PRVALUE)(SEXP x, SEXP v)
{
    if (TYPEOF(x) != PROMSXP)
	error("expecting a 'PROMSXP', not a '%s'", R_typeToChar(x));
#ifdef IMMEDIATE_PROMISE_VALUES
    if (PROMISE_TAG(x)) {
	SET_PROMISE_TAG(x, 0);
	PRVALUE0(x) = R_UnboundValue;
    }
#endif
    FIX_REFCNT(x, PRVALUE0(x), v);
    CHECK_OLD_TO_NEW(x, v);
    PRVALUE0(x) = v;
}

attribute_hidden
void IF_PROMSXP_SET_PRVALUE(SEXP x, SEXP v)
{
    /* promiseArgs produces a list containing promises or R_MissingArg.
       Using IF_PROMSXP_SET_PRVALUE avoids corrupting R_MissingArg. */
    if (TYPEOF(x) == PROMSXP)
        SET_PRVALUE(x, v);
}

/* Hashing Accessors */
#ifdef TESTING_WRITE_BARRIER
attribute_hidden
int (HASHASH)(SEXP x) { return HASHASH(CHK(x)); }
attribute_hidden
int (HASHVALUE)(SEXP x) { return HASHVALUE(CHK(x)); }

attribute_hidden
void (SET_HASHASH)(SEXP x, int v) { SET_HASHASH(CHK(x), v); }
attribute_hidden
void (SET_HASHVALUE)(SEXP x, int v) { SET_HASHVALUE(CHK(x), v); }
#endif

attribute_hidden
SEXP (SET_CXTAIL)(SEXP x, SEXP v) {
#ifdef USE_TYPE_CHECKING
    if(TYPEOF(v) != CHARSXP && TYPEOF(v) != NILSXP)
	error("value of 'SET_CXTAIL' must be a char or NULL, not a '%s'",
	      R_typeToChar(v));
#endif
    /*CHECK_OLD_TO_NEW(x, v); *//* not needed since not properly traced */
    ATTRIB(x) = v;
    return x;
}

/* Test functions */
Rboolean Rf_isNull(SEXP s) { return isNull(CHK(s)); }
Rboolean Rf_isSymbol(SEXP s) { return isSymbol(CHK(s)); }
Rboolean Rf_isLogical(SEXP s) { return isLogical(CHK(s)); }
Rboolean Rf_isReal(SEXP s) { return isReal(CHK(s)); }
Rboolean Rf_isComplex(SEXP s) { return isComplex(CHK(s)); }
Rboolean Rf_isExpression(SEXP s) { return isExpression(CHK(s)); }
Rboolean Rf_isEnvironment(SEXP s) { return isEnvironment(CHK(s)); }
Rboolean Rf_isString(SEXP s) { return isString(CHK(s)); }
Rboolean Rf_isObject(SEXP s) { return isObject(CHK(s)); }

/* Bindings accessors */
attribute_hidden Rboolean
(IS_ACTIVE_BINDING)(SEXP b) {return (Rboolean) IS_ACTIVE_BINDING(CHK(b));}
attribute_hidden Rboolean
(BINDING_IS_LOCKED)(SEXP b) {return (Rboolean) BINDING_IS_LOCKED(CHK(b));}
attribute_hidden void
(SET_ACTIVE_BINDING_BIT)(SEXP b) {SET_ACTIVE_BINDING_BIT(CHK(b));}
attribute_hidden void (LOCK_BINDING)(SEXP b) {LOCK_BINDING(CHK(b));}
attribute_hidden void (UNLOCK_BINDING)(SEXP b) {UNLOCK_BINDING(CHK(b));}

attribute_hidden
void (SET_BASE_SYM_CACHED)(SEXP b) { SET_BASE_SYM_CACHED(CHK(b)); }
attribute_hidden
void (UNSET_BASE_SYM_CACHED)(SEXP b) { UNSET_BASE_SYM_CACHED(CHK(b)); }
attribute_hidden
Rboolean (BASE_SYM_CACHED)(SEXP b) { return (Rboolean) BASE_SYM_CACHED(CHK(b)); }

attribute_hidden
void (SET_SPECIAL_SYMBOL)(SEXP b) { SET_SPECIAL_SYMBOL(CHK(b)); }
attribute_hidden
void (UNSET_SPECIAL_SYMBOL)(SEXP b) { UNSET_SPECIAL_SYMBOL(CHK(b)); }
attribute_hidden // this is a bit returned in an int, so really is Rboolean
Rboolean (IS_SPECIAL_SYMBOL)(SEXP b) { return (Rboolean) IS_SPECIAL_SYMBOL(CHK(b)); }
attribute_hidden
void (SET_NO_SPECIAL_SYMBOLS)(SEXP b) { SET_NO_SPECIAL_SYMBOLS(CHK(b)); }
attribute_hidden
void (UNSET_NO_SPECIAL_SYMBOLS)(SEXP b) { UNSET_NO_SPECIAL_SYMBOLS(CHK(b)); }
attribute_hidden // // this is a bit returned in an int,
Rboolean (NO_SPECIAL_SYMBOLS)(SEXP b) { return (Rboolean) NO_SPECIAL_SYMBOLS(CHK(b)); }

/* R_FunTab accessors, only needed when write barrier is on */
/* Might want to not hide for experimentation without rebuilding R - LT */
attribute_hidden int (PRIMVAL)(SEXP x) { return PRIMVAL(CHK(x)); }
attribute_hidden CCODE (PRIMFUN)(SEXP x) { return PRIMFUN(CHK(x)); }
attribute_hidden void (SET_PRIMFUN)(SEXP x, CCODE f) { PRIMFUN(CHK(x)) = f; }

/* for use when testing the write barrier */
attribute_hidden int (IS_BYTES)(SEXP x) { return IS_BYTES(CHK(x)); }
attribute_hidden int (IS_LATIN1)(SEXP x) { return IS_LATIN1(CHK(x)); }
/* Next two are used in package utils */
int  (IS_ASCII)(SEXP x) { return IS_ASCII(CHK(x)); }
int  (IS_UTF8)(SEXP x) { return IS_UTF8(CHK(x)); }
attribute_hidden void (SET_BYTES)(SEXP x) { SET_BYTES(CHK(x)); }
attribute_hidden void (SET_LATIN1)(SEXP x) { SET_LATIN1(CHK(x)); }
attribute_hidden void (SET_UTF8)(SEXP x) { SET_UTF8(CHK(x)); }
attribute_hidden void (SET_ASCII)(SEXP x) { SET_ASCII(CHK(x)); }
/*attribute_hidden*/ int  (ENC_KNOWN)(SEXP x) { return ENC_KNOWN(CHK(x)); }
attribute_hidden void (SET_CACHED)(SEXP x) { SET_CACHED(CHK(x)); }
/*attribute_hidden*/ int  (IS_CACHED)(SEXP x) { return IS_CACHED(CHK(x)); }

/*******************************************/
/* Non-sampling memory use profiler
   reports all large vector heap
   allocations and all calls to GetNewPage */
/*******************************************/

#ifndef R_MEMORY_PROFILING

NORET SEXP do_Rprofmem(SEXP args)
{
    error(_("memory profiling is not available on this system"));
}

#else
static int R_IsMemReporting;  /* Rboolean more appropriate? */
static FILE *R_MemReportingOutfile;
static R_size_t R_MemReportingThreshold;

static void R_OutputStackTrace(FILE *file)
{
    RCNTXT *cptr;

    for (cptr = R_GlobalContext; cptr; cptr = cptr->nextcontext) {
	if ((cptr->callflag & (CTXT_FUNCTION | CTXT_BUILTIN))
	    && TYPEOF(cptr->call) == LANGSXP) {
	    SEXP fun = CAR(cptr->call);
	    fprintf(file, "\"%s\" ",
		    TYPEOF(fun) == SYMSXP ? CHAR(PRINTNAME(fun)) :
		    "<Anonymous>");
	}
    }
}

static void R_ReportAllocation(R_size_t size)
{
    if (R_IsMemReporting) {
	if(size > R_MemReportingThreshold) {
	    fprintf(R_MemReportingOutfile, "%lu :", (unsigned long) size);
	    R_OutputStackTrace(R_MemReportingOutfile);
	    fprintf(R_MemReportingOutfile, "\n");
	}
    }
    return;
}

static void R_ReportNewPage(void)
{
    if (R_IsMemReporting) {
	fprintf(R_MemReportingOutfile, "new page:");
	R_OutputStackTrace(R_MemReportingOutfile);
	fprintf(R_MemReportingOutfile, "\n");
    }
    return;
}

static void R_EndMemReporting(void)
{
    if(R_MemReportingOutfile != NULL) {
	/* does not fclose always flush? */
	fflush(R_MemReportingOutfile);
	fclose(R_MemReportingOutfile);
	R_MemReportingOutfile=NULL;
    }
    R_IsMemReporting = 0;
    return;
}

static void R_InitMemReporting(SEXP filename, int append,
			       R_size_t threshold)
{
    if(R_MemReportingOutfile != NULL) R_EndMemReporting();
    R_MemReportingOutfile = RC_fopen(filename, append ? "a" : "w", TRUE);
    if (R_MemReportingOutfile == NULL)
	error(_("Rprofmem: cannot open output file '%s'"),
	      translateChar(filename));
    R_MemReportingThreshold = threshold;
    R_IsMemReporting = 1;
    return;
}

SEXP do_Rprofmem(SEXP args)
{
    SEXP filename;
    R_size_t threshold = 0;
    int append_mode;

    if (!isString(CAR(args)) || (LENGTH(CAR(args))) != 1)
	error(_("invalid '%s' argument"), "filename");
    append_mode = asLogical(CADR(args));
    filename = STRING_ELT(CAR(args), 0);
    double tdbl = REAL(CADDR(args))[0];
    if (tdbl > 0) {
	if (tdbl >= (double) R_SIZE_T_MAX)
	    threshold = R_SIZE_T_MAX;
	else
	    threshold = (R_size_t) tdbl;
    }
    if (strlen(CHAR(filename)))
	R_InitMemReporting(filename, append_mode, threshold);
    else
	R_EndMemReporting();
    return R_NilValue;
}

#endif /* R_MEMORY_PROFILING */

/* RBufferUtils, moved from deparse.c */

#include "RBufferUtils.h"

void *R_AllocStringBuffer(size_t blen, R_StringBuffer *buf)
{
    size_t blen1, bsize = buf->defaultSize;

    /* for backwards compatibility, this used to free the buffer */
    if(blen == (size_t)-1)
	error("R_AllocStringBuffer( (size_t)-1 ) is no longer allowed");

    if(blen * sizeof(char) < buf->bufsize) return buf->data;
    blen1 = blen = (blen + 1) * sizeof(char);
    blen = (blen / bsize) * bsize;
    if(blen < blen1) blen += bsize;

    /* Result may be accessed as `wchar_t *` and other types; malloc /
      realloc guarantee correct memory alignment for all object types */
    if(buf->data == NULL) {
	buf->data = (char *) malloc(blen);
	if(buf->data)
	    buf->data[0] = '\0';
    } else
	buf->data = (char *) realloc(buf->data, blen);
    buf->bufsize = blen;
    if(!buf->data) {
	buf->bufsize = 0;
	/* don't translate internal error message */
	error("could not allocate memory (%u %s) in C function 'R_AllocStringBuffer'",
	      (unsigned int) blen/1024/1024, "Mb");
    }
    return buf->data;
}

void R_FreeStringBuffer(R_StringBuffer *buf)
{
    if (buf->data != NULL) {
	free(buf->data);
	buf->bufsize = 0;
	buf->data = NULL;
    }
}

attribute_hidden void R_FreeStringBufferL(R_StringBuffer *buf)
{
    if (buf->bufsize > buf->defaultSize) {
	free(buf->data);
	buf->bufsize = 0;
	buf->data = NULL;
    }
}

/* ======== This needs direct access to gp field for efficiency ======== */

/* this has NA_STRING = NA_STRING */
attribute_hidden
int Seql(SEXP a, SEXP b)
{
    /* The only case where pointer comparisons do not suffice is where
      we have two strings in different encodings (which must be
      non-ASCII strings). Note that one of the strings could be marked
      as unknown. */
    if (a == b) return 1;
    /* Leave this to compiler to optimize */
    if (IS_CACHED(a) && IS_CACHED(b) && ENC_KNOWN(a) == ENC_KNOWN(b))
	return 0;
    else if (IS_BYTES(a) || IS_BYTES(b)) {
	if (IS_BYTES(a) && IS_BYTES(b))
	    /* only get here if at least one is not cached */
	    return !strcmp(CHAR(a), CHAR(b));
	else
	    return 0;
    }
    else {
	SEXP vmax = R_VStack;
	int result = !strcmp(translateCharUTF8(a), translateCharUTF8(b));
	R_VStack = vmax; /* discard any memory used by translateCharUTF8 */
	return result;
    }
}


#ifdef LONG_VECTOR_SUPPORT
NORET R_len_t R_BadLongVector(SEXP x, const char *file, int line)
{
    error(_("long vectors not supported yet: %s:%d"), file, line);
}
#endif

/* Highly experimental resizable vector support */

/* Serializing and unserializing preserves the GROWABLE bit, but
   XTRUELENGTH is set to zero by unserialize. A vector with the
   GROWABLE bit set but XTRUELENGTH zero is therefore considered not
   resizeble. */ 
bool R_isResizable(SEXP x)
{
    return isVector(x) && ! ALTREP(x) && GROWABLE_BIT_SET(x) &&
	XTRUELENGTH(x) != 0 && XLENGTH(x) <= XTRUELENGTH(x);
}

R_xlen_t R_maxLength(SEXP x)
{
    return GROWABLE_BIT_SET(x) ? XTRUELENGTH(x) : xlength(x);
}

SEXP R_allocResizableVector(SEXPTYPE type, R_xlen_t maxlen)
{
    switch (type) {
    case LGLSXP:
    case INTSXP:
    case REALSXP:
    case CPLXSXP:
    case STRSXP:
    case EXPRSXP:
    case VECSXP:
    case RAWSXP:
	break;
    default:
	error(_("cannot make a resizable vector of type '%s'"),
	      sexptype2char(type));
    }
    SEXP val = allocVector(type, maxlen);
    SET_TRUELENGTH(val, maxlen);
    SET_GROWABLE_BIT(val);
    return val;
}

SEXP R_duplicateAsResizable(SEXP x)
{
    if (ALTREP(x))
	error(_("ALTREP objects cannot be made resizable"));
    if (! isVector(x))
	error(_("cannot make non-vector objects resizable"));
    SEXP val = duplicate(x);
    SET_TRUELENGTH(val, XLENGTH(val));
    SET_GROWABLE_BIT(val);
    return val;
}

static R_INLINE void clear_elements(SEXP x, R_xlen_t from, R_xlen_t to)
{
    switch(TYPEOF(x)) {
    case STRSXP:
	for (R_xlen_t i = from; i < to; i++)
	    SET_STRING_ELT(x, i, R_BlankString);
	break;
    case EXPRSXP:
    case VECSXP:
	for (R_xlen_t i = from; i < to; i++)
	    SET_VECTOR_ELT(x, i, R_NilValue);
	break;
    }
}

void R_resizeVector(SEXP x, R_xlen_t newlen)
{
    if (newlen < 0)
	error(_("invalid negative 'newlen'"));
    if (newlen != xlength(x)) {
	if (! R_isResizable(x))
	    error(_("not a resizable vector"));
	if (newlen > XTRUELENGTH(x))
	    error(_("'newlen' is too large"));
	if (ATTRIB(x) != R_NilValue) {
	    // clear length-dependent attributes
	    if (getAttrib(x, R_DimSymbol) != R_NilValue)
		setAttrib(x, R_DimSymbol, R_NilValue);
	    if (getAttrib(x, R_DimNamesSymbol) != R_NilValue)
		setAttrib(x, R_DimNamesSymbol, R_NilValue);
	    if (getAttrib(x, R_NamesSymbol) != R_NilValue)
		setAttrib(x, R_NamesSymbol, R_NilValue);
	}
	R_xlen_t len = XLENGTH(x);
	if (newlen < len) // clear dropped elements to drop refcounts
	    clear_elements(x, newlen, len);
	SET_STDVEC_LENGTH(x, newlen);
	if (len < newlen) // initialize new elements
	    clear_elements(x, len, newlen);
    }
}
