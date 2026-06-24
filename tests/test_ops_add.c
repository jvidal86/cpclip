/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Unit tests for clip_add (src/ops_add.c), driven through an in-memory mock
 * backend so the concatenation, type-coherence, and size-cap logic can be
 * checked directly without any display server. */
#include <criterion/criterion.h>
#include <stdlib.h>
#include <string.h>

#include "backend.h"
#include "ops_add.h"

/* ---- in-memory mock backend -------------------------------------------- */

static unsigned char store[1 << 16];
static size_t store_len;
static int get_status;          /* what mock_get returns */
static unsigned char last_set[1 << 16];
static size_t last_set_len;
static int set_called;

static const char *mock_name(void) { return "mock"; }

static int mock_get(const char *mime, void **out, size_t *out_len)
{
    (void)mime;
    if (get_status != CLIP_GET_OK) {
        *out = NULL;
        *out_len = 0;
        return get_status;
    }
    void *p = malloc(store_len ? store_len : 1);
    if (store_len)
        memcpy(p, store, store_len);
    *out = p;
    *out_len = store_len;
    return CLIP_GET_OK;
}

static int mock_set(const char *mime, const void *data, size_t len)
{
    (void)mime;
    set_called = 1;
    memcpy(last_set, data, len);
    last_set_len = len;
    memcpy(store, data, len);       /* reflect so chained adds see it */
    store_len = len;
    return 0;
}

static int mock_clear(void) { store_len = 0; return 0; }

static const clipboard_backend mock = {
    .name = mock_name, .set = mock_set, .get = mock_get, .clear = mock_clear,
};

static void reset(void)
{
    store_len = 0;
    get_status = CLIP_GET_OK;
    last_set_len = 0;
    set_called = 0;
}

/* ---- tests ------------------------------------------------------------- */

Test(clip_add, empty_clipboard_is_a_plain_copy)
{
    reset();
    cr_assert_eq(clip_add(&mock, NULL, "abc", 3, "\n", 0), 0);
    cr_assert(set_called);
    cr_assert_eq(last_set_len, 3u);             /* no leading separator */
    cr_assert_eq(memcmp(last_set, "abc", 3), 0);
}

Test(clip_add, appends_with_separator)
{
    reset();
    memcpy(store, "one", 3);
    store_len = 3;
    cr_assert_eq(clip_add(&mock, NULL, "two", 3, "\n", 0), 0);
    cr_assert_eq(last_set_len, 7u);
    cr_assert_eq(memcmp(last_set, "one\ntwo", 7), 0);
}

Test(clip_add, binary_safe_with_embedded_nul)
{
    reset();
    memcpy(store, "a\0b", 3);
    store_len = 3;
    cr_assert_eq(clip_add(&mock, NULL, "c\0d", 3, "", 0), 0);
    cr_assert_eq(last_set_len, 6u);
    cr_assert_eq(memcmp(last_set, "a\0bc\0d", 6), 0);
}

Test(clip_add, refuses_non_text_selection)
{
    reset();
    get_status = CLIP_GET_NO_TEXT;
    cr_assert_eq(clip_add(&mock, NULL, "x", 1, "\n", 0), -1);
    cr_assert_eq(set_called, 0);                /* must not clobber */
}

Test(clip_add, propagates_get_error)
{
    reset();
    get_status = CLIP_GET_ERROR;
    cr_assert_eq(clip_add(&mock, NULL, "x", 1, "\n", 0), -1);
    cr_assert_eq(set_called, 0);
}

Test(clip_add, enforces_max_mem_on_total)
{
    reset();
    memcpy(store, "aaaaa", 5);
    store_len = 5;                              /* 5 + 1 sep + 5 = 11 > 8 */
    cr_assert_eq(clip_add(&mock, NULL, "bbbbb", 5, "\n", 8), -1);
    cr_assert_eq(set_called, 0);
}
