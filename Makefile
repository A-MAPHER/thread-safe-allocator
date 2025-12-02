CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -pthread
LDFLAGS := -pthread
# Uncomment to add sanitizers when debugging:
# CFLAGS  += -fsanitize=address -fsanitize=undefined
# LDFLAGS += -fsanitize=address -fsanitize=undefined
# For thread sanitizer (slower):
# CFLAGS  += -fsanitize=thread
# LDFLAGS += -fsanitize=thread

INCLUDES := -I.

all: test_correctness bench

allocator.o: allocator.c allocator.h
	$(CC) $(CFLAGS) $(INCLUDES) -c allocator.c -o $@

libtsalloc.a: allocator.o
	ar rcs $@ $^

tests/correctness.o: tests/correctness.c allocator.h
	$(CC) $(CFLAGS) $(INCLUDES) -c tests/correctness.c -o $@

tests/bench.o: tests/bench.c allocator.h
	$(CC) $(CFLAGS) $(INCLUDES) -c tests/bench.c -o $@

test_correctness: tests/correctness.o libtsalloc.a
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

bench: tests/bench.o libtsalloc.a
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f *.o tests/*.o *.a test_correctness bench

.PHONY: all clean
