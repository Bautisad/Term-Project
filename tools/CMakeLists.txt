# Developer tools (Duke). The top-level CMakeLists.txt includes this file when it exists.

# gt-feeder: fake sensor hub for store tests (P1 kill -9 loop, soak). Installed so
# `make deploy` puts it on the Pi, where the crash tests run.
if(UNIX AND TARGET gt_store)
  add_executable(gt-feeder feeder.c)
  target_link_libraries(gt-feeder PRIVATE gt_store)
  install(TARGETS gt-feeder RUNTIME DESTINATION bin)
endif()
