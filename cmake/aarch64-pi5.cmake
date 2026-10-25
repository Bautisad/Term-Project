# Cross-compile toolchain for the Raspberry Pi 5 (64-bit OS, Cortex-A76).
# Used by the `pi5` preset in CMakePresets.json. Needs gcc-aarch64-linux-gnu.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)

set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-a76")

# Static link so the binaries don't depend on the Pi's glibc version
# (the Pi OS release hasn't been confirmed yet).
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
