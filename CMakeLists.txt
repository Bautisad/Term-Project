cmake_minimum_required(VERSION 3.20)
project(GreenThumb VERSION 0.1.0 LANGUAGES C)

# --- Language: C17, no compiler extensions (CLAUDE.md "Style") ---------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Strict C17 hides POSIX APIs (sigaction, fsync, clock_gettime, ...); ask for them.
add_compile_definitions($<$<PLATFORM_ID:Linux>:_POSIX_C_SOURCE=200809L>)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(GT_SANITIZE "Build with AddressSanitizer + UBSan (make asan)" OFF)

find_package(Threads REQUIRED)

# --- Shared flags: every target links this and inherits the rules ------------
add_library(gt_flags INTERFACE)
target_include_directories(gt_flags INTERFACE "${CMAKE_SOURCE_DIR}/include")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(gt_flags INTERFACE -Wall -Wextra -Werror -Wvla)
  if(GT_SANITIZE)
    target_compile_options(gt_flags INTERFACE
      -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(gt_flags INTERFACE -fsanitize=address,undefined)
  endif()
elseif(MSVC)
  # Lets Visual Studio configure the project; the code itself targets Linux.
  target_compile_options(gt_flags INTERFACE /W4 /WX)
endif()
target_link_libraries(gt_flags INTERFACE Threads::Threads)

# --- Helpers ------------------------------------------------------------------
# Static library from every src/<dir>/*.c except files named *main.c.
# Skipped silently while the directory is empty or missing.
function(gt_library name dir)
  file(GLOB srcs CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/${dir}/*.c")
  list(FILTER srcs EXCLUDE REGEX "main\\.c$")
  if(NOT srcs)
    return()
  endif()
  add_library(${name} STATIC ${srcs})
  target_include_directories(${name} PUBLIC "${CMAKE_SOURCE_DIR}/src/${dir}")
  target_link_libraries(${name} PUBLIC gt_flags)
endfunction()

# Executable (one supervisable daemon) from one source file. Links whichever of
# the listed libraries exist yet, so a daemon builds before its neighbours do.
function(gt_daemon name src)
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/src/${src}")
    return()
  endif()
  add_executable(${name} "src/${src}")
  target_link_libraries(${name} PRIVATE gt_flags)
  foreach(dep IN LISTS ARGN)
    if(TARGET ${dep})
      target_link_libraries(${name} PRIVATE ${dep})
    endif()
  endforeach()
  install(TARGETS ${name} RUNTIME DESTINATION bin)
endfunction()

# --- Libraries (one per area; shared contracts live in include/) -------------
gt_library(gt_ipc      ipc)        # Andrew
gt_library(gt_sensors  sensors)    # Andrew: drivers shared by both sensor procs
gt_library(gt_supervisor_core supervisor)  # Andrew: everything but supervisor/main.c
gt_library(gt_store    store)      # Duke
gt_library(gt_analysis analysis)   # Duke
if(TARGET gt_analysis AND UNIX)
  target_link_libraries(gt_analysis PUBLIC m)
endif()

# --- Daemons ------------------------------------------------------------------
gt_daemon(gt-supervisor    supervisor/main.c     gt_supervisor_core gt_ipc)
gt_daemon(gt-hub           hub/main.c            gt_ipc gt_store)
gt_daemon(gt-sensor-soil   sensors/soil_main.c   gt_sensors gt_ipc)
gt_daemon(gt-sensor-light  sensors/light_main.c  gt_sensors gt_ipc)
gt_daemon(gt-interface     interface/main.c      gt_analysis gt_store)

# --- Tools (tools/CMakeLists.txt; built before tests, which use them) --------
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/CMakeLists.txt")
  add_subdirectory(tools)
endif()

# --- Tests (tests/CMakeLists.txt registers them with add_test) ---------------
include(CTest)   # also calls enable_testing(); needed by `ctest -T memcheck`
if(EXISTS "${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt")
  add_subdirectory(tests)
endif()
