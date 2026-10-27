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
