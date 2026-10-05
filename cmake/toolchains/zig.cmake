# This file configures `zig cc` for the Linux cross toolchains.

if(NOT ZIG_TARGET)
  message(FATAL_ERROR "ZIG_TARGET must be set before including this file")
endif()

find_program(ZIG_EXECUTABLE NAMES zig REQUIRED)
# Require `llvm-strip`. The host `strip` tool cannot read Zig's LLVM ELF files.
find_program(CMAKE_STRIP NAMES llvm-strip REQUIRED)

set(CMAKE_C_COMPILER "${ZIG_EXECUTABLE}" cc)
set(CMAKE_C_COMPILER_TARGET "${ZIG_TARGET}")

set(CMAKE_C_ARCHIVE_CREATE "\"${ZIG_EXECUTABLE}\" ar qc <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_APPEND "\"${ZIG_EXECUTABLE}\" ar q <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_FINISH "\"${ZIG_EXECUTABLE}\" ranlib <TARGET>")

# Zig 0.16 cannot write the linker dependency file that CMake requests.
set(CMAKE_C_LINKER_DEPFILE_SUPPORTED FALSE)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
