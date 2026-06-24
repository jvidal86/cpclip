/* ops_add.c — see ops_add.h and DESIGN.md §5.3.
 *
 * Phase 0 ships the core composition. Phase 3 adds the refinements: a hard
 * ordering guarantee (fully complete the read before the write acquires
 * ownership) and type coherence (reject a text add onto a non-text selection).
 */
#include "ops_add.h"

#include <stdlib.h>
#include <string.h>

int clip_add(const clipboard_backend *b, const char *mime,
             const void *new_data, size_t new_len, const char *sep)
{
    /* Must FULLY read the old owner before becoming the new owner. */
    void *cur = NULL;
    size_t cur_len = 0;
    if (b->get(mime, &cur, &cur_len) != 0)
        return -1;

    size_t sep_len = (cur_len && sep) ? strlen(sep) : 0;  /* no leading sep */
    size_t total = cur_len + sep_len + new_len;

    char *buf = (char *)malloc(total ? total : 1);
    if (!buf) {
        free(cur);
        return -1;
    }

    if (cur_len)
        memcpy(buf, cur, cur_len);
    if (sep_len)
        memcpy(buf + cur_len, sep, sep_len);
    if (new_len)
        memcpy(buf + cur_len + sep_len, new_data, new_len);

    int rc = b->set(mime, buf, total);      /* becomes the new owner */

    free(cur);
    free(buf);
    return rc;
}
