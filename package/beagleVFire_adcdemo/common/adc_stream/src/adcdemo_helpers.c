#include <errno.h>
#include <stdio.h>

#include "adcdemo_helpers.h"

int adcdemo_shm_create(struct shmem_info *shi, struct mvaring **mr,
		       const char *name, size_t item_size, unsigned int slots)
{
	size_t size = MVARING_BYTES(item_size, slots);
	int ret;

	ret = shmem_create(name, size, shi);
	if (ret) {
		printf("shmem_create failed. Name %s, size %lu\n", name,
		       (unsigned long)size);
		return ret;
	}

	*mr = ring_init(shi->buff, size, item_size);
	if (!*mr) {
		printf("Ringbuffer init failed\n");
		return -EINVAL;
	}

	return 0;
}
