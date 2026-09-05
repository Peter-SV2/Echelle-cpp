# The headless core and its self-check.
#
# CMake builds the whole app including the ImGui GUI; this builds only the part
# that has no window, because that part can be compiled and RUN anywhere -- in
# WSL, in CI, on a machine with no GPU -- and the numerics are what most need
# checking. A test you can only run after installing a graphics toolchain is a
# test that stops being run.
#
#   make check
CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion
LDFLAGS  ?=

SRC  := src/table.cpp src/fitstat.cpp src/spec.cpp src/gpexport.cpp src/save.cpp src/ui_state.cpp
CHECK:= src/selfcheck.cpp
BIN  := build/selfcheck
BENCH:= build/bench

.PHONY: check bench clean
check: $(BIN)
	@./$(BIN)

$(BIN): $(SRC) $(CHECK) $(wildcard src/*.hpp)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SRC) $(CHECK) -o $@ $(LDFLAGS)

bench: $(SRC) src/bench.cpp $(wildcard src/*.hpp)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SRC) src/bench.cpp -o $(BENCH) $(LDFLAGS)
	@./$(BENCH)

clean:
	rm -rf build
