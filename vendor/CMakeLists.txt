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

add_library(
  fram_vendor_sharedstuff STATIC sharedstuff/src/arena.c sharedstuff/src/string_buffer.c
)
target_include_directories(
  fram_vendor_sharedstuff SYSTEM
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/sharedstuff/include>"
)
target_link_libraries(fram_vendor_sharedstuff PRIVATE fram_build_project)

add_library(fram_vendor_tomlc17 STATIC tomlc17/tomlc17.c)
target_include_directories(
  fram_vendor_tomlc17 SYSTEM PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/tomlc17>"
)

foreach(target IN ITEMS fram_vendor_mustache fram_vendor_stb fram_vendor_tomlc17)
  target_compile_options(${target} PRIVATE $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>)
  target_link_libraries(${target} PRIVATE fram_build_vendor)
endforeach()

# stb_image_write's JPEG bit buffer deliberately shifts through the sign bit. stb_image_resize2's
# SIMD sRGB lookup deliberately biases a table pointer before its array and adds the exponent back
# before dereferencing it. UBSan diagnoses both implementation techniques even though the final
# accesses are valid. Keep the exceptions on this pinned vendor translation unit only. All
# first-party callers remain fully instrumented.
if(FRAM_SANITIZER STREQUAL "address")
  set_property(
    SOURCE stb/stb.c
    APPEND
    PROPERTY COMPILE_OPTIONS $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-fno-sanitize=shift,bounds>
  )
endif()
