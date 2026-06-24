/* ops_add.h — the composed `add` (append) operation.
 *
 * `add` is not a primitive: neither X11 nor Wayland can append. It is
 * read-current -> concatenate-with-separator -> write-result, implemented once
 * here against the backend vtable so it works on every backend for free.
 *
 * See DESIGN.md §5.3.
 */
#ifndef CLIP_OPS_ADD_H
#define CLIP_OPS_ADD_H

#include <stddef.h>
#include "backend.h"

/* Append `new_data` to the current selection (joined by `sep` when the current
 * selection is non-empty) and write the result back via `b->set`. On an empty
 * clipboard this is exactly a copy — no leading separator.
 *
 * Returns 0 on success, -1 on error. */
int clip_add(const clipboard_backend *b, const char *mime,
             const void *new_data, size_t new_len, const char *sep);

#endif /* CLIP_OPS_ADD_H */
