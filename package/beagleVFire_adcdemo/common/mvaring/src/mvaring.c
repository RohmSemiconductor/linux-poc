// mvaring_ring.c
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../inc/mvaring.h"

#ifdef BE_LAZY
/* CPU pause/yield hint during busy-wait loops */
#if defined(__i386__) || defined(__x86_64__)
#define SPINAWHILE() __asm__ __volatile__("pause" ::: "memory")
#elif defined(__aarch64__) || defined(__arm__)
#define SPINAWHILE() __asm__ __volatile__("yield" ::: "memory")
#else
#define SPINAWHILE() __asm__ __volatile__("" ::: "memory")
#endif
#else
/* Yield CPU to scheduler during busy-wait */
#define SPINAWHILE() sched_yield()
#endif

static unsigned int slot_mask(const struct mvaring *r)
{
	return r->num_slots - 1;
}

struct mvaring *ring_init(void *buff, size_t bufsize, size_t item_size)
{
	struct mvaring *r = buff;
	size_t avail, slots = 1;

	if (!buff || !item_size || item_size > UINT32_MAX)
		return NULL;

	if (bufsize < sizeof(struct mvaring) + item_size)
		return NULL;

	avail = (bufsize - sizeof(struct mvaring)) / item_size;
	while (slots * 2 <= avail && slots * 2 <= (1u << 30))
		slots *= 2;

	/* Clean the headroom */
	memset(r, 0, sizeof(*r));

	r->version = MVARING_VERSION;
	r->size = sizeof(struct mvaring) + slots * item_size;
	r->item_size = item_size;
	r->num_slots = slots;

	/* atomic indexes init */
	atomic_init(&r->rindex, 0);
	atomic_init(&r->windex, 0);
	r->writing = 0;
	r->dropped = 0;

	return r;
}

/**
 * ring_is_ok() - Validate ring buffer structure
 * @r: Pointer to ring buffer
 *
 * Checks if ring buffer has valid version and size fields.
 * Used to detect corruption or version mismatches.
 *
 * Return: true if valid, false otherwise
 */
bool ring_is_ok(struct mvaring *r)
{
	if (!r)
		return false;

	if (r->version != MVARING_VERSION)
		return false;

	if (!r->item_size || !r->num_slots ||
	    (r->num_slots & (r->num_slots - 1)))
		return false;

	if (r->size != MVARING_BYTES(r->item_size, r->num_slots))
		return false;

	return true;
}

/**
 * ring_available() - Get number of readable entries in ring buffer
 * @r: Pointer to ring buffer
 *
 * Returns the number of data chunks currently available for reading.
 * Uses relaxed atomics as this is informational only - precise count
 * may change immediately after return due to concurrent writer.
 *
 * Concurrency: Safe to call concurrently with ring_add() and ring_read()
 *
 * Return: Number of entries available (0 to num_slots - 1)
 */
unsigned int ring_available(struct mvaring *r)
{
	unsigned w = atomic_load_explicit(&r->windex, memory_order_relaxed);
	unsigned rd = atomic_load_explicit(&r->rindex, memory_order_relaxed);
	return (w - rd);
}

/**
 * ring_space() - Get number of free entries in ring buffer
 * @r: Pointer to ring buffer
 *
 * Returns the amount of free space available for writing.
 * Uses relaxed atomics as this is informational only - precise count
 * may change immediately after return due to concurrent reader.
 *
 * Concurrency: Safe to call concurrently with ring_add() and ring_read()
 *
 * One slot is always kept free by ring_add(), so the usable capacity is
 * num_slots - 1.
 *
 * Return: Number of free entries (0 to num_slots - 1)
 */
unsigned int ring_space(struct mvaring *r)
{
	unsigned w = atomic_load_explicit(&r->windex, memory_order_relaxed);
	unsigned rd = atomic_load_explicit(&r->rindex, memory_order_relaxed);
	return r->num_slots - 1 - (w - rd);
}

size_t ring_item_size(const struct mvaring *r)
{
	return r->item_size;
}

bool ring_full(struct mvaring *r)
{
	return (ring_space(r) == 0);
}

bool ring_empty(struct mvaring *r)
{
	return (ring_available(r) == 0);
}

int ring_add(struct mvaring *r, const void *data, size_t len, bool dropfull)
{
	if (len > r->item_size)
		return -EMSGSIZE;

	/* Memory ordering notes:
	 * - windex load uses relaxed: we own it (single writer), no sync needed
	 * - rindex load uses acquire: pairs with reader's release, ensures we see
	 *   reader's updates and can safely determine buffer fullness
	 */
	unsigned w = atomic_load_explicit(&r->windex, memory_order_relaxed);
	unsigned rd = atomic_load_explicit(&r->rindex, memory_order_acquire);

	unsigned next_w = w + 1;
	unsigned buf_idx = w & slot_mask(r);
	int ret = 0;

	/*
	 * TODO: Drop the seqlock as it is not really usefull in our use-case.
	 * My original idea was to avoid atomic write/read indexes and to use
	 * seqlock to protect the data integrity. This, however, feels now like
	 * a bad idea.
	 *
	 * 1. Implementing the read-index update by both writer (when ring is
	 *    full) and by reader (when entries are read) without locks/atomics
	 *    gets tricky. I am not in a mood for tricky it seems.
	 *
	 * 2. We have very frequent writer, writing smallish chunks of data,
	 *    and probably a slower reader (reading larger chunks of data).
	 *    This means our writer is frequently reserving the seqlock for a
	 *    short period at a time. This increases chances that a slower
	 *    reader will need to perfor re-read(s). seqlock would work better
	 *    for writer adding large chuncks - spending longer time fetching
	 *    the data - and fast reader finishing the reads quickly enough to
	 *    keep chances of seqlock to be updated during the read low.
	 */

	/*
	 * Seqlock implementation:
	 * - Increment before write (makes counter ODD = write in progress)
	 * - Increment after write (makes counter EVEN = stable)
	 * - Counter always increases, never decrements
	 *
	 * Reader checks:
	 * 1. If counter is ODD, a write is in progress -> retry
	 * 2. Save counter value before reading
	 * 3. After reading, compare counter value:
	 *    - If changed (even by 2), writer updated during read -> retry
	 *    - If same, read was consistent -> success
	 *
	 * This allows detecting concurrent writes without requiring the writer
	 * to decrement, which would make detection unreliable if a full write
	 * cycle completed during the read.
	 */
	atomic_fetch_add_explicit(&r->writing, 1, memory_order_release); /* Start write: make counter ODD */

	if (next_w == rd + r->num_slots)
	{
		/* buffer full -> drop oldest */
		r->dropped++;
		ret = -ENOSPC;
		if (dropfull)
			goto end;
		atomic_store_explicit(&r->rindex, rd + 1, memory_order_release);
		rd = rd + 1;
	}

	memcpy(&r->data[(size_t)buf_idx * r->item_size], data, len);
	atomic_thread_fence(memory_order_release);

	/* publish new writer index */
	atomic_store_explicit(&r->windex, next_w, memory_order_release);
end:
	atomic_fetch_add_explicit(&r->writing, 1, memory_order_release); /* End write: make counter EVEN (allows reader to detect intervening writes) */

	return ret;
}

/**
 * ring_read() - Read data from ring buffer (single reader)
 * @r: Pointer to ring buffer
 * @buf: Destination buffer for read data
 * @num_chunks: Maximum number of chunks to read
 *
 * Reads up to num_chunks entries from ring buffer. This function implements
 * a lock-free single-reader design using atomic operations and seqlock.
 *
 * Memory ordering:
 * - Reader owns rindex (uses relaxed atomics for own index)
 * - Reader acquires windex to see writer's data updates
 * - Seqlock check ensures no concurrent writer modified data during read
 * - Release semantics on rindex publish informs writer of freed space
 *
 * Retry behavior:
 * - Retries up to MAX_RETRY_ATTEMPTS if writer is active (seqlock odd)
 * - Retries if seqlock changed during read (writer modified buffer)
 * - Uses SPINAWHILE() to reduce CPU/bus contention during busy-wait
 *
 * Concurrency: Only ONE reader thread/process may call this function.
 * Multiple readers will corrupt the buffer. Safe concurrent writer via ring_add().
 *
 * Performance: O(1) amortized. May retry if writer active. Handles wrap-around
 * with at most 2 memcpy operations.
 *
 * Return: Number of chunks read (0 to num_chunks), -EAGAIN if empty or max
 * retries exceeded, -EINVAL on invalid parameters
 */
int ring_read(struct mvaring *r, void *buf, unsigned int num_chunks)
{
	unsigned int tries = 0;
	unsigned int w, rd, available, start, max_contig;
	uint8_t seq1;

	if (!r || !buf || num_chunks == 0)
		return -EINVAL;

retry:
	seq1 = atomic_load_explicit(&r->writing, memory_order_acquire);
	if (seq1 & 1)
	{
		if (++tries < MAX_RETRY_ATTEMPTS)
		{
			/*
			 * TODO: Check if we need to spin here. Other option is
			 * to just keep trying and forget the 'tries' -counter.
			 *
			 * I tried experimenting using sched_yield() and
			 * SCHED_FIFO scheduled processes - and ended up to
			 * deadlocking the system when I forced both processes to
			 * the same core. I need to revise the scheduling if we
			 * want to go that route.
			 */
			SPINAWHILE();
			goto retry;
		}

		return -EAGAIN;
	}

	/* Memory ordering notes:
	 * - windex load uses acquire: pairs with writer's release, ensures we see
	 *   all writer's data updates before reading from buffer
	 * - rindex load uses relaxed: we own it (single reader), no sync needed
	 */
	w = atomic_load_explicit(&r->windex, memory_order_acquire);
	rd = atomic_load_explicit(&r->rindex, memory_order_relaxed);

	available = w - rd;
	if (available == 0)
		return -EAGAIN;

	if (num_chunks > available)
		num_chunks = available;

	start = rd & slot_mask(r);
	max_contig = r->num_slots - start;
	size_t isz = r->item_size;

	if (max_contig >= num_chunks)
	{
		memcpy(buf, &r->data[start * isz], num_chunks * isz);
	}
	else
	{
		/* wrap-around copy */
		memcpy(buf, &r->data[start * isz], max_contig * isz);
		memcpy((uint8_t *)buf + max_contig * isz, &r->data[0],
		       (num_chunks - max_contig) * isz);
	}

	/* re-check sequence */
	uint8_t seq2 = atomic_load_explicit(&r->writing, memory_order_acquire);
	if (seq1 != seq2)
	{
		/* writer updated while we read -> retry */
		if (++tries < MAX_RETRY_ATTEMPTS)
		{
			SPINAWHILE();
			goto retry;
		}
		return -EAGAIN;
	}

	/* Commit consume by advancing rindex */
	atomic_store_explicit(&r->rindex, rd + num_chunks, memory_order_release);

	return (int)num_chunks;
}
