/* ops_add.c — see ops_add.h and DESIGN.md §5.3. */
#include "ops_add.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int clip_add(const clipboard_backend *b, const char *mime,
             const void *new_data, size_t new_len, const char *sep)
{
    /* Ordering guarantee: the read below runs to completion (the backend opens
     * its own connection, reads, and closes) BEFORE set() forks the new owner.
     * So we never read our own freshly-emptied self. */
    void *cur = NULL;
    size_t cur_len = 0;
    int status = b->get(mime, &cur, &cur_len);

    /* Type coherence: a non-text selection must not be silently clobbered. An
     * empty clipboard, by contrast, is fine — the first add is just a copy. */
    if (status == CLIP_GET_NO_TEXT) {
        fprintf(stderr, "cpadd: current selection is not text; refusing to overwrite it\n");
        return -1;
    }
    if (status != CLIP_GET_OK)
        return -1;                          /* infrastructure error, reported */

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
