# This file configures `zig cc` for AArch64 Linux with `musl`.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(ZIG_TARGET aarch64-linux-musl)

include("${CMAKE_CURRENT_LIST_DIR}/zig.cmake")
