CC      = gcc
CFLAGS  = -Wall -Wextra -g -Iinclude
LDFLAGS = -lpthread -lrt -lncurses

TARGETS = arbiter_exec hip_exec asp_exec

all: $(TARGETS)
	@echo "Build complete."

arbiter_exec: arbiter/arbiter.c
	$(CC) $(CFLAGS) arbiter/arbiter.c -o $@ $(LDFLAGS)

hip_exec: hip/hip.c
	$(CC) $(CFLAGS) hip/hip.c -o $@ $(LDFLAGS)

asp_exec: asp/asp.c
	$(CC) $(CFLAGS) asp/asp.c -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
