target_sources(
  fram_app
  PRIVATE cli.c cli_dispatch.c cmd_build.c cmd_config.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES cli.h cli_dispatch.h cmd_build.h cmd_config.h exit_code.h
)

if(FRAM_BUILD_TESTING)
  foreach(module IN ITEMS cli cli_dispatch cmd_config)
    fram_add_unit_test(${module})
  endforeach()
  # The builds of galleries with videos run `ffprobe` and `ffmpeg` fakes, and a video's frame is a
  # file on the real host, so these run on the POSIX one.
  fram_add_unit_test(cmd_build_video)
  # A build reads, walks and writes only through `runtime/os.h`, so its tests use the in-memory
  # host.
  fram_add_unit_test(cmd_build OS memory)
endif()
