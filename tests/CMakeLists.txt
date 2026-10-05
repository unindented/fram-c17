# The golden test suite uses several modules. Keep it in the top-level `tests/` directory.

# Unit-test plumbing shared by the per-module test executables in `src/`.
add_library(fram_test_support STATIC test_support.c)
target_include_directories(fram_test_support PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>")
target_link_libraries(fram_test_support PRIVATE fram_build_tests fram_vendor_acutest fram_app)
if(PROJECT_IS_TOP_LEVEL)
  fram_enable_test_analysis(fram_test_support)
endif()
fram_add_unit_test(test_support)

find_program(FRAM_FFMPEG ffmpeg)
find_program(FRAM_FFPROBE ffprobe)

function(fram_add_golden_test name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "GALLERY;EXPECTED" "BUILD_ARGS")
  add_test(
    NAME "${name}"
    COMMAND
      "${CMAKE_COMMAND}" "-DFRAM_EXECUTABLE=$<TARGET_FILE:fram>" "-DFRAM_VERSION=${PROJECT_VERSION}"
      "-DFRAM_GALLERY_DIR=${arg_GALLERY}"
      "-DFRAM_EXPECTED_DIR=${arg_EXPECTED}"
      "-DFRAM_SCRATCH_DIR=${PROJECT_BINARY_DIR}/golden/$<CONFIG>/${name}"
      "-DFRAM_BUILD_ARGS=${arg_BUILD_ARGS}" -P
      "${PROJECT_SOURCE_DIR}/cmake/fram_run_golden.cmake"
  )
  set_tests_properties(
    "${name}"
    PROPERTIES LABELS "fram;golden"
               ENVIRONMENT_MODIFICATION "UBSAN_OPTIONS=string_prepend:print_stacktrace=1:"
  )
endfunction()

# The fixture gallery mixes images and MP4 sources, so its golden tests need FFmpeg.
if(FRAM_FFMPEG AND FRAM_FFPROBE)
  set(gallery_dir "${CMAKE_CURRENT_LIST_DIR}/fixtures/gallery")
  set(expected_dir "${CMAKE_CURRENT_LIST_DIR}/expected/gallery")
  fram_add_golden_test("fram.golden" GALLERY "${gallery_dir}" EXPECTED "${expected_dir}")
  fram_add_golden_test(
    "fram.golden.verbose"
    GALLERY "${gallery_dir}"
    EXPECTED "${expected_dir}"
    BUILD_ARGS -v
  )
else()
  message(STATUS "ffmpeg or ffprobe not found; skipping golden tests")
endif()
