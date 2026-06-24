/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Unit tests for parse_size (src/parse_util.c). */
#include <criterion/criterion.h>
#include <stddef.h>

#include "parse_util.h"

Test(parse_size, plain_bytes_and_zero)
{
    size_t v = 123;
    cr_assert_eq(parse_size("0", &v), 0);
    cr_assert_eq(v, 0u);                         /* 0 => unlimited */
    cr_assert_eq(parse_size("4096", &v), 0);
    cr_assert_eq(v, 4096u);
    cr_assert_eq(parse_size("1", &v), 0);
    cr_assert_eq(v, 1u);
}

Test(parse_size, binary_suffixes)
{
    size_t v;
    cr_assert_eq(parse_size("1K", &v), 0);
    cr_assert_eq(v, 1024u);
    cr_assert_eq(parse_size("1k", &v), 0);
    cr_assert_eq(v, 1024u);
    cr_assert_eq(parse_size("10M", &v), 0);
    cr_assert_eq(v, 10u * 1024 * 1024);
    cr_assert_eq(parse_size("2G", &v), 0);
    cr_assert_eq(v, 2ull * 1024 * 1024 * 1024);
}

Test(parse_size, iec_and_decimal_tails)
{
    size_t v;
    cr_assert_eq(parse_size("200MiB", &v), 0);
    cr_assert_eq(v, 200u * 1024 * 1024);
    cr_assert_eq(parse_size("200MB", &v), 0);    /* treated as binary too */
    cr_assert_eq(v, 200u * 1024 * 1024);
    cr_assert_eq(parse_size("512KB", &v), 0);
    cr_assert_eq(v, 512u * 1024);
}

Test(parse_size, rejects_malformed)
{
    size_t v;
    cr_assert_neq(parse_size("", &v), 0);
    cr_assert_neq(parse_size("abc", &v), 0);
    cr_assert_neq(parse_size("10X", &v), 0);     /* unknown suffix */
    cr_assert_neq(parse_size("10MM", &v), 0);    /* trailing junk */
    cr_assert_neq(parse_size("-5", &v), 0);      /* no negatives */
    cr_assert_neq(parse_size("M", &v), 0);       /* no digits */
}

Test(parse_size, rejects_overflow)
{
    size_t v;
    cr_assert_neq(parse_size("999999999999G", &v), 0);
}
