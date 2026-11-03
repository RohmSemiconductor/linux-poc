// Compile: gcc -o shm_server shm_server.c -lrt
// Run: ./shm_server

#include <errno.h>
#include <fcntl.h> // O_* constants
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h> // shm_open, mmap
#include <sys/stat.h> // mode constants
#include <unistd.h>

#include "shmem.h"

// #include <pthread.h>
// pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

/* static struct g_shmem_info *g_i; */

void shmem_close(struct shmem_info *info)
{
	if (!info)
		return;

	if (!info->buff)
		return;

	munmap(info->buff, info->size);

	close(info->fd);
}

static int __shmem_open(const char *name, const size_t size, struct shmem_info *info, bool readonly, bool quiet)
{
	void *b;
	int fd;
	int map_flags = PROT_READ;
	int openflags = O_RDONLY;

	if (!readonly)
	{
		map_flags |= PROT_WRITE;
		openflags = O_RDWR;
	}

	if (!info)
		return -EINVAL;

	fd = shm_open(name, openflags, 0);
	if (fd == -1)
	{
		int err = errno;

		if (!quiet)
			perror("shm_open");

		return (err) ? -err : -1;
	}

	b = mmap(NULL, size, map_flags, MAP_SHARED, fd, 0);
	if (b == MAP_FAILED)
	{
		int err = errno;

		perror("mmap");
		close(fd);

		return (err) ? -err : -1;
	}

	info->buff = b;
	info->size = size;
	info->fd = fd;
	info->name = strdup(name);

	return 0;
}

int shmem_open(const char *name, const size_t size, struct shmem_info *info, bool quiet)
{
	return __shmem_open(name, size, info, false, quiet);
}

int shmem_open_ro(const char *name, const size_t size, struct shmem_info *info, bool quiet)
{
	return __shmem_open(name, size, info, true, quiet);
}

int shmem_create(const char *name, const size_t size, struct shmem_info *info)
{
	int fd;
	void *b;

	if (!info)
		return -EINVAL;

	if (!name || name[0] != '/')
		return -EINVAL;

	fd = shm_open(name, O_CREAT | O_RDWR, 0666);
	if (fd == -1)
	{
		int err = errno;

		perror("shm_open");
		return (err) ? -err : -1;
	}

	if (ftruncate(fd, size) == -1)
	{
		int err = errno;

		perror("ftruncate");
		close(fd);
		shm_unlink(name);

		return (err) ? -err : -1;
	}

	// Map into address space

	b = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

	if (b == MAP_FAILED)
	{
		int err = errno;

		perror("mmap");
		close(fd);
		shm_unlink(name);

		return (err) ? -err : -1;
	}

	info->buff = b;
	info->size = size;
	info->fd = fd;
	info->name = strdup(name);

	return 0;
}

void shmem_destroy(struct shmem_info *i)
{
	if (!i || !i->buff)
		return;

	shmem_close(i);

	if (shm_unlink(i->name) == -1)
		perror("shm_unlink");
}
