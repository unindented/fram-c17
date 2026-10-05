target_sources(
  fram_app
  PRIVATE fs.c pool.c proc.c video.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES fs.h pool.h proc.h video.h
)

if(FRAM_BUILD_TESTING)
  foreach(module IN ITEMS fs pool proc video)
    fram_add_unit_test(${module})
  endforeach()
endif()
