CC     = gcc
CFLAGS = -Wall -lpthread -g
DEPS   = ../../csrc/async/async.h ../../csrc/utils/alloc.h
OBJ    = test_async.o ../../csrc/async/async.o ../../csrc/utils/alloc.o

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

test: $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS)

clean:
	rm -f test $(OBJ)