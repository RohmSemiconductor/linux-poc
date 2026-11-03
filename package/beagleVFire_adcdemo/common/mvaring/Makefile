# Builds libmvaring.a. Override CC/CFLAGS for cross builds, e.g.
#   make CFLAGS="-Wall -O2 -m32"
CC ?= gcc
AR ?= ar
CFLAGS ?= -Wall -O2

libmvaring.a: src/mvaring.c inc/mvaring.h
	$(CC) $(CFLAGS) -c -o mvaring.o src/mvaring.c
	$(AR) rcs $@ mvaring.o

test/ring_test: test/ring_test.c libmvaring.a
	$(CC) $(CFLAGS) -g -o $@ test/ring_test.c libmvaring.a

check: test/ring_test
	./test/ring_test

clean:
	rm -f mvaring.o libmvaring.a test/ring_test

.PHONY: clean check
