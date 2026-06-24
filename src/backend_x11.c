/* SPDX-License-Identifier: GPL-2.0-or-later */
/* backend_x11.c — X11 clipboard backend (Xlib + XFixes).
 *
 * The copying process IS the storage: on `set` we take ownership of the
 * CLIPBOARD selection, fork a persistent child, and answer SelectionRequest
 * events for as long as we own it. `get` is a clean one-shot requestor. See
 * DESIGN.md §3 (ownership model) and §7 (X11 notes).
 *
 * Reading is split into a small pipeline of intention-revealing helpers:
 *   request_targets -> choose_target -> convert_and_read -> read_selection_property
 * and writing splits the parent (handshake) from the child (owner daemon).
 *
 * Large payloads use the INCR protocol in BOTH directions: read_property_incr
 * on get, and begin_incr_send/continue_incr_send on serve, so data larger than
 * the server's max request size (~16 MB here) round-trips correctly instead of
 * silently truncating.
 */
#include "backend.h"
#include "proc_util.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

/* How long a one-shot read waits for the owner before giving up, so the CLI
 * never hangs forever on a misbehaving peer. */
#define SELECTION_TIMEOUT_MS   5000
#define INCR_CHUNK_TIMEOUT_MS  10000
#define RECV_INITIAL_CAP       4096

/* Fixed-capacity scratch arrays (clipboard target lists are tiny). */
#define MAX_OFFERED_TARGETS    64
#define MAX_GET_PREFERENCES    8
#define MAX_SERVED_TARGETS     8
/* Hard cap on total bytes accumulated from a single clipboard read. Guards
 * against a malicious clipboard owner triggering unbounded memory growth. */
#define RECV_MAX_BYTES         ((size_t)512 * 1024 * 1024)

static const char *x11_name(void) { return "x11"; }

/* Print an error and return -1, so error paths read as one line. */
static int fail(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    return -1;
}

/* ---- atoms ------------------------------------------------------------- */

typedef struct {
    Atom clipboard, targets, timestamp, multiple, incr;
    Atom utf8, tp_utf8, tp, text;   /* STRING is the predefined XA_STRING */
} atoms_t;

static void intern_atoms(Display *d, atoms_t *a)
{
    a->clipboard = XInternAtom(d, "CLIPBOARD", False);
    a->targets   = XInternAtom(d, "TARGETS", False);
    a->timestamp = XInternAtom(d, "TIMESTAMP", False);
    a->multiple  = XInternAtom(d, "MULTIPLE", False);
    a->incr      = XInternAtom(d, "INCR", False);
    a->utf8      = XInternAtom(d, "UTF8_STRING", False);
    a->tp_utf8   = XInternAtom(d, "text/plain;charset=utf-8", False);
    a->tp        = XInternAtom(d, "text/plain", False);
    a->text      = XInternAtom(d, "TEXT", False);
}

static int atom_in(Atom needle, const Atom *haystack, int n)
{
    for (int i = 0; i < n; i++)
        if (haystack[i] == needle)
            return 1;
    return 0;
}

/* A tiny, never-mapped window used to own the selection (set) or to receive
 * conversion replies (get). PropertyChangeMask is needed for INCR and for the
 * timestamp-capture trick. */
static Window create_agent_window(Display *d)
{
    Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
    XSelectInput(d, w, PropertyChangeMask);
    return w;
}

/* ---- event waiting ----------------------------------------------------- */

/* Block until the X connection is readable or the timeout elapses.
 * Returns 1 if readable (or interrupted — caller should retry), 0 on timeout. */
static int wait_readable(Display *d, int timeout_ms)
{
    int fd = ConnectionNumber(d);
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int r = select(fd + 1, &fds, NULL, NULL, &tv);
    if (r < 0)
        return (errno == EINTR) ? 1 : 0;
    return r > 0 ? 1 : 0;
}

/* Wait for the SelectionNotify that answers our XConvertSelection request. */
static int wait_selection_notify(Display *d, XSelectionEvent *out, int timeout_ms)
{
    for (;;) {
        while (XPending(d)) {
            XEvent ev;
            XNextEvent(d, &ev);
            if (ev.type == SelectionNotify) {
                *out = ev.xselection;
                return 0;
            }
        }
        if (!wait_readable(d, timeout_ms))
            return -1;
    }
}

/* Wait for the next INCR chunk to land on `prop`. */
static int wait_property_new_value(Display *d, Window w, Atom prop, int timeout_ms)
{
    for (;;) {
        while (XPending(d)) {
            XEvent ev;
            XNextEvent(d, &ev);
            if (ev.type == PropertyNotify && ev.xproperty.window == w &&
                ev.xproperty.atom == prop &&
                ev.xproperty.state == PropertyNewValue)
                return 0;
        }
        if (!wait_readable(d, timeout_ms))
            return -1;
    }
}

/* ---- reading the selection property ------------------------------------ */

/* Append `n` bytes to a grow-by-doubling buffer. Returns the (possibly moved)
 * buffer, or NULL after freeing the old one on OOM or overflow. */
static unsigned char *buf_append(unsigned char *buf, size_t *cap, size_t *len,
                                 const unsigned char *src, size_t n)
{
    if (n > SIZE_MAX - *len) {          /* addition would overflow size_t */
        free(buf);
        return NULL;
    }
    if (*len + n > *cap) {
        size_t ncap = *cap;
        do {
            if (ncap > SIZE_MAX / 2) {  /* doubling would overflow */
                free(buf);
                return NULL;
            }
            ncap *= 2;
        } while (*len + n > ncap);
        unsigned char *grown = realloc(buf, ncap);
        if (!grown) {
            free(buf);
            return NULL;
        }
        buf = grown;
        *cap = ncap;
    }
    memcpy(buf + *len, src, n);
    *len += n;
    return buf;
}

/* Read a property delivered all at once (the common case). */
static int read_property_whole(Display *d, Window w, Atom prop,
                               unsigned long nbytes, void **out, size_t *out_len)
{
    Atom type;
    int fmt;
    unsigned long nitems, after;
    unsigned char *val = NULL;
    long units = (long)(nbytes / 4) + 1;        /* XGetWindowProperty: 32-bit units */

    if (XGetWindowProperty(d, w, prop, 0, units, True, AnyPropertyType,
                           &type, &fmt, &nitems, &after, &val) != Success)
        return -1;

    /* fmt must be 8/16/32; guard against a rogue server returning something
     * unexpected, and against the multiplication overflowing size_t. */
    if (fmt != 8 && fmt != 16 && fmt != 32) {
        if (val) XFree(val);
        return -1;
    }
    size_t bytes_per_item = (size_t)(fmt / 8);  /* 1, 2, or 4 */
    if (nitems > SIZE_MAX / bytes_per_item) {
        if (val) XFree(val);
        return -1;
    }
    size_t len = nitems * bytes_per_item;
    void *data = malloc(len ? len : 1);
    if (!data) {
        if (val) XFree(val);
        return -1;
    }
    if (len && val)
        memcpy(data, val, len);
    if (val) XFree(val);

    *out = data;
    *out_len = len;
    return 0;
}

/* Read a property delivered incrementally via the INCR protocol: delete the
 * property to start the transfer, then accumulate chunks until a zero-length
 * property signals the end. */
static int read_property_incr(Display *d, Window w, Atom prop,
                              void **out, size_t *out_len)
{
    XDeleteProperty(d, w, prop);
    XFlush(d);

    size_t cap = RECV_INITIAL_CAP, len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf)
        return -1;

    for (;;) {
        if (wait_property_new_value(d, w, prop, INCR_CHUNK_TIMEOUT_MS) != 0) {
            free(buf);
            return -1;
        }
        Atom type;
        int fmt;
        unsigned long nitems, after;
        unsigned char *chunk = NULL;
        if (XGetWindowProperty(d, w, prop, 0, INT_MAX / 4, True, AnyPropertyType,
                               &type, &fmt, &nitems, &after, &chunk) != Success) {
            free(buf);
            return -1;
        }

        /* Zero nitems signals end of INCR transfer. */
        if (nitems == 0) {
            if (chunk) XFree(chunk);
            break;
        }
        /* Validate fmt; guard nitems * bytes_per_item against overflow. */
        if (fmt != 8 && fmt != 16 && fmt != 32) {
            if (chunk) XFree(chunk);
            free(buf);
            return -1;
        }
        size_t bytes_per_item = (size_t)(fmt / 8);
        if (nitems > SIZE_MAX / bytes_per_item) {
            if (chunk) XFree(chunk);
            free(buf);
            return -1;
        }
        size_t n = nitems * bytes_per_item;
        buf = buf_append(buf, &cap, &len, chunk, n);
        if (chunk) XFree(chunk);
        if (!buf)
            return -1;
        if (len > RECV_MAX_BYTES) {
            fprintf(stderr, "x11: clipboard data exceeds %zu-byte safety cap\n",
                    RECV_MAX_BYTES);
            free(buf);
            return -1;
        }
    }

    *out = buf;
    *out_len = len;
    return 0;
}

/* Read the converted selection from `prop`, transparently following INCR. */
static int read_selection_property(Display *d, Window w, Atom prop,
                                   Atom incr_atom, void **out, size_t *out_len)
{
    Atom type;
    int fmt;
    unsigned long nitems, after;
    unsigned char *val = NULL;

    /* Peek at the type and total size without consuming the data. */
    if (XGetWindowProperty(d, w, prop, 0, 0, False, AnyPropertyType,
                           &type, &fmt, &nitems, &after, &val) != Success)
        return -1;
    if (val) XFree(val);
    if (type == None)
        return -1;

    if (type == incr_atom)
        return read_property_incr(d, w, prop, out, out_len);
    return read_property_whole(d, w, prop, after, out, out_len);
}

/* ---- get (one-shot requestor) ------------------------------------------ */

/* Request `target`, wait for the reply, and read the resulting property. */
static int convert_and_read(Display *d, Window w, const atoms_t *a,
                            Atom target, Atom prop, void **out, size_t *out_len)
{
    XConvertSelection(d, a->clipboard, target, prop, w, CurrentTime);
    XFlush(d);

    XSelectionEvent reply;
    if (wait_selection_notify(d, &reply, SELECTION_TIMEOUT_MS) != 0)
        return -1;
    if (reply.property == None)
        return -1;                              /* owner refused this target */
    return read_selection_property(d, w, prop, a->incr, out, out_len);
}

/* Ask the owner which targets it offers; fill `offered`, return the count. */
static int request_targets(Display *d, Window w, const atoms_t *a, Atom prop,
                           Atom *offered, int max)
{
    XConvertSelection(d, a->clipboard, a->targets, prop, w, CurrentTime);
    XFlush(d);

    XSelectionEvent reply;
    if (wait_selection_notify(d, &reply, SELECTION_TIMEOUT_MS) != 0)
        return -1;
    if (reply.property == None)
        return -1;

    Atom type;
    int fmt;
    unsigned long nitems, after;
    unsigned char *val = NULL;
    if (XGetWindowProperty(d, w, prop, 0, 1024, True, XA_ATOM,
                           &type, &fmt, &nitems, &after, &val) != Success)
        return -1;

    int n = 0;
    if (val && fmt == 32) {
        Atom *list = (Atom *)val;               /* format 32 => array of Atom */
        for (unsigned long i = 0; i < nitems && n < max; i++)
            offered[n++] = list[i];
    }
    if (val) XFree(val);
    return n;
}

/* The text targets we prefer when reading, best first (user -t wins). */
static int build_get_preferences(Display *d, const atoms_t *a, const char *mime,
                                 Atom *pref, int max)
{
    int n = 0;
    if (mime && *mime && n < max) pref[n++] = XInternAtom(d, mime, False);
    if (n < max) pref[n++] = a->utf8;
    if (n < max) pref[n++] = a->tp_utf8;
    if (n < max) pref[n++] = a->tp;
    if (n < max) pref[n++] = XA_STRING;
    if (n < max) pref[n++] = a->text;
    return n;
}

static Atom choose_target(const Atom *pref, int npref,
                          const Atom *offered, int noffered)
{
    for (int i = 0; i < npref; i++)
        if (atom_in(pref[i], offered, noffered))
            return pref[i];
    return None;
}

/* Fallback for owners that don't advertise TARGETS: try common text atoms. */
static int read_without_targets(Display *d, Window w, const atoms_t *a,
                                const char *mime, Atom prop,
                                void **out, size_t *out_len)
{
    Atom candidates[4];
    int n = 0;
    if (mime && *mime) candidates[n++] = XInternAtom(d, mime, False);
    candidates[n++] = a->utf8;
    candidates[n++] = XA_STRING;
    candidates[n++] = a->tp;

    for (int i = 0; i < n; i++)
        if (convert_and_read(d, w, a, candidates[i], prop, out, out_len) == 0)
            return CLIP_GET_OK;
    return CLIP_GET_NO_TEXT;             /* owner present, but no text we can read */
}

/* Negotiate and read the current selection (caller already checked an owner
 * exists and owns a window). */
static int read_clipboard(Display *d, Window w, const atoms_t *a, Atom prop,
                          const char *mime, void **out, size_t *out_len)
{
    Atom offered[MAX_OFFERED_TARGETS];
    int n = request_targets(d, w, a, prop, offered, MAX_OFFERED_TARGETS);
    if (n <= 0)
        return read_without_targets(d, w, a, mime, prop, out, out_len);

    Atom pref[MAX_GET_PREFERENCES];
    int np = build_get_preferences(d, a, mime, pref, MAX_GET_PREFERENCES);
    Atom chosen = choose_target(pref, np, offered, n);
    if (chosen == None)
        return CLIP_GET_NO_TEXT;        /* owner offers no text target */
    if (convert_and_read(d, w, a, chosen, prop, out, out_len) != 0)
        return CLIP_GET_NO_TEXT;        /* advertised text, but couldn't read it */
    return CLIP_GET_OK;
}

static int x11_get(const char *mime, void **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;

    Display *d = XOpenDisplay(NULL);
    if (!d)
        return fail("x11: cannot open display");

    atoms_t a;
    intern_atoms(d, &a);
    Window w = create_agent_window(d);
    Atom prop = XInternAtom(d, "CLIP_RECV", False);

    int rc = 0;                                 /* no owner => empty clipboard */
    if (XGetSelectionOwner(d, a.clipboard) != None)
        rc = read_clipboard(d, w, &a, prop, mime, out, out_len);

    XDestroyWindow(d, w);
    XCloseDisplay(d);
    return rc;
}

/* ---- set (owner; forks and persists) ----------------------------------- */

/* INCR send: a payload larger than the server's max request size cannot go in a
 * single ChangeProperty, so we hand it over in chunks via the INCR protocol
 * (the send counterpart of read_property_incr). Each in-flight transfer tracks
 * how far we have delivered to one requestor. */
#define MAX_INCR_XFERS 8

typedef struct {
    Window requestor;
    Atom prop;
    Atom target;
    size_t offset;      /* bytes already delivered */
    int active;
} incr_xfer;

/* A backgrounded owner must survive a requestor vanishing mid-transfer
 * (BadWindow etc.), so it swallows X errors rather than letting Xlib abort. */
static int ignore_x_error(Display *d, XErrorEvent *e)
{
    (void)d;
    (void)e;
    return 0;
}

typedef struct {
    Display *d;
    Window win;
    atoms_t a;
    Atom served[MAX_SERVED_TARGETS];    /* targets that yield our bytes */
    int n_served;
    const unsigned char *data;
    size_t len;
    Time owner_time;
    int have_xfixes;
    int xfixes_event_base;
    size_t max_chunk;                   /* largest single-ChangeProperty payload */
    incr_xfer xfers[MAX_INCR_XFERS];
} owner_t;

/* The honest text set, plus any custom -t type, all serving the same bytes. */
static int build_served_targets(Display *d, const atoms_t *a, const char *mime,
                                Atom *buf, int max)
{
    int n = 0;
    buf[n++] = a->utf8;
    buf[n++] = a->tp_utf8;
    buf[n++] = a->tp;
    buf[n++] = XA_STRING;
    buf[n++] = a->text;
    if (mime && *mime) {
        Atom custom = XInternAtom(d, mime, False);
        if (!atom_in(custom, buf, n) && n < max)
            buf[n++] = custom;
    }
    return n;
}

/* Grab a real server timestamp via a zero-length property append, so we can
 * answer the TIMESTAMP target and own the selection with a valid time. */
static Time capture_timestamp(Display *d, Window w)
{
    Atom scratch = XInternAtom(d, "CLIP_TS", False);
    XChangeProperty(d, w, scratch, XA_STRING, 8, PropModeAppend,
                    (const unsigned char *)"", 0);
    for (;;) {
        XEvent ev;
        XNextEvent(d, &ev);
        if (ev.type == PropertyNotify && ev.xproperty.atom == scratch)
            return ev.xproperty.time;
    }
}

static incr_xfer *find_xfer(owner_t *o, Window requestor, Atom prop)
{
    for (int i = 0; i < MAX_INCR_XFERS; i++)
        if (o->xfers[i].active && o->xfers[i].requestor == requestor &&
            o->xfers[i].prop == prop)
            return &o->xfers[i];
    return NULL;
}

/* Start an INCR transfer: advertise the total size as type INCR and watch the
 * requestor's property for the deletes that pace the chunks. Returns 1 if
 * started, 0 if no slot is free (caller falls back to a single-shot attempt). */
static int begin_incr_send(owner_t *o, Window requestor, Atom prop, Atom target)
{
    incr_xfer *x = NULL;
    for (int i = 0; i < MAX_INCR_XFERS && !x; i++)
        if (!o->xfers[i].active)
            x = &o->xfers[i];
    if (!x)
        return 0;

    *x = (incr_xfer){ .requestor = requestor, .prop = prop, .target = target,
                      .offset = 0, .active = 1 };

    XSelectInput(o->d, requestor, PropertyChangeMask);
    /* INCR property is format=32; Xlib transmits only the low 32 bits on LP64.
     * Cap the hint at UINT32_MAX for payloads >4 GiB (a size hint, not exact). */
    unsigned long total = (o->len <= (size_t)UINT32_MAX)
                          ? (unsigned long)o->len
                          : (unsigned long)UINT32_MAX;
    XChangeProperty(o->d, requestor, prop, o->a.incr, 32, PropModeReplace,
                    (unsigned char *)&total, 1);
    return 1;
}

/* Push the next INCR chunk when the requestor deletes the property; a final
 * zero-length property signals the end of the transfer. */
static void continue_incr_send(owner_t *o, Window requestor, Atom prop)
{
    incr_xfer *x = find_xfer(o, requestor, prop);
    if (!x)
        return;

    size_t remaining = o->len - x->offset;
    size_t chunk = remaining < o->max_chunk ? remaining : o->max_chunk;
    /* XChangeProperty takes int for element count; chunk <= max_chunk which is
     * derived from the server's max request size (always << INT_MAX in practice,
     * but guard explicitly). */
    int nelt = (chunk <= (size_t)INT_MAX) ? (int)chunk : INT_MAX;
    XChangeProperty(o->d, requestor, prop, x->target, 8, PropModeReplace,
                    o->data + x->offset, nelt);
    XFlush(o->d);
    x->offset += chunk;

    if (chunk == 0) {                   /* terminator sent => transfer complete */
        XSelectInput(o->d, requestor, NoEventMask);
        x->active = 0;
    }
}

/* Fill the requestor's property and prepare the SelectionNotify reply. */
static void answer_target(owner_t *o, XSelectionRequestEvent *req, Atom prop)
{
    if (req->target == o->a.targets) {
        Atom list[2 + MAX_SERVED_TARGETS];
        int n = 0;
        list[n++] = o->a.targets;
        list[n++] = o->a.timestamp;
        for (int i = 0; i < o->n_served; i++)
            list[n++] = o->served[i];
        XChangeProperty(o->d, req->requestor, prop, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)list, n);
    } else if (req->target == o->a.timestamp) {
        XChangeProperty(o->d, req->requestor, prop, XA_INTEGER, 32,
                        PropModeReplace, (unsigned char *)&o->owner_time, 1);
    } else if (o->len > o->max_chunk &&
               begin_incr_send(o, req->requestor, prop, req->target)) {
        /* Large payload handed over incrementally (property set to INCR). */
    } else {
        /* A served text target that fits in a single request. o->len <= max_chunk
         * here (the INCR branch above fires otherwise), so fits in int. */
        int nelt = (o->len <= (size_t)INT_MAX) ? (int)o->len : INT_MAX;
        XChangeProperty(o->d, req->requestor, prop, req->target, 8,
                        PropModeReplace, o->data, nelt);
    }
}

static void serve_request(owner_t *o, XSelectionRequestEvent *req)
{
    XSelectionEvent reply = {0};
    reply.type = SelectionNotify;
    reply.display = req->display;
    reply.requestor = req->requestor;
    reply.selection = req->selection;
    reply.target = req->target;
    reply.time = req->time;
    /* Obsolete clients send property == None; reply onto the target atom. */
    Atom prop = req->property ? req->property : req->target;

    int can_serve = req->target == o->a.targets ||
                    req->target == o->a.timestamp ||
                    atom_in(req->target, o->served, o->n_served);
    if (can_serve) {
        answer_target(o, req, prop);
        reply.property = prop;
    } else {
        reply.property = None;                  /* refuse unknown targets */
    }

    XSendEvent(o->d, req->requestor, False, NoEventMask, (XEvent *)&reply);
    XFlush(o->d);
}

/* True once another client has taken the clipboard away from us. */
static int lost_ownership(owner_t *o, const XEvent *ev)
{
    if (ev->type == SelectionClear)
        return 1;
    if (o->have_xfixes &&
        ev->type == o->xfixes_event_base + XFixesSelectionNotify) {
        const XFixesSelectionNotifyEvent *fe =
            (const XFixesSelectionNotifyEvent *)ev;
        return fe->selection == o->a.clipboard && fe->owner != o->win;
    }
    return 0;
}

/* The owner's lifetime: answer requests until someone else takes over. */
static int serve_until_cleared(owner_t *o)
{
    for (;;) {
        XEvent ev;
        XNextEvent(o->d, &ev);
        if (ev.type == SelectionRequest)
            serve_request(o, &ev.xselectionrequest);
        else if (ev.type == PropertyNotify &&
                 ev.xproperty.state == PropertyDelete)
            continue_incr_send(o, ev.xproperty.window, ev.xproperty.atom);
        else if (lost_ownership(o, &ev))
            return 0;
    }
}

/* Open a connection, take CLIPBOARD ownership, and confirm we hold it. */
static int acquire_ownership(owner_t *o, const char *mime,
                             const void *data, size_t len)
{
    o->d = XOpenDisplay(NULL);
    if (!o->d)
        return fail("x11: cannot open display");

    intern_atoms(o->d, &o->a);
    Window root = DefaultRootWindow(o->d);
    o->win = create_agent_window(o->d);
    o->data = data;
    o->len = len;
    o->n_served = build_served_targets(o->d, &o->a, mime, o->served,
                                       MAX_SERVED_TARGETS);
    o->owner_time = capture_timestamp(o->d, o->win);

    /* Survive requestors that vanish mid-INCR rather than aborting the daemon. */
    XSetErrorHandler(ignore_x_error);

    /* Largest payload one ChangeProperty can carry (request size is in 4-byte
     * units; leave headroom for the request header). Above this we use INCR. */
    long max_req = XExtendedMaxRequestSize(o->d);
    if (max_req == 0)
        max_req = XMaxRequestSize(o->d);
    o->max_chunk = (size_t)(max_req - 64) * 4;

    int error_base = 0;
    o->have_xfixes = XFixesQueryExtension(o->d, &o->xfixes_event_base, &error_base);
    if (o->have_xfixes)
        XFixesSelectSelectionInput(o->d, root, o->a.clipboard,
                                   XFixesSetSelectionOwnerNotifyMask);

    XSetSelectionOwner(o->d, o->a.clipboard, o->win, o->owner_time);
    XSync(o->d, False);
    if (XGetSelectionOwner(o->d, o->a.clipboard) != o->win)
        return fail("x11: failed to acquire CLIPBOARD");
    return 0;
}

/* Child side of the fork: become the persistent owner, signal readiness, then
 * detach and serve forever. Never returns. */
static void run_as_owner_daemon(int ready_fd, const char *mime,
                                const void *data, size_t len)
{
    setsid();
    owner_t o = {0};
    if (acquire_ownership(&o, mime, data, len) != 0)
        _exit(1);                               /* no signal => parent sees EOF */
    if (signal_owner_ready(ready_fd) != 0)
        _exit(1);
    close(ready_fd);

    detach_from_terminal();
    serve_until_cleared(&o);
    _exit(0);
}

static int x11_set(const char *mime, const void *data, size_t len)
{
    if (clip_opt_foreground) {
        owner_t o = {0};
        if (acquire_ownership(&o, mime, data, len) != 0)
            return -1;
        return serve_until_cleared(&o);         /* blocks until SelectionClear */
    }

    /* The readiness handshake closes the "paste in the gap finds no owner"
     * race: the parent does not return success until the child owns it. */
    int handshake[2];
    if (pipe(handshake) != 0) {
        perror("x11: pipe");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("x11: fork");
        return -1;
    }

    if (pid == 0) {
        close(handshake[0]);
        run_as_owner_daemon(handshake[1], mime, data, len);     /* no return */
    }

    close(handshake[1]);
    if (wait_for_owner_ready(handshake[0]) != 0)
        return fail("x11: owner child failed to start");
    return 0;
}

/* ---- clear ------------------------------------------------------------- */

static int x11_clear(void)
{
    Display *d = XOpenDisplay(NULL);
    if (!d)
        return fail("x11: cannot open display");

    Atom clipboard = XInternAtom(d, "CLIPBOARD", False);
    XSetSelectionOwner(d, clipboard, None, CurrentTime);    /* relinquish */
    XSync(d, False);
    XCloseDisplay(d);
    return 0;
}

const clipboard_backend *backend_x11(void)
{
    static const clipboard_backend b = {
        .name = x11_name,
        .set = x11_set,
        .get = x11_get,
        .clear = x11_clear,
    };
    return &b;
}
