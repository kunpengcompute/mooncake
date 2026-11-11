cmake_minimum_required(VERSION 3.20)
project(mooncake_nof_staging_cpu_tests LANGUAGES CXX)

# This isolated target validates the registry and unsupported-backend behavior
# without Linux RDMA/SPDK/GPU dependencies. It does NOT validate hardware I/O.
# Supply an existing official GoogleTest checkout for an offline build, or use
# an installed GTest package. This target never downloads dependencies.
set(NOF_STAGING_GTEST_SOURCE_DIR
    ""
    CACHE PATH "Existing GoogleTest source tree")
if(NOF_STAGING_GTEST_SOURCE_DIR)
  set(BUILD_GMOCK
      OFF
      CACHE BOOL "" FORCE)
  set(INSTALL_GTEST
      OFF
      CACHE BOOL "" FORCE)
  add_subdirectory("${NOF_STAGING_GTEST_SOURCE_DIR}" googletest
                   EXCLUDE_FROM_ALL)
else()
  find_package(GTest REQUIRED)
endif()

add_executable(nof_staging_device_test ../nof_staging_device_test.cpp
                                       ../../src/nof_staging_device.cpp)
target_include_directories(nof_staging_device_test PRIVATE ../../include)
target_compile_features(nof_staging_device_test PRIVATE cxx_std_20)
target_link_libraries(nof_staging_device_test PRIVATE GTest::gtest_main)

enable_testing()
include(GoogleTest)
gtest_discover_tests(nof_staging_device_test)
