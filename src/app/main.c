#include "app/cli_dispatch.h"

/**
 * @brief Process entry point. Delegates to `cli_dispatch`.
 *
 * It only calls `cli_dispatch`, so every decision stays in a file the unit tests can link.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return The exit code `cli_dispatch` resolved.
 */
int main(int argc, char** argv) {
  // The cast is required, not cosmetic. The underlying type of `enum ExitCode` is implementation
  // defined and may be unsigned, so returning one uncast trips `-Wsign-conversion` under `-Werror`.
  // That looks like an inconsistency to tidy away, and tidying it breaks the build.
  return (int)cli_dispatch(argc, argv);
}
