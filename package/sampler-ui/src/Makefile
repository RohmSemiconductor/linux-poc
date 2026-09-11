
LIB = libiio_wrapper_but_python_shouldnt_import_this.so

all: $(LIB)

$(LIB): libiio_wrapper.o
	$(CC) -shared -flto -L$(HOST_DIR)/lib -o $@ $? -liio

%.o: %.c
	$(CC) -g -O3 -fPIC -c -I$(HOST_DIR)/include $?

clean:
	rm -f *.o *.so

.PHONY: all clean
