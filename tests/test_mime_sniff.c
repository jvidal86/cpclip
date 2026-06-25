/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Unit tests for sniff_mime (src/mime_sniff.c). */
#include <criterion/criterion.h>
#include <string.h>

#include "mime_sniff.h"

/* Shorthand aliases for the three return values. */
#define UTF8  "text/plain;charset=utf-8"
#define TEXT  "text/plain"
#define BIN   "application/octet-stream"

/* ---- empty and ASCII --------------------------------------------------- */

Test(sniff_mime, empty_is_utf8)
{
    cr_assert_str_eq(sniff_mime("", 0), UTF8);
}

Test(sniff_mime, pure_ascii_is_utf8)
{
    cr_assert_str_eq(sniff_mime("hello world\n", 12), UTF8);
}

Test(sniff_mime, printable_ascii_range)
{
    /* Every printable ASCII byte — no NUL, valid UTF-8. */
    char buf[94];
    for (int i = 0; i < 94; i++)
        buf[i] = (char)(33 + i);        /* '!' .. '~' */
    cr_assert_str_eq(sniff_mime(buf, sizeof buf), UTF8);
}

/* ---- NUL detection (binary) ------------------------------------------- */

Test(sniff_mime, nul_at_start_is_binary)
{
    cr_assert_str_eq(sniff_mime("\x00hello", 6), BIN);
}

Test(sniff_mime, nul_in_middle_is_binary)
{
    cr_assert_str_eq(sniff_mime("hel\x00lo", 6), BIN);
}

Test(sniff_mime, nul_at_last_probe_byte_is_binary)
{
    /* The probe window is exactly 8192 bytes; buf[8191] is still inside it. */
    char buf[8192];
    memset(buf, 'a', sizeof buf);
    buf[8191] = '\0';
    cr_assert_str_eq(sniff_mime(buf, sizeof buf), BIN);
}

Test(sniff_mime, nul_beyond_probe_window_is_not_detected)
{
    /* NUL sits one byte past the 8192-byte probe; sniff should not see it. */
    char buf[8193];
    memset(buf, 'a', sizeof buf);
    buf[8192] = '\0';
    cr_assert_str_eq(sniff_mime(buf, sizeof buf), UTF8);
}

/* ---- valid multibyte UTF-8 -------------------------------------------- */

Test(sniff_mime, two_byte_utf8)
{
    /* U+00E9 LATIN SMALL LETTER E WITH ACUTE: 0xC3 0xA9 */
    cr_assert_str_eq(sniff_mime("\xC3\xA9", 2), UTF8);
}

Test(sniff_mime, three_byte_utf8)
{
    /* U+20AC EURO SIGN: 0xE2 0x82 0xAC */
    cr_assert_str_eq(sniff_mime("\xE2\x82\xAC", 3), UTF8);
}

Test(sniff_mime, four_byte_utf8)
{
    /* U+1F600 GRINNING FACE: 0xF0 0x9F 0x98 0x80 */
    cr_assert_str_eq(sniff_mime("\xF0\x9F\x98\x80", 4), UTF8);
}

/* ---- invalid UTF-8 (non-NUL, so text/plain not binary) ---------------- */

Test(sniff_mime, bare_continuation_byte_is_text_plain)
{
    /* 0x80 is a continuation byte with no preceding lead byte. */
    cr_assert_str_eq(sniff_mime("\x80", 1), TEXT);
}

Test(sniff_mime, overlong_two_byte_is_text_plain)
{
    /* Overlong encoding of U+0041 'A': 0xC1 0x81 — lead byte < 0xC2. */
    cr_assert_str_eq(sniff_mime("\xC1\x81", 2), TEXT);
}

Test(sniff_mime, truncated_two_byte_sequence_is_text_plain)
{
    /* Lead byte 0xC3 with no continuation. */
    cr_assert_str_eq(sniff_mime("\xC3", 1), TEXT);
}

Test(sniff_mime, truncated_three_byte_sequence_is_text_plain)
{
    /* Lead 0xE2, one continuation, then EOF. */
    cr_assert_str_eq(sniff_mime("\xE2\x82", 2), TEXT);
}

Test(sniff_mime, surrogate_is_text_plain)
{
    /* U+D800 (surrogate): 0xED 0xA0 0x80 — forbidden in UTF-8. */
    cr_assert_str_eq(sniff_mime("\xED\xA0\x80", 3), TEXT);
}

Test(sniff_mime, above_unicode_max_is_text_plain)
{
    /* 0xF5 would start a code point above U+10FFFF. */
    cr_assert_str_eq(sniff_mime("\xF5\x80\x80\x80", 4), TEXT);
}

Test(sniff_mime, latin1_high_bytes_is_text_plain)
{
    /* A typical Latin-1 string — 0xFF is not valid UTF-8. */
    cr_assert_str_eq(sniff_mime("caf\xe9", 4), TEXT);
}

/* ---- mixed content ---------------------------------------------------- */

Test(sniff_mime, valid_utf8_then_nul_is_binary)
{
    /* Valid multibyte UTF-8 before a NUL byte — still binary. */
    cr_assert_str_eq(sniff_mime("\xC3\xA9\x00rest", 7), BIN);
}
