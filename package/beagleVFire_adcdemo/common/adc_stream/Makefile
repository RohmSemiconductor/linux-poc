# Builds libadcstream.a: hardware independent ADC streamer (incl. main()).
# The product provides adcdemo_backend(). Override CC/CFLAGS for cross
# builds, e.g.  make CFLAGS="-Wall -O2 -m32"
CC ?= gcc
AR ?= ar
CFLAGS ?= -Wall -O2
INC = -Iinc -I../mvaring/inc -I../shmem/inc

OBJS = adc_stream.o adcdemo_helpers.o
HDRS = inc/adc_stream.h inc/adcdemo_helpers.h

libadcstream.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

%.o: src/%.c $(HDRS)
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

clean:
	rm -f $(OBJS) libadcstream.a

.PHONY: clean
