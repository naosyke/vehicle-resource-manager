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
)
target_include_directories(vrm_core PUBLIC include)
target_link_libraries(vrm_core PUBLIC yaml-cpp)
target_compile_options(vrm_core PRIVATE -Wall -Wextra -Wpedantic)

# DDS types generated from IDL.
idlcxx_generate(TARGET vrm_idl FILES idl/LifecycleMsgs.idl WARNINGS no-implicit-extensibility)

# DDS lifecycle protocol and the ManagedNode base class.
add_library(vrm_dds
  src/dds/dds_lifecycle.cpp
  src/dds/managed_node.cpp
)
target_link_libraries(vrm_dds PUBLIC vrm_core vrm_idl CycloneDDS-CXX::ddscxx)
target_compile_options(vrm_dds PRIVATE -Wall -Wextra)

add_executable(vrm_manager
  src/manager/main.cpp
  src/manager/manager.cpp
  src/manager/lifecycle_client.cpp
  src/manager/process.cpp
)
target_link_libraries(vrm_manager PRIVATE vrm_dds)
target_compile_options(vrm_manager PRIVATE -Wall -Wextra)

add_executable(vrm_demo_node src/nodes/demo_node.cpp)
target_link_libraries(vrm_demo_node PRIVATE vrm_dds)
target_compile_options(vrm_demo_node PRIVATE -Wall -Wextra)

# Tests
enable_testing()
find_package(GTest REQUIRED)

add_executable(vrm_unit_tests
  tests/test_lifecycle.cpp
  tests/test_manifest.cpp
)
target_link_libraries(vrm_unit_tests PRIVATE vrm_core GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(vrm_unit_tests)

# End-to-end: the manager starts real node processes over DDS and stops them.
add_test(NAME system_startup_and_shutdown
  COMMAND vrm_manager ${CMAKE_SOURCE_DIR}/config/test_system.yaml --exit-after 1)
add_test(NAME safety_critical_failure_aborts_startup
  COMMAND vrm_manager ${CMAKE_SOURCE_DIR}/config/test_safety_failure.yaml --exit-after 1)
set_tests_properties(safety_critical_failure_aborts_startup PROPERTIES WILL_FAIL TRUE)
