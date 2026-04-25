CXX      = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread

# Using ncurses (recommended for Docker)
LIBS = -lncurses -lrt

TARGETS = arbiter_exec hip_exec asp_exec

all: $(TARGETS)
	@echo Build complete.

arbiter_exec: arbiter/arbiter.cpp
	$(CXX) $(CXXFLAGS) arbiter/*.cpp -o $@ $(LIBS)

hip_exec: hip/hip.cpp
	$(CXX) $(CXXFLAGS) hip/*.cpp -o $@ $(LIBS)

asp_exec: asp/asp.cpp
	$(CXX) $(CXXFLAGS) asp/*.cpp -o $@ $(LIBS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
