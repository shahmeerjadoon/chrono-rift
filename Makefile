CXX      = g++
CXXFLAGS = -Wall -Wextra -g -std=c++17 -pthread -Iinclude -D_GNU_SOURCE
LDFLAGS  = -pthread -lrt
LDLIBSSFML = -lsfml-graphics -lsfml-window -lsfml-system

SHARED = src/weapons.cpp src/shared/log.cpp

TARGETS = arbiter_exec hip_exec asp_exec

all: $(TARGETS)
	@echo "build ok — run ./arbiter_exec from this directory"

arbiter_exec: src/arbiter/main.cpp $(SHARED)
	$(CXX) $(CXXFLAGS) src/arbiter/main.cpp $(SHARED) -o $@ $(LDFLAGS)

hip_exec: src/hip/main.cpp $(SHARED)
	$(CXX) $(CXXFLAGS) src/hip/main.cpp $(SHARED) -o $@ $(LDFLAGS) $(LDLIBSSFML)

asp_exec: src/asp/main.cpp $(SHARED)
	$(CXX) $(CXXFLAGS) src/asp/main.cpp $(SHARED) -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGETS)

install: all
	cp arbiter_exec hip_exec asp_exec /app/
	cp -r Sprites /app/

# Submission archive for grading: includes README_TA_DEMO.md, docker/run-dev.sh, sources, assets.
SUBMISSION_ZIP = submission.zip
.PHONY: submission-zip
submission-zip: clean
	rm -f $(SUBMISSION_ZIP)
	zip -r $(SUBMISSION_ZIP) . \
	  -x '$(SUBMISSION_ZIP)' \
	  -x '.git/*' \
	  -x 'arbiter_exec' -x 'hip_exec' -x 'asp_exec' \
	  -x '*.o' \
	  -x '.cursor/*' \
	  -x '*/__pycache__/*' \
	  -x '*.pyc'
	@echo "$(SUBMISSION_ZIP) ready (contains README_TA_DEMO.md and docker/run-dev.sh)"

.PHONY: all clean install submission-zip
