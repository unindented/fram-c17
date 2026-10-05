# These targets provide shared compile and link requirements.

include_guard(GLOBAL)

include(CheckCCompilerFlag)

add_library(fram_build_project INTERFACE)
add_library(fram_build_tests INTERFACE)
add_library(fram_build_vendor INTERFACE)

foreach(role IN ITEMS fram_build_project fram_build_tests fram_build_vendor)
  # Require C17 or later. A parent project cannot reduce this requirement.
  target_compile_features(${role} INTERFACE c_std_17)

  target_compile_options(
    ${role}
    INTERFACE
      $<$<AND:$<C_COMPILER_ID:GNU,Clang,AppleClang>,$<CONFIG:Debug>>:-Og>
      "$<$<AND:$<C_COMPILER_ID:GNU,Clang,AppleClang>,$<CONFIG:Release,RelWithDebInfo,MinSizeRel>>:SHELL:-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3>"
  )
endforeach()

foreach(role IN ITEMS fram_build_project fram_build_tests)
  target_compile_options(
    ${role}
    INTERFACE
      $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic;-Wformat=2;-Wvla>
      $<$<C_COMPILER_ID:GNU>:-Wlogical-op;-Wduplicated-cond;-Wshift-overflow=2>
      $<$<C_COMPILER_ID:Clang,AppleClang>:-Wconditional-uninitialized;-Wassign-enum;-Wcomma>
  )
endforeach()

# GCC and Clang support these warnings in different versions. Check each warning before use so all
# supported compilers can build the project.
foreach(flag IN ITEMS -Wformat-signedness -Wjump-misses-init)
  string(MAKE_C_IDENTIFIER "fram_have_warning${flag}" cache_var)
  check_c_compiler_flag("${flag}" "${cache_var}")
  if(${cache_var})
    target_compile_options(fram_build_project INTERFACE "${flag}")
    target_compile_options(fram_build_tests INTERFACE "${flag}")
  endif()
endforeach()

target_compile_options(
  fram_build_project
  INTERFACE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wconversion;-Wsign-conversion;-Wstrict-prototypes;-Wmissing-prototypes;-Wshadow;-Wundef;-Wcast-qual;-Wwrite-strings>
)

set(FRAM_SANITIZER
    "none"
    CACHE STRING "Sanitizer to instrument the build with"
)
set_property(CACHE FRAM_SANITIZER PROPERTY STRINGS none address thread)

if(FRAM_SANITIZER STREQUAL "none")
  set(fram_sanitizer_flags "")
elseif(FRAM_SANITIZER STREQUAL "address")
  set(fram_sanitizer_flags -fsanitize=address,undefined -fno-sanitize-recover=all
                            -fno-omit-frame-pointer
  )
elseif(FRAM_SANITIZER STREQUAL "thread")
  set(fram_sanitizer_flags -fsanitize=thread -fno-sanitize-recover=all -fno-omit-frame-pointer)
else()
  message(FATAL_ERROR "FRAM_SANITIZER must be one of none, address, thread; got '${FRAM_SANITIZER}'")
endif()

foreach(role IN ITEMS fram_build_project fram_build_tests)
  target_compile_options(${role} INTERFACE ${fram_sanitizer_flags})
  target_link_options(${role} INTERFACE ${fram_sanitizer_flags})
endforeach()

# Apply sanitizer instrumentation to vendored code. Disable the two UBSan checks that the vendored
# code does not support. Static vendored libraries do not need sanitizer link options.
target_compile_options(fram_build_vendor INTERFACE ${fram_sanitizer_flags})
if(FRAM_SANITIZER STREQUAL "address")
  target_compile_options(fram_build_vendor INTERFACE -fno-sanitize=null,object-size)
endif()
