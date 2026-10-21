target_sources(
  fram_app
  PRIVATE memory.c pool.c proc.c video.c walk.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES memory.h os.h pool.h proc.h video.h walk.h
)

# The POSIX implementation of `os.h` stays out of `fram_app`, so each executable chooses its host
# when it is linked: the `fram` executable and every unit test registered without `OS memory` link
# this one, and the tests registered with it link the in-memory one from `tests/`. It is an object
# library because `fram_app` refers to it, and a static archive listed before its user would be
# skipped by a single-pass linker.
add_library(fram_os_posix OBJECT fs.c)
target_sources(
  fram_os_posix PUBLIC FILE_SET HEADERS BASE_DIRS "${PROJECT_SOURCE_DIR}/src" FILES fs.h
)
target_link_libraries(fram_os_posix PRIVATE fram_build_project fram_app)
if(PROJECT_IS_TOP_LEVEL)
  fram_enable_project_analysis(fram_os_posix)
endif()

if(FRAM_BUILD_TESTING)
  # `fs` is the POSIX host itself, and processes and video frames always run on the real host.
  # `pool` reaches no host.
  foreach(module IN ITEMS fs pool proc video)
    fram_add_unit_test(${module})
  endforeach()
  fram_add_unit_test(memory OS memory)
  fram_add_unit_test(walk OS memory)
  # The contract tests of `os.h`: one source, run against each host.
  fram_add_unit_test(os NAME os_posix)
  fram_add_unit_test(os NAME os_memory OS memory)
endif()
