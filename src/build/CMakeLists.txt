target_sources(
  fram_app
  PRIVATE album_scanner.c derivative_renderer.c job.c manifest_builder.c output_path.c
          page_renderer.c site_writer.c template.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES album_scanner.h derivative_renderer.h job.h manifest_builder.h output_path.h
               page_renderer.h site_writer.h template.h
)
target_link_libraries(fram_app PRIVATE fram_vendor_mustache)

if(FRAM_BUILD_TESTING)
  foreach(module IN ITEMS album_scanner derivative_renderer job manifest_builder output_path
                          page_renderer site_writer template
  )
    fram_add_unit_test(${module})
  endforeach()
endif()
