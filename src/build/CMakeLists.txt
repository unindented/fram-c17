target_sources(
  fram_app
  PRIVATE album_scanner.c derivative_renderer.c job.c manifest_builder.c output_path.c
          output_pruner.c output_writer.c page_renderer.c site_writer.c template.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES album_scanner.h derivative_renderer.h job.h manifest_builder.h output_path.h
               output_pruner.h output_writer.h page_renderer.h site_writer.h template.h
)
target_link_libraries(fram_app PRIVATE fram_vendor_mustache)

if(FRAM_BUILD_TESTING)
  foreach(module IN ITEMS output_path template)
    fram_add_unit_test(${module})
  endforeach()
  # The video cases of the scanner and the renderer are executables of their own on the POSIX host:
  # a video's frame is a file on the real host, and the scanner's cases run `ffprobe` and `ffmpeg`
  # fakes.
  foreach(module IN ITEMS album_scanner_video derivative_renderer_video)
    fram_add_unit_test(${module})
  endforeach()
  # A case-insensitive filesystem is a fact of the real host, so the prune's case is on the POSIX
  # one too.
  fram_add_unit_test(output_pruner_case)
  # These read and write only through `runtime/os.h`, so they run against the in-memory host.
  foreach(module IN ITEMS album_scanner derivative_renderer job manifest_builder output_pruner
                          output_writer page_renderer site_writer
  )
    fram_add_unit_test(${module} OS memory)
  endforeach()
endif()
