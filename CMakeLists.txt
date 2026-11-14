cmake_minimum_required(VERSION 3.25)

project(pairring
  VERSION 0.1.0
  DESCRIPTION "Point-to-point message channel over remotely writable memory"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
find_package(Doxygen QUIET)

option(BUILD_SHARED_LIBS "Build pairring as a shared library" OFF)
option(PAIRRING_BUILD_TESTS "Build unit tests" ${PROJECT_IS_TOP_LEVEL})
option(PAIRRING_BUILD_EXAMPLES "Build example programs" ${PROJECT_IS_TOP_LEVEL})
option(PAIRRING_BUILD_DOCS "Build the Doxygen API reference ('docs' / 'docs-check' targets)" ${DOXYGEN_FOUND})
option(PAIRRING_ENABLE_CLANG_TIDY "Run clang-tidy on the pairring target while building" OFF)
option(PAIRRING_ENABLE_CPPCHECK "Run cppcheck on the pairring target while building" OFF)
option(PAIRRING_ENABLE_COVERAGE "Build pairring and its tests with --coverage instrumentation" OFF)
option(PAIRRING_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(PAIRRING_SANITIZER "" CACHE STRING
  "Semicolon-separated sanitizers to enable, e.g. 'address;undefined' or 'thread'")

include(CompilerWarnings)
include(Sanitizers)
include(Coverage)
include(ClangTidy)
include(Cppcheck)
include(Doxygen)
include(Dependencies)

include(GNUInstallDirs)  # src/ installs the generated header
add_subdirectory(src)

if(PAIRRING_BUILD_TESTS)
  enable_testing()
  pairring_fetch_test_dependencies()
  add_subdirectory(tests)
endif()

if(PAIRRING_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(PAIRRING_BUILD_DOCS)
  pairring_add_doxygen_targets()
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS pairring
  EXPORT pairringTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/pairring DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT pairringTargets
  FILE pairringTargets.cmake
  NAMESPACE pairring::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pairring)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/pairringConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/pairringConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pairring)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/pairringConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/pairringConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/pairringConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pairring)
