# The private application library contains project code except `app/main.c` and the POSIX host in
# `runtime/fs.c`. The `fram` executable is built from `app/main.c` and links the POSIX host. Each
# unit test links the same library, the host it is registered with, and the unit-test `os_oom` from
# `tests/`.

configure_file(
  core/fram_version.c.in "${CMAKE_CURRENT_BINARY_DIR}/core/fram_version.c" @ONLY
)

add_library(fram_app STATIC "${CMAKE_CURRENT_BINARY_DIR}/core/fram_version.c")

add_subdirectory(app)
add_subdirectory(build)
add_subdirectory(core)
add_subdirectory(domain)
add_subdirectory(formats)
add_subdirectory(runtime)

target_link_libraries(
  fram_app
  PUBLIC fram_vendor_tomlc17
         fram_vendor_sharedstuff
  PRIVATE fram_build_project
          fram_vendor_copt
          Threads::Threads
)

add_executable(fram app/main.c)
target_link_libraries(fram PRIVATE fram_build_project fram_app fram_os_posix)

if(PROJECT_IS_TOP_LEVEL)
  fram_enable_project_analysis(fram_app)
  fram_enable_project_analysis(fram)
endif()
