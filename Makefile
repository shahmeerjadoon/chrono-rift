CC      = gcc
CFLAGS  = -Wall -Wextra -g -Iinclude
LDFLAGS = -lpthread -lrt -lncurses

SHARED = shared/weapons.c

TARGETS = arbiter_exec hip_exec asp_exec

all: $(TARGETS)
	@echo "Build complete."

arbiter_exec: arbiter/arbiter.c $(SHARED)
	$(CC) $(CFLAGS) arbiter/arbiter.c $(SHARED) -o $@ $(LDFLAGS)

hip_exec: hip/hip.c $(SHARED)
	$(CC) $(CFLAGS) hip/hip.c $(SHARED) -o $@ $(LDFLAGS)

asp_exec: asp/asp.c $(SHARED)
	$(CC) $(CFLAGS) asp/asp.c $(SHARED) -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean