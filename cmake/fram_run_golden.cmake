# This script builds one copied fixture gallery twice. It checks that the second build leaves the
# generated output unchanged, then compares the result with the expected output. The release
# workflow runs it against a packaged binary, with `FRAM_EMULATOR` naming a launcher such as
# `qemu-aarch64` when the binary targets another architecture.

foreach(required IN ITEMS FRAM_EXECUTABLE FRAM_VERSION FRAM_GALLERY_DIR FRAM_EXPECTED_DIR
                        FRAM_SCRATCH_DIR
)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

file(REMOVE_RECURSE "${FRAM_SCRATCH_DIR}")
file(MAKE_DIRECTORY "${FRAM_SCRATCH_DIR}")
file(COPY "${FRAM_GALLERY_DIR}/" DESTINATION "${FRAM_SCRATCH_DIR}")

set(actual_dir "${FRAM_SCRATCH_DIR}/public")
set(first_dir "${FRAM_SCRATCH_DIR}/first/public")

execute_process(
  COMMAND ${FRAM_EMULATOR} "${FRAM_EXECUTABLE}" build --workers 2 ${FRAM_BUILD_ARGS}
  WORKING_DIRECTORY "${FRAM_SCRATCH_DIR}"
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr
)
if(NOT build_result EQUAL 0)
  message(
    FATAL_ERROR "first fixture gallery build failed with exit status ${build_result}:\n"
                "${build_stdout}${build_stderr}"
  )
endif()
file(COPY "${actual_dir}" DESTINATION "${FRAM_SCRATCH_DIR}/first")

execute_process(
  COMMAND ${FRAM_EMULATOR} "${FRAM_EXECUTABLE}" build --workers 2 ${FRAM_BUILD_ARGS}
  WORKING_DIRECTORY "${FRAM_SCRATCH_DIR}"
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr
)
if(NOT build_result EQUAL 0)
  message(
    FATAL_ERROR "second fixture gallery build failed with exit status ${build_result}:\n"
                "${build_stdout}${build_stderr}"
  )
endif()

file(GLOB_RECURSE first_files RELATIVE "${first_dir}" "${first_dir}/*")
foreach(file IN LISTS first_files)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${first_dir}/${file}" "${actual_dir}/${file}"
    RESULT_VARIABLE compare_result
    OUTPUT_QUIET ERROR_QUIET
  )
  if(NOT compare_result EQUAL 0)
    message(FATAL_ERROR "second build changed generated output: '${file}'")
  endif()
endforeach()

# Collect both file lists after `fram` runs. Runtime lists are not build inputs. Fixture gallery
# changes do not require CMake to run again.
file(GLOB_RECURSE expected_files RELATIVE "${FRAM_EXPECTED_DIR}" "${FRAM_EXPECTED_DIR}/*")
file(GLOB_RECURSE actual_files RELATIVE "${actual_dir}" "${actual_dir}/*")
list(SORT expected_files)
list(SORT actual_files)

set(missing_files ${expected_files})
if(actual_files)
  list(REMOVE_ITEM missing_files ${actual_files})
endif()
if(missing_files)
  list(JOIN missing_files "', '" missing_files_display)
  message(FATAL_ERROR "expected output not produced: '${missing_files_display}'")
endif()

set(extra_files ${actual_files})
if(expected_files)
  list(REMOVE_ITEM extra_files ${expected_files})
endif()
if(extra_files)
  list(JOIN extra_files "', '" extra_files_display)
  message(FATAL_ERROR "output not expected: '${extra_files_display}'")
endif()

foreach(file IN LISTS expected_files)
  # An original is a byte copy of its source, so it is compared exactly like every non-media file.
  if(NOT file MATCHES "^_fram/originals/" AND file MATCHES "\\.jpg$")
    # Generated JPEG bytes vary across stb versions and SIMD paths. The list check above pins the
    # path. Unit tests pin dimensions and pixels, and this check rules out an empty write.
    file(SIZE "${actual_dir}/${file}" actual_len)
    if(actual_len EQUAL 0)
      message(FATAL_ERROR "generated media is empty: '${file}'")
    endif()
  elseif(file MATCHES "\\.html$")
    # Pages embed the project version, so the expected pages hold a placeholder that a release
    # does not have to update.
    file(READ "${FRAM_EXPECTED_DIR}/${file}" expected_content)
    string(REPLACE "@PROJECT_VERSION@" "${FRAM_VERSION}" expected_content "${expected_content}")
    file(READ "${actual_dir}/${file}" actual_content)
    if(NOT actual_content STREQUAL expected_content)
      message(FATAL_ERROR "output differs from expected: '${file}'")
    endif()
  else()
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E compare_files "${FRAM_EXPECTED_DIR}/${file}"
              "${actual_dir}/${file}"
      RESULT_VARIABLE compare_result
      OUTPUT_QUIET ERROR_QUIET
    )
    if(NOT compare_result EQUAL 0)
      message(FATAL_ERROR "output differs from expected: '${file}'")
    endif()
  endif()
endforeach()
