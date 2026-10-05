# This file configures formatting and static analysis for first-party sources.

include_guard(GLOBAL)

set(fram_owned_sources
    "${PROJECT_SOURCE_DIR}/src/app/cli.c"
    "${PROJECT_SOURCE_DIR}/src/app/cli.h"
    "${PROJECT_SOURCE_DIR}/src/app/cli_dispatch.c"
    "${PROJECT_SOURCE_DIR}/src/app/cli_dispatch.h"
    "${PROJECT_SOURCE_DIR}/src/app/cmd_build.c"
    "${PROJECT_SOURCE_DIR}/src/app/cmd_build.h"
    "${PROJECT_SOURCE_DIR}/src/app/cmd_config.c"
    "${PROJECT_SOURCE_DIR}/src/app/cmd_config.h"
    "${PROJECT_SOURCE_DIR}/src/app/exit_code.h"
    "${PROJECT_SOURCE_DIR}/src/app/main.c"
    "${PROJECT_SOURCE_DIR}/src/app/test_cli.c"
    "${PROJECT_SOURCE_DIR}/src/app/test_cli_dispatch.c"
    "${PROJECT_SOURCE_DIR}/src/app/test_cmd_build.c"
    "${PROJECT_SOURCE_DIR}/src/app/test_cmd_config.c"
    "${PROJECT_SOURCE_DIR}/src/build/album_scanner.c"
    "${PROJECT_SOURCE_DIR}/src/build/album_scanner.h"
    "${PROJECT_SOURCE_DIR}/src/build/derivative_renderer.c"
    "${PROJECT_SOURCE_DIR}/src/build/derivative_renderer.h"
    "${PROJECT_SOURCE_DIR}/src/build/job.c"
    "${PROJECT_SOURCE_DIR}/src/build/job.h"
    "${PROJECT_SOURCE_DIR}/src/build/manifest_builder.c"
    "${PROJECT_SOURCE_DIR}/src/build/manifest_builder.h"
    "${PROJECT_SOURCE_DIR}/src/build/output_path.c"
    "${PROJECT_SOURCE_DIR}/src/build/output_path.h"
    "${PROJECT_SOURCE_DIR}/src/build/page_renderer.c"
    "${PROJECT_SOURCE_DIR}/src/build/page_renderer.h"
    "${PROJECT_SOURCE_DIR}/src/build/site_writer.c"
    "${PROJECT_SOURCE_DIR}/src/build/site_writer.h"
    "${PROJECT_SOURCE_DIR}/src/build/template.c"
    "${PROJECT_SOURCE_DIR}/src/build/template.h"
    "${PROJECT_SOURCE_DIR}/src/build/test_album_scanner.c"
    "${PROJECT_SOURCE_DIR}/src/build/test_derivative_renderer.c"
    "${PROJECT_SOURCE_DIR}/src/build/test_job.c"
    "${PROJECT_SOURCE_DIR}/src/build/test_jpeg.h"
    "${PROJECT_SOURCE_DIR}/src/build/test_manifest_builder.c"
    "${PROJECT_SOURCE_DIR}/src/build/test_output_path.c"
    "${PROJECT_SOURCE_DIR}/src/build/test_page_renderer.c"
    "${PROJECT_SOURCE_DIR}/src/build/test_site_writer.c"
    "${PROJECT_SOURCE_DIR}/src/build/test_template.c"
    "${PROJECT_SOURCE_DIR}/src/core/ascii.h"
    "${PROJECT_SOURCE_DIR}/src/core/error.c"
    "${PROJECT_SOURCE_DIR}/src/core/error.h"
    "${PROJECT_SOURCE_DIR}/src/core/fram_version.h"
    "${PROJECT_SOURCE_DIR}/src/core/grow.c"
    "${PROJECT_SOURCE_DIR}/src/core/grow.h"
    "${PROJECT_SOURCE_DIR}/src/core/parse.c"
    "${PROJECT_SOURCE_DIR}/src/core/parse.h"
    "${PROJECT_SOURCE_DIR}/src/core/path.c"
    "${PROJECT_SOURCE_DIR}/src/core/path.h"
    "${PROJECT_SOURCE_DIR}/src/core/path_list.c"
    "${PROJECT_SOURCE_DIR}/src/core/path_list.h"
    "${PROJECT_SOURCE_DIR}/src/core/test_ascii.c"
    "${PROJECT_SOURCE_DIR}/src/core/test_error.c"
    "${PROJECT_SOURCE_DIR}/src/core/test_grow.c"
    "${PROJECT_SOURCE_DIR}/src/core/test_parse.c"
    "${PROJECT_SOURCE_DIR}/src/core/test_path.c"
    "${PROJECT_SOURCE_DIR}/src/core/test_path_list.c"
    "${PROJECT_SOURCE_DIR}/src/core/test_text.c"
    "${PROJECT_SOURCE_DIR}/src/core/text.c"
    "${PROJECT_SOURCE_DIR}/src/core/text.h"
    "${PROJECT_SOURCE_DIR}/src/domain/album.c"
    "${PROJECT_SOURCE_DIR}/src/domain/album.h"
    "${PROJECT_SOURCE_DIR}/src/domain/gallery_config.c"
    "${PROJECT_SOURCE_DIR}/src/domain/gallery_config.h"
    "${PROJECT_SOURCE_DIR}/src/domain/manifest.c"
    "${PROJECT_SOURCE_DIR}/src/domain/manifest.h"
    "${PROJECT_SOURCE_DIR}/src/domain/media_item.c"
    "${PROJECT_SOURCE_DIR}/src/domain/media_item.h"
    "${PROJECT_SOURCE_DIR}/src/domain/test_album.c"
    "${PROJECT_SOURCE_DIR}/src/domain/test_gallery_config.c"
    "${PROJECT_SOURCE_DIR}/src/domain/test_manifest.c"
    "${PROJECT_SOURCE_DIR}/src/domain/test_media_item.c"
    "${PROJECT_SOURCE_DIR}/src/formats/exif.c"
    "${PROJECT_SOURCE_DIR}/src/formats/exif.h"
    "${PROJECT_SOURCE_DIR}/src/formats/html.c"
    "${PROJECT_SOURCE_DIR}/src/formats/html.h"
    "${PROJECT_SOURCE_DIR}/src/formats/image.c"
    "${PROJECT_SOURCE_DIR}/src/formats/image.h"
    "${PROJECT_SOURCE_DIR}/src/formats/test_exif.c"
    "${PROJECT_SOURCE_DIR}/src/formats/test_html.c"
    "${PROJECT_SOURCE_DIR}/src/formats/test_image.c"
    "${PROJECT_SOURCE_DIR}/src/formats/test_toml.c"
    "${PROJECT_SOURCE_DIR}/src/formats/toml.c"
    "${PROJECT_SOURCE_DIR}/src/formats/toml.h"
    "${PROJECT_SOURCE_DIR}/src/runtime/fs.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/fs.h"
    "${PROJECT_SOURCE_DIR}/src/runtime/pool.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/pool.h"
    "${PROJECT_SOURCE_DIR}/src/runtime/proc.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/proc.h"
    "${PROJECT_SOURCE_DIR}/src/runtime/test_fs.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/test_pool.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/test_proc.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/test_video.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/video.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/video.h"
    "${PROJECT_SOURCE_DIR}/tests/test_support.c"
    "${PROJECT_SOURCE_DIR}/tests/test_support.h"
    "${PROJECT_SOURCE_DIR}/tests/test_test_support.c"
)
set(fram_configured_c_source "${PROJECT_SOURCE_DIR}/src/core/fram_version.c.in")

find_program(FRAM_CLANG_FORMAT NAMES clang-format-22 clang-format)
find_program(FRAM_CLANG_TIDY NAMES clang-tidy-22 clang-tidy)
find_program(FRAM_CPPCHECK NAMES cppcheck)

set(fram_clang_tidy_command "${FRAM_CLANG_TIDY}")
set(fram_cppcheck_command "${FRAM_CPPCHECK}")
set(fram_cppcheck_standard "c17")
if(CMAKE_CROSSCOMPILING)
  message(VERBOSE "cross compiling; clang-tidy and cppcheck are disabled")
  set(fram_clang_tidy_command "")
  set(fram_cppcheck_command "")
endif()

# Production and test targets run cppcheck with the same arguments.
set(fram_cppcheck_property
    "${fram_cppcheck_command};--enable=warning,performance,portability;--std=${fram_cppcheck_standard};--error-exitcode=1;--quiet"
)

function(fram_enable_project_analysis target)
  if(fram_clang_tidy_command)
    set_property(
      TARGET ${target}
      PROPERTY C_CLANG_TIDY
               "${fram_clang_tidy_command};--quiet;--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy"
    )
  endif()
  if(fram_cppcheck_command)
    set_property(TARGET ${target} PROPERTY C_CPPCHECK "${fram_cppcheck_property}")
  endif()
endfunction()

function(fram_enable_test_analysis target)
  if(fram_cppcheck_command)
    set_property(TARGET ${target} PROPERTY C_CPPCHECK "${fram_cppcheck_property}")
  endif()
endfunction()

if(FRAM_CLANG_FORMAT)
  add_custom_target(
    fram_format
    COMMAND "${FRAM_CLANG_FORMAT}" -i ${fram_owned_sources}
    COMMAND
      "${FRAM_CLANG_FORMAT}" -i --assume-filename=fram_version.c
      "${fram_configured_c_source}"
    COMMENT "Formatting first-party sources"
    COMMAND_EXPAND_LISTS VERBATIM
  )
  add_custom_target(
    fram_lint
    COMMAND "${FRAM_CLANG_FORMAT}" --dry-run --Werror ${fram_owned_sources}
    COMMAND
      "${FRAM_CLANG_FORMAT}" --dry-run --Werror --assume-filename=fram_version.c
      "${fram_configured_c_source}"
    COMMENT "Checking first-party source formatting"
    COMMAND_EXPAND_LISTS VERBATIM
  )
else()
  add_custom_target(fram_format COMMAND "${CMAKE_COMMAND}" -E echo "clang-format not found; skipping")
  add_custom_target(fram_lint COMMAND "${CMAKE_COMMAND}" -E echo "clang-format not found; skipping")
endif()
