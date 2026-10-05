#include "app/cmd_config.h"

#include <errno.h>
#include <stdio.h>

#include "core/error.h"
#include "domain/gallery_config.h"

enum ExitCode cmd_config_run(void) {
  struct GalleryConfig gallery_config;
  gallery_config_init(&gallery_config);

  enum ExitCode rc = EXIT_CODE_FAILURE;
  char error_message[ERROR_MESSAGE_SIZE];
  if (gallery_config_load(&gallery_config, GALLERY_CONFIG_PATH_DEFAULT, error_message,
                          sizeof(error_message)) != 0) {
    fprintf(stderr, "%s\n", error_message);
    goto cleanup;
  }
  if (gallery_config_print(stdout, &gallery_config) != 0) {
    // Relay the stream's own reason. A full disk and a closed or invalid descriptor call for
    // different responses, and without this they are indistinguishable. A closed pipe reaches here
    // only when the caller ignored `SIGPIPE`. Under the default disposition, the process dies of
    // signal 13 inside the flush instead. A read of `errno` after a callee returns is valid only
    // under an explicit contract, and there is one. `gallery_config_print` documents that it sets
    // `errno`, and substitutes `EIO` when the stream latched an error whose own `errno` may have
    // been overwritten since.
    fprintf(stderr, "failed to write resolved config to 'stdout': %s\n",
            error_system_message(error_message, sizeof(error_message), errno));
    goto cleanup;
  }
  rc = EXIT_CODE_OK;

cleanup:
  gallery_config_free(&gallery_config);
  return rc;
}
