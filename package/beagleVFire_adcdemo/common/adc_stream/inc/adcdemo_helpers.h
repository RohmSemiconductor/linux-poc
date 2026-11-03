#ifndef _ADCDEMO_HELPERS_H
#define _ADCDEMO_HELPERS_H

#include "mvaring.h"
#include "shmem.h"

/* Create the shared memory and init a ring of 'slots' items of item_size */
int adcdemo_shm_create(struct shmem_info *shi, struct mvaring **mr,
		       const char *name, size_t item_size, unsigned int slots);

#endif
