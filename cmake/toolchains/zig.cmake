# This file configures `zig cc` for the Linux cross toolchains.

if(NOT ZIG_TARGET)
  message(FATAL_ERROR "ZIG_TARGET must be set before including this file")
endif()

find_program(ZIG_EXECUTABLE NAMES zig REQUIRED)
# Require Zig 0.17. Zig 0.16 crashes when it writes the linker dependency file that CMake requests.
execute_process(
  COMMAND "${ZIG_EXECUTABLE}" version
  OUTPUT_VARIABLE ZIG_VERSION
  OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY)
if(ZIG_VERSION VERSION_LESS 0.17)
  message(FATAL_ERROR "Zig 0.17 or newer is required; got '${ZIG_VERSION}'")
endif()

# Require `llvm-strip`. The host `strip` tool cannot read Zig's LLVM ELF files.
find_program(CMAKE_STRIP NAMES llvm-strip REQUIRED)

set(CMAKE_C_COMPILER "${ZIG_EXECUTABLE}" cc)
set(CMAKE_C_COMPILER_TARGET "${ZIG_TARGET}")

set(CMAKE_C_ARCHIVE_CREATE "\"${ZIG_EXECUTABLE}\" ar qc <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_APPEND "\"${ZIG_EXECUTABLE}\" ar q <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_FINISH "\"${ZIG_EXECUTABLE}\" ranlib <TARGET>")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
