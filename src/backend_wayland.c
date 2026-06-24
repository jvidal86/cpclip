/* backend_wayland.c — Wayland clipboard backend via ext-data-control.
 *
 * We use ext_data_control_manager_v1 rather than the core wl_data_device.
 * Rationale: the core protocol's set_selection requires an input-event serial,
 * which a headless CLI can only obtain by briefly mapping a focusable surface.
 * ext-data-control is the freedesktop-standardized protocol built for exactly
 * this case — clipboard managers and command-line tools that own/read the
 * selection without being a focused application. No serial, no surface, no
 * window flash. (This is also what wl-copy/wl-paste use.)
 *
 * The ownership model is the same as X11 (DESIGN.md §3): on `set` the process
 * becomes the selection owner and must stay alive to answer send requests.
 *
 * THE gotcha (DESIGN.md §7, "known risks"): a backgrounded owner MUST run a
 * dispatch loop. Without it the copy appears to succeed but every paste returns
 * empty, because nothing answers ext_data_control_source.send. See
 * serve_until_cancelled().
 */
#include "backend.h"
#include "io_util.h"
#include "proc_util.h"
#include "ext-data-control-v1-client-protocol.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

static const char *wl_backend_name(void) { return "wayland"; }

static int fail(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    return -1;
}

/* The honest text set we offer on copy / prefer on paste, best first. The
 * X11-style names (UTF8_STRING, STRING, TEXT) help XWayland interop. */
static const char *const TEXT_MIMES[] = {
    "text/plain;charset=utf-8",
    "text/plain",
    "UTF8_STRING",
    "STRING",
    "TEXT",
};
#define N_TEXT_MIMES ((int)(sizeof TEXT_MIMES / sizeof TEXT_MIMES[0]))

/* ---- connection + globals ---------------------------------------------- */

typedef struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_seat *seat;
    struct ext_data_control_manager_v1 *manager;
} wl_conn;

static void on_global(void *data, struct wl_registry *reg, uint32_t name,
                      const char *iface, uint32_t version)
{
    (void)version;
    wl_conn *c = data;
    if (strcmp(iface, ext_data_control_manager_v1_interface.name) == 0)
        c->manager = wl_registry_bind(reg, name,
                                      &ext_data_control_manager_v1_interface, 1);
    else if (strcmp(iface, wl_seat_interface.name) == 0)
        c->seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
}

static void on_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data;
    (void)reg;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = on_global,
    .global_remove = on_global_remove,
};

static int wl_conn_open(wl_conn *c)
{
    memset(c, 0, sizeof *c);
    c->display = wl_display_connect(NULL);
    if (!c->display)
        return fail("wayland: cannot connect to compositor");

    c->registry = wl_display_get_registry(c->display);
    wl_registry_add_listener(c->registry, &registry_listener, c);
    wl_display_roundtrip(c->display);           /* learn the advertised globals */

    if (!c->manager)
        return fail("wayland: compositor has no ext-data-control support");
    if (!c->seat)
        return fail("wayland: no wl_seat available");
    return 0;
}

static void wl_conn_close(wl_conn *c)
{
    if (c->manager) ext_data_control_manager_v1_destroy(c->manager);
    if (c->seat) wl_seat_destroy(c->seat);
    if (c->registry) wl_registry_destroy(c->registry);
    if (c->display) wl_display_disconnect(c->display);
}

/* ---- a growable list of mime strings (one per offer) -------------------- */

typedef struct {
    char **items;
    int count;
    int cap;
} mime_list;

static void mime_list_add(mime_list *l, const char *s)
{
    if (l->count == l->cap) {
        int ncap = l->cap ? l->cap * 2 : 8;
        char **grown = realloc(l->items, (size_t)ncap * sizeof *grown);
        if (!grown)
            return;                             /* drop on OOM; non-fatal */
        l->items = grown;
        l->cap = ncap;
    }
    char *copy = strdup(s);
    if (copy)
        l->items[l->count++] = copy;
}

static int mime_list_has(const mime_list *l, const char *s)
{
    for (int i = 0; i < l->count; i++)
        if (strcmp(l->items[i], s) == 0)
            return 1;
    return 0;
}

static void mime_list_free(mime_list *l)
{
    if (!l)
        return;
    for (int i = 0; i < l->count; i++)
        free(l->items[i]);
    free(l->items);
    free(l);
}

/* ---- get (one-shot read) ----------------------------------------------- */

/* Each offer accumulates its advertised mime types into a mime_list stored as
 * the offer's user_data. */
static void on_offer_mime(void *data, struct ext_data_control_offer_v1 *offer,
                          const char *mime)
{
    (void)offer;
    mime_list_add((mime_list *)data, mime);
}

static const struct ext_data_control_offer_v1_listener offer_listener = {
    .offer = on_offer_mime,
};

#define MAX_OFFERS 8

typedef struct {
    struct ext_data_control_offer_v1 *offers[MAX_OFFERS];
    int n_offers;
    struct ext_data_control_offer_v1 *selection;   /* clipboard offer, or NULL */
} get_state;

static void on_data_offer(void *data, struct ext_data_control_device_v1 *dev,
                          struct ext_data_control_offer_v1 *offer)
{
    (void)dev;
    get_state *g = data;
    mime_list *l = calloc(1, sizeof *l);
    if (l)
        ext_data_control_offer_v1_add_listener(offer, &offer_listener, l);
    if (g->n_offers < MAX_OFFERS)
        g->offers[g->n_offers++] = offer;
}

static void on_selection(void *data, struct ext_data_control_device_v1 *dev,
                         struct ext_data_control_offer_v1 *offer)
{
    (void)dev;
    ((get_state *)data)->selection = offer;     /* NULL => empty clipboard */
}

static void on_finished(void *data, struct ext_data_control_device_v1 *dev)
{
    (void)data;
    (void)dev;
}

static void on_primary_selection(void *data, struct ext_data_control_device_v1 *dev,
                                 struct ext_data_control_offer_v1 *offer)
{
    (void)data;
    (void)dev;
    (void)offer;                                /* primary selection unused */
}

static const struct ext_data_control_device_v1_listener device_listener = {
    .data_offer = on_data_offer,
    .selection = on_selection,
    .finished = on_finished,
    .primary_selection = on_primary_selection,
};

static mime_list *offer_mimes(struct ext_data_control_offer_v1 *offer)
{
    return (mime_list *)wl_proxy_get_user_data((struct wl_proxy *)offer);
}

/* Pick the best offered text type (user -t wins, then our defaults). */
static const char *choose_mime(const mime_list *l, const char *user)
{
    if (user && *user && mime_list_has(l, user))
        return user;
    for (int i = 0; i < N_TEXT_MIMES; i++)
        if (mime_list_has(l, TEXT_MIMES[i]))
            return TEXT_MIMES[i];
    return NULL;
}

/* Ask the owner to write the selection to a pipe, then read it to EOF. The
 * owner (another process — our own daemon, or Klipper) does the writing from
 * its dispatch loop, which is why that loop must exist. */
static int receive_offer(struct wl_display *d,
                         struct ext_data_control_offer_v1 *offer,
                         const char *mime, void **out, size_t *out_len)
{
    int fds[2];
    if (pipe(fds) != 0)
        return fail("wayland: pipe");

    ext_data_control_offer_v1_receive(offer, mime, fds[1]);
    wl_display_flush(d);                        /* push the request to the server */
    close(fds[1]);                              /* the owner holds the write end */

    int rc = read_all_fd(fds[0], out, out_len, -1);
    close(fds[0]);
    return rc == 0 ? 0 : -1;
}

static void get_state_cleanup(get_state *g)
{
    for (int i = 0; i < g->n_offers; i++) {
        mime_list_free(offer_mimes(g->offers[i]));
        ext_data_control_offer_v1_destroy(g->offers[i]);
    }
}

static int wl_get(const char *mime, void **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;

    wl_conn c;
    if (wl_conn_open(&c) != 0)
        return -1;

    struct ext_data_control_device_v1 *dev =
        ext_data_control_manager_v1_get_data_device(c.manager, c.seat);
    get_state g = {0};
    ext_data_control_device_v1_add_listener(dev, &device_listener, &g);
    wl_display_roundtrip(c.display);            /* receive offers + selection */

    int rc = 0;                                 /* no selection => empty */
    if (g.selection) {
        const char *chosen = choose_mime(offer_mimes(g.selection), mime);
        if (!chosen)
            rc = fail("wayland: clipboard offers no text type");
        else
            rc = receive_offer(c.display, g.selection, chosen, out, out_len);
    }

    get_state_cleanup(&g);
    ext_data_control_device_v1_destroy(dev);
    wl_conn_close(&c);
    return rc;
}

/* ---- set (owner; forks and persists) ----------------------------------- */

typedef struct {
    const unsigned char *data;
    size_t len;
    int cancelled;
} source_state;

static void on_source_send(void *data, struct ext_data_control_source_v1 *src,
                           const char *mime, int32_t fd)
{
    (void)src;
    (void)mime;                                 /* same bytes for every type */
    source_state *s = data;
    write_all(fd, s->data, s->len);
    close(fd);
}

static void on_source_cancelled(void *data, struct ext_data_control_source_v1 *src)
{
    ((source_state *)data)->cancelled = 1;      /* another client took over */
    ext_data_control_source_v1_destroy(src);
}

static const struct ext_data_control_source_v1_listener source_listener = {
    .send = on_source_send,
    .cancelled = on_source_cancelled,
};

static int mime_is_default(const char *mime)
{
    for (int i = 0; i < N_TEXT_MIMES; i++)
        if (strcmp(mime, TEXT_MIMES[i]) == 0)
            return 1;
    return 0;
}

static void offer_text_mimes(struct ext_data_control_source_v1 *src,
                             const char *custom)
{
    for (int i = 0; i < N_TEXT_MIMES; i++)
        ext_data_control_source_v1_offer(src, TEXT_MIMES[i]);

    if (custom && *custom && !mime_is_default(custom))
        ext_data_control_source_v1_offer(src, custom);
}

typedef struct {
    wl_conn conn;
    struct ext_data_control_device_v1 *device;
    struct ext_data_control_source_v1 *source;
    source_state src;
} owner;

static int acquire_ownership(owner *o, const char *mime,
                             const void *data, size_t len)
{
    if (wl_conn_open(&o->conn) != 0)
        return -1;

    o->device = ext_data_control_manager_v1_get_data_device(o->conn.manager,
                                                            o->conn.seat);
    o->source = ext_data_control_manager_v1_create_data_source(o->conn.manager);
    o->src.data = data;
    o->src.len = len;
    o->src.cancelled = 0;
    ext_data_control_source_v1_add_listener(o->source, &source_listener, &o->src);
    offer_text_mimes(o->source, mime);
    ext_data_control_device_v1_set_selection(o->device, o->source);

    if (wl_display_roundtrip(o->conn.display) < 0)
        return fail("wayland: failed to set selection");
    return 0;
}

/* THE Wayland gotcha: keep dispatching so we can answer source.send. Exits when
 * another client takes the selection (cancelled) or the connection drops. */
static int serve_until_cancelled(owner *o)
{
    while (!o->src.cancelled)
        if (wl_display_dispatch(o->conn.display) < 0)
            break;                              /* connection lost */
    return 0;
}

static void run_as_owner_daemon(int ready_fd, const char *mime,
                                const void *data, size_t len)
{
    setsid();
    owner o = {0};
    if (acquire_ownership(&o, mime, data, len) != 0)
        _exit(1);                               /* no signal => parent sees EOF */
    if (signal_owner_ready(ready_fd) != 0)
        _exit(1);
    close(ready_fd);

    detach_from_terminal();
    serve_until_cancelled(&o);
    _exit(0);
}

static int wl_set(const char *mime, const void *data, size_t len)
{
    if (clip_opt_foreground) {
        owner o = {0};
        if (acquire_ownership(&o, mime, data, len) != 0)
            return -1;
        return serve_until_cancelled(&o);       /* blocks until cancelled */
    }

    /* Same readiness handshake as X11: the parent returns success only once the
     * child actually owns the selection. */
    int handshake[2];
    if (pipe(handshake) != 0) {
        perror("wayland: pipe");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("wayland: fork");
        return -1;
    }

    if (pid == 0) {
        close(handshake[0]);
        run_as_owner_daemon(handshake[1], mime, data, len);     /* no return */
    }

    close(handshake[1]);
    if (wait_for_owner_ready(handshake[0]) != 0)
        return fail("wayland: owner child failed to start");
    return 0;
}

/* ---- clear ------------------------------------------------------------- */

static int wl_clear(void)
{
    wl_conn c;
    if (wl_conn_open(&c) != 0)
        return -1;

    struct ext_data_control_device_v1 *dev =
        ext_data_control_manager_v1_get_data_device(c.manager, c.seat);
    ext_data_control_device_v1_set_selection(dev, NULL);    /* relinquish */
    wl_display_roundtrip(c.display);

    ext_data_control_device_v1_destroy(dev);
    wl_conn_close(&c);
    return 0;
}

const clipboard_backend *backend_wayland(void)
{
    static const clipboard_backend b = {
        .name = wl_backend_name,
        .set = wl_set,
        .get = wl_get,
        .clear = wl_clear,
    };
    return &b;
}
