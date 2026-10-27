// `mkdtemp`, `open` and `access` are POSIX rather than C17, so this file asks for them.
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "build/output_pruner.h"
#include "core/path.h"
#include "domain/gallery_config.h"
#include "domain/manifest.h"
#include "runtime/fs.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

/**
 * @brief Reports whether `/tmp` is on a case-insensitive filesystem.
 *
 * It creates `Probe` in a directory of its own and looks for `probe`. Acutest refuses to skip a
 * test that has already checked something, so this runs before any check and makes none itself. A
 * probe it cannot create reads as case-sensitive, so the test skips rather than fails.
 *
 * @return `true` when `probe` names the file created as `Probe`, otherwise `false`.
 */
static bool is_tmp_case_insensitive(void) {
  char probe_dir[] = "/tmp/fram-pruner-case-probe.XXXXXX";
  if (mkdtemp(probe_dir) == NULL) {
    return false;
  }
  char created_path[PATH_MAX];
  char folded_path[PATH_MAX];
  (void)snprintf(created_path, sizeof(created_path), "%s/Probe", probe_dir);
  (void)snprintf(folded_path, sizeof(folded_path), "%s/probe", probe_dir);
  bool is_case_insensitive = false;
  const int fd = open(created_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd >= 0) {
    (void)close(fd);
    is_case_insensitive = access(folded_path, F_OK) == 0;
    (void)unlink(created_path);
  }
  (void)rmdir(probe_dir);
  return is_case_insensitive;
}

// A case-insensitive filesystem keeps the case of the name a file was created with, so an output
// whose name changed only in case is on disk under its old name. The prune compares identities, so
// it keeps that file. Only a real case-insensitive filesystem can show this, so the test skips on a
// case-sensitive one.
static void test_keeps_output_named_in_another_case(void) {
  if (!is_tmp_case_insensitive()) {
    TEST_SKIP("needs a case-insensitive filesystem");
    return;
  }
  char root_dir[] = "/tmp/fram-pruner-case.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  struct Arena arena = test_arena();
  const struct Str output_dir = path_join(str_from_cstr(root_dir), S("public"), &arena);
  const struct Str written_path = path_join(output_dir, S("Logo.png"), &arena);
  struct Log log = test_log();
  TEST_CHECK(os_write(os_posix(), written_path, (struct OsSource){.bytes = S("logo")},
                      S("failed to write fixture"), &log) == 0);
  const struct Str output_path = path_join(output_dir, S("logo.png"), &arena);
  TEST_CHECK(access(str_cstr(output_path, &arena), F_OK) == 0);

  struct GalleryConfig gallery_config;
  gallery_config_init(&gallery_config);
  gallery_config.input_dir = path_join(str_from_cstr(root_dir), S("photos"), &arena);
  gallery_config.output_dir = output_dir;
  gallery_config.templates_dir = path_join(str_from_cstr(root_dir), S("templates"), &arena);
  gallery_config.static_dir = path_join(str_from_cstr(root_dir), S("static"), &arena);
  struct Manifest manifest = {0};
  TEST_CHECK(manifest_add(&manifest, output_path, S("static/logo.png"), &arena, NULL) == 0);

  ptrdiff_t removed_count = -1;
  TEST_CHECK(output_pruner_prune(&manifest, &gallery_config, os_posix(), test_arena(),
                                 &removed_count, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(removed_count == 0);
  TEST_CHECK(access(str_cstr(written_path, &arena), F_OK) == 0);
  remove_fixture_tree(root_dir);
}

TEST_LIST = {
    {"keeps output named in another case", test_keeps_output_named_in_another_case},
    {NULL, NULL},
};
