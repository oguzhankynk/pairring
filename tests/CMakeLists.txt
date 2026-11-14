include(GoogleTest)

add_executable(pairring_unit_tests
  unit/test_version.cpp
  unit/test_crc32c.cpp
  unit/test_config.cpp
  unit/test_channel.cpp
  unit/test_processes.cpp
)

target_link_libraries(pairring_unit_tests PRIVATE pairring::pairring GTest::gtest GTest::gtest_main)

pairring_set_warnings(pairring_unit_tests)
pairring_enable_sanitizers(pairring_unit_tests)
pairring_enable_coverage(pairring_unit_tests)
pairring_apply_sanitizer_test_wrapper(pairring_unit_tests)

gtest_discover_tests(pairring_unit_tests)

pairring_add_coverage_target()
if(TARGET coverage)
  # The 'coverage' custom target has no implicit dependency on the test
  # binaries; without this, a clean build runs ctest before
  # pairring_unit_tests exists and every test reports NOT_BUILT.
  add_dependencies(coverage pairring_unit_tests)
endif()
