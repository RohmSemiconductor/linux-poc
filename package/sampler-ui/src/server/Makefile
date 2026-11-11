
LIB = libiio_wrapper_but_python_shouldnt_import_this.so

all: $(LIB)

$(LIB): libiio_wrapper.o
	$(CC) -shared -flto -L$(TARGET_DIR)/usr/lib -o $@ $? -liio

%.o: %.c
	$(CC) -g -O3 -fPIC -c -I$(TARGET_DIR)/usr/include $?

clean:
	rm -f *.o *.so

.PHONY: all clean
