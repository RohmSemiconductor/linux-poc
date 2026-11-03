#ifndef MVARING_H
#define MVARING_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * mvaring - single writer, single reader ring buffer for fixed size items.
 *
 * The ring does not know or care about the type of the stored items; it only
 * knows the item size. The type and layout of the data is the responsibility
 * of the users (producer and consumer).
 *
 * The version only describes the layout of the ring header, it must not be
 * changed because of the stored data type.
 */
#define MVARING_VERSION 5
#define MAX_RETRY_ATTEMPTS 1000

/* Header fields are little endian */
struct mvaring {
	uint8_t version;  /* ring header layout version */
	_Atomic uint8_t writing;  /* seqlock for write protection */
	uint16_t dropped; /* counter for overwritten entries (too slow reader) */
	uint32_t size;    /* Total size of the ring in bytes, header included */
	atomic_uint rindex;
	atomic_uint windex;
	uint32_t item_size; /* Size of one slot in bytes */
	uint32_t num_slots; /* Number of slots, power of two */
	uint8_t data[];     /* num_slots * item_size bytes */
};

/* Total bytes needed by a ring of @slots slots of @isize bytes each */
#define MVARING_BYTES(isize, slots) \
	(sizeof(struct mvaring) + (size_t)(isize) * (slots))

/*
 * Initialise a ring in @buff. The slot count is the largest power of two
 * which fits in @bufsize. Returns NULL if @buff is too small for one slot
 * or arguments are invalid.
 */
struct mvaring *ring_init(void *buff, size_t bufsize, size_t item_size);
bool ring_full(struct mvaring *r);
bool ring_empty(struct mvaring *r);
/* Check version and header consistency (does not look at the stored data) */
bool ring_is_ok(struct mvaring *r);
unsigned int ring_available(struct mvaring *r);
/* One slot is kept free: capacity is num_slots - 1 items */
unsigned int ring_space(struct mvaring *r);
size_t ring_item_size(const struct mvaring *r);
/*
 * Ring add returns zero when an item was added to a buffer, and buffer had
 * space. Non zero value is returned if there was not enough space in the ring
 * (whether or not the old values were overwritten), or -EMSGSIZE if @len is
 * larger than the item size.
 *
 * The @len bytes are copied to the slot as is; if @len is smaller than the
 * item size the rest of the slot is left unmodified.
 *
 * The @dropfull controls whether the data is dropped when ring is full, or if
 * the old data is overwritten. Setting dropfull to true will cause new data
 * to be dropped, setting it false makes old to be overwritten.
 */
int ring_add(struct mvaring *r, const void *data, size_t len, bool dropfull);
/*
 * Read up to @num_items items. @buf must have room for @num_items *
 * ring_item_size() bytes. Returns number of items read, or -EAGAIN.
 */
int ring_read(struct mvaring *r, void *buf, unsigned int num_items);

#ifdef __cplusplus
}
#endif

#endif
