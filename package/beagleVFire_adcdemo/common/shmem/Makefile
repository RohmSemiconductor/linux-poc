# Builds libshmem.a (POSIX shared memory helpers). Override CC/CFLAGS for
# cross builds, e.g.  make CFLAGS="-Wall -O2 -m32"
CC ?= gcc
AR ?= ar
CFLAGS ?= -Wall -O2

libshmem.a: src/shmem.c inc/shmem.h
	$(CC) $(CFLAGS) -Iinc -c -o shmem.o src/shmem.c
	$(AR) rcs $@ shmem.o

clean:
	rm -f shmem.o libshmem.a

.PHONY: clean
