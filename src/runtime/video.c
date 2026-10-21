// `mkostemp` is a GNU extension in glibc and musl, so Linux needs `_GNU_SOURCE` to see it, and
// macOS needs `_DARWIN_C_SOURCE`. Both must precede every `#include`, because a libc header
// resolves its own visibility guards the first time it is included.
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE

#include "runtime/video.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/ascii.h"
#include "core/error.h"
#include "core/path.h"
#include "core/text.h"
#include "runtime/proc.h"
#include "shared/arena.h"
#include "shared/log.h"
#include "shared/str.h"

/** Size of the buffer `video_extract_frame` formats a seek position into, terminator included. */
enum { VIDEO_POSITION_SIZE = 64 };

// The widest non-negative `ptrdiff_t` of whole seconds, the `.` and three millisecond digits, so a
// position formatted in `video_extract_frame` cannot truncate.
_Static_assert(VIDEO_POSITION_SIZE > sizeof("9223372036854775807.999") - 1,
               "video position buffer must hold the widest ptrdiff_t position");

/**
 * @brief Refuses a video path holding a `NUL` byte, because the argument a child process receives
 *        would end at that byte and name another file.
 *
 * @param file_path Video path the tool would receive.
 * @param operation Operation leading the failure message.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @return `0` when the path holds no `NUL` byte, or `-1` after logging the failure.
 */
static int check_video_path(struct Str file_path, struct Str operation, struct Log* log)
    __attribute__((nonnull(3)));

/**
 * @brief Creates the frame's temporary file from the slot's template, so the name `ffmpeg` writes
 *        to is one this build owns. A step of `video_extract_frame`.
 *
 * The descriptor is close-on-exec from the start, so a child another worker spawns before it is
 * closed cannot inherit it.
 *
 * @param frame     Slot whose template is filled in and whose `is_created` is set once the file
 *                  exists. Must not be `NULL`.
 * @param file_path Video the frame is for, named at the end of a failure message.
 * @param operation Operation leading the failure message.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @return `0` when the file exists and is closed, or `-1` after logging the failure.
 */
static int video_extract_frame_create(struct VideoFrame* frame,
                                      struct Str file_path,
                                      struct Str operation,
                                      struct Log* log) __attribute__((nonnull(1, 4)));

/**
 * @brief Checks that `ffmpeg` left a nonempty regular file at the frame's path. A step of
 *        `video_extract_frame`.
 *
 * The frame is a file on the real host whichever `os.h` implementation the executable links,
 * because `ffmpeg` wrote it there, so it is inspected with `stat` directly. The frame is gone once
 * the build ends, so each message also names the video the frame is for.
 *
 * @param frame_path Terminated path of the frame. Must not be `NULL`.
 * @param file_path  Video the frame is for.
 * @param operation  Operation leading the failure message.
 * @param log        Log that receives the failure. Must not be `NULL`.
 * @return `0` when the frame is a nonempty regular file, or `-1` after logging the failure.
 */
static int video_extract_frame_inspect(const char* frame_path,
                                       struct Str file_path,
                                       struct Str operation,
                                       struct Log* log) __attribute__((nonnull(1, 4)));

/**
 * @brief Parses the decimal seconds emitted by `ffprobe` into milliseconds.
 *
 * @param text            `ffprobe`'s standard output.
 * @param duration_ms_out Receives the duration on success. Must not be `NULL`.
 * @param operation       Operation leading the failure message.
 * @param file_path       Video the duration is for, named at the end of the message.
 * @param log             Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the text is not a duration or the duration is too large.
 */
static int parse_duration_ms(struct Str text,
                             ptrdiff_t* duration_ms_out,
                             struct Str operation,
                             struct Str file_path,
                             struct Log* log) __attribute__((nonnull(2, 5)));

/**
 * @brief Skips leading ASCII whitespace.
 *
 * @param text Text to trim.
 * @return View of `text` from its first byte that is not whitespace.
 */
static struct Str parse_duration_ms_skip_space(struct Str text);

void video_frame_init(struct VideoFrame* frame, struct Arena* perm) {
  const char* temp_dir = getenv("TMPDIR");
  if (temp_dir == NULL || temp_dir[0] == '\0') {
    temp_dir = "/tmp";
  }
  const struct Str dir = str_from_cstr(temp_dir);
  const struct Str name = S("fram-video-frame.XXXXXX");
  // `mkostemp` fills the template in place, so the slot keeps one writable, terminated copy of the
  // path. The join runs in a copy of `perm` past that copy, which drops it. The arena zeroes the
  // copy, so the byte after the path is already its terminator, and the separator `path_join` may
  // add is the one spare byte.
  char* path = arena_new(perm, dir.len + 1 + name.len + 1, char);
  struct Arena scratch = *perm;
  const struct Str joined = path_join(dir, name, &scratch);
  memcpy(path, joined.data, (size_t)joined.len);
  *frame = (struct VideoFrame){.path = path};
}

void video_frame_remove(struct VideoFrame* frame) {
  // A frame whose unlink failed for any other reason may still exist, so it stays marked for the
  // next cleanup. `ENOENT` means it is already gone, so there is nothing left to retry.
  if (frame->is_created && (unlink(frame->path) == 0 || errno == ENOENT)) {
    frame->is_created = false;
  }
}

int video_probe_duration(struct Str file_path,
                         struct Arena scratch,
                         ptrdiff_t* duration_ms_out,
                         struct Str operation,
                         struct Log* log) {
  if (check_video_path(file_path, operation, log) != 0) {
    return -1;
  }
  const char* const argv[] = {
      "ffprobe",
      "-v",
      "error",
      "-show_entries",
      "format=duration",
      "-of",
      "default=noprint_wrappers=1:nokey=1",
      "--",
      str_cstr(file_path, &scratch),
      NULL,
  };
  struct Str output = {0};
  if (proc_run(argv, &scratch, &output, operation, file_path, log) != 0) {
    return -1;
  }
  return parse_duration_ms(output, duration_ms_out, operation, file_path, log);
}

int video_extract_frame(struct VideoFrame* frame,
                        struct Str file_path,
                        ptrdiff_t position_ms,
                        struct Arena scratch,
                        struct Str operation,
                        struct Log* log) {
  if (check_video_path(file_path, operation, log) != 0) {
    return -1;
  }
  assert(position_ms >= 0);
  char position[VIDEO_POSITION_SIZE];
  (void)snprintf(position, sizeof(position), "%td.%03td", position_ms / 1000, position_ms % 1000);

  const char* frame_path = frame->path;
  // FFmpeg applies display-matrix rotation by default. The extracted JPEG is therefore upright, and
  // the image path can probe its real dimensions without parsing version-sensitive side data.
  const char* const argv[] = {
      "ffmpeg",    "-v",     "error", "-nostdin",
      "-ss",       position, "-i",    str_cstr(file_path, &scratch),
      "-frames:v", "1",      "-f",    "image2",
      "-vcodec",   "mjpeg",  "-y",    frame_path,
      NULL,
  };
  if (video_extract_frame_create(frame, file_path, operation, log) != 0 ||
      proc_run(argv, &scratch, NULL, operation, file_path, log) != 0 ||
      video_extract_frame_inspect(frame_path, file_path, operation, log) != 0) {
    video_frame_remove(frame);
    return -1;
  }
  return 0;
}

static int check_video_path(struct Str file_path, struct Str operation, struct Log* log) {
  if (!text_is_nul_free(file_path)) {
    return log_error(log, "%.*s: video path contains an embedded NUL byte ('%.*s')",
                     STR_FMT(operation), STR_FMT(file_path));
  }
  return 0;
}

static int video_extract_frame_create(struct VideoFrame* frame,
                                      struct Str file_path,
                                      struct Str operation,
                                      struct Log* log) {
  const int fd = mkostemp(frame->path, O_CLOEXEC);
  if (fd < 0) {
    char message[ERROR_SYSTEM_MESSAGE_SIZE];
    return log_error(log, "%.*s: cannot create temporary video frame: %s ('%s') (for '%.*s')",
                     STR_FMT(operation), error_system_message(message, sizeof(message), errno),
                     frame->path, STR_FMT(file_path));
  }
  frame->is_created = true;
  if (close(fd) != 0) {
    char message[ERROR_SYSTEM_MESSAGE_SIZE];
    return log_error(log, "%.*s: cannot close temporary video frame: %s ('%s') (for '%.*s')",
                     STR_FMT(operation), error_system_message(message, sizeof(message), errno),
                     frame->path, STR_FMT(file_path));
  }
  return 0;
}

static int video_extract_frame_inspect(const char* frame_path,
                                       struct Str file_path,
                                       struct Str operation,
                                       struct Log* log) {
  struct stat st;
  if (stat(frame_path, &st) != 0) {
    char message[ERROR_SYSTEM_MESSAGE_SIZE];
    return log_error(log, "%.*s: cannot inspect video frame: %s ('%s') (for '%.*s')",
                     STR_FMT(operation), error_system_message(message, sizeof(message), errno),
                     frame_path, STR_FMT(file_path));
  }
  if (!S_ISREG(st.st_mode)) {
    return log_error(log, "%.*s: video frame is not a regular file ('%s') (for '%.*s')",
                     STR_FMT(operation), frame_path, STR_FMT(file_path));
  }
  if (st.st_size == 0) {
    return log_error(log, "%.*s: FFmpeg produced an empty video frame ('%s') (for '%.*s')",
                     STR_FMT(operation), frame_path, STR_FMT(file_path));
  }
  return 0;
}

static int parse_duration_ms(struct Str text,
                             ptrdiff_t* duration_ms_out,
                             struct Str operation,
                             struct Str file_path,
                             struct Log* log) {
  struct Str rest = parse_duration_ms_skip_space(text);
  if (str_startswith(rest, S("N/A"))) {
    rest = parse_duration_ms_skip_space(str_tail(rest, 3));
    if (rest.len == 0) {
      *duration_ms_out = 0;
      return 0;
    }
  }
  if (rest.len == 0 || !ascii_is_digit((unsigned char)rest.data[0])) {
    return log_error(log, "%.*s: ffprobe returned an invalid duration ('%.*s')", STR_FMT(operation),
                     STR_FMT(file_path));
  }
  ptrdiff_t pos = 0;
  ptrdiff_t seconds = 0;
  while (pos < rest.len && ascii_is_digit((unsigned char)rest.data[pos])) {
    const ptrdiff_t digit = rest.data[pos] - '0';
    if (seconds > (PTRDIFF_MAX - digit) / 10) {
      return log_error(log, "%.*s: video duration is too large ('%.*s')", STR_FMT(operation),
                       STR_FMT(file_path));
    }
    seconds = seconds * 10 + digit;
    pos++;
  }

  ptrdiff_t milliseconds = 0;
  ptrdiff_t fractional_digits = 0;
  if (pos < rest.len && rest.data[pos] == '.') {
    pos++;
    if (pos == rest.len || !ascii_is_digit((unsigned char)rest.data[pos])) {
      return log_error(log, "%.*s: ffprobe returned an invalid duration ('%.*s')",
                       STR_FMT(operation), STR_FMT(file_path));
    }
    while (pos < rest.len && ascii_is_digit((unsigned char)rest.data[pos])) {
      if (fractional_digits < 3) {
        milliseconds = milliseconds * 10 + (rest.data[pos] - '0');
      }
      fractional_digits++;
      pos++;
    }
  }
  while (fractional_digits < 3) {
    milliseconds *= 10;
    fractional_digits++;
  }
  if (parse_duration_ms_skip_space(str_tail(rest, pos)).len != 0) {
    return log_error(log, "%.*s: ffprobe returned an invalid duration ('%.*s')", STR_FMT(operation),
                     STR_FMT(file_path));
  }
  if (seconds > (PTRDIFF_MAX - milliseconds) / 1000) {
    return log_error(log, "%.*s: video duration is too large ('%.*s')", STR_FMT(operation),
                     STR_FMT(file_path));
  }
  *duration_ms_out = seconds * 1000 + milliseconds;
  return 0;
}

static struct Str parse_duration_ms_skip_space(struct Str text) {
  ptrdiff_t pos = 0;
  while (pos < text.len && ascii_is_space((unsigned char)text.data[pos])) {
    pos++;
  }
  return str_tail(text, pos);
}
