#ifndef FRAM_TEST_JOB_ARENAS_H
#define FRAM_TEST_JOB_ARENAS_H

#include <stddef.h>

#include "build/job.h"
#include "shared/arena.h"
#include "test_support.h"

// This is header-only and `static inline` so that every unit test running a parallel phase can
// include it without a separate test library to link.

/**
 * @brief Allocates one `JobArenas` per worker, each region a fresh `test_arena` reservation.
 *
 * @param worker_count Number of workers to allocate arenas for.
 * @param perm         Arena that owns the array. Must not be `NULL`.
 * @return The array, one entry per worker.
 */
static inline struct JobArenas* test_job_arenas(ptrdiff_t worker_count, struct Arena* perm)
    __attribute__((nonnull(2)));

static inline struct JobArenas* test_job_arenas(ptrdiff_t worker_count, struct Arena* perm) {
  struct JobArenas* arenas = arena_new(perm, worker_count, struct JobArenas);
  for (ptrdiff_t i = 0; i < worker_count; i++) {
    arenas[i].perm = test_arena();
    arenas[i].scratch = test_arena();
  }
  return arenas;
}

#endif
