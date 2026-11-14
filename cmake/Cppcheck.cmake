include_guard(GLOBAL)

## pairring_enable_cppcheck(<target>)
#
# Sets CXX_CPPCHECK on <target> when PAIRRING_ENABLE_CPPCHECK is ON, so cppcheck
# runs as part of the normal build for that target, as a second static
# analyzer alongside clang-tidy. PAIRRING_WARNINGS_AS_ERRORS makes any cppcheck
# finding fail the build (--error-exitcode=1); otherwise findings are only
# printed.
function(pairring_enable_cppcheck target)
  if(NOT PAIRRING_ENABLE_CPPCHECK)
    return()
  endif()

  find_program(CPPCHECK_EXECUTABLE cppcheck)
  if(NOT CPPCHECK_EXECUTABLE)
    message(WARNING "PAIRRING_ENABLE_CPPCHECK is ON but cppcheck was not found; skipping ${target}.")
    return()
  endif()

  set(cppcheck_command
    "${CPPCHECK_EXECUTABLE}"
    --enable=warning,style,performance,portability
    --std=c++17
    --language=c++
    --inline-suppr
    --suppress=missingIncludeSystem
    --suppress=unmatchedSuppression
  )
  if(PAIRRING_WARNINGS_AS_ERRORS)
    list(APPEND cppcheck_command --error-exitcode=1)
  endif()

  set_target_properties(${target} PROPERTIES
    CXX_CPPCHECK "${cppcheck_command}")
endfunction()
