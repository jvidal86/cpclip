/* backend_null.c — a fake backend for testing the wiring with zero
 * display-server noise (DESIGN.md teaching beat, Phase 0).
 *
 * Unlike the real backends, the copying process is NOT the storage here: the
 * selection is persisted to a small per-user file so that separate cpclip /
 * cppaste / cpadd invocations round-trip without any fork or ownership
 * protocol. This deliberately sidesteps everything that makes X11/Wayland hard,
 * leaving only the vtable dispatch on display.
 *
 * On-disk format (binary-safe):
 *   [4 bytes LE: mime length N][N bytes: mime][rest of file: raw data bytes]
 */
#include "backend.h"
#include "io_util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The store begins with a little-endian uint32 holding the mime length. */
#define MIME_LEN_BYTES 4

static void put_u32_le(unsigned char b[MIME_LEN_BYTES], uint32_t v)
{
    b[0] = (unsigned char)(v & 0xff);
    b[1] = (unsigned char)((v >> 8) & 0xff);
    b[2] = (unsigned char)((v >> 16) & 0xff);
    b[3] = (unsigned char)((v >> 24) & 0xff);
}

static uint32_t get_u32_le(const unsigned char b[MIME_LEN_BYTES])
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static const char *null_store_path(void)
{
    static char path[PATH_MAX];
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir)
        dir = "/tmp";
    snprintf(path, sizeof path, "%s/cpclip-null.%u", dir, (unsigned)getuid());
    return path;
}

static const char *null_name(void)
{
    return "null";
}

static int null_set(const char *mime, const void *data, size_t len)
{
    if (!mime)
        mime = "text/plain";
    uint32_t mlen = (uint32_t)strlen(mime);

    int fd = open(null_store_path(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        perror("cpclip(null): open for write");
        return -1;
    }

    unsigned char hdr[MIME_LEN_BYTES];
    put_u32_le(hdr, mlen);

    int rc = 0;
    if (write_all(fd, hdr, sizeof hdr) != 0 ||
        write_all(fd, mime, mlen) != 0 ||
        (len && write_all(fd, data, len) != 0)) {
        perror("cpclip(null): write");
        rc = -1;
    }

    close(fd);
    return rc;
}

static int null_get(const char *mime, void **out, size_t *out_len)
{
    (void)mime;             /* single-slot store; the type is informational */
    *out = NULL;
    *out_len = 0;

    int fd = open(null_store_path(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT)
            return 0;       /* empty clipboard */
        perror("cpclip(null): open for read");
        return -1;
    }

    void *all = NULL;
    size_t all_len = 0;
    int rc = read_all_fd(fd, &all, &all_len, -1);
    close(fd);
    if (rc != 0)
        return -1;

    if (all_len < MIME_LEN_BYTES) {     /* empty or truncated => treat as empty */
        free(all);
        return 0;
    }

    const unsigned char *p = (const unsigned char *)all;
    size_t mlen = get_u32_le(p);
    if (MIME_LEN_BYTES + mlen > all_len) {
        free(all);
        return 0;
    }

    size_t dlen = all_len - MIME_LEN_BYTES - mlen;
    void *data = malloc(dlen ? dlen : 1);
    if (!data) {
        free(all);
        return -1;
    }
    if (dlen)
        memcpy(data, p + MIME_LEN_BYTES + mlen, dlen);
    free(all);

    *out = data;
    *out_len = dlen;
    return 0;
}

static int null_clear(void)
{
    if (unlink(null_store_path()) != 0 && errno != ENOENT) {
        perror("cpclip(null): unlink");
        return -1;
    }
    return 0;
}

const clipboard_backend *backend_null(void)
{
    static const clipboard_backend b = {
        .name = null_name,
        .set = null_set,
        .get = null_get,
        .clear = null_clear,
    };
    return &b;
}
