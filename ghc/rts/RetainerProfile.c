/* -----------------------------------------------------------------------------
 * $Id: RetainerProfile.c,v 1.9 2003/04/23 08:54:45 simonmar Exp $
 *
 * (c) The GHC Team, 2001
 * Author: Sungwoo Park
 *
 * Retainer profiling.
 *
 * ---------------------------------------------------------------------------*/

#ifdef PROFILING

#include <stdio.h>

#include "Rts.h"
#include "RtsUtils.h"
#include "RetainerProfile.h"
#include "RetainerSet.h"
#include "Schedule.h"
#include "Printer.h"
#include "Storage.h"
#include "StoragePriv.h"
#include "RtsFlags.h"
#include "Weak.h"
#include "Sanity.h"
#include "StablePriv.h"
#include "Profiling.h"
#include "Stats.h"
#include "BlockAlloc.h"
#include "ProfHeap.h"
#include "Apply.h"

/*
  Note: what to change in order to plug-in a new retainer profiling scheme?
    (1) type retainer in ../includes/StgRetainerProf.h
    (2) retainer function R(), i.e., getRetainerFrom()
    (3) the two hashing functions, hashKeySingleton() and hashKeyAddElement(),
        in RetainerSet.h, if needed.
    (4) printRetainer() and printRetainerSetShort() in RetainerSet.c.
 */

/* -----------------------------------------------------------------------------
 * Declarations...
 * -------------------------------------------------------------------------- */

static nat retainerGeneration;	// generation

static nat numObjectVisited;	// total number of objects visited
static nat timesAnyObjectVisited; // number of times any objects are visited

/*
  The rs field in the profile header of any object points to its retainer
  set in an indirect way: if flip is 0, it points to the retainer set;
  if flip is 1, it points to the next byte after the retainer set (even
  for NULL pointers). Therefore, with flip 1, (rs ^ 1) is the actual
  pointer. See retainerSetOf().
 */

StgWord flip = 0;     // flip bit
                      // must be 0 if DEBUG_RETAINER is on (for static closures)

#define setRetainerSetToNull(c)   \
  (c)->header.prof.hp.rs = (RetainerSet *)((StgWord)NULL | flip)

static void retainStack(StgClosure *, retainer, StgPtr, StgPtr);
static void retainClosure(StgClosure *, StgClosure *, retainer);
#ifdef DEBUG_RETAINER
static void belongToHeap(StgPtr p);
#endif

#ifdef DEBUG_RETAINER
/*
  cStackSize records how many times retainStack() has been invoked recursively,
  that is, the number of activation records for retainStack() on the C stack.
  maxCStackSize records its max value.
  Invariants:
    cStackSize <= maxCStackSize
 */
static nat cStackSize, maxCStackSize;

static nat sumOfNewCost;	// sum of the cost of each object, computed
				// when the object is first visited
static nat sumOfNewCostExtra;   // for those objects not visited during
                                // retainer profiling, e.g., MUT_VAR
static nat costArray[N_CLOSURE_TYPES];

nat sumOfCostLinear;		// sum of the costs of all object, computed
				// when linearly traversing the heap after
				// retainer profiling
nat costArrayLinear[N_CLOSURE_TYPES];
#endif

/* -----------------------------------------------------------------------------
 * Retainer stack - header
 *   Note:
 *     Although the retainer stack implementation could be separated *
 *     from the retainer profiling engine, there does not seem to be
 *     any advantage in doing that; retainer stack is an integral part
 *     of retainer profiling engine and cannot be use elsewhere at
 *     all.
 * -------------------------------------------------------------------------- */

typedef enum {
    posTypeStep,
    posTypePtrs,
    posTypeSRT,
} nextPosType;

typedef union {
    // fixed layout or layout specified by a field in the closure
    StgWord step;

    // layout.payload
    struct {
    // See StgClosureInfo in InfoTables.h
#if SIZEOF_VOID_P == 8
	StgWord32 pos;
	StgWord32 ptrs;
#else
	StgWord16 pos;
	StgWord16 ptrs;
#endif
	StgPtr payload;
    } ptrs;

    // SRT
    struct {
	StgClosure **srt;
	StgClosure **srt_end;
    } srt;
} nextPos;

typedef struct {
    nextPosType type;
    nextPos next;
} stackPos;

typedef struct {
    StgClosure *c;
    retainer c_child_r;
    stackPos info;
} stackElement;

/*
  Invariants:
    firstStack points to the first block group.
    currentStack points to the block group currently being used.
    currentStack->free == stackLimit.
    stackTop points to the topmost byte in the stack of currentStack.
    Unless the whole stack is empty, stackTop must point to the topmost
    object (or byte) in the whole stack. Thus, it is only when the whole stack
    is empty that stackTop == stackLimit (not during the execution of push()
    and pop()).
    stackBottom == currentStack->start.
    stackLimit == currentStack->start + BLOCK_SIZE_W * currentStack->blocks.
  Note:
    When a current stack becomes empty, stackTop is set to point to
    the topmost element on the previous block group so as to satisfy
    the invariants described above.
 */
static bdescr *firstStack = NULL;
static bdescr *currentStack;
static stackElement *stackBottom, *stackTop, *stackLimit;

/*
  currentStackBoundary is used to mark the current stack chunk.
  If stackTop == currentStackBoundary, it means that the current stack chunk
  is empty. It is the responsibility of the user to keep currentStackBoundary
  valid all the time if it is to be employed.
 */
static stackElement *currentStackBoundary;

/*
  stackSize records the current size of the stack.
  maxStackSize records its high water mark.
  Invariants:
    stackSize <= maxStackSize
  Note:
    stackSize is just an estimate measure of the depth of the graph. The reason
    is that some heap objects have only a single child and may not result
    in a new element being pushed onto the stack. Therefore, at the end of
    retainer profiling, maxStackSize + maxCStackSize is some value no greater
    than the actual depth of the graph.
 */
#ifdef DEBUG_RETAINER
static int stackSize, maxStackSize;
#endif

// number of blocks allocated for one stack
#define BLOCKS_IN_STACK 1

/* -----------------------------------------------------------------------------
 * Add a new block group to the stack.
 * Invariants:
 *  currentStack->link == s.
 * -------------------------------------------------------------------------- */
static inline void
newStackBlock( bdescr *bd )
{
    currentStack = bd;
    stackTop     = (stackElement *)(bd->start + BLOCK_SIZE_W * bd->blocks);
    stackBottom  = (stackElement *)bd->start;
    stackLimit   = (stackElement *)stackTop;
    bd->free     = (StgPtr)stackLimit;
}

/* -----------------------------------------------------------------------------
 * Return to the previous block group.
 * Invariants:
 *   s->link == currentStack.
 * -------------------------------------------------------------------------- */
static inline void
returnToOldStack( bdescr *bd )
{
    currentStack = bd;
    stackTop = (stackElement *)bd->free;
    stackBottom = (stackElement *)bd->start;
    stackLimit = (stackElement *)(bd->start + BLOCK_SIZE_W * bd->blocks);
    bd->free = (StgPtr)stackLimit;
}

/* -----------------------------------------------------------------------------
 *  Initializes the traverse stack.
 * -------------------------------------------------------------------------- */
static void
initializeTraverseStack( void )
{
    if (firstStack != NULL) {
	freeChain(firstStack);
    }

    firstStack = allocGroup(BLOCKS_IN_STACK);
    firstStack->link = NULL;
    firstStack->u.back = NULL;

    newStackBlock(firstStack);
}

/* -----------------------------------------------------------------------------
 * Frees all the block groups in the traverse stack.
 * Invariants:
 *   firstStack != NULL
 * -------------------------------------------------------------------------- */
static void
closeTraverseStack( void )
{
    freeChain(firstStack);
    firstStack = NULL;
}

/* -----------------------------------------------------------------------------
 * Returns rtsTrue if the whole stack is empty.
 * -------------------------------------------------------------------------- */
static inline rtsBool
isEmptyRetainerStack( void )
{
    return (firstStack == currentStack) && stackTop == stackLimit;
}

/* -----------------------------------------------------------------------------
 * Returns size of stack
 * -------------------------------------------------------------------------- */
lnat
retainerStackBlocks()
{
    bdescr* bd;
    lnat res = 0;

    for (bd = firstStack; bd != NULL; bd = bd->link) 
      res += bd->blocks;

    return res;
}

/* -----------------------------------------------------------------------------
 * Returns rtsTrue if stackTop is at the stack boundary of the current stack,
 * i.e., if the current stack chunk is empty.
 * -------------------------------------------------------------------------- */
static inline rtsBool
isOnBoundary( void )
{
    return stackTop == currentStackBoundary;
}

/* -----------------------------------------------------------------------------
 * Initializes *info from ptrs and payload.
 * Invariants:
 *   payload[] begins with ptrs pointers followed by non-pointers.
 * -------------------------------------------------------------------------- */
static inline void
init_ptrs( stackPos *info, nat ptrs, StgPtr payload )
{
    info->type              = posTypePtrs;
    info->next.ptrs.pos     = 0;
    info->next.ptrs.ptrs    = ptrs;
    info->next.ptrs.payload = payload;
}

/* -----------------------------------------------------------------------------
 * Find the next object from *info.
 * -------------------------------------------------------------------------- */
static inline StgClosure *
find_ptrs( stackPos *info )
{
    if (info->next.ptrs.pos < info->next.ptrs.ptrs) {
	return (StgClosure *)info->next.ptrs.payload[info->next.ptrs.pos++];
    } else {
	return NULL;
    }
}

/* -----------------------------------------------------------------------------
 *  Initializes *info from SRT information stored in *infoTable.
 * -------------------------------------------------------------------------- */
static inline void
init_srt_fun( stackPos *info, StgFunInfoTable *infoTable )
{
    info->type = posTypeSRT;
    info->next.srt.srt = (StgClosure **)(infoTable->srt);
    info->next.srt.srt_end = info->next.srt.srt + infoTable->i.srt_len;
}

static inline void
init_srt_thunk( stackPos *info, StgThunkInfoTable *infoTable )
{
    info->type = posTypeSRT;
    info->next.srt.srt = (StgClosure **)(infoTable->srt);
    info->next.srt.srt_end = info->next.srt.srt + infoTable->i.srt_len;
}

/* -----------------------------------------------------------------------------
 * Find the next object from *info.
 * -------------------------------------------------------------------------- */
static inline StgClosure *
find_srt( stackPos *info )
{
    StgClosure *c;

    if (info->next.srt.srt < info->next.srt.srt_end) {
	// See scavenge_srt() in GC.c for details.
#ifdef ENABLE_WIN32_DLL_SUPPORT
	if ((unsigned long)(*(info->next.srt.srt)) & 0x1)
	    c = (* (StgClosure **)((unsigned long)*(info->next.srt.srt)) & ~0x1);
	else
	    c = *(info->next.srt.srt);
#else
	c = *(info->next.srt.srt);
#endif
	info->next.srt.srt++;
	return c;
    } else {
	return NULL;
    }
}

/* -----------------------------------------------------------------------------
 *  push() pushes a stackElement representing the next child of *c
 *  onto the traverse stack. If *c has no child, *first_child is set
 *  to NULL and nothing is pushed onto the stack. If *c has only one
 *  child, *c_chlid is set to that child and nothing is pushed onto
 *  the stack.  If *c has more than two children, *first_child is set
 *  to the first child and a stackElement representing the second
 *  child is pushed onto the stack.

 *  Invariants:
 *     *c_child_r is the most recent retainer of *c's children.
 *     *c is not any of TSO, AP, PAP, AP_STACK, which means that
 *        there cannot be any stack objects.
 *  Note: SRTs are considered to  be children as well.
 * -------------------------------------------------------------------------- */
static inline void
push( StgClosure *c, retainer c_child_r, StgClosure **first_child )
{
    stackElement se;
    bdescr *nbd;      // Next Block Descriptor

#ifdef DEBUG_RETAINER
    // fprintf(stderr, "push(): stackTop = 0x%x, currentStackBoundary = 0x%x\n", stackTop, currentStackBoundary);
#endif

    ASSERT(get_itbl(c)->type != TSO);
    ASSERT(get_itbl(c)->type != AP_STACK);

    //
    // fill in se
    //

    se.c = c;
    se.c_child_r = c_child_r;

    // fill in se.info
    switch (get_itbl(c)->type) {
	// no child, no SRT
    case CONSTR_0_1:
    case CONSTR_0_2:
    case CAF_BLACKHOLE:
    case BLACKHOLE:
    case SE_BLACKHOLE:
    case SE_CAF_BLACKHOLE:
    case ARR_WORDS:
	*first_child = NULL;
	return;

	// one child (fixed), no SRT
    case MUT_VAR:
    case MUT_CONS:
	*first_child = ((StgMutVar *)c)->var;
	return;
    case BLACKHOLE_BQ:
	// blocking_queue must be TSO and the head of a linked list of TSOs.
	// Shoule it be a child? Seems to be yes.
	*first_child = (StgClosure *)((StgBlockingQueue *)c)->blocking_queue;
	return;
    case THUNK_SELECTOR:
	*first_child = ((StgSelector *)c)->selectee;
	return;
    case IND_PERM:
    case IND_OLDGEN_PERM:
    case IND_OLDGEN:
	*first_child = ((StgIndOldGen *)c)->indirectee;
	return;
    case CONSTR_1_0:
    case CONSTR_1_1:
	*first_child = c->payload[0];
	return;

	// For CONSTR_2_0 and MVAR, we use se.info.step to record the position
	// of the next child. We do not write a separate initialization code.
	// Also we do not have to initialize info.type;

	// two children (fixed), no SRT
	// need to push a stackElement, but nothing to store in se.info
    case CONSTR_2_0:
	*first_child = c->payload[0];         // return the first pointer
	// se.info.type = posTypeStep;
	// se.info.next.step = 2;            // 2 = second
	break;

	// three children (fixed), no SRT
	// need to push a stackElement
    case MVAR:
	// head must be TSO and the head of a linked list of TSOs.
	// Shoule it be a child? Seems to be yes.
	*first_child = (StgClosure *)((StgMVar *)c)->head;
	// se.info.type = posTypeStep;
	se.info.next.step = 2;            // 2 = second
	break;

	// three children (fixed), no SRT
    case WEAK:
	*first_child = ((StgWeak *)c)->key;
	// se.info.type = posTypeStep;
	se.info.next.step = 2;
	break;

	// layout.payload.ptrs, no SRT
    case CONSTR:
    case FOREIGN:
    case STABLE_NAME:
    case BCO:
    case CONSTR_STATIC:
	init_ptrs(&se.info, get_itbl(c)->layout.payload.ptrs,
		  (StgPtr)c->payload);
	*first_child = find_ptrs(&se.info);
	if (*first_child == NULL)
	    return;   // no child
	break;

	// StgMutArrPtr.ptrs, no SRT
    case MUT_ARR_PTRS:
    case MUT_ARR_PTRS_FROZEN:
	init_ptrs(&se.info, ((StgMutArrPtrs *)c)->ptrs,
		  (StgPtr)(((StgMutArrPtrs *)c)->payload));
	*first_child = find_ptrs(&se.info);
	if (*first_child == NULL)
	    return;
	break;

    // layout.payload.ptrs, SRT
    case FUN:           // *c is a heap object.
    case FUN_2_0:
	init_ptrs(&se.info, get_itbl(c)->layout.payload.ptrs, (StgPtr)c->payload);
	*first_child = find_ptrs(&se.info);
	if (*first_child == NULL)
	    // no child from ptrs, so check SRT
	    goto fun_srt_only;
	break;

    case THUNK:
    case THUNK_2_0:
	init_ptrs(&se.info, get_itbl(c)->layout.payload.ptrs, (StgPtr)c->payload);
	*first_child = find_ptrs(&se.info);
	if (*first_child == NULL)
	    // no child from ptrs, so check SRT
	    goto thunk_srt_only;
	break;

	// 1 fixed child, SRT
    case FUN_1_0:
    case FUN_1_1:
	*first_child = c->payload[0];
	ASSERT(*first_child != NULL);
	init_srt_fun(&se.info, get_fun_itbl(c));
	break;

    case THUNK_1_0:
    case THUNK_1_1:
	*first_child = c->payload[0];
	ASSERT(*first_child != NULL);
	init_srt_thunk(&se.info, get_thunk_itbl(c));
	break;

    case FUN_STATIC:      // *c is a heap object.
	ASSERT(get_itbl(c)->srt_len != 0);
    case FUN_0_1:
    case FUN_0_2:
    fun_srt_only:
        init_srt_fun(&se.info, get_fun_itbl(c));
	*first_child = find_srt(&se.info);
	if (*first_child == NULL)
	    return;     // no child
	break;

    // SRT only
    case THUNK_STATIC:
	ASSERT(get_itbl(c)->srt_len != 0);
    case THUNK_0_1:
    case THUNK_0_2:
    thunk_srt_only:
        init_srt_thunk(&se.info, get_thunk_itbl(c));
	*first_child = find_srt(&se.info);
	if (*first_child == NULL)
	    return;     // no child
	break;

	// cannot appear
    case PAP:
    case AP:
    case AP_STACK:
    case TSO:
    case IND_STATIC:
    case CONSTR_INTLIKE:
    case CONSTR_CHARLIKE:
    case CONSTR_NOCAF_STATIC:
	// stack objects
    case UPDATE_FRAME:
    case CATCH_FRAME:
    case STOP_FRAME:
    case RET_DYN:
    case RET_BCO:
    case RET_SMALL:
    case RET_VEC_SMALL:
    case RET_BIG:
    case RET_VEC_BIG:
	// invalid objects
    case IND:
    case BLOCKED_FETCH:
    case FETCH_ME:
    case FETCH_ME_BQ:
    case RBH:
    case REMOTE_REF:
    case EVACUATED:
    case INVALID_OBJECT:
    default:
	barf("Invalid object *c in push()");
	return;
    }

    if (stackTop - 1 < stackBottom) {
#ifdef DEBUG_RETAINER
	// fprintf(stderr, "push() to the next stack.\n");
#endif
	// currentStack->free is updated when the active stack is switched
	// to the next stack.
	currentStack->free = (StgPtr)stackTop;

	if (currentStack->link == NULL) {
	    nbd = allocGroup(BLOCKS_IN_STACK);
	    nbd->link = NULL;
	    nbd->u.back = currentStack;
	    currentStack->link = nbd;
	} else
	    nbd = currentStack->link;

	newStackBlock(nbd);
    }

    // adjust stackTop (acutal push)
    stackTop--;
    // If the size of stackElement was huge, we would better replace the
    // following statement by either a memcpy() call or a switch statement
    // on the type of the element. Currently, the size of stackElement is
    // small enough (5 words) that this direct assignment seems to be enough.
    *stackTop = se;

#ifdef DEBUG_RETAINER
    stackSize++;
    if (stackSize > maxStackSize) maxStackSize = stackSize;
    // ASSERT(stackSize >= 0);
    // fprintf(stderr, "stackSize = %d\n", stackSize);
#endif
}

/* -----------------------------------------------------------------------------
 *  popOff() and popOffReal(): Pop a stackElement off the traverse stack.
 *  Invariants:
 *    stackTop cannot be equal to stackLimit unless the whole stack is
 *    empty, in which case popOff() is not allowed.
 *  Note:
 *    You can think of popOffReal() as a part of popOff() which is
 *    executed at the end of popOff() in necessary. Since popOff() is
 *    likely to be executed quite often while popOffReal() is not, we
 *    separate popOffReal() from popOff(), which is declared as an
 *    inline function (for the sake of execution speed).  popOffReal()
 *    is called only within popOff() and nowhere else.
 * -------------------------------------------------------------------------- */
static void
popOffReal(void)
{
    bdescr *pbd;    // Previous Block Descriptor

#ifdef DEBUG_RETAINER
    // fprintf(stderr, "pop() to the previous stack.\n");
#endif

    ASSERT(stackTop + 1 == stackLimit);
    ASSERT(stackBottom == (stackElement *)currentStack->start);

    if (firstStack == currentStack) {
	// The stack is completely empty.
	stackTop++;
	ASSERT(stackTop == stackLimit);
#ifdef DEBUG_RETAINER
	stackSize--;
	if (stackSize > maxStackSize) maxStackSize = stackSize;
	/*
	  ASSERT(stackSize >= 0);
	  fprintf(stderr, "stackSize = %d\n", stackSize);
	*/
#endif
	return;
    }

    // currentStack->free is updated when the active stack is switched back
    // to the previous stack.
    currentStack->free = (StgPtr)stackLimit;

    // find the previous block descriptor
    pbd = currentStack->u.back;
    ASSERT(pbd != NULL);

    returnToOldStack(pbd);

#ifdef DEBUG_RETAINER
    stackSize--;
    if (stackSize > maxStackSize) maxStackSize = stackSize;
    /*
      ASSERT(stackSize >= 0);
      fprintf(stderr, "stackSize = %d\n", stackSize);
    */
#endif
}

static inline void
popOff(void) {
#ifdef DEBUG_RETAINER
    // fprintf(stderr, "\tpopOff(): stackTop = 0x%x, currentStackBoundary = 0x%x\n", stackTop, currentStackBoundary);
#endif

    ASSERT(stackTop != stackLimit);
    ASSERT(!isEmptyRetainerStack());

    // <= (instead of <) is wrong!
    if (stackTop + 1 < stackLimit) {
	stackTop++;
#ifdef DEBUG_RETAINER
	stackSize--;
	if (stackSize > maxStackSize) maxStackSize = stackSize;
	/*
	  ASSERT(stackSize >= 0);
	  fprintf(stderr, "stackSize = %d\n", stackSize);
	*/
#endif
	return;
    }

    popOffReal();
}

/* -----------------------------------------------------------------------------
 *  Finds the next object to be considered for retainer profiling and store
 *  its pointer to *c.
 *  Test if the topmost stack element indicates that more objects are left,
 *  and if so, retrieve the first object and store its pointer to *c. Also,
 *  set *cp and *r appropriately, both of which are stored in the stack element.
 *  The topmost stack element then is overwritten so as for it to now denote
 *  the next object.
 *  If the topmost stack element indicates no more objects are left, pop
 *  off the stack element until either an object can be retrieved or
 *  the current stack chunk becomes empty, indicated by rtsTrue returned by
 *  isOnBoundary(), in which case *c is set to NULL.
 *  Note:
 *    It is okay to call this function even when the current stack chunk
 *    is empty.
 * -------------------------------------------------------------------------- */
static inline void
pop( StgClosure **c, StgClosure **cp, retainer *r )
{
    stackElement *se;

#ifdef DEBUG_RETAINER
    // fprintf(stderr, "pop(): stackTop = 0x%x, currentStackBoundary = 0x%x\n", stackTop, currentStackBoundary);
#endif

    do {
	if (isOnBoundary()) {     // if the current stack chunk is depleted
	    *c = NULL;
	    return;
	}

	se = stackTop;

	switch (get_itbl(se->c)->type) {
	    // two children (fixed), no SRT
	    // nothing in se.info
	case CONSTR_2_0:
	    *c = se->c->payload[1];
	    *cp = se->c;
	    *r = se->c_child_r;
	    popOff();
	    return;

	    // three children (fixed), no SRT
	    // need to push a stackElement
	case MVAR:
	    if (se->info.next.step == 2) {
		*c = (StgClosure *)((StgMVar *)se->c)->tail;
		se->info.next.step++;             // move to the next step
		// no popOff
	    } else {
		*c = ((StgMVar *)se->c)->value;
		popOff();
	    }
	    *cp = se->c;
	    *r = se->c_child_r;
	    return;

	    // three children (fixed), no SRT
	case WEAK:
	    if (se->info.next.step == 2) {
		*c = ((StgWeak *)se->c)->value;
		se->info.next.step++;
		// no popOff
	    } else {
		*c = ((StgWeak *)se->c)->finalizer;
		popOff();
	    }
	    *cp = se->c;
	    *r = se->c_child_r;
	    return;

	case CONSTR:
	case FOREIGN:
	case STABLE_NAME:
	case BCO:
	case CONSTR_STATIC:
	    // StgMutArrPtr.ptrs, no SRT
	case MUT_ARR_PTRS:
	case MUT_ARR_PTRS_FROZEN:
	    *c = find_ptrs(&se->info);
	    if (*c == NULL) {
		popOff();
		break;
	    }
	    *cp = se->c;
	    *r = se->c_child_r;
	    return;

	    // layout.payload.ptrs, SRT
	case FUN:         // always a heap object
	case FUN_2_0:
	    if (se->info.type == posTypePtrs) {
		*c = find_ptrs(&se->info);
		if (*c != NULL) {
		    *cp = se->c;
		    *r = se->c_child_r;
		    return;
		}
		init_srt_fun(&se->info, get_fun_itbl(se->c));
	    }
	    goto do_srt;

	case THUNK:
	case THUNK_2_0:
	    if (se->info.type == posTypePtrs) {
		*c = find_ptrs(&se->info);
		if (*c != NULL) {
		    *cp = se->c;
		    *r = se->c_child_r;
		    return;
		}
		init_srt_thunk(&se->info, get_thunk_itbl(se->c));
	    }
	    goto do_srt;

	    // SRT
	do_srt:
	case THUNK_STATIC:
	case FUN_STATIC:
	case FUN_0_1:
	case FUN_0_2:
	case THUNK_0_1:
	case THUNK_0_2:
	case FUN_1_0:
	case FUN_1_1:
	case THUNK_1_0:
	case THUNK_1_1:
	    *c = find_srt(&se->info);
	    if (*c != NULL) {
		*cp = se->c;
		*r = se->c_child_r;
		return;
	    }
	    popOff();
	    break;

	    // no child (fixed), no SRT
	case CONSTR_0_1:
	case CONSTR_0_2:
	case CAF_BLACKHOLE:
	case BLACKHOLE:
	case SE_BLACKHOLE:
	case SE_CAF_BLACKHOLE:
	case ARR_WORDS:
	    // one child (fixed), no SRT
	case MUT_VAR:
	case MUT_CONS:
	case BLACKHOLE_BQ:
	case THUNK_SELECTOR:
	case IND_PERM:
	case IND_OLDGEN_PERM:
	case IND_OLDGEN:
	case CONSTR_1_1:
	    // cannot appear
	case PAP:
	case AP:
	case AP_STACK:
	case TSO:
	case IND_STATIC:
	case CONSTR_INTLIKE:
	case CONSTR_CHARLIKE:
	case CONSTR_NOCAF_STATIC:
	    // stack objects
	case RET_DYN:
	case UPDATE_FRAME:
	case CATCH_FRAME:
	case STOP_FRAME:
	case RET_BCO:
	case RET_SMALL:
	case RET_VEC_SMALL:
	case RET_BIG:
	case RET_VEC_BIG:
	    // invalid objects
	case IND:
	case BLOCKED_FETCH:
	case FETCH_ME:
	case FETCH_ME_BQ:
	case RBH:
	case REMOTE_REF:
	case EVACUATED:
	case INVALID_OBJECT:
	default:
	    barf("Invalid object *c in pop()");
	    return;
	}
    } while (rtsTrue);
}

/* -----------------------------------------------------------------------------
 * RETAINER PROFILING ENGINE
 * -------------------------------------------------------------------------- */

void
initRetainerProfiling( void )
{
    initializeAllRetainerSet();
    retainerGeneration = 0;
}

/* -----------------------------------------------------------------------------
 *  This function must be called before f-closing prof_file.
 * -------------------------------------------------------------------------- */
void
endRetainerProfiling( void )
{
#ifdef SECOND_APPROACH
    outputAllRetainerSet(prof_file);
#endif
}

/* -----------------------------------------------------------------------------
 *  Returns the actual pointer to the retainer set of the closure *c.
 *  It may adjust RSET(c) subject to flip.
 *  Side effects:
 *    RSET(c) is initialized to NULL if its current value does not
 *    conform to flip.
 *  Note:
 *    Even though this function has side effects, they CAN be ignored because
 *    subsequent calls to retainerSetOf() always result in the same return value
 *    and retainerSetOf() is the only way to retrieve retainerSet of a given
 *    closure.
 *    We have to perform an XOR (^) operation each time a closure is examined.
 *    The reason is that we do not know when a closure is visited last.
 * -------------------------------------------------------------------------- */
static inline void
maybeInitRetainerSet( StgClosure *c )
{
    if (!isRetainerSetFieldValid(c)) {
	setRetainerSetToNull(c);
    }
}

/* -----------------------------------------------------------------------------
 * Returns rtsTrue if *c is a retainer.
 * -------------------------------------------------------------------------- */
static inline rtsBool
isRetainer( StgClosure *c )
{
    switch (get_itbl(c)->type) {
	//
	//  True case
	//
	// TSOs MUST be retainers: they constitute the set of roots.
    case TSO:

	// mutable objects
    case MVAR:
    case MUT_VAR:
    case MUT_CONS:
    case MUT_ARR_PTRS:
    case MUT_ARR_PTRS_FROZEN:

	// thunks are retainers.
    case THUNK:
    case THUNK_1_0:
    case THUNK_0_1:
    case THUNK_2_0:
    case THUNK_1_1:
    case THUNK_0_2:
    case THUNK_SELECTOR:
    case AP:
    case AP_STACK:

	// Static thunks, or CAFS, are obviously retainers.
    case THUNK_STATIC:

	// WEAK objects are roots; there is separate code in which traversing
	// begins from WEAK objects.
    case WEAK:
	return rtsTrue;

	//
	// False case
	//

	// constructors
    case CONSTR:
    case CONSTR_1_0:
    case CONSTR_0_1:
    case CONSTR_2_0:
    case CONSTR_1_1:
    case CONSTR_0_2:
	// functions
    case FUN:
    case FUN_1_0:
    case FUN_0_1:
    case FUN_2_0:
    case FUN_1_1:
    case FUN_0_2:
	// partial applications
    case PAP:
	// blackholes
    case CAF_BLACKHOLE:
    case BLACKHOLE:
    case SE_BLACKHOLE:
    case SE_CAF_BLACKHOLE:
    case BLACKHOLE_BQ:
	// indirection
    case IND_PERM:
    case IND_OLDGEN_PERM:
    case IND_OLDGEN:
	// static objects
    case CONSTR_STATIC:
    case FUN_STATIC:
	// misc
    case FOREIGN:
    case STABLE_NAME:
    case BCO:
    case ARR_WORDS:
	return rtsFalse;

	//
	// Error case
	//
	// IND_STATIC cannot be *c, *cp, *r in the retainer profiling loop.
    case IND_STATIC:
	// CONSTR_INTLIKE, CONSTR_CHARLIKE, and CONSTR_NOCAF_STATIC
	// cannot be *c, *cp, *r in the retainer profiling loop.
    case CONSTR_INTLIKE:
    case CONSTR_CHARLIKE:
    case CONSTR_NOCAF_STATIC:
	// Stack objects are invalid because they are never treated as
	// legal objects during retainer profiling.
    case UPDATE_FRAME:
    case CATCH_FRAME:
    case STOP_FRAME:
    case RET_DYN:
    case RET_BCO:
    case RET_SMALL:
    case RET_VEC_SMALL:
    case RET_BIG:
    case RET_VEC_BIG:
	// other cases
    case IND:
    case BLOCKED_FETCH:
    case FETCH_ME:
    case FETCH_ME_BQ:
    case RBH:
    case REMOTE_REF:
    case EVACUATED:
    case INVALID_OBJECT:
    default:
	barf("Invalid object in isRetainer(): %d", get_itbl(c)->type);
	return rtsFalse;
    }
}

/* -----------------------------------------------------------------------------
 *  Returns the retainer function value for the closure *c, i.e., R(*c).
 *  This function does NOT return the retainer(s) of *c.
 *  Invariants:
 *    *c must be a retainer.
 *  Note:
 *    Depending on the definition of this function, the maintenance of retainer
 *    sets can be made easier. If most retainer sets are likely to be created
 *    again across garbage collections, refreshAllRetainerSet() in
 *    RetainerSet.c can simply do nothing.
 *    If this is not the case, we can free all the retainer sets and
 *    re-initialize the hash table.
 *    See refreshAllRetainerSet() in RetainerSet.c.
 * -------------------------------------------------------------------------- */
static inline retainer
getRetainerFrom( StgClosure *c )
{
    ASSERT(isRetainer(c));

#if defined(RETAINER_SCHEME_INFO)
    // Retainer scheme 1: retainer = info table
    return get_itbl(c);
#elif defined(RETAINER_SCHEME_CCS)
    // Retainer scheme 2: retainer = cost centre stack
    return c->header.prof.ccs;
#elif defined(RETAINER_SCHEME_CC)
    // Retainer scheme 3: retainer = cost centre
    return c->header.prof.ccs->cc;
#endif
}

/* -----------------------------------------------------------------------------
 *  Associates the retainer set *s with the closure *c, that is, *s becomes
 *  the retainer set of *c.
 *  Invariants:
 *    c != NULL
 *    s != NULL
 * -------------------------------------------------------------------------- */
static inline void
associate( StgClosure *c, RetainerSet *s )
{
    // StgWord has the same size as pointers, so the following type
    // casting is okay.
    RSET(c) = (RetainerSet *)((StgWord)s | flip);
}

/* -----------------------------------------------------------------------------
 * Call retainClosure for each of the closures in an SRT.
 * ------------------------------------------------------------------------- */

static inline void
retainSRT (StgClosure **srt, nat srt_len, StgClosure *c, retainer c_child_r)
{
  StgClosure **srt_end;

  srt_end = srt + srt_len;

  for (; srt < srt_end; srt++) {
    /* Special-case to handle references to closures hiding out in DLLs, since
       double indirections required to get at those. The code generator knows
       which is which when generating the SRT, so it stores the (indirect)
       reference to the DLL closure in the table by first adding one to it.
       We check for this here, and undo the addition before evacuating it.

       If the SRT entry hasn't got bit 0 set, the SRT entry points to a
       closure that's fixed at link-time, and no extra magic is required.
    */
#ifdef ENABLE_WIN32_DLL_SUPPORT
    if ( (unsigned long)(*srt) & 0x1 ) {
       retainClosure(*stgCast(StgClosure**,(stgCast(unsigned long, *srt) & ~0x1)), 
		     c, c_child_r);
    } else {
       retainClosure(*srt,c,c_child_r);
    }
#else
    retainClosure(*srt,c,c_child_r);
#endif
  }
}

/* -----------------------------------------------------------------------------
   Call retainClosure for each of the closures covered by a large bitmap.
   -------------------------------------------------------------------------- */

static void
retain_large_bitmap (StgPtr p, StgLargeBitmap *large_bitmap, nat size,
		     StgClosure *c, retainer c_child_r)
{
    nat i, b;
    StgWord bitmap;
    
    b = 0;
    bitmap = large_bitmap->bitmap[b];
    for (i = 0; i < size; ) {
	if ((bitmap & 1) == 0) {
	    retainClosure((StgClosure *)*p, c, c_child_r);
	}
	i++;
	p++;
	if (i % BITS_IN(W_) == 0) {
	    b++;
	    bitmap = large_bitmap->bitmap[b];
	} else {
	    bitmap = bitmap >> 1;
	}
    }
}

static inline StgPtr
retain_small_bitmap (StgPtr p, nat size, StgWord bitmap,
		     StgClosure *c, retainer c_child_r)
{
    while (size > 0) {
	if ((bitmap & 1) == 0) {
	    retainClosure((StgClosure *)*p, c, c_child_r);
	}
	p++;
	bitmap = bitmap >> 1;
	size--;
    }
    return p;
}

/* -----------------------------------------------------------------------------
 *  Process all the objects in the stack chunk from stackStart to stackEnd
 *  with *c and *c_child_r being their parent and their most recent retainer,
 *  respectively. Treat stackOptionalFun as another child of *c if it is
 *  not NULL.
 *  Invariants:
 *    *c is one of the following: TSO, AP_STACK.
 *    If *c is TSO, c == c_child_r.
 *    stackStart < stackEnd.
 *    RSET(c) and RSET(c_child_r) are valid, i.e., their
 *    interpretation conforms to the current value of flip (even when they
 *    are interpreted to be NULL).
 *    If *c is TSO, its state is not any of ThreadRelocated, ThreadComplete,
 *    or ThreadKilled, which means that its stack is ready to process.
 *  Note:
 *    This code was almost plagiarzied from GC.c! For each pointer,
 *    retainClosure() is invoked instead of evacuate().
 * -------------------------------------------------------------------------- */
static void
retainStack( StgClosure *c, retainer c_child_r,
	     StgPtr stackStart, StgPtr stackEnd )
{
    stackElement *oldStackBoundary;
    StgPtr p;
    StgRetInfoTable *info;
    StgWord32 bitmap;
    nat size;

#ifdef DEBUG_RETAINER
    cStackSize++;
    if (cStackSize > maxCStackSize) maxCStackSize = cStackSize;
#endif

    /*
      Each invocation of retainStack() creates a new virtual
      stack. Since all such stacks share a single common stack, we
      record the current currentStackBoundary, which will be restored
      at the exit.
    */
    oldStackBoundary = currentStackBoundary;
    currentStackBoundary = stackTop;

#ifdef DEBUG_RETAINER
    // fprintf(stderr, "retainStack() called: oldStackBoundary = 0x%x, currentStackBoundary = 0x%x\n", oldStackBoundary, currentStackBoundary);
#endif

    ASSERT(get_itbl(c)->type != TSO || 
	   (((StgTSO *)c)->what_next != ThreadRelocated &&
	    ((StgTSO *)c)->what_next != ThreadComplete &&
	    ((StgTSO *)c)->what_next != ThreadKilled));
    
    p = stackStart;
    while (p < stackEnd) {
	info = get_ret_itbl((StgClosure *)p);

	switch(info->i.type) {

	case UPDATE_FRAME:
	    retainClosure(((StgUpdateFrame *)p)->updatee, c, c_child_r);
	    p += sizeofW(StgUpdateFrame);
	    continue;

	case STOP_FRAME:
	case CATCH_FRAME:
	case RET_SMALL:
	case RET_VEC_SMALL:
	    bitmap = BITMAP_BITS(info->i.layout.bitmap);
	    size   = BITMAP_SIZE(info->i.layout.bitmap);
	    p++;
	    p = retain_small_bitmap(p, size, bitmap, c, c_child_r);

	follow_srt:
	    retainSRT((StgClosure **)info->srt, info->i.srt_len, c, c_child_r);
	    continue;

	case RET_BCO: {
	    StgBCO *bco;
	    
	    p++;
	    retainClosure((StgClosure *)*p, c, c_child_r);
	    bco = (StgBCO *)*p;
	    p++;
	    size = BCO_BITMAP_SIZE(bco);
	    retain_large_bitmap(p, BCO_BITMAP(bco), size, c, c_child_r);
	    p += size;
	    continue;
	}

	    // large bitmap (> 32 entries, or > 64 on a 64-bit machine) 
	case RET_BIG:
	case RET_VEC_BIG:
	    size = info->i.layout.large_bitmap->size;
	    p++;
	    retain_large_bitmap(p, info->i.layout.large_bitmap,
				size, c, c_child_r);
	    p += size;
	    // and don't forget to follow the SRT 
	    goto follow_srt;

	    // Dynamic bitmap: the mask is stored on the stack 
	case RET_DYN: {
	    StgWord dyn;
	    dyn = ((StgRetDyn *)p)->liveness;

	    // traverse the bitmap first
	    bitmap = GET_LIVENESS(dyn);
	    p      = (P_)&((StgRetDyn *)p)->payload[0];
	    size   = RET_DYN_BITMAP_SIZE;
	    p = retain_small_bitmap(p, size, bitmap, c, c_child_r);
	    
	    // skip over the non-ptr words
	    p += GET_NONPTRS(dyn) + RET_DYN_NONPTR_REGS_SIZE;
	    
	    // follow the ptr words
	    for (size = GET_PTRS(dyn); size > 0; size--) {
		retainClosure((StgClosure *)*p, c, c_child_r);
		p++;
	    }
	    continue;
	}

	case RET_FUN: {
	    StgRetFun *ret_fun = (StgRetFun *)p;
	    StgFunInfoTable *fun_info;
	    
	    retainClosure(ret_fun->fun, c, c_child_r);
	    fun_info = get_fun_itbl(ret_fun->fun);
	    
	    p = (P_)&ret_fun->payload;
	    switch (fun_info->fun_type) {
	    case ARG_GEN:
		bitmap = BITMAP_BITS(fun_info->bitmap);
		size = BITMAP_SIZE(fun_info->bitmap);
		p = retain_small_bitmap(p, size, bitmap, c, c_child_r);
		break;
	    case ARG_GEN_BIG:
		size = ((StgLargeBitmap *)fun_info->bitmap)->size;
		retain_large_bitmap(p, (StgLargeBitmap *)fun_info->bitmap, 
				    size, c, c_child_r);
		p += size;
		break;
	    default:
		bitmap = BITMAP_BITS(stg_arg_bitmaps[fun_info->fun_type]);
		size = BITMAP_SIZE(stg_arg_bitmaps[fun_info->fun_type]);
		p = retain_small_bitmap(p, size, bitmap, c, c_child_r);
		break;
	    }
	    goto follow_srt;
	}

	default:
	    barf("Invalid object found in retainStack(): %d",
		 (int)(info->i.type));
	}
    }

    // restore currentStackBoundary
    currentStackBoundary = oldStackBoundary;
#ifdef DEBUG_RETAINER
    // fprintf(stderr, "retainStack() finished: currentStackBoundary = 0x%x\n", currentStackBoundary);
#endif

#ifdef DEBUG_RETAINER
    cStackSize--;
#endif
}

/* ----------------------------------------------------------------------------
 * Call retainClosure for each of the children of a PAP/AP
 * ------------------------------------------------------------------------- */

static inline StgPtr
retain_PAP (StgPAP *pap, retainer c_child_r)
{
    StgPtr p;
    StgWord bitmap, size;
    StgFunInfoTable *fun_info;

    retainClosure(pap->fun, (StgClosure *)pap, c_child_r);
    fun_info = get_fun_itbl(pap->fun);
    ASSERT(fun_info->i.type != PAP);

    p = (StgPtr)pap->payload;
    size = pap->n_args;

    switch (fun_info->fun_type) {
    case ARG_GEN:
	bitmap = BITMAP_BITS(fun_info->bitmap);
	p = retain_small_bitmap(p, pap->n_args, bitmap, 
				(StgClosure *)pap, c_child_r);
	break;
    case ARG_GEN_BIG:
	retain_large_bitmap(p, (StgLargeBitmap *)fun_info->bitmap,
			    size, (StgClosure *)pap, c_child_r);
	p += size;
	break;
    case ARG_BCO:
	retain_large_bitmap((StgPtr)pap->payload, BCO_BITMAP(pap->fun),
			    size, (StgClosure *)pap, c_child_r);
	p += size;
	break;
    default:
	bitmap = BITMAP_BITS(stg_arg_bitmaps[fun_info->fun_type]);
	p = retain_small_bitmap(p, pap->n_args, bitmap, 
				(StgClosure *)pap, c_child_r);
	break;
    }
    return p;
}

/* -----------------------------------------------------------------------------
 *  Compute the retainer set of *c0 and all its desecents by traversing.
 *  *cp0 is the parent of *c0, and *r0 is the most recent retainer of *c0.
 *  Invariants:
 *    c0 = cp0 = r0 holds only for root objects.
 *    RSET(cp0) and RSET(r0) are valid, i.e., their
 *    interpretation conforms to the current value of flip (even when they
 *    are interpreted to be NULL).
 *    However, RSET(c0) may be corrupt, i.e., it may not conform to
 *    the current value of flip. If it does not, during the execution
 *    of this function, RSET(c0) must be initialized as well as all
 *    its descendants.
 *  Note:
 *    stackTop must be the same at the beginning and the exit of this function.
 *    *c0 can be TSO (as well as AP_STACK).
 * -------------------------------------------------------------------------- */
static void
retainClosure( StgClosure *c0, StgClosure *cp0, retainer r0 )
{
    // c = Current closure
    // cp = Current closure's Parent
    // r = current closures' most recent Retainer
    // c_child_r = current closure's children's most recent retainer
    // first_child = first child of c
    StgClosure *c, *cp, *first_child;
    RetainerSet *s, *retainerSetOfc;
    retainer r, c_child_r;
    StgWord typeOfc;

#ifdef DEBUG_RETAINER
    // StgPtr oldStackTop;
#endif

#ifdef DEBUG_RETAINER
    // oldStackTop = stackTop;
    // fprintf(stderr, "retainClosure() called: c0 = 0x%x, cp0 = 0x%x, r0 = 0x%x\n", c0, cp0, r0);
#endif

    // (c, cp, r) = (c0, cp0, r0)
    c = c0;
    cp = cp0;
    r = r0;
    goto inner_loop;

loop:
    //fprintf(stderr, "loop");
    // pop to (c, cp, r);
    pop(&c, &cp, &r);

    if (c == NULL) {
#ifdef DEBUG_RETAINER
	// fprintf(stderr, "retainClosure() ends: oldStackTop = 0x%x, stackTop = 0x%x\n", oldStackTop, stackTop);
#endif
	return;
    }

    //fprintf(stderr, "inner_loop");

inner_loop:
    // c  = current closure under consideration,
    // cp = current closure's parent,
    // r  = current closure's most recent retainer
    //
    // Loop invariants (on the meaning of c, cp, r, and their retainer sets):
    //   RSET(cp) and RSET(r) are valid.
    //   RSET(c) is valid only if c has been visited before.
    //
    // Loop invariants (on the relation between c, cp, and r)
    //   if cp is not a retainer, r belongs to RSET(cp).
    //   if cp is a retainer, r == cp.

    typeOfc = get_itbl(c)->type;

#ifdef DEBUG_RETAINER
    switch (typeOfc) {
    case IND_STATIC:
    case CONSTR_INTLIKE:
    case CONSTR_CHARLIKE:
    case CONSTR_NOCAF_STATIC:
    case CONSTR_STATIC:
    case THUNK_STATIC:
    case FUN_STATIC:
	break;
    default:
	if (retainerSetOf(c) == NULL) {   // first visit?
	    costArray[typeOfc] += cost(c);
	    sumOfNewCost += cost(c);
	}
	break;
    }
#endif

    // special cases
    switch (typeOfc) {
    case TSO:
	if (((StgTSO *)c)->what_next == ThreadComplete ||
	    ((StgTSO *)c)->what_next == ThreadKilled) {
#ifdef DEBUG_RETAINER
	    fprintf(stderr, "ThreadComplete or ThreadKilled encountered in retainClosure()\n");
#endif
	    goto loop;
	}
	if (((StgTSO *)c)->what_next == ThreadRelocated) {
#ifdef DEBUG_RETAINER
	    fprintf(stderr, "ThreadRelocated encountered in retainClosure()\n");
#endif
	    c = (StgClosure *)((StgTSO *)c)->link;
	    goto inner_loop;
	}
	break;

    case IND_STATIC:
	// We just skip IND_STATIC, so its retainer set is never computed.
	c = ((StgIndStatic *)c)->indirectee;
	goto inner_loop;
    case CONSTR_INTLIKE:
    case CONSTR_CHARLIKE:
	// static objects with no pointers out, so goto loop.
    case CONSTR_NOCAF_STATIC:
	// It is not just enough not to compute the retainer set for *c; it is
	// mandatory because CONSTR_NOCAF_STATIC are not reachable from
	// scavenged_static_objects, the list from which is assumed to traverse
	// all static objects after major garbage collections.
	goto loop;
    case THUNK_STATIC:
    case FUN_STATIC:
	if (get_itbl(c)->srt_len == 0) {
	    // No need to compute the retainer set; no dynamic objects
	    // are reachable from *c.
	    //
	    // Static objects: if we traverse all the live closures,
	    // including static closures, during each heap census then
	    // we will observe that some static closures appear and
	    // disappear.  eg. a closure may contain a pointer to a
	    // static function 'f' which is not otherwise reachable
	    // (it doesn't indirectly point to any CAFs, so it doesn't
	    // appear in any SRTs), so we would find 'f' during
	    // traversal.  However on the next sweep there may be no
	    // closures pointing to 'f'.
	    //
	    // We must therefore ignore static closures whose SRT is
	    // empty, because these are exactly the closures that may
	    // "appear".  A closure with a non-empty SRT, and which is
	    // still required, will always be reachable.
	    //
	    // But what about CONSTR_STATIC?  Surely these may be able
	    // to appear, and they don't have SRTs, so we can't
	    // check.  So for now, we're calling
	    // resetStaticObjectForRetainerProfiling() from the
	    // garbage collector to reset the retainer sets in all the
	    // reachable static objects.
	    goto loop;
	}
    default:
	break;
    }

    // The above objects are ignored in computing the average number of times
    // an object is visited.
    timesAnyObjectVisited++;

    // If this is the first visit to c, initialize its retainer set.
    maybeInitRetainerSet(c);
    retainerSetOfc = retainerSetOf(c);

    // Now compute s:
    //    isRetainer(cp) == rtsTrue => s == NULL
    //    isRetainer(cp) == rtsFalse => s == cp.retainer
    if (isRetainer(cp))
	s = NULL;
    else
	s = retainerSetOf(cp);

    // (c, cp, r, s) is available.

    // (c, cp, r, s, R_r) is available, so compute the retainer set for *c.
    if (retainerSetOfc == NULL) {
	// This is the first visit to *c.
	numObjectVisited++;

	if (s == NULL)
	    associate(c, singleton(r));
	else
	    // s is actually the retainer set of *c!
	    associate(c, s);

	// compute c_child_r
	c_child_r = isRetainer(c) ? getRetainerFrom(c) : r;
    } else {
	// This is not the first visit to *c.
	if (isMember(r, retainerSetOfc))
	    goto loop;          // no need to process child

	if (s == NULL)
	    associate(c, addElement(r, retainerSetOfc));
	else {
	    // s is not NULL and cp is not a retainer. This means that
	    // each time *cp is visited, so is *c. Thus, if s has
	    // exactly one more element in its retainer set than c, s
	    // is also the new retainer set for *c.
	    if (s->num == retainerSetOfc->num + 1) {
		associate(c, s);
	    }
	    // Otherwise, just add R_r to the current retainer set of *c.
	    else {
		associate(c, addElement(r, retainerSetOfc));
	    }
	}

	if (isRetainer(c))
	    goto loop;          // no need to process child

	// compute c_child_r
	c_child_r = r;
    }

    // now, RSET() of all of *c, *cp, and *r is valid.
    // (c, c_child_r) are available.

    // process child

    // Special case closures: we process these all in one go rather
    // than attempting to save the current position, because doing so
    // would be hard.
    switch (typeOfc) {
    case TSO:
	retainStack(c, c_child_r,
		    ((StgTSO *)c)->sp,
		    ((StgTSO *)c)->stack + ((StgTSO *)c)->stack_size);
	goto loop;

    case PAP:
    case AP:
	retain_PAP((StgPAP *)c, c_child_r);
	goto loop;

    case AP_STACK:
	retainClosure(((StgAP_STACK *)c)->fun, c, c_child_r);
	retainStack(c, c_child_r,
		    (StgPtr)((StgAP_STACK *)c)->payload,
		    (StgPtr)((StgAP_STACK *)c)->payload +
		             ((StgAP_STACK *)c)->size);
	goto loop;
    }

    push(c, c_child_r, &first_child);

    // If first_child is null, c has no child.
    // If first_child is not null, the top stack element points to the next
    // object. push() may or may not push a stackElement on the stack.
    if (first_child == NULL)
	goto loop;

    // (c, cp, r) = (first_child, c, c_child_r)
    r = c_child_r;
    cp = c;
    c = first_child;
    goto inner_loop;
}

/* -----------------------------------------------------------------------------
 *  Compute the retainer set for every object reachable from *tl.
 * -------------------------------------------------------------------------- */
static void
retainRoot( StgClosure **tl )
{
    // We no longer assume that only TSOs and WEAKs are roots; any closure can
    // be a root.

    ASSERT(isEmptyRetainerStack());
    currentStackBoundary = stackTop;

    if (isRetainer(*tl)) {
	retainClosure(*tl, *tl, getRetainerFrom(*tl));
    } else {
	retainClosure(*tl, *tl, CCS_SYSTEM);
    }

    // NOT TRUE: ASSERT(isMember(getRetainerFrom(*tl), retainerSetOf(*tl)));
    // *tl might be a TSO which is ThreadComplete, in which
    // case we ignore it for the purposes of retainer profiling.
}

/* -----------------------------------------------------------------------------
 *  Compute the retainer set for each of the objects in the heap.
 * -------------------------------------------------------------------------- */
static void
computeRetainerSet( void )
{
    StgWeak *weak;
    RetainerSet *rtl;
    nat g;
    StgMutClosure *ml;
#ifdef DEBUG_RETAINER
    RetainerSet tmpRetainerSet;
#endif

    GetRoots(retainRoot);	// for scheduler roots

    // This function is called after a major GC, when key, value, and finalizer
    // all are guaranteed to be valid, or reachable.
    //
    // The following code assumes that WEAK objects are considered to be roots
    // for retainer profilng.
    for (weak = weak_ptr_list; weak != NULL; weak = weak->link)
	// retainRoot((StgClosure *)weak);
	retainRoot((StgClosure **)&weak);

    // Consider roots from the stable ptr table.
    markStablePtrTable(retainRoot);

    // The following code resets the rs field of each unvisited mutable
    // object (computing sumOfNewCostExtra and updating costArray[] when
    // debugging retainer profiler).
    for (g = 0; g < RtsFlags.GcFlags.generations; g++) {
	ASSERT(g != 0 ||
	       (generations[g].mut_list == END_MUT_LIST &&
		generations[g].mut_once_list == END_MUT_LIST));

	// Todo:
	// I think traversing through mut_list is unnecessary.
	// Think about removing this part.
	for (ml = generations[g].mut_list; ml != END_MUT_LIST;
	     ml = ml->mut_link) {

	    maybeInitRetainerSet((StgClosure *)ml);
	    rtl = retainerSetOf((StgClosure *)ml);

#ifdef DEBUG_RETAINER
	    if (rtl == NULL) {
		// first visit to *ml
		// This is a violation of the interface rule!
		RSET(ml) = (RetainerSet *)((StgWord)(&tmpRetainerSet) | flip);

		switch (get_itbl((StgClosure *)ml)->type) {
		case IND_STATIC:
		    // no cost involved
		    break;
		case CONSTR_INTLIKE:
		case CONSTR_CHARLIKE:
		case CONSTR_NOCAF_STATIC:
		case CONSTR_STATIC:
		case THUNK_STATIC:
		case FUN_STATIC:
		    barf("Invalid object in computeRetainerSet(): %d", get_itbl((StgClosure*)ml)->type);
		    break;
		default:
		    // dynamic objects
		    costArray[get_itbl((StgClosure *)ml)->type] += cost((StgClosure *)ml);
		    sumOfNewCostExtra += cost((StgClosure *)ml);
		    break;
		}
	    }
#endif
	}

	// Traversing through mut_once_list is, in contrast, necessary
	// because we can find MUT_VAR objects which have not been
	// visited during retainer profiling.
	for (ml = generations[g].mut_once_list; ml != END_MUT_LIST;
	     ml = ml->mut_link) {

	    maybeInitRetainerSet((StgClosure *)ml);
	    rtl = retainerSetOf((StgClosure *)ml);
#ifdef DEBUG_RETAINER
	    if (rtl == NULL) {
		// first visit to *ml
		// This is a violation of the interface rule!
		RSET(ml) = (RetainerSet *)((StgWord)(&tmpRetainerSet) | flip);

		switch (get_itbl((StgClosure *)ml)->type) {
		case IND_STATIC:
		    // no cost involved
		    break;
		case CONSTR_INTLIKE:
		case CONSTR_CHARLIKE:
		case CONSTR_NOCAF_STATIC:
		case CONSTR_STATIC:
		case THUNK_STATIC:
		case FUN_STATIC:
		    barf("Invalid object in computeRetainerSet(): %d", get_itbl((StgClosure*)ml)->type);
		    break;
		default:
		    // dynamic objects
		    costArray[get_itbl((StgClosure *)ml)->type] += cost((StgClosure *)ml);
		    sumOfNewCostExtra += cost((StgClosure *)ml);
		    break;
		}
	    }
#endif
	}
    }
}

/* -----------------------------------------------------------------------------
 *  Traverse all static objects for which we compute retainer sets,
 *  and reset their rs fields to NULL, which is accomplished by
 *  invoking maybeInitRetainerSet(). This function must be called
 *  before zeroing all objects reachable from scavenged_static_objects
 *  in the case of major gabage collections. See GarbageCollect() in
 *  GC.c.
 *  Note:
 *    The mut_once_list of the oldest generation must also be traversed?
 *    Why? Because if the evacuation of an object pointed to by a static
 *    indirection object fails, it is put back to the mut_once_list of
 *    the oldest generation.
 *    However, this is not necessary because any static indirection objects
 *    are just traversed through to reach dynamic objects. In other words,
 *    they are not taken into consideration in computing retainer sets.
 * -------------------------------------------------------------------------- */
void
resetStaticObjectForRetainerProfiling( void )
{
#ifdef DEBUG_RETAINER
    nat count;
#endif
    StgClosure *p;

#ifdef DEBUG_RETAINER
    count = 0;
#endif
    p = scavenged_static_objects;
    while (p != END_OF_STATIC_LIST) {
#ifdef DEBUG_RETAINER
	count++;
#endif
	switch (get_itbl(p)->type) {
	case IND_STATIC:
	    // Since we do not compute the retainer set of any
	    // IND_STATIC object, we don't have to reset its retainer
	    // field.
	    p = IND_STATIC_LINK(p);
	    break;
	case THUNK_STATIC:
	    maybeInitRetainerSet(p);
	    p = THUNK_STATIC_LINK(p);
	    break;
	case FUN_STATIC:
	    maybeInitRetainerSet(p);
	    p = FUN_STATIC_LINK(p);
	    break;
	case CONSTR_STATIC:
	    maybeInitRetainerSet(p);
	    p = STATIC_LINK(get_itbl(p), p);
	    break;
	default:
	    barf("resetStaticObjectForRetainerProfiling: %p (%s)",
		 p, get_itbl(p)->type);
	    break;
	}
    }
#ifdef DEBUG_RETAINER
    // fprintf(stderr, "count in scavenged_static_objects = %d\n", count);
#endif
}

/* -----------------------------------------------------------------------------
 * Perform retainer profiling.
 * N is the oldest generation being profilied, where the generations are
 * numbered starting at 0.
 * Invariants:
 * Note:
 *   This function should be called only immediately after major garbage
 *   collection.
 * ------------------------------------------------------------------------- */
void
retainerProfile(void)
{
#ifdef DEBUG_RETAINER
  nat i;
  nat totalHeapSize;        // total raw heap size (computed by linear scanning)
#endif

#ifdef DEBUG_RETAINER
  fprintf(stderr, " < retainerProfile() invoked : %d>\n", retainerGeneration);
#endif

  stat_startRP();

  // We haven't flipped the bit yet.
#ifdef DEBUG_RETAINER
  fprintf(stderr, "Before traversing:\n");
  sumOfCostLinear = 0;
  for (i = 0;i < N_CLOSURE_TYPES; i++)
    costArrayLinear[i] = 0;
  totalHeapSize = checkHeapSanityForRetainerProfiling();

  fprintf(stderr, "\tsumOfCostLinear = %d, totalHeapSize = %d\n", sumOfCostLinear, totalHeapSize);
  /*
  fprintf(stderr, "costArrayLinear[] = ");
  for (i = 0;i < N_CLOSURE_TYPES; i++)
    fprintf(stderr, "[%u:%u] ", i, costArrayLinear[i]);
  fprintf(stderr, "\n");
  */

  ASSERT(sumOfCostLinear == totalHeapSize);

/*
#define pcostArrayLinear(index) \
  if (costArrayLinear[index] > 0) \
    fprintf(stderr, "costArrayLinear[" #index "] = %u\n", costArrayLinear[index])
  pcostArrayLinear(THUNK_STATIC);
  pcostArrayLinear(FUN_STATIC);
  pcostArrayLinear(CONSTR_STATIC);
  pcostArrayLinear(CONSTR_NOCAF_STATIC);
  pcostArrayLinear(CONSTR_INTLIKE);
  pcostArrayLinear(CONSTR_CHARLIKE);
*/
#endif

  // Now we flips flip.
  flip = flip ^ 1;

#ifdef DEBUG_RETAINER
  stackSize = 0;
  maxStackSize = 0;
  cStackSize = 0;
  maxCStackSize = 0;
#endif
  numObjectVisited = 0;
  timesAnyObjectVisited = 0;

#ifdef DEBUG_RETAINER
  fprintf(stderr, "During traversing:\n");
  sumOfNewCost = 0;
  sumOfNewCostExtra = 0;
  for (i = 0;i < N_CLOSURE_TYPES; i++)
    costArray[i] = 0;
#endif

  /*
    We initialize the traverse stack each time the retainer profiling is
    performed (because the traverse stack size varies on each retainer profiling
    and this operation is not costly anyhow). However, we just refresh the
    retainer sets.
   */
  initializeTraverseStack();
#ifdef DEBUG_RETAINER
  initializeAllRetainerSet();
#else
  refreshAllRetainerSet();
#endif
  computeRetainerSet();

#ifdef DEBUG_RETAINER
  fprintf(stderr, "After traversing:\n");
  sumOfCostLinear = 0;
  for (i = 0;i < N_CLOSURE_TYPES; i++)
    costArrayLinear[i] = 0;
  totalHeapSize = checkHeapSanityForRetainerProfiling();

  fprintf(stderr, "\tsumOfCostLinear = %d, totalHeapSize = %d\n", sumOfCostLinear, totalHeapSize);
  ASSERT(sumOfCostLinear == totalHeapSize);

  // now, compare the two results
  /*
    Note:
      costArray[] must be exactly the same as costArrayLinear[].
      Known exceptions:
        1) Dead weak pointers, whose type is CONSTR. These objects are not
           reachable from any roots.
  */
  fprintf(stderr, "Comparison:\n");
  fprintf(stderr, "\tcostArrayLinear[] (must be empty) = ");
  for (i = 0;i < N_CLOSURE_TYPES; i++)
    if (costArray[i] != costArrayLinear[i])
      // nothing should be printed except MUT_VAR after major GCs
      fprintf(stderr, "[%u:%u] ", i, costArrayLinear[i]);
  fprintf(stderr, "\n");

  fprintf(stderr, "\tsumOfNewCost = %u\n", sumOfNewCost);
  fprintf(stderr, "\tsumOfNewCostExtra = %u\n", sumOfNewCostExtra);
  fprintf(stderr, "\tcostArray[] (must be empty) = ");
  for (i = 0;i < N_CLOSURE_TYPES; i++)
    if (costArray[i] != costArrayLinear[i])
      // nothing should be printed except MUT_VAR after major GCs
      fprintf(stderr, "[%u:%u] ", i, costArray[i]);
  fprintf(stderr, "\n");

  // only for major garbage collection
  ASSERT(sumOfNewCost + sumOfNewCostExtra == sumOfCostLinear);
#endif

  // post-processing
  closeTraverseStack();
#ifdef DEBUG_RETAINER
  closeAllRetainerSet();
#else
  // Note that there is no post-processing for the retainer sets.
#endif
  retainerGeneration++;

  stat_endRP(
    retainerGeneration - 1,   // retainerGeneration has just been incremented!
#ifdef DEBUG_RETAINER
    maxCStackSize, maxStackSize,
#endif
    (double)timesAnyObjectVisited / numObjectVisited);
}

/* -----------------------------------------------------------------------------
 * DEBUGGING CODE
 * -------------------------------------------------------------------------- */

#ifdef DEBUG_RETAINER

#define LOOKS_LIKE_PTR(r) ((LOOKS_LIKE_STATIC_CLOSURE(r) || \
        ((HEAP_ALLOCED(r) && Bdescr((P_)r)->free != (void *)-1))) && \
        ((StgWord)(*(StgPtr)r)!=0xaaaaaaaa))

static nat
sanityCheckHeapClosure( StgClosure *c )
{
    StgInfoTable *info;

    ASSERT(LOOKS_LIKE_GHC_INFO(c->header.info));
    ASSERT(!closure_STATIC(c));
    ASSERT(LOOKS_LIKE_PTR(c));

    if ((((StgWord)RSET(c) & 1) ^ flip) != 0) {
	if (get_itbl(c)->type == CONSTR &&
	    !strcmp(get_itbl(c)->prof.closure_type, "DEAD_WEAK") &&
	    !strcmp(get_itbl(c)->prof.closure_desc, "DEAD_WEAK")) {
	    fprintf(stderr, "\tUnvisited dead weak pointer object found: c = %p\n", c);
	    costArray[get_itbl(c)->type] += cost(c);
	    sumOfNewCost += cost(c);
	} else
	    fprintf(stderr,
		    "Unvisited object: flip = %d, c = %p(%d, %s, %s), rs = %p\n",
		    flip, c, get_itbl(c)->type,
		    get_itbl(c)->prof.closure_type, get_itbl(c)->prof.closure_desc,
		    RSET(c));
    } else {
	// fprintf(stderr, "sanityCheckHeapClosure) S: flip = %d, c = %p(%d), rs = %p\n", flip, c, get_itbl(c)->type, RSET(c));
    }

    info = get_itbl(c);
    switch (info->type) {
    case TSO:
	return tso_sizeW((StgTSO *)c);

    case THUNK:
    case THUNK_1_0:
    case THUNK_0_1:
    case THUNK_2_0:
    case THUNK_1_1:
    case THUNK_0_2:
	return stg_max(sizeW_fromITBL(info), sizeofW(StgHeader) + MIN_UPD_SIZE);

    case MVAR:
	return sizeofW(StgMVar);

    case MUT_ARR_PTRS:
    case MUT_ARR_PTRS_FROZEN:
	return mut_arr_ptrs_sizeW((StgMutArrPtrs *)c);

    case AP:
    case PAP:
	return pap_sizeW((StgPAP *)c);

    case AP:
	return ap_stack_sizeW((StgAP_STACK *)c);

    case ARR_WORDS:
	return arr_words_sizeW((StgArrWords *)c);

    case CONSTR:
    case CONSTR_1_0:
    case CONSTR_0_1:
    case CONSTR_2_0:
    case CONSTR_1_1:
    case CONSTR_0_2:
    case FUN:
    case FUN_1_0:
    case FUN_0_1:
    case FUN_2_0:
    case FUN_1_1:
    case FUN_0_2:
    case WEAK:
    case MUT_VAR:
    case MUT_CONS:
    case CAF_BLACKHOLE:
    case BLACKHOLE:
    case SE_BLACKHOLE:
    case SE_CAF_BLACKHOLE:
    case BLACKHOLE_BQ:
    case IND_PERM:
    case IND_OLDGEN:
    case IND_OLDGEN_PERM:
    case FOREIGN:
    case BCO:
    case STABLE_NAME:
	return sizeW_fromITBL(info);

    case THUNK_SELECTOR:
	return sizeofW(StgHeader) + MIN_UPD_SIZE;

	/*
	  Error case
	*/
    case IND_STATIC:
    case CONSTR_STATIC:
    case FUN_STATIC:
    case THUNK_STATIC:
    case CONSTR_INTLIKE:
    case CONSTR_CHARLIKE:
    case CONSTR_NOCAF_STATIC:
    case UPDATE_FRAME:
    case CATCH_FRAME:
    case STOP_FRAME:
    case RET_DYN:
    case RET_BCO:
    case RET_SMALL:
    case RET_VEC_SMALL:
    case RET_BIG:
    case RET_VEC_BIG:
    case IND:
    case BLOCKED_FETCH:
    case FETCH_ME:
    case FETCH_ME_BQ:
    case RBH:
    case REMOTE_REF:
    case EVACUATED:
    case INVALID_OBJECT:
    default:
	barf("Invalid object in sanityCheckHeapClosure(): %d",
	     get_itbl(c)->type);
	return 0;
    }
}

static nat
heapCheck( bdescr *bd )
{
    StgPtr p;
    static nat costSum, size;

    costSum = 0;
    while (bd != NULL) {
	p = bd->start;
	while (p < bd->free) {
	    size = sanityCheckHeapClosure((StgClosure *)p);
	    sumOfCostLinear += size;
	    costArrayLinear[get_itbl((StgClosure *)p)->type] += size;
	    p += size;
	    // no need for slop check; I think slops are not used currently.
	}
	ASSERT(p == bd->free);
	costSum += bd->free - bd->start;
	bd = bd->link;
    }

    return costSum;
}

static nat
smallObjectPoolCheck(void)
{
    bdescr *bd;
    StgPtr p;
    static nat costSum, size;

    bd = small_alloc_list;
    costSum = 0;

    // first block
    if (bd == NULL)
	return costSum;

    p = bd->start;
    while (p < alloc_Hp) {
	size = sanityCheckHeapClosure((StgClosure *)p);
	sumOfCostLinear += size;
	costArrayLinear[get_itbl((StgClosure *)p)->type] += size;
	p += size;
    }
    ASSERT(p == alloc_Hp);
    costSum += alloc_Hp - bd->start;

    bd = bd->link;
    while (bd != NULL) {
	p = bd->start;
	while (p < bd->free) {
	    size = sanityCheckHeapClosure((StgClosure *)p);
	    sumOfCostLinear += size;
	    costArrayLinear[get_itbl((StgClosure *)p)->type] += size;
	    p += size;
	}
	ASSERT(p == bd->free);
	costSum += bd->free - bd->start;
	bd = bd->link;
    }

    return costSum;
}

static nat
chainCheck(bdescr *bd)
{
    nat costSum, size;

    costSum = 0;
    while (bd != NULL) {
	// bd->free - bd->start is not an accurate measurement of the
	// object size.  Actually it is always zero, so we compute its
	// size explicitly.
	size = sanityCheckHeapClosure((StgClosure *)bd->start);
	sumOfCostLinear += size;
	costArrayLinear[get_itbl((StgClosure *)bd->start)->type] += size;
	costSum += size;
	bd = bd->link;
    }

    return costSum;
}

static nat
checkHeapSanityForRetainerProfiling( void )
{
    nat costSum, g, s;

    costSum = 0;
    fprintf(stderr, "START: sumOfCostLinear = %d, costSum = %d\n", sumOfCostLinear, costSum);
    if (RtsFlags.GcFlags.generations == 1) {
	costSum += heapCheck(g0s0->to_blocks);
	fprintf(stderr, "heapCheck: sumOfCostLinear = %d, costSum = %d\n", sumOfCostLinear, costSum);
	costSum += chainCheck(g0s0->large_objects);
	fprintf(stderr, "chainCheck: sumOfCostLinear = %d, costSum = %d\n", sumOfCostLinear, costSum);
    } else {
	for (g = 0; g < RtsFlags.GcFlags.generations; g++)
	for (s = 0; s < generations[g].n_steps; s++) {
	    /*
	      After all live objects have been scavenged, the garbage
	      collector may create some objects in
	      scheduleFinalizers(). These objects are created throught
	      allocate(), so the small object pool or the large object
	      pool of the g0s0 may not be empty.
	    */
	    if (g == 0 && s == 0) {
		costSum += smallObjectPoolCheck();
		fprintf(stderr, "smallObjectPoolCheck(): sumOfCostLinear = %d, costSum = %d\n", sumOfCostLinear, costSum);
		costSum += chainCheck(generations[g].steps[s].large_objects);
		fprintf(stderr, "chainCheck(): sumOfCostLinear = %d, costSum = %d\n", sumOfCostLinear, costSum);
	    } else {
		costSum += heapCheck(generations[g].steps[s].blocks);
		fprintf(stderr, "heapCheck(): sumOfCostLinear = %d, costSum = %d\n", sumOfCostLinear, costSum);
		costSum += chainCheck(generations[g].steps[s].large_objects);
		fprintf(stderr, "chainCheck(): sumOfCostLinear = %d, costSum = %d\n", sumOfCostLinear, costSum);
	    }
	}
    }

    return costSum;
}

void
findPointer(StgPtr p)
{
    StgPtr q, r, e;
    bdescr *bd;
    nat g, s;

    for (g = 0; g < RtsFlags.GcFlags.generations; g++) {
	for (s = 0; s < generations[g].n_steps; s++) {
	    // if (g == 0 && s == 0) continue;
	    bd = generations[g].steps[s].blocks;
	    for (; bd; bd = bd->link) {
		for (q = bd->start; q < bd->free; q++) {
		    if (*q == (StgWord)p) {
			r = q;
			while (!LOOKS_LIKE_GHC_INFO(*r)) r--;
			fprintf(stderr, "Found in gen[%d], step[%d]: q = %p, r = %p\n", g, s, q, r);
			// return;
		    }
		}
	    }
	    bd = generations[g].steps[s].large_objects;
	    for (; bd; bd = bd->link) {
		e = bd->start + cost((StgClosure *)bd->start);
		for (q = bd->start; q < e; q++) {
		    if (*q == (StgWord)p) {
			r = q;
			while (*r == 0 || !LOOKS_LIKE_GHC_INFO(*r)) r--;
			fprintf(stderr, "Found in gen[%d], large_objects: %p\n", g, r);
			// return;
		    }
		}
	    }
	}
    }
}

static void
belongToHeap(StgPtr p)
{
    bdescr *bd;
    nat g, s;

    for (g = 0; g < RtsFlags.GcFlags.generations; g++) {
	for (s = 0; s < generations[g].n_steps; s++) {
	    // if (g == 0 && s == 0) continue;
	    bd = generations[g].steps[s].blocks;
	    for (; bd; bd = bd->link) {
		if (bd->start <= p && p < bd->free) {
		    fprintf(stderr, "Belongs to gen[%d], step[%d]", g, s);
		    return;
		}
	    }
	    bd = generations[g].steps[s].large_objects;
	    for (; bd; bd = bd->link) {
		if (bd->start <= p && p < bd->start + getHeapClosureSize((StgClosure *)bd->start)) {
		    fprintf(stderr, "Found in gen[%d], large_objects: %p\n", g, bd->start);
		    return;
		}
	    }
	}
    }
}
#endif // DEBUG_RETAINER

#endif /* PROFILING */
