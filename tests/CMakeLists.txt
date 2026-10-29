# Tests live here. Register each one with add_test(); the top-level
# CMakeLists.txt includes this file automatically.

# Smoke test: the supervisor builds, starts, and exits cleanly.
if(TARGET gt-supervisor)
  add_test(NAME supervisor_version COMMAND gt-supervisor --version)
endif()

# Supervisor behaviour: restart/backoff, no restart on config exit, clean SIGTERM, no fd leak.
if(UNIX AND TARGET gt-supervisor)
  add_executable(gt-fake-child fake_child.c)
  target_link_libraries(gt-fake-child PRIVATE gt_flags)
  add_test(NAME supervisor_behavior
           COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/supervisor_test.sh
                   $<TARGET_FILE:gt-supervisor> $<TARGET_FILE:gt-fake-child>)
  set_tests_properties(supervisor_behavior PROPERTIES TIMEOUT 60)
endif()

# Store write path: CRC32, record encoding, segments, append, lock, tail check.
if(UNIX AND TARGET gt_store)
  add_executable(store_test store_test.c)
  target_link_libraries(store_test PRIVATE gt_store)
  add_test(NAME store_append COMMAND store_test)
  set_tests_properties(store_append PROPERTIES TIMEOUT 60)
endif()

# Disk-full, read-only and EIO handling. ENOSPC/EROFS come from a real 64 KB tmpfs in a
# private user+mount namespace; EIO and failures with the file already open are injected by
# wrapping write/fsync/ftruncate at link time (GNU ld), so the store itself is untouched.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET gt_store)
  add_executable(store_fault_test store_fault_test.c)
  target_link_libraries(store_fault_test PRIVATE gt_store)
  target_link_options(store_fault_test PRIVATE
                      -Wl,--wrap=write -Wl,--wrap=fsync -Wl,--wrap=ftruncate)
  add_test(NAME store_failures COMMAND store_fault_test)
  set_tests_properties(store_failures PROPERTIES TIMEOUT 60)

  # Fault-injection sweep over every libc call the store makes: the Nth call fails, for every N.
  add_executable(store_sweep_test store_sweep_test.c)
  target_link_libraries(store_sweep_test PRIVATE gt_store)
  target_link_options(store_sweep_test PRIVATE
                      -Wl,--wrap=open -Wl,--wrap=openat -Wl,--wrap=fstat -Wl,--wrap=pread
                      -Wl,--wrap=readdir -Wl,--wrap=fdopendir -Wl,--wrap=closedir
                      -Wl,--wrap=mkdir -Wl,--wrap=flock -Wl,--wrap=fsync -Wl,--wrap=ftruncate
                      -Wl,--wrap=write -Wl,--wrap=close -Wl,--wrap=calloc -Wl,--wrap=gmtime_r)
  add_test(NAME store_error_paths COMMAND store_sweep_test)
  set_tests_properties(store_error_paths PROPERTIES TIMEOUT 300)
endif()

# Fake sensor feeder: run as a child process (count, resume, pacing, SIGTERM, SIGKILL,
# bad arguments), then on its own so `make memcheck` runs the feeder itself under Valgrind.
# feeder_smoke keeps its directory between runs, so it also exercises the resume path.
if(UNIX AND TARGET gt-feeder)
  add_executable(feeder_test feeder_test.c)
  target_link_libraries(feeder_test PRIVATE gt_store)
  add_test(NAME feeder_behavior COMMAND feeder_test $<TARGET_FILE:gt-feeder>)
  set_tests_properties(feeder_behavior PROPERTIES TIMEOUT 120)
  add_test(NAME feeder_smoke
           COMMAND gt-feeder --dir ${CMAKE_BINARY_DIR}/feeder_smoke --rate 0 --count 2000)
  set_tests_properties(feeder_smoke PROPERTIES TIMEOUT 120)
endif()

# kill -9 loop (tools/killloop.sh): passes, reports a failing or damaged store, stops on
# request, repeats its delays for a seed, rejects bad arguments.
if(UNIX AND TARGET gt-feeder AND TARGET gt-logcheck)
  add_test(NAME killloop_behavior
           COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/killloop_test.sh
                   ${CMAKE_SOURCE_DIR}/tools/killloop.sh $<TARGET_FILE_DIR:gt-feeder>)
  set_tests_properties(killloop_behavior PROPERTIES TIMEOUT 120)
endif()

# Soak check (tools/soakcheck.sh): passes, and fails on a dying feeder, fd or RSS growth, a
# sequence gap, a rejected store, too few records and ERROR lines; bad arguments exit 2.
if(UNIX AND TARGET gt-feeder AND TARGET gt-logcheck)
  add_test(NAME soakcheck_behavior
           COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/soakcheck_test.sh
                   ${CMAKE_SOURCE_DIR}/tools/soakcheck.sh $<TARGET_FILE_DIR:gt-feeder>)
  set_tests_properties(soakcheck_behavior PROPERTIES TIMEOUT 120)
endif()

# Log checker: clean and corrupted logs, seq gaps, writer-log and progress checks, and the
# kill -9 / restart chain with the real feeder.
if(UNIX AND TARGET gt-logcheck AND TARGET gt-feeder)
  add_executable(logcheck_test logcheck_test.c)
  target_link_libraries(logcheck_test PRIVATE gt_store)
  add_test(NAME logcheck_behavior
           COMMAND logcheck_test $<TARGET_FILE:gt-logcheck> $<TARGET_FILE:gt-feeder>)
  set_tests_properties(logcheck_behavior PROPERTIES TIMEOUT 120)
endif()
