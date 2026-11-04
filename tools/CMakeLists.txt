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

  # gt-csv: exports a store as CSV for the analysis work. Reads through the store's own
  # reader, so it shares the store's CRC32 and record decoder.
  add_executable(gt-csv csvexport.c)
  target_link_libraries(gt-csv PRIVATE gt_store)
  install(TARGETS gt-csv RUNTIME DESTINATION bin)

  # gt-analyze: runs the dry-down analysis (#27) on a store. Installed for the Pi, where the
  # recorded data is.
  if(TARGET gt_analysis)
    add_executable(gt-analyze analyze.c)
    target_link_libraries(gt-analyze PRIVATE gt_analysis gt_store)
    install(TARGETS gt-analyze RUNTIME DESTINATION bin)
  endif()

  # killloop.sh: the P1 driver. Installed next to the binaries it runs, for the Pi.
  install(PROGRAMS killloop.sh DESTINATION bin)

  # soakcheck.sh: the 1-hour soak (`make soakcheck`). Installed for the Pi too.
  install(PROGRAMS soakcheck.sh DESTINATION bin)

  # soilread.sh logs raw soil sensor reads and soilstats.sh judges them (S1-S5). Installed
  # for the Pi, where the sensor is.
  install(PROGRAMS soilread.sh soilstats.sh DESTINATION bin)
endif()
