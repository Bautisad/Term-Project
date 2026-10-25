# Project Green Thumb: build entry points. Run from WSL/Linux.
# CMake (CMakeLists.txt + CMakePresets.json) defines the build; this file only
# gives the team the short commands from CLAUDE.md.

JOBS   ?= $(shell nproc)
PI     ?=
PI_DIR ?= greenthumb/bin
STAGE  := build-pi5/stage

.DEFAULT_GOAL := all
.PHONY: all test asan memcheck soakcheck deploy clean

all:
	cmake --preset host
	cmake --build --preset host -j $(JOBS)

test: all
	ctest --preset host

asan:
	cmake --preset asan
	cmake --build --preset asan -j $(JOBS)
	ctest --preset asan

memcheck: all
	cd build && ctest -T memcheck --output-on-failure --no-tests=error \
		--overwrite MemoryCheckCommandOptions="--leak-check=full --error-exitcode=1"

# 1-hour miniature of the 48h soak; the script lives in tools/ (Duke's area).
soakcheck: all
	@test -f tools/soakcheck.sh || { echo "soakcheck: tools/soakcheck.sh not written yet"; exit 1; }
	sh tools/soakcheck.sh build

# usage: make deploy PI=pi@<host>   (cross-builds for the Pi 5, copies binaries)
deploy:
	@test -n "$(PI)" || { echo "usage: make deploy PI=pi@<host>"; exit 1; }
	cmake --preset pi5
	cmake --build --preset pi5 -j $(JOBS)
	cmake --install build-pi5 --prefix $(STAGE)
	ssh $(PI) mkdir -p $(PI_DIR)
	rsync -av $(STAGE)/bin/ $(PI):$(PI_DIR)/

clean:
	rm -rf build build-asan build-pi5
