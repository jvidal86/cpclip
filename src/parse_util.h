/* SPDX-License-Identifier: GPL-2.0-or-later */
/* parse_util.h — small pure CLI parsers (kept separate so they are unit-testable
 * without pulling in main()'s dispatch or any backend). */
#ifndef CLIP_PARSE_UTIL_H
#define CLIP_PARSE_UTIL_H

#include <stddef.h>

/* Parse a human size like "200", "512K", "10M", "2G" into bytes. K/M/G are
 * binary (1024-based); an optional "i"/"B" tail is accepted ("200MiB", "200MB").
 * "0" means unlimited. Negative values are rejected. Returns 0 on success and
 * stores the byte count in *out, or -1 if the string is malformed or overflows. */
int parse_size(const char *s, size_t *out);

#endif /* CLIP_PARSE_UTIL_H */
