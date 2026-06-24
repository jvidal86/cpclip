/* SPDX-License-Identifier: GPL-2.0-or-later */
/* io_util.c — see io_util.h. */
#include "io_util.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

/* Starting buffer size for read_all_fd; doubles as needed. */
#define READ_INITIAL_CAP 4096

int write_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t left = len;
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        left -= (size_t)n;
    }
    return 0;
}

int read_all_fd(int fd, void **out, size_t *out_len, int mirror_fd, size_t limit)
{
    size_t cap = READ_INITIAL_CAP, len = 0;
    /* When limited, never hold more than limit+1 bytes: the +1 lets an
     * exactly-`limit` input fit while a one-byte-over input still trips. */
    if (limit && cap > limit + 1)
        cap = limit + 1;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return -1;

    for (;;) {
        if (len == cap) {
            size_t ncap = cap * 2;          /* double on exhaustion */
            if (limit && ncap > limit + 1)
                ncap = limit + 1;
            if (ncap <= cap) {              /* at the ceiling with no room left */
                free(buf);
                return CLIP_READ_TOO_LARGE;
            }
            char *nb = (char *)realloc(buf, ncap);
            if (!nb) {
                free(buf);
                return -1;
            }
            buf = nb;
            cap = ncap;
        }

        ssize_t n = read(fd, buf + len, cap - len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            return -1;
        }
        if (n == 0)
            break;                          /* EOF */

        /* TTY pass-through: mirror each chunk as it arrives. Best-effort —
         * a closed mirror must not abort the copy. */
        if (mirror_fd >= 0)
            (void)write_all(mirror_fd, buf + len, (size_t)n);

        len += (size_t)n;
        if (limit && len > limit) {        /* more data than allowed */
            free(buf);
            return CLIP_READ_TOO_LARGE;
        }
    }

    *out = buf;
    *out_len = len;
    return 0;
}
