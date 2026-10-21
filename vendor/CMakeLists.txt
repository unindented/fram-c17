# This file isolates third-party sources from first-party warnings and analyzers.

add_library(fram_vendor_acutest INTERFACE)
target_include_directories(
  fram_vendor_acutest SYSTEM INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/acutest>"
)

add_library(fram_vendor_copt INTERFACE)
target_include_directories(
  fram_vendor_copt SYSTEM INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/copt>"
)

add_library(fram_vendor_mustache STATIC mustache4c/mustache.c)
target_include_directories(
  fram_vendor_mustache SYSTEM PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/mustache4c>"
)

add_library(fram_vendor_stb STATIC stb/stb.c)
target_include_directories(
  fram_vendor_stb SYSTEM PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/stb>"
)
# `stb.c` routes stb's allocator hooks to `image_alloc` in `src/formats/image.c`, so it includes that
# module's header, which includes sharedstuff's `str.h`.
target_include_directories(fram_vendor_stb PRIVATE "${PROJECT_SOURCE_DIR}/src")

add_library(
  fram_vendor_sharedstuff STATIC sharedstuff/src/arena.c sharedstuff/src/buf.c
                                  sharedstuff/src/log.c sharedstuff/src/str.c
)
target_include_directories(
  fram_vendor_sharedstuff SYSTEM
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/sharedstuff/include>"
)
target_link_libraries(fram_vendor_sharedstuff PRIVATE fram_build_project)
target_link_libraries(fram_vendor_stb PRIVATE fram_vendor_sharedstuff)

add_library(fram_vendor_tomlc17 STATIC tomlc17/tomlc17.c)
target_include_directories(
  fram_vendor_tomlc17 SYSTEM PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/tomlc17>"
)

foreach(target IN ITEMS fram_vendor_mustache fram_vendor_stb fram_vendor_tomlc17)
  target_compile_options(${target} PRIVATE $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>)
  target_link_libraries(${target} PRIVATE fram_build_vendor)
endforeach()

# stb_image_write's JPEG bit buffer deliberately shifts through the sign bit. UBSan diagnoses the
# shift even though the result is valid. Keep the exception on this pinned vendor translation unit
# only. All first-party callers remain fully instrumented.
if(FRAM_SANITIZER STREQUAL "address")
  set_property(
    SOURCE stb/stb.c
    APPEND
    PROPERTY COMPILE_OPTIONS $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-fno-sanitize=shift>
  )
endif()
