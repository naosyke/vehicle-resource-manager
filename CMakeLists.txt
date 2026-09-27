cmake_minimum_required(VERSION 3.16)
project(vehicle_resource_manager VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(CycloneDDS-CXX REQUIRED)
find_package(yaml-cpp REQUIRED)

# Core logic without DDS: lifecycle state machine, manifest, logging.
add_library(vrm_core
  src/core/lifecycle.cpp
  src/core/manifest.cpp
  src/core/log.cpp
  src/core/cgroup.cpp
  src/core/deadline_monitor.cpp
  src/core/arbiter.cpp
)
target_include_directories(vrm_core PUBLIC include)
target_link_libraries(vrm_core PUBLIC yaml-cpp)
target_compile_options(vrm_core PRIVATE -Wall -Wextra -Wpedantic)

# DDS types generated from IDL.
idlcxx_generate(TARGET vrm_idl FILES idl/LifecycleMsgs.idl idl/TelemetryMsgs.idl WARNINGS no-implicit-extensibility)

# DDS lifecycle protocol and the ManagedNode base class.
add_library(vrm_dds
  src/dds/dds_lifecycle.cpp
  src/dds/managed_node.cpp
  src/dds/dds_telemetry.cpp
)
target_link_libraries(vrm_dds PUBLIC vrm_core vrm_idl CycloneDDS-CXX::ddscxx)
target_compile_options(vrm_dds PRIVATE -Wall -Wextra)

add_executable(vrm_manager
  src/manager/main.cpp
  src/manager/manager.cpp
  src/manager/lifecycle_client.cpp
  src/manager/process.cpp
  src/manager/cpu_tracer.cpp
  src/manager/telemetry.cpp
)
target_link_libraries(vrm_manager PRIVATE vrm_dds)
target_compile_options(vrm_manager PRIVATE -Wall -Wextra)

add_executable(vrm_monitor src/tools/monitor.cpp)
target_link_libraries(vrm_monitor PRIVATE vrm_dds)
target_compile_options(vrm_monitor PRIVATE -Wall -Wextra)

add_executable(vrm_demo_node src/nodes/demo_node.cpp)
target_link_libraries(vrm_demo_node PRIVATE vrm_dds)
target_compile_options(vrm_demo_node PRIVATE -Wall -Wextra)

# Tests
enable_testing()
find_package(GTest REQUIRED)

add_executable(vrm_unit_tests
  tests/test_lifecycle.cpp
  tests/test_manifest.cpp
  tests/test_cgroup_helpers.cpp
  tests/test_deadline_monitor.cpp
  tests/test_arbiter.cpp
  tests/test_cpu_tracer.cpp
  src/manager/cpu_tracer.cpp
)
target_link_libraries(vrm_unit_tests PRIVATE vrm_core GTest::gtest_main)
target_compile_definitions(vrm_unit_tests PRIVATE VRM_CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")
target_include_directories(vrm_unit_tests PRIVATE src/manager)
include(GoogleTest)
gtest_discover_tests(vrm_unit_tests)

# Needs a writable cgroup v2 filesystem (docker run --privileged --cgroupns=private);
# the tests skip themselves otherwise.
add_executable(vrm_cgroup_tests tests/test_cgroup_enforcement.cpp)
target_link_libraries(vrm_cgroup_tests PRIVATE vrm_core GTest::gtest_main)
gtest_discover_tests(vrm_cgroup_tests)

# End-to-end: the manager starts real node processes over DDS and stops them.
add_test(NAME system_startup_and_shutdown
  COMMAND vrm_manager ${CMAKE_SOURCE_DIR}/config/test_system.yaml --exit-after 1)
add_test(NAME safety_critical_failure_aborts_startup
  COMMAND vrm_manager ${CMAKE_SOURCE_DIR}/config/test_safety_failure.yaml --exit-after 1)
set_tests_properties(safety_critical_failure_aborts_startup PROPERTIES WILL_FAIL TRUE)

# Two systems with the same node names on one network must not control each
# other's nodes: system A stops while B is running; B's nodes must stay up.
add_test(NAME two_systems_do_not_interfere
  COMMAND sh -c "($<TARGET_FILE:vrm_manager> ${CMAKE_SOURCE_DIR}/config/test_system.yaml --exit-after 0.5 --no-cgroups > /dev/null &) \
    && $<TARGET_FILE:vrm_manager> ${CMAKE_SOURCE_DIR}/config/test_system.yaml --exit-after 3 --no-cgroups \
    --status-file two_systems.json > /dev/null \
    && ! grep -q '\"outcome\":\"exited\"' two_systems.json")

# A crashing node is restarted with backoff until its restart limit.
add_test(NAME crashed_node_is_restarted
  COMMAND sh -c "$<TARGET_FILE:vrm_manager> ${CMAKE_SOURCE_DIR}/config/test_restart.yaml --exit-after 4 \
    --report-interval 0 --status-file restart.json > /dev/null \
    && grep -q '\"name\":\"brake_control\",.*\"restarts\":2' restart.json \
    && grep -q 'reached its restart limit' restart.json")

# A hung node is detected by its missing heartbeat, killed and restarted.
add_test(NAME hung_node_is_detected_and_restarted
  COMMAND sh -c "$<TARGET_FILE:vrm_manager> ${CMAKE_SOURCE_DIR}/config/test_hang.yaml --exit-after 3 \
    --report-interval 0 --status-file hang.json > /dev/null \
    && grep -q 'heartbeat lost' hang.json \
    && grep -q '\"restarts\":1' hang.json")

# Real-time priority is applied (needs CAP_SYS_NICE, e.g. --privileged; skipped otherwise).
add_test(NAME realtime_priority_is_applied
  COMMAND sh -c "chrt -f 1 true 2>/dev/null || exit 77; \
    $<TARGET_FILE:vrm_manager> ${CMAKE_SOURCE_DIR}/config/demo_cpu_contention.yaml --exit-after 2 \
    --report-interval 0 --status-file realtime.json > /dev/null & \
    sleep 1.5 && grep -q '\"name\":\"brake_control\",.*\"policy\":\"SCHED_FIFO\",\"priority\":80' realtime.json; \
    status=$?; wait; exit $status")
set_tests_properties(realtime_priority_is_applied PROPERTIES SKIP_RETURN_CODE 77)

# Arbitration (needs a writable cgroupfs, e.g. --privileged --cgroupns=private; skipped otherwise).
set(NEEDS_CGROUPS "test -w /sys/fs/cgroup/cgroup.subtree_control && chrt -f 1 true 2>/dev/null || exit 77")

# With equal weights the hogs starve perception, and the arbiter makes them yield.
add_test(NAME arbitration_lowers_weights_of_hogs
  COMMAND sh -c "${NEEDS_CGROUPS}; $<TARGET_FILE:vrm_manager> ${CMAKE_SOURCE_DIR}/config/demo_arbitration.yaml \
    --equal-weights --exit-after 4 --report-interval 0 --status-file arbitration.json > /dev/null \
    && grep -q 'arbitration: lowered the CPU weight' arbitration.json")

# With weights by criticality the nodes get their cgroup cpu.weight.
add_test(NAME cpu_weights_follow_criticality
  COMMAND sh -c "${NEEDS_CGROUPS}; $<TARGET_FILE:vrm_manager> ${CMAKE_SOURCE_DIR}/config/demo_arbitration.yaml \
    --exit-after 1 --report-interval 0 --status-file weights.json > /dev/null \
    && grep -q '\"name\":\"perception\",.*\"cpu_weight\":1000' weights.json \
    && grep -q '\"name\":\"infotainment\",.*\"cpu_weight\":10}' weights.json")

add_test(NAME arbitration_stops_before_oom
  COMMAND sh -c "${NEEDS_CGROUPS}; $<TARGET_FILE:vrm_manager> ${CMAKE_SOURCE_DIR}/config/demo_memory_leak.yaml \
    --exit-after 5 --report-interval 0 --status-file oom.json > /dev/null \
    && grep -q 'before the OOM killer' oom.json && grep -q '\"oom_kills\":0' oom.json \
    && ! grep -q '\"oom_kills\":[1-9]' oom.json")

add_test(NAME arbitration_demotes_runaway_realtime_node
  COMMAND sh -c "${NEEDS_CGROUPS}; $<TARGET_FILE:vrm_manager> ${CMAKE_SOURCE_DIR}/config/test_rt_runaway.yaml \
    --exit-after 4 --report-interval 0 --status-file runaway.json > /dev/null \
    && grep -q 'demoted planner to SCHED_OTHER' runaway.json")

set_tests_properties(arbitration_lowers_weights_of_hogs cpu_weights_follow_criticality arbitration_stops_before_oom
  arbitration_demotes_runaway_realtime_node PROPERTIES SKIP_RETURN_CODE 77)

# Telemetry over DDS: a separate process (vrm_monitor) that starts 2 s late
# still sees the manager's nodes and the events from startup (transient local).
add_test(NAME monitor_receives_telemetry_over_dds
  COMMAND sh -c "($<TARGET_FILE:vrm_manager> ${CMAKE_SOURCE_DIR}/config/test_system.yaml --exit-after 5 \
    --no-cgroups --report-interval 0 > /dev/null 2>&1 &) \
    && sleep 2 && $<TARGET_FILE:vrm_monitor> --once 1.5 > monitor.txt \
    && grep -q 'SYSTEM test_system' monitor.txt \
    && grep -q '^brake_control .* active' monitor.txt \
    && grep -q '^media_sync .* skipped' monitor.txt \
    && grep -q 'startup complete' monitor.txt; status=$?; sleep 2; exit $status")

# The status file for the dashboard is valid JSON with the final node states.
add_test(NAME status_file_for_dashboard
  COMMAND sh -c "$<TARGET_FILE:vrm_manager> ${CMAKE_SOURCE_DIR}/config/test_system.yaml --exit-after 1 \
    --status-file status_test.json > /dev/null \
    && grep -q '\"system\":\"test_system\"' status_test.json \
    && grep -q '\"name\":\"media_sync\",.*\"outcome\":\"skipped\"' status_test.json")
