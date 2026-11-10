# Builds bvf_adc_server. Override CC/CFLAGS/LDFLAGS for cross builds.
ROOT ?= ../..
BVFDIR = $(ROOT)/beagleVFire_adcdemo
MVADIR = $(ROOT)/common/mvaring
SHMDIR = $(ROOT)/common/shmem
CC ?= gcc
CFLAGS ?= -Wall -O2
INC = -I. -I$(BVFDIR)/inc -I$(MVADIR)/inc -I$(SHMDIR)/inc
SRC = adc_server.c ws.c $(MVADIR)/src/mvaring.c $(SHMDIR)/src/shmem.c

bvf_adc_server: $(SRC) ws.h $(BVFDIR)/inc/bvf_adc_data.h
	$(CC) $(CFLAGS) $(INC) -o $@ $(SRC) $(LDFLAGS) -lpthread -lrt

clean:
	rm -f bvf_adc_server

.PHONY: clean
