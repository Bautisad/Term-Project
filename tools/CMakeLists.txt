# Developer tools (Duke). The top-level CMakeLists.txt includes this file when it exists.

# gt-feeder: fake sensor hub for store tests (P1 kill -9 loop, soak). Installed so
# `make deploy` puts it on the Pi, where the crash tests run.
if(UNIX AND TARGET gt_store)
  add_executable(gt-feeder feeder.c)
  target_link_libraries(gt-feeder PRIVATE gt_store)
  install(TARGETS gt-feeder RUNTIME DESTINATION bin)

  # gt-logcheck: judges a store after a crash (P1). Reads through the store's own reader,
  # so it shares the store's CRC32 and record decoder.
  add_executable(gt-logcheck logcheck.c)
  target_link_libraries(gt-logcheck PRIVATE gt_store)
  install(TARGETS gt-logcheck RUNTIME DESTINATION bin)
endif()
