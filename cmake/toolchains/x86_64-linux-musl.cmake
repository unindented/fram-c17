# This file configures `zig cc` for x86_64 Linux with `musl`.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(ZIG_TARGET x86_64-linux-musl)

include("${CMAKE_CURRENT_LIST_DIR}/zig.cmake")
