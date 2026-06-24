/* SPDX-License-Identifier: GPL-2.0-or-later */
/* io_util.h — binary-safe I/O helpers shared by every backend and the CLI.
 *
 * See DESIGN.md §6 (binary-safe everywhere; grow-buffer stdin; TTY pass-through).
 */
#ifndef CLIP_IO_UTIL_H
#define CLIP_IO_UTIL_H

#include <stddef.h>

/* read_all_fd returns this when `limit` is exceeded (distinct from -1). */
#define CLIP_READ_TOO_LARGE (-2)

/* Read all of `fd` into a freshly malloc'd buffer using a grow-by-doubling
 * loop (input size is unknown). The caller must free(*out).
 *
 * If `mirror_fd >= 0`, every chunk is also written to `mirror_fd` as it is read
 * — this is the cpclip/cpadd TTY pass-through (raw bytes, tee-style). A mirror
 * write failure is non-fatal: the clipboard copy still proceeds. Pass -1 to
 * disable mirroring (cppaste, and internal reads).
 *
 * If `limit` is nonzero, the read stops and fails with CLIP_READ_TOO_LARGE once
 * more than `limit` bytes are available, bounding memory to ~limit. Pass 0 for
 * no limit.
 *
 * Returns 0 on success (including empty input => *out non-NULL, *out_len 0),
 * -1 on a read/alloc error, or CLIP_READ_TOO_LARGE. */
int read_all_fd(int fd, void **out, size_t *out_len, int mirror_fd, size_t limit);

/* Write exactly `len` bytes to `fd`, looping over partial writes and retrying
 * EINTR. Returns 0 on success, -1 on error. */
int write_all(int fd, const void *buf, size_t len);

#endif /* CLIP_IO_UTIL_H */
