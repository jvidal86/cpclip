/* SPDX-License-Identifier: GPL-2.0-or-later */
/* parse_util.c — see parse_util.h. */
#include "parse_util.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

int parse_size(const char *s, size_t *out)
{
    if (*s == '-')                          /* no negative sizes */
        return -1;

    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s || errno)
        return -1;                          /* no digits, or overflow */

    unsigned long long mult = 1;
    if (*end) {
        switch (*end++) {
        case 'k': case 'K': mult = 1024ULL; break;
        case 'm': case 'M': mult = 1024ULL * 1024; break;
        case 'g': case 'G': mult = 1024ULL * 1024 * 1024; break;
        case 'b': case 'B': mult = 1; break;
        default: return -1;
        }
        if (*end == 'i' || *end == 'I') end++;      /* accept KiB / MiB / GiB */
        if (*end == 'b' || *end == 'B') end++;      /* accept KB / MB / ...   */
        if (*end)
            return -1;                              /* trailing junk */
    }

    if (v > (unsigned long long)SIZE_MAX / mult)
        return -1;                          /* would overflow size_t */
    *out = (size_t)(v * mult);
    return 0;
}
