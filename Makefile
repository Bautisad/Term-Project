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

# supervisor_behavior is a shell script, so valgrind must wrap the binaries it starts
# (via GT_VALGRIND), not the shell; exit code 99 keeps findings apart from the supervisor's 0/1.
# killloop/soiltools/soakcheck_behavior are shell scripts too, but unlike supervisor_behavior
# they cannot take GT_VALGRIND yet: under valgrind they only check dash (a 15-byte strdup leak
# in the shell). They still run in `make test` and `make asan`. TODO (Duke):
# thread GT_VALGRIND through tests/*_test.sh and tools/{killloop,soakcheck}.sh, then drop them here.
memcheck: all
	cd build && ctest -T memcheck -E 'supervisor_behavior|killloop_behavior|soiltools_behavior|soakcheck_behavior' --output-on-failure --no-tests=error \
		--overwrite MemoryCheckCommandOptions="--leak-check=full --error-exitcode=1"
	cd build && GT_VALGRIND="valgrind --trace-children=yes --leak-check=full --error-exitcode=99" \
		ctest -R supervisor_behavior --output-on-failure --no-tests=error

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
