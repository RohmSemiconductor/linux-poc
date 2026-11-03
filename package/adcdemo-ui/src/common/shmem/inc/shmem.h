#ifndef _MVA_SHMEM
#define _MVA_SHMEM

#include <stdbool.h>
#include <stddef.h>

struct shmem_info {
	void *buff;
	int fd;
	const char *name;
	size_t size;
};

int shmem_create(const char *name, const size_t size, struct shmem_info *info);
int shmem_open(const char *name, const size_t size, struct shmem_info *info, bool quiet);
int shmem_open_ro(const char *name, const size_t size, struct shmem_info *info, bool quiet);
void shmem_close(struct shmem_info *info);
void shmem_destroy(struct shmem_info *info);



#endif
