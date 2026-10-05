# This helper registers unit tests next to their source modules.

include_guard(GLOBAL)

function(fram_add_unit_test module)
  set(target "fram_unit_${module}")
  add_executable(${target} "test_${module}.c")
  target_link_libraries(
    ${target} PRIVATE fram_build_tests fram_vendor_acutest fram_test_support fram_app
  )

  add_test(NAME "fram.${module}" COMMAND ${target} WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(
    "fram.${module}"
    PROPERTIES LABELS "fram;unit"
               ENVIRONMENT_MODIFICATION "UBSAN_OPTIONS=string_prepend:print_stacktrace=1:"
  )

  if(PROJECT_IS_TOP_LEVEL)
    fram_enable_test_analysis(${target})
  endif()
endfunction()
