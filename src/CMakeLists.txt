# Generated header: pairring/version.hpp carries PROJECT_VERSION.
set(PAIRRING_GENERATED_INCLUDE_DIR "${PROJECT_BINARY_DIR}/generated/include")
configure_file(version.hpp.in "${PAIRRING_GENERATED_INCLUDE_DIR}/pairring/version.hpp" @ONLY)

# The library: header-only.
add_library(pairring INTERFACE)
add_library(pairring::pairring ALIAS pairring)

target_compile_features(pairring INTERFACE cxx_std_17)

target_include_directories(pairring
  INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PAIRRING_GENERATED_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(FILES "${PAIRRING_GENERATED_INCLUDE_DIR}/pairring/version.hpp"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/pairring)

# Compiles every header on its own, so that the warning policy and the static
# analyzers apply to the header-only code. Not installed, and not part of the
# coverage figure: it is compiled, never run.
add_library(pairring_header_check OBJECT header_check.cpp)
target_link_libraries(pairring_header_check PRIVATE pairring)

pairring_set_warnings(pairring_header_check)
pairring_enable_sanitizers(pairring_header_check)
pairring_enable_clang_tidy(pairring_header_check)
pairring_enable_cppcheck(pairring_header_check)
