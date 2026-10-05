target_sources(
  fram_app
  PRIVATE album.c gallery_config.c manifest.c media_item.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES album.h gallery_config.h manifest.h media_item.h
)

if(FRAM_BUILD_TESTING)
  foreach(module IN ITEMS album gallery_config manifest media_item)
    fram_add_unit_test(${module})
  endforeach()
endif()
