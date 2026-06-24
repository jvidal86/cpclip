/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIME_SNIFF_H
#define MIME_SNIFF_H
#include <stddef.h>

/*
 * Probe up to 8 KiB of data and return a MIME type string:
 *   "application/octet-stream"  — NUL byte found (binary)
 *   "text/plain;charset=utf-8"  — no NUL, valid UTF-8 (includes ASCII)
 *   "text/plain"                — no NUL, not valid UTF-8 (legacy 8-bit encoding)
 *
 * Returns a pointer to a string literal; never NULL.
 * The caller's -t/--type flag should take precedence over this.
 */
const char *sniff_mime(const void *data, size_t len);

#endif /* MIME_SNIFF_H */
