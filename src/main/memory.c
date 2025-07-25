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
#include <R_ext/Rdynload.h>
#include <R_ext/Rallocators.h> /* for R_allocator_t structure */
#include <Rmath.h> // R_pow_di
#include <Print.h> // R_print
#include "timeR.h"

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

static int gc_pending = 0;
#ifdef GC_TORTURE
/* **** if the user specified a wait before starting to force
   **** collections it might make sense to also wait before starting
   **** to inhibit releases */
static int gc_force_wait = 0;
static int gc_force_gap = 0;
static Rboolean gc_inhibit_release = FALSE;
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

static SEXP R_VStack = NULL;		/* R_alloc stack pointer */
static SEXP R_PreciousList = NULL;      /* List of Persistent Objects */
static R_size_t R_LargeVallocSize = 0;
static R_size_t R_SmallVallocSize = 0;
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

typedef union PAGE_HEADER {
  union PAGE_HEADER *next;
  double align;
} PAGE_HEADER;

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
#define VHEAP_FREE() (R_VSize - R_LargeVallocSize - R_SmallVallocSize)


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
static struct {
    SEXP Old[NUM_OLD_GENERATIONS], New, Free;
    SEXPREC OldPeg[NUM_OLD_GENERATIONS], NewPeg;
#ifndef EXPEL_OLD_TO_NEW
    SEXP OldToNew[NUM_OLD_GENERATIONS];
    SEXPREC OldToNewPeg[NUM_OLD_GENERATIONS];
#endif
    int OldCount[NUM_OLD_GENERATIONS], AllocCount, PageCount;
    PAGE_HEADER *pages;
} R_GenHeap[NUM_NODE_CLASSES];

static R_size_t R_NodesInUse = 0;

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

#define MARK_AND_UNSNAP_NODE(s) do {		\
	SEXP mu__n__ = (s);			\
	CHECK_FOR_FREE_NODE(mu__n__);		\
	MARK_NODE(mu__n__);			\
	UNSNAP_NODE(mu__n__);			\
    } while (0)

#define FORWARD_NODE(s) do { \
  SEXP fn__n__ = (s); \
  if (fn__n__ && ! NODE_IS_MARKED(fn__n__)) { \
    MARK_AND_UNSNAP_NODE(fn__n__); \
    SET_NEXT_NODE(fn__n__, forwarded_nodes); \
    forwarded_nodes = fn__n__; \
  } \
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
	if (fpn__n__ && ! NODE_IS_MARKED(fpn__n__)) {	\
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

#define CLASS_GET_FREE_NODE(c,s) do { \
  SEXP __n__ = R_GenHeap[c].Free; \
  if (__n__ == R_GenHeap[c].New) { \
    GetNewPage(c); \
    __n__ = R_GenHeap[c].Free; \
  } \
  R_GenHeap[c].Free = NEXT_NODE(__n__); \
  R_NodesInUse++; \
  (s) = __n__; \
} while (0)

#define NO_FREE_NODES() (R_NodesInUse >= R_NSize)
#define GET_FREE_NODE(s) CLASS_GET_FREE_NODE(0,s)

/* versions that assume nodes are available without adding a new page */
#define CLASS_QUICK_GET_FREE_NODE(c,s) do {		\
	SEXP __n__ = R_GenHeap[c].Free;			\
	if (__n__ == R_GenHeap[c].New)			\
	    error("need new page - should not happen");	\
	R_GenHeap[c].Free = NEXT_NODE(__n__);		\
	R_NodesInUse++;					\
	(s) = __n__;					\
    } while (0)

#define QUICK_GET_FREE_NODE(s) CLASS_QUICK_GET_FREE_NODE(0,s)

/* QUICK versions can be used if (CLASS_)NEED_NEW_PAGE returns FALSE */
#define CLASS_NEED_NEW_PAGE(c) (R_GenHeap[c].Free == R_GenHeap[c].New)
#define NEED_NEW_PAGE() CLASS_NEED_NEW_PAGE(0)


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

    page = malloc(R_PAGE_SIZE);
    if (page == NULL) {
	R_gc_no_finalizers(0);
	page = malloc(R_PAGE_SIZE);
	if (page == NULL)
	    mem_err_malloc((R_size_t) R_PAGE_SIZE);
    }
#ifdef R_MEMORY_PROFILING
    R_ReportNewPage();
#endif
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
        MARK_TIMER();
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
	    } else {
            RELEASE_TIMER();
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
    FORWARD_NODE(R_Warnings);	           /* Warnings, if any */
    FORWARD_NODE(R_ReturnedValue);

    FORWARD_NODE(R_HandlerStack);          /* Condition handler stack */
    FORWARD_NODE(R_RestartStack);          /* Available restarts stack */

    FORWARD_NODE(R_BCbody);                /* Current byte code object */
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

    if (R_CurrentExpr != NULL)	           /* Current expression */
	FORWARD_NODE(R_CurrentExpr);

    for (i = 0; i < R_MaxDevices; i++) {   /* Device display lists */
	pGEDevDesc gdd = GEgetDevice(i);
	if (gdd) {
	    FORWARD_NODE(gdd->displayList);
	    FORWARD_NODE(gdd->savedSnapshot);
	    if (gdd->dev)
		FORWARD_NODE(gdd->dev->eventEnv);
	}
    }

    for (ctxt = R_GlobalContext ; ctxt != NULL ; ctxt = ctxt->nextcontext) {
	FORWARD_NODE(ctxt->conexit);       /* on.exit expressions */
	FORWARD_NODE(ctxt->promargs);	   /* promises supplied to closure */
	FORWARD_NODE(ctxt->callfun);       /* the closure called */
	FORWARD_NODE(ctxt->sysparent);     /* calling environment */
	FORWARD_NODE(ctxt->call);          /* the call */
	FORWARD_NODE(ctxt->cloenv);        /* the closure environment */
	FORWARD_NODE(ctxt->bcbody);        /* the current byte code object */
	FORWARD_NODE(ctxt->handlerstack);  /* the condition handler stack */
	FORWARD_NODE(ctxt->restartstack);  /* the available restarts stack */
	FORWARD_NODE(ctxt->srcref);	   /* the current source reference */
	if (ctxt->returnValue.tag == 0)    /* For on.exit calls */
	    FORWARD_NODE(ctxt->returnValue.u.sxpval);
    }

    FORWARD_NODE(R_PreciousList);

    for (i = 0; i < R_PPStackTop; i++)	   /* Protected pointers */
	FORWARD_NODE(R_PPStack[i]);

    FORWARD_NODE(R_VStack);		   /* R_alloc stack */

    for (R_bcstack_t *sp = R_BCNodeStackBase; sp < R_BCNodeStackTop; sp++) {
	if (sp->tag == RAWMEM_TAG)
	    sp += sp->u.ival;
	else if (sp->tag == 0 || IS_PARTIAL_SXP_TAG(sp->tag))
	    FORWARD_NODE(sp->u.sxpval);
    }

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

    return gens_collected;
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

attribute_hidden void InitMemory(void)
{
    int i;
    int gen;
    char *arg;

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
    if (!(R_PPStack = (SEXP *) malloc(R_RealPPStackSize * sizeof(SEXP))))
	R_Suicide("couldn't allocate memory for pointer stack");
    R_PPStackTop = 0;
#if VALGRIND_LEVEL > 1
    VALGRIND_MAKE_MEM_NOACCESS(R_PPStack+R_PPStackSize, PP_REDZONE_SIZE);
#endif
    vsfac = sizeof(VECREC);
    R_VSize = (R_VSize + 1)/vsfac;
    if (R_MaxVSize < R_SIZE_T_MAX) R_MaxVSize = (R_MaxVSize + 1)/vsfac;

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
    GET_FREE_NODE(R_NilValue);
    R_NilValue->sxpinfo = UnmarkedNodeTemplate.sxpinfo;
    INIT_REFCNT(R_NilValue);
    SET_REFCNT(R_NilValue, REFCNTMAX);
    SET_TYPEOF(R_NilValue, NILSXP);
    CAR0(R_NilValue) = R_NilValue;
    CDR(R_NilValue) = R_NilValue;
    TAG(R_NilValue) = R_NilValue;
    ATTRIB(R_NilValue) = R_NilValue;
    MARK_NOT_MUTABLE(R_NilValue);

    R_BCNodeStackBase =
	(R_bcstack_t *) malloc(R_BCNODESTACKSIZE * sizeof(R_bcstack_t));
    if (R_BCNodeStackBase == NULL)
	R_Suicide("couldn't allocate node stack");
    R_BCNodeStackTop = R_BCNodeStackBase;
    R_BCNodeStackEnd = R_BCNodeStackBase + R_BCNODESTACKSIZE;
    R_BCProtTop = R_BCNodeStackTop;

    R_weak_refs = R_NilValue;

    R_HandlerStack = R_RestartStack = R_NilValue;

    /*  Unbound values which are to be preserved through GCs */
    R_PreciousList = R_NilValue;

    /*  The current source line */
    R_Srcref = R_NilValue;

    /* R_TrueValue and R_FalseValue */
    R_TrueValue = mkTrue();
    MARK_NOT_MUTABLE(R_TrueValue);
    R_FalseValue = mkFalse();
    MARK_NOT_MUTABLE(R_FalseValue);
    R_LogicalNAValue = allocVector(LGLSXP, 1);
    LOGICAL(R_LogicalNAValue)[0] = NA_LOGICAL;
    MARK_NOT_MUTABLE(R_LogicalNAValue);
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
	return (char *) DATAPTR(s);
    }
    /* One programmer has relied on this, but it is undocumented! */
    else return NULL;
}

#ifdef HAVE_STDALIGN_H
# include <stdalign.h>
#endif

#include <stdint.h>

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
    SEXP s;
    if (FORCE_GC || NO_FREE_NODES()) {
	R_gc_internal(0);
	if (NO_FREE_NODES())
	    mem_err_cons();
    }
    GET_FREE_NODE(s);
    s->sxpinfo = UnmarkedNodeTemplate.sxpinfo;
    INIT_REFCNT(s);
    SET_TYPEOF(s, t);
    CAR0(s) = R_NilValue;
    CDR(s) = R_NilValue;
    TAG(s) = R_NilValue;
    ATTRIB(s) = R_NilValue;
    return s;
}

static SEXP allocSExpNonCons(SEXPTYPE t)
{
    SEXP s;
    if (FORCE_GC || NO_FREE_NODES()) {
	R_gc_internal(0);
	if (NO_FREE_NODES())
	    mem_err_cons();
    }
    GET_FREE_NODE(s);
    s->sxpinfo = UnmarkedNodeTemplate.sxpinfo;
    INIT_REFCNT(s);
    SET_TYPEOF(s, t);
    TAG(s) = R_NilValue;
    ATTRIB(s) = R_NilValue;
    return s;
}

/* cons is defined directly to avoid the need to protect its arguments
   unless a GC will actually occur. */
SEXP cons(SEXP car, SEXP cdr)
{
    BEGIN_TIMER(TR_cons);
    SEXP s;
    if (FORCE_GC || NO_FREE_NODES()) {
	PROTECT(car);
	PROTECT(cdr);
	R_gc_internal(0);
	UNPROTECT(2);
	if (NO_FREE_NODES())
	    mem_err_cons();
    }

    if (NEED_NEW_PAGE()) {
	PROTECT(car);
	PROTECT(cdr);
	GET_FREE_NODE(s);
	UNPROTECT(2);
    }
    else
	QUICK_GET_FREE_NODE(s);

    s->sxpinfo = UnmarkedNodeTemplate.sxpinfo;
    INIT_REFCNT(s);
    SET_TYPEOF(s, LISTSXP);
    CAR0(s) = CHK(car); if (car) INCREMENT_REFCNT(car);
    CDR(s) = CHK(cdr); if (cdr) INCREMENT_REFCNT(cdr);
    TAG(s) = R_NilValue;
    ATTRIB(s) = R_NilValue;
    END_TIMER(TR_cons);
    return s;
}

attribute_hidden SEXP CONS_NR(SEXP car, SEXP cdr)
{
    BEGIN_TIMER(TR_cons);
    SEXP s;
    if (FORCE_GC || NO_FREE_NODES()) {
	PROTECT(car);
	PROTECT(cdr);
	R_gc_internal(0);
	UNPROTECT(2);
	if (NO_FREE_NODES())
	    mem_err_cons();
    }

    if (NEED_NEW_PAGE()) {
	PROTECT(car);
	PROTECT(cdr);
	GET_FREE_NODE(s);
	UNPROTECT(2);
    }
    else
	QUICK_GET_FREE_NODE(s);

    s->sxpinfo = UnmarkedNodeTemplate.sxpinfo;
    INIT_REFCNT(s);
    DISABLE_REFCNT(s);
    SET_TYPEOF(s, LISTSXP);
    CAR0(s) = CHK(car);
    CDR(s) = CHK(cdr);
    TAG(s) = R_NilValue;
    ATTRIB(s) = R_NilValue;
    END_TIMER(TR_cons);
    return s;
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
    SEXP v, n, newrho;

    if (FORCE_GC || NO_FREE_NODES()) {
	PROTECT(namelist);
	PROTECT(valuelist);
	PROTECT(rho);
	R_gc_internal(0);
	UNPROTECT(3);
	if (NO_FREE_NODES())
	    mem_err_cons();
    }

    if (NEED_NEW_PAGE()) {
	PROTECT(namelist);
	PROTECT(valuelist);
	PROTECT(rho);
	GET_FREE_NODE(newrho);
	UNPROTECT(3);
    }
    else
	QUICK_GET_FREE_NODE(newrho);

    newrho->sxpinfo = UnmarkedNodeTemplate.sxpinfo;
    INIT_REFCNT(newrho);
    SET_TYPEOF(newrho, ENVSXP);
    FRAME(newrho) = valuelist; INCREMENT_REFCNT(valuelist);
    ENCLOS(newrho) = CHK(rho); if (rho != NULL) INCREMENT_REFCNT(rho);
    HASHTAB(newrho) = R_NilValue;
    ATTRIB(newrho) = R_NilValue;

    v = CHK(valuelist);
    n = CHK(namelist);
    while (v != R_NilValue && n != R_NilValue) {
	SET_TAG(v, TAG(n));
	v = CDR(v);
	n = CDR(n);
    }
    return (newrho);
}

/* mkPROMISE is defined directly do avoid the need to protect its arguments
   unless a GC will actually occur. */
attribute_hidden SEXP mkPROMISE(SEXP expr, SEXP rho)
{
    SEXP s;
    if (FORCE_GC || NO_FREE_NODES()) {
	PROTECT(expr);
	PROTECT(rho);
	R_gc_internal(0);
	UNPROTECT(2);
	if (NO_FREE_NODES())
	    mem_err_cons();
    }

    if (NEED_NEW_PAGE()) {
	PROTECT(expr);
	PROTECT(rho);
	GET_FREE_NODE(s);
	UNPROTECT(2);
    }
    else
	QUICK_GET_FREE_NODE(s);

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
    return s;
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
    BEGIN_TIMER(TR_allocVector);
    SEXP s;     /* For the generational collector it would be safer to
		   work in terms of a VECSXP here, but that would
		   require several casts below... */
    R_size_t size = 0, alloc_size, old_R_VSize;
    int node_class;
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
	    if (FORCE_GC || NO_FREE_NODES() || VHEAP_FREE() < alloc_size) {
		R_gc_internal(alloc_size);
		if (NO_FREE_NODES())
		    mem_err_cons();
		if (VHEAP_FREE() < alloc_size)
		    mem_err_heap(size);
	    }

	    CLASS_GET_FREE_NODE(node_class, s);
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
	    R_SmallVallocSize += alloc_size;
	    /* Note that we do not include the header size into VallocSize,
	       but it is counted into memory usage via R_NodesInUse. */
	    ATTRIB(s) = R_NilValue;
	    SET_TYPEOF(s, type);
	    SET_STDVEC_LENGTH(s, (R_len_t) length); // is 1
	    SET_STDVEC_TRUELENGTH(s, 0);
	    INIT_REFCNT(s);
        END_TIMER(TR_allocVector);
	    return(s);
	}
    }

    if (length > R_XLEN_T_MAX)
	error(_("vector is too large")); /**** put length into message */
    else if (length < 0 )
	error(_("negative length vectors are not allowed"));
    /* number of vector cells to allocate */
    switch (type) {
    case NILSXP:
    END_TIMER(TR_allocVector);
	return R_NilValue;
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
	if(length == 0) {
        END_TIMER(TR_allocVector);
        return R_NilValue;
    }
#ifdef LONG_VECTOR_SUPPORT
	if (length > R_SHORT_LEN_MAX) error("invalid length for pairlist");
#endif
	s = allocList((int) length);
	SET_TYPEOF(s, LANGSXP);
    END_TIMER(TR_allocVector);
	return s;
    case LISTSXP:
#ifdef LONG_VECTOR_SUPPORT
	if (length > R_SHORT_LEN_MAX) error("invalid length for pairlist");
#endif
	s = allocList((int) length);
    END_TIMER(TR_allocVector);
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
	R_gc_internal(alloc_size);
	if (NO_FREE_NODES())
	    mem_err_cons();
	if (VHEAP_FREE() < alloc_size)
	    mem_err_heap(size);
    }

    if (size > 0) {
	if (node_class < NUM_SMALL_NODE_CLASSES) {
	    CLASS_GET_FREE_NODE(node_class, s);
#if VALGRIND_LEVEL > 1
	    VALGRIND_MAKE_MEM_UNDEFINED(STDVEC_DATAPTR(s), actual_size);
#endif
	    s->sxpinfo = UnmarkedNodeTemplate.sxpinfo;
	    INIT_REFCNT(s);
	    SET_NODE_CLASS(s, node_class);
	    R_SmallVallocSize += alloc_size;
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
	    if (!allocator) R_LargeVallocSize += size;
	    R_GenHeap[node_class].AllocCount++;
	    R_NodesInUse++;
	    SNAP_NODE(s, R_GenHeap[node_class].New);
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
    END_TIMER(TR_allocVector);
    return s;
}

/* For future hiding of allocVector(CHARSXP) */
attribute_hidden SEXP allocCharsxp(R_len_t len)
{
    return allocVector(intCHARSXP, len);
}

SEXP allocList(int n)
{
    BEGIN_TIMER(TR_allocList);
    int i;
    SEXP result;
    result = R_NilValue;
    for (i = 0; i < n; i++)
	result = CONS(R_NilValue, result);
    END_TIMER(TR_allocList);
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
BEGIN_TIMER(TR_allocS4);
   SEXP s;
   GC_PROT(s = allocSExpNonCons(OBJSXP));
   SET_S4_OBJECT(s);
   END_TIMER(TR_allocS4);
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
    num_old_gens_to_collect = NUM_OLD_GENERATIONS;
    R_gc_internal(0);
#ifndef IMMEDIATE_FINALIZERS
    R_RunPendingFinalizers();
#endif
}

void R_gc_lite(void)
{
    R_gc_internal(0);
#ifndef IMMEDIATE_FINALIZERS
    R_RunPendingFinalizers();
#endif
}

static void R_gc_no_finalizers(R_size_t size_needed)
{
    num_old_gens_to_collect = NUM_OLD_GENERATIONS;
    R_gc_internal(size_needed);
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
    BEGIN_TIMER(TR_GCInternal);

    R_CHECK_THREAD;
    if (!R_GCEnabled || R_in_gc) {
      if (R_in_gc)
        gc_error("*** recursive gc invocation\n");
      if (NO_FREE_NODES())
	R_NSize = R_NodesInUse + 1;

      if (num_old_gens_to_collect < NUM_OLD_GENERATIONS &&
	  VHEAP_FREE() < size_needed + R_MinFreeFrac * R_VSize)
	num_old_gens_to_collect++;

      if (size_needed > VHEAP_FREE()) {
	  R_size_t expand = size_needed - VHEAP_FREE();
	  if (R_VSize + expand > R_MaxVSize)
	      mem_err_heap(size_needed);
	  R_VSize += expand;
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

    END_TIMER(TR_GCInternal);
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

static int use_precious_hash = FALSE;
static int precious_inited = FALSE;

void R_PreserveObject(SEXP object)
{
    R_CHECK_THREAD;
    if (! precious_inited) {
	precious_inited = TRUE;
	if (getenv("R_HASH_PRECIOUS"))
	    use_precious_hash = TRUE;
    }
    if (use_precious_hash) {
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
    if (! precious_inited)
	return; /* can't be anything to delete yet */
    if (use_precious_hash) {
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
