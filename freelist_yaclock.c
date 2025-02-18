// NOTE THAT FREELIST IS simply linked list of empty buffer descriptors (each descriptor has a 1-1 correspondence to a buffer pool slot
// and holds the metadata of the stored page in the corresponding slot)

/*-------------------------------------------------------------------------
 *
 * freelist.c
 *	  routines for managing the buffer pool's replacement strategy.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/freelist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "pgstat.h"
#include "port/atomics.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"

#define INT_ACCESS_ONCE(var) ((int)(*((volatile int *)&(var))))

/*
 * The shared freelist control information.
 * THIS IS THE FREELIST METADATA!
 */
typedef struct
{
	/* Spinlock: protects the values below */
	slock_t buffer_strategy_lock;

	/*
	 * Clock sweep hand: index of next buffer to consider grabbing. Note that
	 * this isn't a concrete buffer - we only ever increase the value. So, to
	 * get an actual buffer, it needs to be used modulo NBuffers.
	 */
	pg_atomic_uint32 nextVictimBuffer;

	int firstFreeBuffer; /* Head of list of unused buffers */
	int lastFreeBuffer;	 /* Tail of list of unused buffers */

	/*
	 * NOTE: lastFreeBuffer is undefined when firstFreeBuffer is -1 (that is,
	 * when the list is empty)
	 */

	/*
	 * Statistics.  These counters should be wide enough that they can't
	 * overflow during a single bgwriter cycle.
	 */
	uint32 completePasses;			  /* Complete cycles of the clock sweep */
	pg_atomic_uint32 numBufferAllocs; /* Buffers allocated since last reset */

	/*
	 * Bgworker process to be notified upon activity or -1 if none. See
	 * StrategyNotifyBgWriter.
	 */
	int bgwprocno;

	int *queue;
	bool *ref_bits;
	int queue_head;
	int queue_tail;
	int num_elements;
	int next;
} BufferStrategyControl;

/* Pointers to shared state */
static BufferStrategyControl *StrategyControl = NULL;

/*
 * Private (non-shared) state for managing a ring of shared buffers to re-use.
 * This is currently the only kind of BufferAccessStrategy object, but someday
 * we might have more kinds.
 */
typedef struct BufferAccessStrategyData
{
	/* Overall strategy type */
	BufferAccessStrategyType btype;
	/* Number of elements in buffers[] array */
	int nbuffers;

	/*
	 * Index of the "current" slot in the ring, ie, the one most recently
	 * returned by GetBufferFromRing.
	 */
	int current;

	/*
	 * Array of buffer numbers.  InvalidBuffer (that is, zero) indicates we
	 * have not yet selected a buffer for this ring slot.  For allocation
	 * simplicity this is palloc'd together with the fixed fields of the
	 * struct.
	 */
	Buffer buffers[FLEXIBLE_ARRAY_MEMBER];
} BufferAccessStrategyData;

void StrategyAccessBuffer(int buf_id, int event_num); /* cs3223 */

/* Prototypes for internal functions */
static BufferDesc *GetBufferFromRing(BufferAccessStrategy strategy,
									 uint32 *buf_state);
static void AddBufferToRing(BufferAccessStrategy strategy,
							BufferDesc *buf);
void updateCaseOne(int buffer_id);
void updateCaseTwo(int buffer_id);
void updateCaseThree(int buffer_id);
void updateCaseFour(int buffer_id);
void addToQueueTail(int buffer_id);
void removeFromQueue(int buffer_id);
void logQueueState(const char *message);

/*
 * ClockSweepTick - Helper routine for StrategyGetBuffer()
 *
 * Move the clock hand one buffer ahead of its current position and return the
 * id of the buffer now under the hand.
 */
static inline uint32
ClockSweepTick(void)
{
	uint32 victim;

	/*
	 * Atomically move hand ahead one buffer - if there's several processes
	 * doing this, this can lead to buffers being returned slightly out of
	 * apparent order.
	 */
	victim =
		pg_atomic_fetch_add_u32(&StrategyControl->nextVictimBuffer, 1);

	if (victim >= NBuffers)
	{
		uint32 originalVictim = victim;

		/* always wrap what we look up in BufferDescriptors */
		victim = victim % NBuffers;

		/*
		 * If we're the one that just caused a wraparound, force
		 * completePasses to be incremented while holding the spinlock. We
		 * need the spinlock so StrategySyncStart() can return a consistent
		 * value consisting of nextVictimBuffer and completePasses.
		 */
		if (victim == 0)
		{
			uint32 expected;
			uint32 wrapped;
			bool success = false;

			expected = originalVictim + 1;

			while (!success)
			{
				/*
				 * Acquire the spinlock while increasing completePasses. That
				 * allows other readers to read nextVictimBuffer and
				 * completePasses in a consistent manner which is required for
				 * StrategySyncStart().  In theory delaying the increment
				 * could lead to an overflow of nextVictimBuffers, but that's
				 * highly unlikely and wouldn't be particularly harmful.
				 */
				SpinLockAcquire(&StrategyControl->buffer_strategy_lock);

				wrapped = expected % NBuffers;

				success = pg_atomic_compare_exchange_u32(&StrategyControl->nextVictimBuffer,
														 &expected, wrapped);
				if (success)
					StrategyControl->completePasses++;
				SpinLockRelease(&StrategyControl->buffer_strategy_lock);
			}
		}
	}
	return victim;
}

/*
 * have_free_buffer -- a lockless check to see if there is a free buffer in
 *					   buffer pool.
 *
 * If the result is true that will become stale once free buffers are moved out
 * by other operations, so the caller who strictly want to use a free buffer
 * should not call this.
 */
bool have_free_buffer(void)
{
	if (StrategyControl->firstFreeBuffer >= 0)
		return true;
	else
		return false;
}

/*
 * StrategyGetBuffer
 *
 *	Called by the bufmgr to get the next candidate buffer to use in
 *	BufferAlloc(). The only hard requirement BufferAlloc() has is that
 *	the selected buffer must not currently be pinned by anyone.
 *
 *	strategy is a BufferAccessStrategy object, or NULL for default strategy.
 *
 *	To ensure that no one else can pin the buffer before we do, we must
 *	return the buffer with the buffer header spinlock still held.
 *  ----------------------------------------------------------------------------
 *  First checks the free list for available buffers.
 *  If the free list is empty, it falls back to the clock sweep algorithm.
 */

// TODO: handle cases corresponding to event_num = 2 and event_num = 3
BufferDesc *
StrategyGetBuffer(BufferAccessStrategy strategy, uint32 *buf_state, bool *from_ring)
{
	BufferDesc *buf;
	int bgwprocno;
	int trycounter;
	uint32 local_buf_state; /* to avoid repeated (de-)referencing */

	*from_ring = false;

	/*
	 * If given a strategy object, see whether it can select a buffer. We
	 * assume strategy objects don't need buffer_strategy_lock.
	 */
	if (strategy != NULL)
	{
		buf = GetBufferFromRing(strategy, buf_state);
		if (buf != NULL)
		{
			*from_ring = true;
			return buf;
		}
	}

	/*
	 * If asked, we need to waken the bgwriter. Since we don't want to rely on
	 * a spinlock for this we force a read from shared memory once, and then
	 * set the latch based on that value. We need to go through that length
	 * because otherwise bgwprocno might be reset while/after we check because
	 * the compiler might just reread from memory.
	 *
	 * This can possibly set the latch of the wrong process if the bgwriter
	 * dies in the wrong moment. But since PGPROC->procLatch is never
	 * deallocated the worst consequence of that is that we set the latch of
	 * some arbitrary process.
	 */
	bgwprocno = INT_ACCESS_ONCE(StrategyControl->bgwprocno);
	if (bgwprocno != -1)
	{
		/* reset bgwprocno first, before setting the latch */
		StrategyControl->bgwprocno = -1;

		/*
		 * Not acquiring ProcArrayLock here which is slightly icky. It's
		 * actually fine because procLatch isn't ever freed, so we just can
		 * potentially set the wrong process' (or no process') latch.
		 */
		SetLatch(&ProcGlobal->allProcs[bgwprocno].procLatch);
	}

	/*
	 * We count buffer allocation requests so that the bgwriter can estimate
	 * the rate of buffer consumption.  Note that buffers recycled by a
	 * strategy object are intentionally not counted here.
	 */
	pg_atomic_fetch_add_u32(&StrategyControl->numBufferAllocs, 1);

	/*
	 * First check, without acquiring the lock, whether there's buffers in the
	 * freelist. Since we otherwise don't require the spinlock in every
	 * StrategyGetBuffer() invocation, it'd be sad to acquire it here -
	 * uselessly in most cases. That obviously leaves a race where a buffer is
	 * put on the freelist but we don't see the store yet - but that's pretty
	 * harmless, it'll just get used during the next buffer acquisition.
	 *
	 * If there's buffers on the freelist, acquire the spinlock to pop one
	 * buffer of the freelist. Then check whether that buffer is usable and
	 * repeat if not.
	 *
	 * Note that the freeNext fields are considered to be protected by the
	 * buffer_strategy_lock not the individual buffer spinlocks, so it's OK to
	 * manipulate them without holding the spinlock.
	 */
	if (StrategyControl->firstFreeBuffer >= 0)
	{
		while (true)
		{
			/* Acquire the spinlock to remove element from the freelist */
			SpinLockAcquire(&StrategyControl->buffer_strategy_lock);

			if (StrategyControl->firstFreeBuffer < 0)
			{
				SpinLockRelease(&StrategyControl->buffer_strategy_lock);
				break;
			}

			buf = GetBufferDescriptor(StrategyControl->firstFreeBuffer);
			Assert(buf->freeNext != FREENEXT_NOT_IN_LIST);

			/* Unconditionally remove buffer from freelist */
			StrategyControl->firstFreeBuffer = buf->freeNext;
			buf->freeNext = FREENEXT_NOT_IN_LIST;

			/*
			 * Release the lock so someone else can access the freelist while
			 * we check out this buffer.
			 */
			SpinLockRelease(&StrategyControl->buffer_strategy_lock);

			/*
			 * If the buffer is pinned or has a nonzero usage_count, we cannot
			 * use it; discard it and retry.  (This can only happen if VACUUM
			 * put a valid buffer in the freelist and then someone else used
			 * it before we got to it.  It's probably impossible altogether as
			 * of 8.3, but we'd better check anyway.)
			 */
			local_buf_state = LockBufHdr(buf);
			// if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0 && BUF_STATE_GET_USAGECOUNT(local_buf_state) == 0)
			if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0) // we are not using usage count anymore
			{
				StrategyAccessBuffer(buf->buf_id, 2);

				if (strategy != NULL)
					AddBufferToRing(strategy, buf);
				*buf_state = local_buf_state;
				elog(INFO, "[StrategyGetBuffer][case 2]: Allocated buffer from free list: %d", buf->buf_id);
				return buf;
			}
			UnlockBufHdr(buf, local_buf_state);
		}
	}

	/* Nothing on the freelist, so run the "clock sweep" algorithm */
	if (StrategyControl->next == -1)
	{
		StrategyControl->next = StrategyControl->queue_head;
		elog(INFO, "[StrategyGetBuffer][case 3]: Initializing next pointer to queue head: %d", StrategyControl->next);
	}

	trycounter = NBuffers;

	for (;;)
	{
		int buffer_id = StrategyControl->queue[StrategyControl->next];
		buf = GetBufferDescriptor(buffer_id);
		elog(INFO, "[StrategyGetBuffer][case 3]: Checking buffer %d", buffer_id);
		logQueueState("[StrategyGetBuffer] current state");
		elog(INFO, "buffer %d, next=%d, head=%d, tail=%d, num_elements=%d",
			buffer_id, StrategyControl->next, StrategyControl->queue_head, 
			StrategyControl->queue_tail, StrategyControl->num_elements);

		/*
		 * If the buffer is pinned or has a nonzero usage_count, we cannot use
		 * it; decrement the usage_count (unless pinned) and keep scanning.
		 */
		local_buf_state = LockBufHdr(buf);

		if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0)
		{
			if (StrategyControl->ref_bits[buffer_id])
			{
				elog(INFO, "[StrategyGetBuffer][case 3]: Buffer %d has reference bit set, giving second chance", buffer_id);
				StrategyControl->ref_bits[buffer_id] = false;
			}
			else
			{
				/* Found a usable buffer */
				elog(INFO, "[StrategyGetBuffer][case 3]: Buffer %d is selected for replacement", buffer_id);
				StrategyAccessBuffer(buffer_id, 3);

				if (strategy != NULL)
					AddBufferToRing(strategy, buf);
				*buf_state = local_buf_state;
				return buf;
			}
		}
		else if (--trycounter == 0) {
			/*
			* We've scanned all the buffers without making any state changes,
			* so all the buffers are pinned (or were when we looked at them).
			* We could hope that someone will free one eventually, but it's
			* probably better to fail than to risk getting stuck in an
			* infinite loop.
			*/
			UnlockBufHdr(buf, local_buf_state);
			elog(ERROR, "no unpinned buffers available");
		}
		StrategyControl->next = (StrategyControl->next + 1) % NBuffers;
		UnlockBufHdr(buf, local_buf_state);
	}
}

/*
 * StrategyFreeBuffer: adds a buffer back to the free list, making it available for reuse.
 */

// TODO: Handle case corresponding to event_num = 4.
void StrategyFreeBuffer(BufferDesc *buf)
{
	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);

	/*
	 * It is possible that we are told to put something in the freelist that
	 * is already in it; don't screw up the list if so.
	 */
	if (buf->freeNext == FREENEXT_NOT_IN_LIST)
	{
		buf->freeNext = StrategyControl->firstFreeBuffer;
		if (buf->freeNext < 0)
			StrategyControl->lastFreeBuffer = buf->buf_id;
		StrategyControl->firstFreeBuffer = buf->buf_id;
		StrategyAccessBuffer(buf->buf_id, 4);
		elog(INFO, "[StrategyFreeBuffer]: Freed buffer: %d", buf->buf_id);
	}

	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
}

/*
 * StrategySyncStart -- tell BufferSync where to start syncing
 *
 * The result is the buffer index of the best buffer to sync first.
 * BufferSync() will proceed circularly around the buffer array from there.
 *
 * In addition, we return the completed-pass count (which is effectively
 * the higher-order bits of nextVictimBuffer) and the count of recent buffer
 * allocs if non-NULL pointers are passed.  The alloc count is reset after
 * being read.
 */
int StrategySyncStart(uint32 *complete_passes, uint32 *num_buf_alloc)
{
	uint32 nextVictimBuffer;
	int result;

	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
	nextVictimBuffer = pg_atomic_read_u32(&StrategyControl->nextVictimBuffer);
	result = nextVictimBuffer % NBuffers;

	if (complete_passes)
	{
		*complete_passes = StrategyControl->completePasses;

		/*
		 * Additionally add the number of wraparounds that happened before
		 * completePasses could be incremented. C.f. ClockSweepTick().
		 */
		*complete_passes += nextVictimBuffer / NBuffers;
	}

	if (num_buf_alloc)
	{
		*num_buf_alloc = pg_atomic_exchange_u32(&StrategyControl->numBufferAllocs, 0);
	}
	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
	return result;
}

/*
 * StrategyNotifyBgWriter -- set or clear allocation notification latch
 *
 * If bgwprocno isn't -1, the next invocation of StrategyGetBuffer will
 * set that latch.  Pass -1 to clear the pending notification before it
 * happens.  This feature is used by the bgwriter process to wake itself up
 * from hibernation, and is not meant for anybody else to use.
 */
void StrategyNotifyBgWriter(int bgwprocno)
{
	/*
	 * We acquire buffer_strategy_lock just to ensure that the store appears
	 * atomic to StrategyGetBuffer.  The bgwriter should call this rather
	 * infrequently, so there's no performance penalty from being safe.
	 */
	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
	StrategyControl->bgwprocno = bgwprocno;
	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
}

/*
 * StrategyShmemSize
 *
 * estimate the size of shared memory used by the freelist-related structures.
 *
 * Note: for somewhat historical reasons, the buffer lookup hashtable size
 * is also determined here.
 */

// TODO: to account for any additional shared meomry used by YACLOCL-related DS using functions such as add_size, mul_size
Size StrategyShmemSize(void)
{
	Size size = 0;

	/* size of lookup hash table ... see comment in StrategyInitialize */
	size = add_size(size, BufTableShmemSize(NBuffers + NUM_BUFFER_PARTITIONS));

	/* size of the shared replacement strategy control block */
	size = add_size(size, MAXALIGN(sizeof(BufferStrategyControl)));

	/* size of the ref_bits array */
	size = add_size(size, mul_size(NBuffers, sizeof(bool)));

	/* size of the queue array */
	size = add_size(size, mul_size(NBuffers, sizeof(int)));

	return size;
}

/*
 * StrategyInitialize -- initialize the buffer cache replacement
 *		strategy.
 *
 * Assumes: All of the buffers are already built into a linked list.
 *		Only called by postmaster and only during initialization.
 */

// TODO: Include and initialisation shared memory of YALOCK-related data structures using ShmemInitStruct function
void StrategyInitialize(bool init)
{
	bool found;

	/*
	 * Initialize the shared buffer lookup hashtable.
	 *
	 * Since we can't tolerate running out of lookup table entries, we must be
	 * sure to specify an adequate table size here.  The maximum steady-state
	 * usage is of course NBuffers entries, but BufferAlloc() tries to insert
	 * a new entry before deleting the old.  In principle this could be
	 * happening in each partition concurrently, so we could need as many as
	 * NBuffers + NUM_BUFFER_PARTITIONS entries.
	 */
	InitBufTable(NBuffers + NUM_BUFFER_PARTITIONS);

	/*
	 * Get or create the shared strategy control block
	 */
	StrategyControl = (BufferStrategyControl *)
		ShmemInitStruct("Buffer Strategy Status",
						sizeof(BufferStrategyControl),
						&found);

	if (!found)
	{
		/*
		 * Only done once, usually in postmaster
		 */
		Assert(init);

		SpinLockInit(&StrategyControl->buffer_strategy_lock);

		/*
		 * Grab the whole linked list of free buffers for our strategy. We
		 * assume it was previously set up by InitBufferPool().
		 */
		StrategyControl->firstFreeBuffer = 0;
		StrategyControl->lastFreeBuffer = NBuffers - 1;

		/* Initialize the clock sweep pointer */
		pg_atomic_init_u32(&StrategyControl->nextVictimBuffer, 0); 

		/* Clear statistics */
		StrategyControl->completePasses = 0;
		pg_atomic_init_u32(&StrategyControl->numBufferAllocs, 0);

		/* No pending notification */
		StrategyControl->bgwprocno = -1;

		StrategyControl->queue = (int *)ShmemInitStruct("Buffer Queue", mul_size(NBuffers, sizeof(int)), &found);
		StrategyControl->ref_bits = (bool *)ShmemInitStruct("Ref Bit Array", mul_size(NBuffers, sizeof(bool)), &found);

		for (int i = 0; i < NBuffers; ++i)
		{
			StrategyControl->queue[i] = i;
			StrategyControl->ref_bits[i] = false;
		}
		StrategyControl->queue_head = 0;
		StrategyControl->queue_tail = 0;
		StrategyControl->next = -1;
		StrategyControl->num_elements = 0;
	}
	else
		Assert(!init);

	elog(INFO, "YACLOCK strategy initialized: queue_head=%d, queue_tail=%d, next=%d, num_elements=%d",
			StrategyControl->queue_head, StrategyControl->queue_tail, StrategyControl->next, StrategyControl->num_elements);
}

/* ----------------------------------------------------------------
 *				Backend-private buffer ring management
 * ----------------------------------------------------------------
 */

/*
 * GetAccessStrategy -- create a BufferAccessStrategy object
 *
 * The object is allocated in the current memory context.
 */
BufferAccessStrategy
GetAccessStrategy(BufferAccessStrategyType btype)
{
	int ring_size_kb;

	/*
	 * Select ring size to use.  See buffer/README for rationales.
	 *
	 * Note: if you change the ring size for BAS_BULKREAD, see also
	 * SYNC_SCAN_REPORT_INTERVAL in access/heap/syncscan.c.
	 */
	switch (btype)
	{
	case BAS_NORMAL:
		/* if someone asks for NORMAL, just give 'em a "default" object */
		return NULL;

	case BAS_BULKREAD:
		ring_size_kb = 256;
		break;
	case BAS_BULKWRITE:
		ring_size_kb = 16 * 1024;
		break;
	case BAS_VACUUM:
		ring_size_kb = 2048;
		break;

	default:
		elog(ERROR, "unrecognized buffer access strategy: %d",
			 (int)btype);
		return NULL; /* keep compiler quiet */
	}

	return GetAccessStrategyWithSize(btype, ring_size_kb);
}

/*
 * GetAccessStrategyWithSize -- create a BufferAccessStrategy object with a
 *		number of buffers equivalent to the passed in size.
 *
 * If the given ring size is 0, no BufferAccessStrategy will be created and
 * the function will return NULL.  ring_size_kb must not be negative.
 */
BufferAccessStrategy
GetAccessStrategyWithSize(BufferAccessStrategyType btype, int ring_size_kb)
{
	int ring_buffers;
	BufferAccessStrategy strategy;

	Assert(ring_size_kb >= 0);

	/* Figure out how many buffers ring_size_kb is */
	ring_buffers = ring_size_kb / (BLCKSZ / 1024);

	/* 0 means unlimited, so no BufferAccessStrategy required */
	if (ring_buffers == 0)
		return NULL;

	/* Cap to 1/8th of shared_buffers */
	ring_buffers = Min(NBuffers / 8, ring_buffers);

	/* NBuffers should never be less than 16, so this shouldn't happen */
	Assert(ring_buffers > 0);

	/* Allocate the object and initialize all elements to zeroes */
	strategy = (BufferAccessStrategy)
		palloc0(offsetof(BufferAccessStrategyData, buffers) +
				ring_buffers * sizeof(Buffer));

	/* Set fields that don't start out zero */
	strategy->btype = btype;
	strategy->nbuffers = ring_buffers;

	return strategy;
}

/*
 * GetAccessStrategyBufferCount -- an accessor for the number of buffers in
 *		the ring
 *
 * Returns 0 on NULL input to match behavior of GetAccessStrategyWithSize()
 * returning NULL with 0 size.
 */
int GetAccessStrategyBufferCount(BufferAccessStrategy strategy)
{
	if (strategy == NULL)
		return 0;

	return strategy->nbuffers;
}

/*
 * GetAccessStrategyPinLimit -- get cap of number of buffers that should be pinned
 *
 * When pinning extra buffers to look ahead, users of a ring-based strategy are
 * in danger of pinning too much of the ring at once while performing look-ahead.
 * For some strategies, that means "escaping" from the ring, and in others it
 * means forcing dirty data to disk very frequently with associated WAL
 * flushing.  Since external code has no insight into any of that, allow
 * individual strategy types to expose a clamp that should be applied when
 * deciding on a maximum number of buffers to pin at once.
 *
 * Callers should combine this number with other relevant limits and take the
 * minimum.
 */
int GetAccessStrategyPinLimit(BufferAccessStrategy strategy)
{
	if (strategy == NULL)
		return NBuffers;

	switch (strategy->btype)
	{
	case BAS_BULKREAD:

		/*
		 * Since BAS_BULKREAD uses StrategyRejectBuffer(), dirty buffers
		 * shouldn't be a problem and the caller is free to pin up to the
		 * entire ring at once.
		 */
		return strategy->nbuffers;

	default:

		/*
		 * Tell caller not to pin more than half the buffers in the ring.
		 * This is a trade-off between look ahead distance and deferring
		 * writeback and associated WAL traffic.
		 */
		return strategy->nbuffers / 2;
	}
}

/*
 * FreeAccessStrategy -- release a BufferAccessStrategy object
 *
 * A simple pfree would do at the moment, but we would prefer that callers
 * don't assume that much about the representation of BufferAccessStrategy.
 */
void FreeAccessStrategy(BufferAccessStrategy strategy)
{
	/* don't crash if called on a "default" strategy */
	if (strategy != NULL)
		pfree(strategy);
}

/*
 * GetBufferFromRing -- returns a buffer from the ring, or NULL if the
 *		ring is empty / not usable.
 *
 * The bufhdr spin lock is held on the returned buffer.
 */
static BufferDesc *
GetBufferFromRing(BufferAccessStrategy strategy, uint32 *buf_state)
{
	// strategy refers to BufferAccessStrategyData
	BufferDesc *buf;
	Buffer bufnum;
	uint32 local_buf_state; /* to avoid repeated (de-)referencing */

	/* Advance to next ring slot */
	if (++strategy->current >= strategy->nbuffers)
		strategy->current = 0;

	/*
	 * If the slot hasn't been filled yet, tell the caller to allocate a new
	 * buffer with the normal allocation strategy.  He will then fill this
	 * slot by calling AddBufferToRing with the new buffer.
	 */
	bufnum = strategy->buffers[strategy->current];
	if (bufnum == InvalidBuffer)
		return NULL;

	/*
	 * If the buffer is pinned we cannot use it under any circumstances.
	 *
	 * If usage_count is 0 or 1 then the buffer is fair game (we expect 1,
	 * since our own previous usage of the ring element would have left it
	 * there, but it might've been decremented by clock sweep since then). A
	 * higher usage_count indicates someone else has touched the buffer, so we
	 * shouldn't re-use it.
	 */
	buf = GetBufferDescriptor(bufnum - 1);
	local_buf_state = LockBufHdr(buf);
	if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0 && BUF_STATE_GET_USAGECOUNT(local_buf_state) <= 1)
	{
		*buf_state = local_buf_state;
		return buf;
	}
	UnlockBufHdr(buf, local_buf_state);

	/*
	 * Tell caller to allocate a new buffer with the normal allocation
	 * strategy.  He'll then replace this ring element via AddBufferToRing.
	 */
	return NULL;
}

/*
 * AddBufferToRing -- add a buffer to the buffer ring
 *
 * Caller must hold the buffer header spinlock on the buffer.  Since this
 * is called with the spinlock held, it had better be quite cheap.
 */
static void
AddBufferToRing(BufferAccessStrategy strategy, BufferDesc *buf)
{
	strategy->buffers[strategy->current] = BufferDescriptorGetBuffer(buf);
}

/*
 * Utility function returning the IOContext of a given BufferAccessStrategy's
 * strategy ring.
 */
IOContext
IOContextForStrategy(BufferAccessStrategy strategy)
{
	if (!strategy)
		return IOCONTEXT_NORMAL;

	switch (strategy->btype)
	{
	case BAS_NORMAL:

		/*
		 * Currently, GetAccessStrategy() returns NULL for
		 * BufferAccessStrategyType BAS_NORMAL, so this case is
		 * unreachable.
		 */
		pg_unreachable();
		return IOCONTEXT_NORMAL;
	case BAS_BULKREAD:
		return IOCONTEXT_BULKREAD;
	case BAS_BULKWRITE:
		return IOCONTEXT_BULKWRITE;
	case BAS_VACUUM:
		return IOCONTEXT_VACUUM;
	}

	elog(ERROR, "unrecognized BufferAccessStrategyType: %d", strategy->btype);
	pg_unreachable();
}

/*
 * StrategyRejectBuffer -- consider rejecting a dirty buffer
 *
 * When a nondefault strategy is used, the buffer manager calls this function
 * when it turns out that the buffer selected by StrategyGetBuffer needs to
 * be written out and doing so would require flushing WAL too.  This gives us
 * a chance to choose a different victim.
 *
 * Returns true if buffer manager should ask for a new victim, and false
 * if this buffer should be written and re-used.
 */
bool StrategyRejectBuffer(BufferAccessStrategy strategy, BufferDesc *buf, bool from_ring)
{
	/* We only do this in bulkread mode */
	if (strategy->btype != BAS_BULKREAD)
		return false;

	/* Don't muck with behavior of normal buffer-replacement strategy */
	if (!from_ring ||
		strategy->buffers[strategy->current] != BufferDescriptorGetBuffer(buf))
		return false;

	/*
	 * Remove the dirty buffer from the ring; necessary to prevent infinite
	 * loop if all ring members are dirty.
	 */
	strategy->buffers[strategy->current] = InvalidBuffer;

	return true;
}

/*
cs3223
StrategyAccessBuffer  -- update YACLOCK's data structures when a buffer page is accessed.
Note that event_num must be 1, 2, 3, or 4 corresponding to the four events in YACLOCK replacement policy.
*/
void StrategyAccessBuffer(int buf_id, int event_num)
{

	switch (event_num)
	{
	// P exist in buffer pool so add one to reference list, used in ReadRecentBuffer()
	case 1:
		updateCaseOne(buf_id);
		break;

	// P is not in buffer pool and the free list is not empty
	case 2:
		updateCaseTwo(buf_id);
		break;

	// P is not in buffer pool and free list is empty
	case 3:
		updateCaseThree(buf_id);
		break;

	case 4:
		updateCaseFour(buf_id);
	default:
		break;
	}
}

void updateCaseOne(int buffer_id)
{
	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
	StrategyControl->ref_bits[buffer_id] = true;
	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
	elog(INFO, "[updateCaseOne]: Set ref_bit for buffer %d", buffer_id);
	logQueueState("[updateCaseOne] finished");
}

void updateCaseTwo(int buffer_id)
{
	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
	addToQueueTail(buffer_id);
	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
	elog(INFO, "[updateCaseTwo]: Inserted buffer %d into queue tail", buffer_id);
	logQueueState("[updateCaseTwo] finished");
}

void updateCaseThree(int buffer_id)
{
	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
	elog(INFO, "[updateCaseThree]: Evicting buffer %d", buffer_id);
	removeFromQueue(buffer_id);
	elog(INFO, "[updateCaseThree]: Evicted buffer %d", buffer_id);
	addToQueueTail(buffer_id);
	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
	logQueueState("[updateCaseThree] finished");
}

void updateCaseFour(int buffer_id)
{
	elog(INFO, "[updateCaseFour]: removing buffer %d from queue", buffer_id);
	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
	removeFromQueue(buffer_id);
	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
	elog(INFO, "[updateCaseFour]: Buffer %d removed", buffer_id);
	logQueueState("[updateCaseFour] finished");
}

void addToQueueTail(int buffer_id) {
	elog(INFO, "[addToQueueTail]: Adding buffer %d to tail", buffer_id);
	StrategyControl->queue[StrategyControl->queue_tail] = buffer_id;
	StrategyControl->ref_bits[buffer_id] = false;
	StrategyControl->queue_tail = StrategyControl->queue_tail + 1;
	StrategyControl->num_elements++;
	elog(INFO, "[addToQueueTail]: Inserted buffer %d into queue tail", buffer_id);
	logQueueState("[addToQueueTail] finished");
}

/*
 * Assume that this would be called when spinlock is already held, then we dont lock it here
 * This function, remove the buffer and shifts all remaining elements left
 * Since we are shifting the elements left, next would not need to be updated
 */
void removeFromQueue(int buffer_id)
{
	int index = -1;
	for (int i = 0; i < StrategyControl->num_elements; ++i)
	{
		int queue_index = (StrategyControl->queue_head + i) % NBuffers;
        if (StrategyControl->queue[queue_index] == buffer_id)
        {
            index = queue_index;
            break;
        }
	}

	if (index == -1)
    {
        elog(ERROR, "[removeFromQueue]: Buffer %d not found in queue", buffer_id);
        return;
    }

	elog(INFO, "[removeFromQueue]: Removing buffer %d from queue at index %d", buffer_id, index);	

	/* Shift remaining elements left */
	for (int i = index; i != (StrategyControl->queue_tail - 1 + NBuffers) % NBuffers; i = (i + 1) % NBuffers)
    {
        StrategyControl->queue[i] = StrategyControl->queue[(i + 1) % NBuffers];
    }

	StrategyControl->queue_tail = (StrategyControl->queue_tail - 1 + NBuffers) % NBuffers;
	StrategyControl->num_elements--;

	if (StrategyControl->num_elements == 0)
	{
		StrategyControl->next = -1;
		StrategyControl->queue_head = 0;
        StrategyControl->queue_tail = 0;
	}

	logQueueState("[removeFromQueue] finished");
}

#define QUEUE_LOG_SIZE 1024
char buffer_state[QUEUE_LOG_SIZE];

void logQueueState(const char *message)
{
	elog(INFO, "%s: Queue state -> [head=%d, tail=%d, next=%d, num_elements=%d]",
			message, StrategyControl->queue_head, StrategyControl->queue_tail,
			StrategyControl->next, StrategyControl->num_elements);

	// Log buffer IDs in the queue
	char buffer_state[QUEUE_LOG_SIZE];
	int pos = 0;
	pos += snprintf(buffer_state + pos, sizeof(buffer_state) - pos, "Queue: [");

	for (int i = 0; i < StrategyControl->num_elements; i++)
	{
		int index = (StrategyControl->queue_head + i) % NBuffers;
		pos += snprintf(buffer_state + pos, sizeof(buffer_state) - pos, "%d ", StrategyControl->queue[index]);
	}

	snprintf(buffer_state + pos, sizeof(buffer_state) - pos, "]");
	elog(INFO, "%s", buffer_state);
}
