#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "runtime/video.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/ascii.h"
#include "core/error.h"
#include "runtime/fs.h"
#include "runtime/proc.h"

/** Parses the decimal seconds emitted by `ffprobe` into milliseconds. */
static int parse_duration_ms(const char* text,
                             size_t* duration_ms_out,
                             char* reason,
                             size_t reason_len) __attribute__((nonnull(1, 2)));

/** Allocates the secure-frame `mkstemp` template in the configured temporary directory. */
static char* make_frame_template(char* reason, size_t reason_len);

int video_probe_duration(const char* file_path,
                         size_t* duration_ms_out,
                         char* reason,
                         size_t reason_len) {
  const char* const argv[] = {
      "ffprobe",
      "-v",
      "error",
      "-show_entries",
      "format=duration",
      "-of",
      "default=noprint_wrappers=1:nokey=1",
      "--",
      file_path,
      NULL,
  };
  char output[256];
  if (proc_run(argv, output, sizeof(output), reason, reason_len) != 0) {
    return -1;
  }
  return parse_duration_ms(output, duration_ms_out, reason, reason_len);
}

int video_extract_frame(const char* file_path,
                        size_t position_ms,
                        char** jpeg_path_out,
                        char* reason,
                        size_t reason_len) {
  char* frame_path = make_frame_template(reason, reason_len);
  if (frame_path == NULL) {
    return -1;
  }
  const int fd = mkstemp(frame_path);
  if (fd < 0) {
    char message[FS_REASON_SIZE];
    const int rc = error_report(reason, reason_len, "cannot create temporary video frame: %s",
                                error_system_message(message, sizeof(message), errno));
    free(frame_path);
    return rc;
  }
  if (close(fd) != 0) {
    char message[FS_REASON_SIZE];
    const int rc = error_report(reason, reason_len, "cannot close temporary video frame: %s",
                                error_system_message(message, sizeof(message), errno));
    (void)unlink(frame_path);
    free(frame_path);
    return rc;
  }

  char position[64];
  const int position_len =
      snprintf(position, sizeof(position), "%zu.%03zu", position_ms / 1000, position_ms % 1000);
  if (position_len < 0 || (size_t)position_len >= sizeof(position)) {
    (void)unlink(frame_path);
    free(frame_path);
    return error_report(reason, reason_len, "video frame position cannot be represented");
  }

  // FFmpeg applies display-matrix rotation by default. The extracted JPEG is therefore upright, and
  // the image path can probe its real dimensions without parsing version-sensitive side data.
  const char* const argv[] = {
      "ffmpeg", "-v", "error",  "-nostdin", "-ss",   position, "-i",       file_path, "-frames:v",
      "1",      "-f", "image2", "-vcodec",  "mjpeg", "-y",     frame_path, NULL,
  };
  if (proc_run(argv, NULL, 0, reason, reason_len) != 0) {
    (void)unlink(frame_path);
    free(frame_path);
    return -1;
  }
  struct FsMeta meta;
  if (fs_stat_meta(frame_path, &meta, reason, reason_len) != 0) {
    (void)unlink(frame_path);
    free(frame_path);
    return -1;
  }
  if (meta.size_len == 0) {
    (void)error_report(reason, reason_len, "FFmpeg produced an empty video frame");
    (void)unlink(frame_path);
    free(frame_path);
    return -1;
  }
  *jpeg_path_out = frame_path;
  return 0;
}

static int parse_duration_ms(const char* text,
                             size_t* duration_ms_out,
                             char* reason,
                             size_t reason_len) {
  const unsigned char* p = (const unsigned char*)text;
  while (ascii_is_space(*p)) {
    p++;
  }
  if (p[0] == 'N' && p[1] == '/' && p[2] == 'A') {
    p += 3;
    while (ascii_is_space(*p)) {
      p++;
    }
    if (*p == '\0') {
      *duration_ms_out = 0;
      return 0;
    }
  }
  if (!ascii_is_digit(*p)) {
    return error_report(reason, reason_len, "ffprobe returned an invalid duration");
  }
  size_t seconds = 0;
  while (ascii_is_digit(*p)) {
    const size_t digit = (size_t)(*p - '0');
    if (seconds > (SIZE_MAX - digit) / 10) {
      return error_report(reason, reason_len, "video duration is too large");
    }
    seconds = seconds * 10 + digit;
    p++;
  }

  size_t milliseconds = 0;
  size_t fractional_digits = 0;
  if (*p == '.') {
    p++;
    if (!ascii_is_digit(*p)) {
      return error_report(reason, reason_len, "ffprobe returned an invalid duration");
    }
    while (ascii_is_digit(*p)) {
      if (fractional_digits < 3) {
        milliseconds = milliseconds * 10 + (size_t)(*p - '0');
      }
      fractional_digits++;
      p++;
    }
  }
  while (fractional_digits < 3) {
    milliseconds *= 10;
    fractional_digits++;
  }
  while (ascii_is_space(*p)) {
    p++;
  }
  if (*p != '\0') {
    return error_report(reason, reason_len, "ffprobe returned an invalid duration");
  }
  if (seconds > (SIZE_MAX - milliseconds) / 1000) {
    return error_report(reason, reason_len, "video duration is too large");
  }
  *duration_ms_out = seconds * 1000 + milliseconds;
  return 0;
}

static char* make_frame_template(char* reason, size_t reason_len) {
  static const char name[] = "fram-video-frame.XXXXXX";
  const char* temp_dir = getenv("TMPDIR");
  if (temp_dir == NULL || temp_dir[0] == '\0') {
    temp_dir = "/tmp";
  }
  const size_t dir_len = strlen(temp_dir);
  const bool needs_slash = temp_dir[dir_len - 1] != '/';
  if (dir_len > SIZE_MAX - sizeof(name) - (size_t)needs_slash) {
    (void)error_report(reason, reason_len, "temporary directory path is too long");
    return NULL;
  }
  char* path = malloc(dir_len + (size_t)needs_slash + sizeof(name));
  if (path == NULL) {
    (void)error_report(reason, reason_len, "out of memory");
    return NULL;
  }
  memcpy(path, temp_dir, dir_len);
  size_t offset = dir_len;
  if (needs_slash) {
    path[offset++] = '/';
  }
  memcpy(path + offset, name, sizeof(name));
  return path;
}
