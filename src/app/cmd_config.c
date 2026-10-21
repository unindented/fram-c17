#include "app/cmd_config.h"

#include <stddef.h>

#include "domain/gallery_config.h"
#include "runtime/memory.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"

/**
 * Bytes `cmd_config_run` reserves. One third of the block holds the loaded values, one the file
 * bytes, and one the log. A config is capped at 1 MiB, so 16 MiB is plenty.
 */
enum { CMD_CONFIG_RESERVE_LEN = 16 * 1024 * 1024 };

enum ExitCode cmd_config_run(struct Os* os) {
  struct Arena block;
  const int shift = arena_reserve_halving(CMD_CONFIG_RESERVE_LEN, &block);
  struct Arena scratch = block;
  struct Arena perm = arena_split(&scratch, (CMD_CONFIG_RESERVE_LEN / 3) >> shift);
  struct Arena log_arena = arena_split(&scratch, (CMD_CONFIG_RESERVE_LEN / 3) >> shift);
  struct Log log = {.buf = {.arena = &log_arena}};
  memory_oom_set_log(&log);
  struct GalleryConfig gallery_config;
  gallery_config_init(&gallery_config);

  enum ExitCode rc = EXIT_CODE_FAILURE;
  // A load that read the file still reports every bad key, and none of them may be printed as the
  // resolved config.
  if (gallery_config_load(&gallery_config, GALLERY_CONFIG_PATH_DEFAULT, os, &perm, scratch, &log) !=
          0 ||
      log.errors > 0) {
    (void)os_print(os, 2, buf_str(&log.buf));
    goto cleanup;
  }
  if (gallery_config_print(&gallery_config, os, scratch) != 0) {
    goto cleanup;
  }
  rc = EXIT_CODE_OK;

cleanup:
  memory_oom_set_log(NULL);
  arena_release(block);
  return rc;
}
