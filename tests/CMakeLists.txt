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
