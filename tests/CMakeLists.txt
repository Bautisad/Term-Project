# Tests live here. Register each one with add_test(); the top-level
# CMakeLists.txt includes this file automatically.

# Smoke test: the supervisor builds, starts, and exits cleanly.
if(TARGET gt-supervisor)
  add_test(NAME supervisor_version COMMAND gt-supervisor --version)
endif()

# Supervisor behaviour: restart/backoff, no restart on config exit, clean SIGTERM, no fd leak,
# heartbeat deadlines (the fake child sends heartbeats through ipc_send, so it links gt_ipc).
if(UNIX AND TARGET gt-supervisor)
  add_executable(gt-fake-child fake_child.c)
  target_link_libraries(gt-fake-child PRIVATE gt_flags gt_ipc)
  add_test(NAME supervisor_behavior
           COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/supervisor_test.sh
                   $<TARGET_FILE:gt-supervisor> $<TARGET_FILE:gt-fake-child>)
  set_tests_properties(supervisor_behavior PROPERTIES TIMEOUT 120)
endif()

# Crash-loop rules (schedule_restart, child_watch): the window edge, recovery. No processes.
if(UNIX AND TARGET gt_supervisor_core)
  add_executable(child_test child_test.c)
  target_link_libraries(child_test PRIVATE gt_supervisor_core)
  add_test(NAME child_logic COMMAND child_test)
  set_tests_properties(child_logic PROPERTIES TIMEOUT 30)
endif()

# IPC send/receive: round trips, malformed messages, EOF, listening socket.
if(UNIX AND TARGET gt_ipc)
  add_executable(ipc_test ipc_test.c)
  target_link_libraries(ipc_test PRIVATE gt_ipc)
  add_test(NAME ipc_roundtrip COMMAND ipc_test)
  set_tests_properties(ipc_roundtrip PROPERTIES TIMEOUT 30)
endif()

# Soil sensor logic: median, range check, stuck detection. No hardware needed.
if(UNIX AND TARGET gt_sensors)
  add_executable(soil_logic_test soil_logic_test.c)
  target_link_libraries(soil_logic_test PRIVATE gt_sensors)
  add_test(NAME soil_logic COMMAND soil_logic_test)
  set_tests_properties(soil_logic PROPERTIES TIMEOUT 30)
endif()

# Light sensor logic: window totals, stuck and chatter rules. No hardware needed.
if(UNIX AND TARGET gt_sensors)
  add_executable(light_logic_test light_logic_test.c)
  target_link_libraries(light_logic_test PRIVATE gt_sensors)
  add_test(NAME light_logic COMMAND light_logic_test)
  set_tests_properties(light_logic PROPERTIES TIMEOUT 30)
endif()

# Hub logic: record building, cycle timing and queueing. No sockets or store needed.
if(UNIX AND TARGET gt_hub)
  add_executable(hub_logic_test hub_logic_test.c)
  target_link_libraries(hub_logic_test PRIVATE gt_hub)
  add_test(NAME hub_logic COMMAND hub_logic_test)
  set_tests_properties(hub_logic PROPERTIES TIMEOUT 30)
endif()

# Soil sensor process: the real loop with a fake sensor, against a stand-in hub and supervisor
# (messages, heartbeats, retries, hub restart, exit codes). Linux only: prctl, unix sockets.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET gt_sensors)
  add_executable(soil_proc_test soil_proc_test.c)
  target_link_libraries(soil_proc_test PRIVATE gt_sensors)
  add_test(NAME soil_process COMMAND soil_proc_test)
  set_tests_properties(soil_process PROPERTIES TIMEOUT 60)

  # Light sensor process: the same, with a pipe standing in for the GPIO line.
  add_executable(light_proc_test light_proc_test.c)
  target_link_libraries(light_proc_test PRIVATE gt_sensors)
  add_test(NAME light_process COMMAND light_proc_test)
  set_tests_properties(light_process PROPERTIES TIMEOUT 60)
endif()

# Hub process: the real loop against stand-in sensors and supervisor, with a real store
# (records, resume, bad messages, fd count, idle record, disk-full style failure, exit codes).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET gt_hub)
  add_executable(hub_proc_test hub_proc_test.c)
  target_link_libraries(hub_proc_test PRIVATE gt_hub)
  add_test(NAME hub_process COMMAND hub_proc_test)
  set_tests_properties(hub_process PROPERTIES TIMEOUT 60)
endif()

# Analysis: dry-down cycle detection on synthetic soil series.
if(UNIX AND TARGET gt_analysis)
  add_executable(analysis_dry_test analysis_dry_test.c)
  target_link_libraries(analysis_dry_test PRIVATE gt_analysis)
  add_test(NAME analysis_dry COMMAND analysis_dry_test)
  set_tests_properties(analysis_dry PROPERTIES TIMEOUT 60)

  # Analysis: time-until-dry from the cycles, and the reasons it holds back.
  add_executable(analysis_predict_test analysis_predict_test.c)
  target_link_libraries(analysis_predict_test PRIVATE gt_analysis)
  add_test(NAME analysis_predict COMMAND analysis_predict_test)
  set_tests_properties(analysis_predict PROPERTIES TIMEOUT 60)

  # Analysis: the light-adjusted estimate (#26), when it holds back, and the C3 comparison.
  add_executable(analysis_light_test analysis_light_test.c)
  target_link_libraries(analysis_light_test PRIVATE gt_analysis)
  add_test(NAME analysis_light COMMAND analysis_light_test)
  set_tests_properties(analysis_light PROPERTIES TIMEOUT 60)

  # Analysis: the needs-water state, its hysteresis, and when it says unknown.
  add_executable(analysis_thirst_test analysis_thirst_test.c)
  target_link_libraries(analysis_thirst_test PRIVATE gt_analysis)
  add_test(NAME analysis_thirst COMMAND analysis_thirst_test)
  set_tests_properties(analysis_thirst PROPERTIES TIMEOUT 60)

  # Analysis on a real store on disk, through the library and the real gt-analyze.
  if(TARGET gt-analyze AND TARGET gt_store)
    add_executable(analysis_store_test analysis_store_test.c)
    target_link_libraries(analysis_store_test PRIVATE gt_analysis gt_store)
    add_test(NAME analysis_store COMMAND analysis_store_test $<TARGET_FILE:gt-analyze>)
    set_tests_properties(analysis_store PROPERTIES TIMEOUT 300)
  endif()
endif()

# LAN page logic: states, reasons, history notes and both renderings, with no network.
if(UNIX AND TARGET gt_interface)
  add_executable(interface_page_test interface_page_test.c)
  target_link_libraries(interface_page_test PRIVATE gt_interface)
  add_test(NAME interface_page COMMAND interface_page_test)
  set_tests_properties(interface_page PROPERTIES TIMEOUT 30)

  # LAN page HTTP: the strict request parser (every refusal, every split position, a seeded
  # fuzz pass), the response builder and the private-peer check.
  add_executable(interface_http_test interface_http_test.c)
  target_link_libraries(interface_http_test PRIVATE gt_interface)
  add_test(NAME interface_http COMMAND interface_http_test)
  set_tests_properties(interface_http PROPERTIES TIMEOUT 120)
endif()

# LAN page daemon: the real loop in a forked child against real loopback TCP and a stand-in
# supervisor (routes, store states, refresh, connection limit and deadline, heartbeats, exit
# codes, no leaked descriptors).  Linux only: prctl, /proc.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET gt_interface)
  add_executable(interface_daemon_test interface_daemon_test.c)
  target_link_libraries(interface_daemon_test PRIVATE gt_interface)
  add_test(NAME interface_daemon COMMAND interface_daemon_test)
  set_tests_properties(interface_daemon PROPERTIES TIMEOUT 120)
endif()

# LAN page binary: the real gt-interface next to a real gt-feeder, started the way the supervisor
# starts it (control socket on fd 3): exit codes, heartbeats, the page following a store that
# appears, no stray descriptors, clean SIGTERM.  Linux only: prctl, /proc.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET gt-interface AND TARGET gt-feeder)
  add_executable(interface_lifecycle_test interface_lifecycle_test.c)
  target_link_libraries(interface_lifecycle_test PRIVATE gt_interface)
  add_test(NAME interface_lifecycle
           COMMAND interface_lifecycle_test $<TARGET_FILE:gt-interface> $<TARGET_FILE:gt-feeder>)
  set_tests_properties(interface_lifecycle PROPERTIES TIMEOUT 120)
endif()

# No outbound network (CLAUDE.md, a graded boundary): tools/nonetcheck.sh on the real src/ and
# the built gt-interface, and the guard's own test, which plants each kind of violation in a
# copy of src/ and checks that it is caught (and that comments and plain names are not).
if(UNIX)
  add_test(NAME nonet_behavior
           COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/nonet_test.sh
                   ${CMAKE_SOURCE_DIR}/tools/nonetcheck.sh ${CMAKE_SOURCE_DIR}/src)
  set_tests_properties(nonet_behavior PROPERTIES TIMEOUT 120)
endif()
if(UNIX AND TARGET gt-interface)
  add_test(NAME nonet_src
           COMMAND sh ${CMAKE_SOURCE_DIR}/tools/nonetcheck.sh ${CMAKE_SOURCE_DIR}/src
                   $<TARGET_FILE:gt-interface>)
  set_tests_properties(nonet_src PROPERTIES TIMEOUT 60)
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

# CSV export (gt-csv): every column and flag, flagged readings kept, ranges, two days, a corrupt
# record, an empty and a missing store, a full stdout, bad arguments.
if(UNIX AND TARGET gt-csv)
  add_executable(csvexport_test csvexport_test.c)
  target_link_libraries(csvexport_test PRIVATE gt_store)
  add_test(NAME csvexport_behavior COMMAND csvexport_test $<TARGET_FILE:gt-csv>)
  set_tests_properties(csvexport_behavior PROPERTIES TIMEOUT 60)
endif()

# Soil test tools (S1-S5): soilread.sh against a fake i2ctransfer (every read logged, 0xffff
# kept, failures logged, SIGTERM, arguments) and soilstats.sh on files with known answers
# (pass, fail and boundary for each test).
if(UNIX)
  add_test(NAME soiltools_behavior
           COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/soiltools_test.sh
                   ${CMAKE_SOURCE_DIR}/tools/soilread.sh ${CMAKE_SOURCE_DIR}/tools/soilstats.sh)
  set_tests_properties(soiltools_behavior PROPERTIES TIMEOUT 120)
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
