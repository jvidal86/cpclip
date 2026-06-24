/* SPDX-License-Identifier: GPL-2.0-or-later */
/* mime_sniff.c — lightweight content-type sniffer; see mime_sniff.h. */
#include "mime_sniff.h"

#include <stdint.h>
#include <string.h>

/* Probe at most this many bytes (same window git and file(1) use). */
#define PROBE_BYTES ((size_t)8192)

/*
 * Validate that [p, p+len) is well-formed UTF-8.
 *
 * Rejects overlong encodings, surrogates (U+D800–U+DFFF), and code points
 * above U+10FFFF.  Pure ASCII (all bytes < 0x80) is always accepted.
 */
static int utf8_valid(const unsigned char *p, size_t len)
{
    size_t i = 0;
    while (i < len) {
        unsigned char c = p[i++];
        if (c < 0x80)
            continue;                   /* ASCII — always valid */

        uint32_t cp;
        uint32_t min_cp;
        int extra;

        if      (c < 0xC2)  return 0;  /* bare continuation or overlong 2-byte */
        else if (c < 0xE0)  { extra = 1; cp = c & 0x1F; min_cp = 0x80;    }
        else if (c < 0xF0)  { extra = 2; cp = c & 0x0F; min_cp = 0x800;   }
        else if (c <= 0xF4) { extra = 3; cp = c & 0x07; min_cp = 0x10000; }
        else                  return 0; /* above U+10FFFF */

        for (int j = 0; j < extra; j++) {
            if (i >= len)              return 0;   /* truncated sequence */
            unsigned char b = p[i++];
            if ((b & 0xC0) != 0x80)   return 0;   /* expected continuation */
            cp = (cp << 6) | (b & 0x3F);
        }

        if (cp < min_cp)                    return 0;   /* overlong */
        if (cp >= 0xD800 && cp <= 0xDFFF)  return 0;   /* surrogate */
        /* cp > 0x10FFFF is impossible: 0xF4 leader caps at 0x13FFFF and
         * min_cp check excludes 0x110000+, but be explicit for clarity. */
        if (cp > 0x10FFFF)                  return 0;
    }
    return 1;
}

const char *sniff_mime(const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t probe = len < PROBE_BYTES ? len : PROBE_BYTES;

    if (memchr(p, '\0', probe))
        return "application/octet-stream";

    return utf8_valid(p, probe) ? "text/plain;charset=utf-8" : "text/plain";
}
