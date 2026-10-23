# The golden test suite uses several modules. Keep it in the top-level `tests/` directory.

# Unit-test plumbing shared by every unit-test executable. It is an object library rather than a
# static one so that its `os_oom` is always linked in: a static archive member would be pulled only
# by a reference that precedes it on the link line, and the reference comes from the arena in
# `fram_vendor_sharedstuff`, which follows it.
add_library(fram_test_support OBJECT test_support.c)
target_include_directories(fram_test_support PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>")
target_link_libraries(fram_test_support PRIVATE fram_build_tests fram_vendor_acutest fram_app)
# The fixture JPEG encoder and the WebP output decoder call their codecs themselves.
target_sources(fram_test_support PRIVATE test_jpeg_fixture.c test_webp_output.c)
target_link_libraries(fram_test_support PRIVATE fram_vendor_libjpeg_turbo fram_vendor_libwebp)
if(PROJECT_IS_TOP_LEVEL)
  fram_enable_test_analysis(fram_test_support)
endif()
fram_add_unit_test(test_support)

# The in-memory implementation of `src/runtime/os.h`, which a unit test registered with `OS memory`
# links in place of the POSIX one. It is an object library for the same reason as the POSIX one.
add_library(fram_os_memory OBJECT os_memory.c)
target_include_directories(fram_os_memory PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>")
target_link_libraries(fram_os_memory PRIVATE fram_build_tests fram_vendor_acutest fram_app)
if(PROJECT_IS_TOP_LEVEL)
  fram_enable_test_analysis(fram_os_memory)
endif()

# Seeded random tests over the EXIF and JPEG-header parsers.
fram_add_unit_test(random)

find_program(FRAM_FFMPEG ffmpeg)
find_program(FRAM_FFPROBE ffprobe)

function(fram_add_golden_test name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "GALLERY;EXPECTED" "")
  add_test(
    NAME "${name}"
    COMMAND
      "${CMAKE_COMMAND}" "-DFRAM_EXECUTABLE=$<TARGET_FILE:fram>" "-DFRAM_VERSION=${PROJECT_VERSION}"
      "-DFRAM_GALLERY_DIR=${arg_GALLERY}"
      "-DFRAM_EXPECTED_DIR=${arg_EXPECTED}"
      "-DFRAM_SCRATCH_DIR=${PROJECT_BINARY_DIR}/golden/$<CONFIG>/${name}" -P
      "${PROJECT_SOURCE_DIR}/cmake/fram_run_golden.cmake"
  )
  set_tests_properties(
    "${name}"
    PROPERTIES LABELS "fram;golden"
               ENVIRONMENT_MODIFICATION "UBSAN_OPTIONS=string_prepend:print_stacktrace=1:"
  )
endfunction()

# The fixture gallery mixes images and MP4 sources, so its golden test needs FFmpeg.
if(FRAM_FFMPEG AND FRAM_FFPROBE)
  set(gallery_dir "${CMAKE_CURRENT_LIST_DIR}/fixtures/gallery")
  set(expected_dir "${CMAKE_CURRENT_LIST_DIR}/expected/gallery")
  fram_add_golden_test("fram.golden" GALLERY "${gallery_dir}" EXPECTED "${expected_dir}")
else()
  message(STATUS "ffmpeg or ffprobe not found; skipping golden tests")
endif()
