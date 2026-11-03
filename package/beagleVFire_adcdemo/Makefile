# Buildroot copy: the library sources live inside the package directory
MVADIR=common/mvaring
MVALIB=$(MVADIR)/libmvaring.a
SHMDIR=common/shmem
SHMLIB=$(SHMDIR)/libshmem.a
STRDIR=common/adc_stream
STRLIB=$(STRDIR)/libadcstream.a
CFLAGS=-Wall
INCFLAGS=-I$(MVADIR)/inc -I$(SHMDIR)/inc -I$(STRDIR)/inc
DBGFLAGS=-ggdb
SRC=src/bvf_backend.c src/beagle_uio.c
# HDR2=inc/mvaring.h inc/common.h inc/adc_common.h
# SRC2=src/shmem.c src/mvaring.c src/adcdemo_helpers.c
OUT=bin/bvf_adc_stream
HDR=inc/beagle_uio.h inc/bvf_adc_data.h inc/common.h inc/adc_common.h
DISPSRC=rpi_opengl_graph.c shmem.c mvaring.c adcdemo_helpers.c
DISPLDFLAGS=-lm -lglut -lGLEW -lGL
# INSOUT=rpi_ring_inspect
# INSSRC=rpi_ring_inspect.c mvaring.c
INSLDFLAGS=-lncurses -lm
CC=gcc

all: $(OUT)
# $(INSOUT)
# dbg: $(OUT)_dbg $(DISPOUT)_dbg $(OUT2)_dbg $(INSOUT)_dbg
# Library is rebuilt by its own Makefile (a no-op when up to date)
$(STRLIB): FORCE
	$(MAKE) -C $(STRDIR) CC="$(CC)" CFLAGS="-Wall -O2 "

$(SHMLIB): FORCE
	$(MAKE) -C $(SHMDIR) CC="$(CC)" CFLAGS="-Wall -O2 "

$(MVALIB): FORCE
	$(MAKE) -C $(MVADIR) CC="$(CC)" CFLAGS="-Wall -O2"

FORCE:

$(OUT): $(SRC) $(HDR) $(MVALIB) $(SHMLIB) $(STRLIB)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INCFLAGS) -o $(OUT) $(SRC) $(STRLIB) $(MVALIB) $(SHMLIB) $(LDFLAGS)

$(OUT2): $(SRC2) $(HDR2)
	$(CC) $(CFLAGS) -o $(OUT2) $(SRC2)

$(DISPOUT): $(DISPSRC) $(HDR)
	$(CC) $(CFLAGS) -o $(DISPOUT) $(DISPSRC) $(DISPLDFLAGS)

$(INSOUT): $(INSSRC) $(HDR2)
	$(CC) $(CFLAGS) -o $(INSOUT) $(INSSRC) $(INSLDFLAGS)

$(OUT)_dbg: $(SRC) $(HDR) $(MVALIB) $(SHMLIB) $(STRLIB)
	$(CC) $(CFLAGS) $(DBGFLAGS) -o $(OUT)_dbg $(SRC) $(STRLIB) $(MVALIB) $(SHMLIB)

# $(OUT2)_dbg: $(SRC2) $(HDR2)
# 	$(CC) $(CFLAGS) $(DBGFLAGS) -o $(OUT2)_dbg $(SRC2)

# $(DISPOUT)_dbg: $(DISPSRC) $(HDR)
# 	$(CC) $(CFLAGS) $(DBGFLAGS) -o $(DISPOUT)_dbg $(DISPSRC) $(DISPLDFLAGS)

# $(INSOUT)_dbg: $(INSSRC) $(HDR2)
# 	$(CC) $(CFLAGS) $(DBGFLAGS) -o $(INSOUT)_dbg $(INSSRC) $(INSLDFLAGS)

clean:
	$(MAKE) -C $(MVADIR) clean
	$(MAKE) -C $(SHMDIR) clean
	$(MAKE) -C $(STRDIR) clean
	rm -rf $(DISPOUT) $(OUT) $(OUT2) $(INSOUT)
	rm -rf $(DISPOUT)_dbg $(OUT)_dbg $(OUT2)_dbg $(INSOUT)_dbg
