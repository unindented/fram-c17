cmake_minimum_required(VERSION 3.25...4.0)

# Set this target before `project()` because compiler checks use it. Builders can raise the value.
set(CMAKE_OSX_DEPLOYMENT_TARGET
    "13.0"
    CACHE STRING "Minimum macOS version the build targets"
)

project(
  fram
  VERSION 0.2.0
  DESCRIPTION "A small static photo and video gallery generator written in C17"
  HOMEPAGE_URL "https://github.com/unindented/fram-c17"
  LANGUAGES C
)

if(PROJECT_IS_TOP_LEVEL AND "${PROJECT_SOURCE_DIR}" STREQUAL "${PROJECT_BINARY_DIR}")
  message(FATAL_ERROR "in-source builds are unsupported; use a preset or '-B build/<name>'")
endif()

if(NOT DEFINED FRAM_BUILD_TESTING)
  set(FRAM_BUILD_TESTING "${PROJECT_IS_TOP_LEVEL}")
endif()

if(FRAM_BUILD_TESTING)
  if(PROJECT_IS_TOP_LEVEL)
    enable_testing()
  else()
    message(STATUS "fram tests are enabled as a child; the parent must call 'enable_testing()' at its top level")
  endif()
endif()

if(NOT DEFINED CMAKE_C_STANDARD)
  set(CMAKE_C_STANDARD 17)
endif()
if(NOT DEFINED CMAKE_C_STANDARD_REQUIRED)
  set(CMAKE_C_STANDARD_REQUIRED TRUE)
endif()
if(NOT DEFINED CMAKE_C_EXTENSIONS)
  set(CMAKE_C_EXTENSIONS FALSE)
endif()

set(CMAKE_LINK_LIBRARIES_ONLY_TARGETS TRUE)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

include(fram_build_profiles)

if(PROJECT_IS_TOP_LEVEL)
  include(fram_quality)
endif()
if(FRAM_BUILD_TESTING)
  include(fram_testing)
endif()

add_subdirectory(vendor)
add_subdirectory(src)

if(FRAM_BUILD_TESTING)
  add_subdirectory(tests)
endif()

if(PROJECT_IS_TOP_LEVEL)
  include(fram_packaging)
endif()
