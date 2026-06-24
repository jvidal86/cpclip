/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Unit tests for read_all_fd / write_all (src/io_util.c). */
#include <criterion/criterion.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "io_util.h"

/* Return a readable fd holding `n` bytes (keep n well under the 64 KiB pipe
 * buffer so the single write never blocks). */
static int fd_with(const void *data, size_t n)
{
    int p[2];
    cr_assert_eq(pipe(p), 0);
    cr_assert_eq(write(p[1], data, n), (ssize_t)n);
    close(p[1]);
    return p[0];
}

Test(read_all_fd, empty_input)
{
    int fd = fd_with("", 0);
    void *out = NULL;
    size_t len = 99;
    cr_assert_eq(read_all_fd(fd, &out, &len, -1, 0), 0);
    cr_assert_eq(len, 0u);
    cr_assert_not_null(out);                     /* non-NULL even when empty */
    free(out);
    close(fd);
}

Test(read_all_fd, reads_all_bytes_including_nul)
{
    static const char s[] = "hello\0world";      /* 11 payload bytes */
    int fd = fd_with(s, 11);
    void *out;
    size_t len;
    cr_assert_eq(read_all_fd(fd, &out, &len, -1, 0), 0);
    cr_assert_eq(len, 11u);
    cr_assert_eq(memcmp(out, s, 11), 0);
    free(out);
    close(fd);
}

Test(read_all_fd, grows_past_initial_cap)
{
    char buf[5000];
    memset(buf, 'z', sizeof buf);                /* > 4096 initial capacity */
    int fd = fd_with(buf, sizeof buf);
    void *out;
    size_t len;
    cr_assert_eq(read_all_fd(fd, &out, &len, -1, 0), 0);
    cr_assert_eq(len, sizeof buf);
    free(out);
    close(fd);
}

Test(read_all_fd, exact_limit_is_accepted)
{
    char buf[100];
    memset(buf, 'a', sizeof buf);
    int fd = fd_with(buf, sizeof buf);
    void *out;
    size_t len;
    cr_assert_eq(read_all_fd(fd, &out, &len, -1, 100), 0);
    cr_assert_eq(len, 100u);
    free(out);
    close(fd);
}

Test(read_all_fd, one_byte_over_limit_fails)
{
    char buf[101];
    memset(buf, 'a', sizeof buf);
    int fd = fd_with(buf, sizeof buf);
    void *out = NULL;
    size_t len;
    cr_assert_eq(read_all_fd(fd, &out, &len, -1, 100), CLIP_READ_TOO_LARGE);
    close(fd);
}

Test(read_all_fd, limit_below_initial_cap_still_enforced)
{
    char buf[200];
    memset(buf, 'x', sizeof buf);
    int fd = fd_with(buf, sizeof buf);
    void *out = NULL;
    size_t len;
    cr_assert_eq(read_all_fd(fd, &out, &len, -1, 50), CLIP_READ_TOO_LARGE);
    close(fd);
}

Test(write_all, writes_everything)
{
    int p[2];
    cr_assert_eq(pipe(p), 0);
    static const char s[] = "abcdef";
    cr_assert_eq(write_all(p[1], s, 6), 0);
    close(p[1]);

    char rb[16];
    cr_assert_eq(read(p[0], rb, sizeof rb), 6);
    cr_assert_eq(memcmp(rb, s, 6), 0);
    close(p[0]);
}
