/* backend.h — the clipboard backend interface.
 *
 * One abstraction, several implementations. This struct of function pointers
 * IS the polymorphism: the CLI in main.c never knows which backend is live.
 * Every payload is (void *, size_t) — never a C string — so NUL bytes and
 * arbitrary binary survive end to end.
 *
 * See DESIGN.md §5.1.
 */
#ifndef CLIP_BACKEND_H
#define CLIP_BACKEND_H

#include <stddef.h>

typedef struct {
    /* Human-readable backend name, e.g. "x11", "wayland", "null". */
    const char *(*name)(void);

    /* Become the clipboard owner serving `mime`/`data`. Real backends fork a
     * persistent child here (the process IS the storage); the null backend
     * just writes a file. Returns 0 on success, -1 on error. */
    int (*set)(const char *mime, const void *data, size_t len);

    /* Fetch the current selection as `mime` (or best text type) into a freshly
     * malloc'd buffer the caller must free. Empty clipboard => *out=NULL,
     * *out_len=0, return 0. Returns -1 on error. */
    int (*get)(const char *mime, void **out, size_t *out_len);

    /* Relinquish ownership entirely; the clipboard becomes empty/unowned. */
    int (*clear)(void);
} clipboard_backend;

/* Backend selectors. Each returns a pointer to a static vtable instance.
 * Implemented per phase; absent ones are wired in resolve_backend() in main.c. */
const clipboard_backend *backend_null(void);     /* Phase 0 */
const clipboard_backend *backend_x11(void);      /* Phase 1 */
/* const clipboard_backend *backend_wayland(void); Phase 2 */

/* Set by main.c from -f/--foreground; consulted by forking backends (Phase 1+)
 * to block in the foreground instead of detaching. Harmless for the null
 * backend, which never forks. */
extern int clip_opt_foreground;

#endif /* CLIP_BACKEND_H */
