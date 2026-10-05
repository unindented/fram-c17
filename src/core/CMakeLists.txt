target_sources(
  fram_app
  PRIVATE error.c grow.c parse.c path.c path_list.c text.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES ascii.h error.h fram_version.h grow.h parse.h path.h path_list.h text.h
)

if(FRAM_BUILD_TESTING)
  foreach(module IN ITEMS ascii error grow parse path path_list text)
    fram_add_unit_test(${module})
  endforeach()
endif()
