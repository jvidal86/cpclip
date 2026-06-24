/* SPDX-License-Identifier: GPL-2.0-or-later */
/* main.c — argv[0] dispatch, per-command flag parsing, backend detection.
 *
 * One binary with four faces, selected by the name it is invoked as (the
 * BusyBox / gzip-gunzip-zcat pattern). See DESIGN.md §4.
 */
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "backend.h"
#include "io_util.h"
#include "ops_add.h"
#include "parse_util.h"

/* Injected by the Makefile (-DCPCLIP_VERSION); fallback for ad-hoc builds. */
#ifndef CPCLIP_VERSION
#define CPCLIP_VERSION "dev"
#endif

/* Set from -f/--foreground; read by forking backends (Phase 1+). */
int clip_opt_foreground = 0;

/* Exit status for a command-line mistake (distinct from a runtime failure). */
#define EXIT_USAGE 2

/* Default cap on a single copy/add (10 MiB); overridable with --maxmem, and
 * --maxmem 0 disables it. The owner holds the whole payload in memory, so this
 * guards against an accidental huge input pinning RAM in the background owner. */
#define CLIP_DEFAULT_MAXMEM ((size_t)10 * 1024 * 1024)

typedef enum { VERB_COPY, VERB_ADD, VERB_PASTE, VERB_CLEAR } verb_t;

typedef enum { BK_NONE, BK_NULL, BK_X11, BK_WAYLAND } backend_kind;

/* Outcome of parsing argv: run the command, exit cleanly after --help, or
 * exit with a usage error. */
typedef enum { PARSE_OK, PARSE_HELP, PARSE_VERSION, PARSE_USAGE_ERROR } parse_result;

typedef struct {
    const char *mime;       /* -t/--type; NULL => backend default text set */
    const char *separator;  /* --separator (add); default "\n" */
    const char *backend;    /* --backend; NULL => auto */
    const char *text;       /* positional TEXT, or NULL => read stdin */
    size_t max_mem;         /* -m/--maxmem byte cap (copy/add); 0 => unlimited */
    int have_text;
    int foreground;         /* -f */
    int no_newline;         /* -n (paste) */
} options;

static const char *verb_name(verb_t v)
{
    switch (v) {
    case VERB_COPY:  return "cpclip";
    case VERB_ADD:   return "cpadd";
    case VERB_PASTE: return "cppaste";
    case VERB_CLEAR: return "cpclear";
    }
    return "cpclip";
}

static void usage(verb_t v, FILE *f)
{
    switch (v) {
    case VERB_COPY:
        fprintf(f,
            "Usage: cpclip [TEXT] [-t MIME] [-f] [--backend NAME]\n"
            "  Capture stdin (or TEXT) to the clipboard, replacing it.\n"
            "  Mirrors stdin to stdout when stdout is a TTY.\n"
            "    -t, --type MIME    content type to advertise\n"
            "    -f, --foreground   stay in foreground instead of forking\n"
            "    -m, --maxmem SIZE  max input size, e.g. 200M (default 10M; 0=off)\n"
            "        --backend NAME x11 | wayland | null | auto (default auto)\n"
            "    -h, --help\n"
            "    -V, --version\n");
        break;
    case VERB_ADD:
        fprintf(f,
            "Usage: cpadd [TEXT] [-t MIME] [--separator STR] [-f] [--backend NAME]\n"
            "  Append stdin (or TEXT) to the clipboard (copy if empty).\n"
            "  Mirrors stdin to stdout when stdout is a TTY.\n"
            "    -t, --type MIME    content type\n"
            "        --separator STR joiner between entries (default newline)\n"
            "    -f, --foreground   stay in foreground instead of forking\n"
            "    -m, --maxmem SIZE  max input size, e.g. 200M (default 10M; 0=off)\n"
            "        --backend NAME x11 | wayland | null | auto (default auto)\n"
            "    -h, --help\n"
            "    -V, --version\n");
        break;
    case VERB_PASTE:
        fprintf(f,
            "Usage: cppaste [-t MIME] [-n] [--backend NAME]\n"
            "  Write the clipboard to stdout.\n"
            "    -t, --type MIME    preferred content type\n"
            "    -n, --no-newline   do not append a trailing newline\n"
            "        --backend NAME x11 | wayland | null | auto (default auto)\n"
            "    -h, --help\n"
            "    -V, --version\n");
        break;
    case VERB_CLEAR:
        fprintf(f,
            "Usage: cpclear [--backend NAME]\n"
            "  Empty the clipboard (relinquish ownership).\n"
            "        --backend NAME x11 | wayland | null | auto (default auto)\n"
            "    -h, --help\n"
            "    -V, --version\n");
        break;
    }
}

/* argv[0] basename without modifying the argument (POSIX basename() may). */
static const char *prog_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int dispatch_verb(const char *prog, verb_t *out)
{
    if (!strcmp(prog, "cpclip"))  { *out = VERB_COPY;  return 0; }
    if (!strcmp(prog, "cpadd"))   { *out = VERB_ADD;   return 0; }
    if (!strcmp(prog, "cppaste")) { *out = VERB_PASTE; return 0; }
    if (!strcmp(prog, "cpclear")) { *out = VERB_CLEAR; return 0; }
    return -1;
}

static backend_kind detect_backend(void)
{
    const char *w = getenv("WAYLAND_DISPLAY");
    if (w && *w)
        return BK_WAYLAND;
    const char *x = getenv("DISPLAY");
    if (x && *x)
        return BK_X11;
    return BK_NONE;
}

static const clipboard_backend *resolve_backend(const char *name)
{
    backend_kind k;
    if (!name || !strcmp(name, "auto"))
        k = detect_backend();
    else if (!strcmp(name, "null"))
        k = BK_NULL;
    else if (!strcmp(name, "x11"))
        k = BK_X11;
    else if (!strcmp(name, "wayland"))
        k = BK_WAYLAND;
    else {
        fprintf(stderr, "unknown backend '%s' (x11 | wayland | null | auto)\n", name);
        return NULL;
    }

    switch (k) {
    case BK_NULL:
        return backend_null();
    case BK_X11:
        return backend_x11();
    case BK_WAYLAND:
        return backend_wayland();
    case BK_NONE:
    default:
        fprintf(stderr, "no display server; clipboard unavailable in this session\n");
        return NULL;
    }
}

enum { OPT_BACKEND = 256, OPT_SEPARATOR };

/* Which flags appeared on the command line (for per-command rejection). */
typedef struct {
    int type, foreground, no_newline, separator, maxmem;
} seen_flags;

/* Reject flags that do not apply to this command. Returns PARSE_OK or
 * PARSE_USAGE_ERROR. */
static parse_result check_flag_applicability(verb_t verb, const seen_flags *seen)
{
    const char *me = verb_name(verb);
    if (seen->no_newline && verb != VERB_PASTE)
        return fprintf(stderr, "%s: -n/--no-newline applies only to cppaste\n", me),
               PARSE_USAGE_ERROR;
    if (seen->separator && verb != VERB_ADD)
        return fprintf(stderr, "%s: --separator applies only to cpadd\n", me),
               PARSE_USAGE_ERROR;
    if (seen->foreground && verb != VERB_COPY && verb != VERB_ADD)
        return fprintf(stderr, "%s: -f/--foreground applies only to cpclip/cpadd\n", me),
               PARSE_USAGE_ERROR;
    if (seen->type && verb == VERB_CLEAR)
        return fprintf(stderr, "%s: -t/--type does not apply to cpclear\n", me),
               PARSE_USAGE_ERROR;
    if (seen->maxmem && verb != VERB_COPY && verb != VERB_ADD)
        return fprintf(stderr, "%s: -m/--maxmem applies only to cpclip/cpadd\n", me),
               PARSE_USAGE_ERROR;
    return PARSE_OK;
}

/* Take the optional positional TEXT argument (copy/add accept one; the others
 * accept none). Returns PARSE_OK or PARSE_USAGE_ERROR. */
static parse_result take_positional(verb_t verb, int argc, char **argv,
                                    options *opt)
{
    const char *me = verb_name(verb);
    int extra = argc - optind;

    if (verb != VERB_COPY && verb != VERB_ADD) {
        if (extra > 0)
            return fprintf(stderr, "%s: unexpected argument '%s'\n", me, argv[optind]),
                   PARSE_USAGE_ERROR;
        return PARSE_OK;
    }

    if (extra > 1)
        return fprintf(stderr, "%s: too many arguments\n", me), PARSE_USAGE_ERROR;
    if (extra == 1) {
        opt->text = argv[optind];
        opt->have_text = 1;
    }
    return PARSE_OK;
}

/* Parse argv for `verb` into `opt`. */
static parse_result parse_args(verb_t verb, int argc, char **argv, options *opt)
{
    static const struct option longopts[] = {
        {"type",       required_argument, 0, 't'},
        {"foreground", no_argument,       0, 'f'},
        {"no-newline", no_argument,       0, 'n'},
        {"separator",  required_argument, 0, OPT_SEPARATOR},
        {"backend",    required_argument, 0, OPT_BACKEND},
        {"maxmem",     required_argument, 0, 'm'},
        {"help",       no_argument,       0, 'h'},
        {"version",    no_argument,       0, 'V'},
        {0, 0, 0, 0},
    };

    seen_flags seen = {0};
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "t:fnhVm:", longopts, NULL)) != -1) {
        switch (c) {
        case 't': opt->mime = optarg; seen.type = 1; break;
        case 'f': opt->foreground = 1; seen.foreground = 1; break;
        case 'n': opt->no_newline = 1; seen.no_newline = 1; break;
        case OPT_SEPARATOR: opt->separator = optarg; seen.separator = 1; break;
        case OPT_BACKEND: opt->backend = optarg; break;
        case 'm':
            if (parse_size(optarg, &opt->max_mem) != 0) {
                fprintf(stderr, "%s: invalid --maxmem value '%s'\n",
                        verb_name(verb), optarg);
                return PARSE_USAGE_ERROR;
            }
            seen.maxmem = 1;
            break;
        case 'V': printf("cpclip %s\n", CPCLIP_VERSION); return PARSE_VERSION;
        case 'h': usage(verb, stdout); return PARSE_HELP;
        default: usage(verb, stderr); return PARSE_USAGE_ERROR; /* getopt explained */
        }
    }

    parse_result pr = check_flag_applicability(verb, &seen);
    if (pr != PARSE_OK)
        return pr;
    return take_positional(verb, argc, argv, opt);
}

/* Collect the input for copy/add: either the TEXT arg or stdin (mirrored to a
 * TTY stdout). On success sets data, len, and owned (non-NULL if heap-owned). */
static void report_too_large(verb_t verb, size_t limit)
{
    fprintf(stderr, "%s: input exceeds the %zu-byte (~%.1f MiB) limit; raise it "
                    "with --maxmem (e.g. --maxmem 200M) or disable with --maxmem 0\n",
            verb_name(verb), limit, (double)limit / (1024.0 * 1024.0));
}

static int collect_input(const options *opt, verb_t verb,
                         const void **data, size_t *len, void **owned)
{
    *owned = NULL;
    if (opt->have_text) {
        size_t tlen = strlen(opt->text);
        if (opt->max_mem && tlen > opt->max_mem) {
            report_too_large(verb, opt->max_mem);
            return -1;
        }
        *data = opt->text;
        *len = tlen;
        return 0;
    }

    int mirror = isatty(STDOUT_FILENO) ? STDOUT_FILENO : -1;
    void *buf = NULL;
    size_t n = 0;
    int rc = read_all_fd(STDIN_FILENO, &buf, &n, mirror, opt->max_mem);
    if (rc == CLIP_READ_TOO_LARGE) {
        report_too_large(verb, opt->max_mem);
        return -1;
    }
    if (rc != 0) {
        fprintf(stderr, "%s: failed to read stdin\n", verb_name(verb));
        return -1;
    }
    *data = buf;
    *len = n;
    *owned = buf;
    return 0;
}

static int do_copy(const clipboard_backend *b, const options *opt, verb_t verb)
{
    const void *data;
    size_t len;
    void *owned;
    if (collect_input(opt, verb, &data, &len, &owned) != 0)
        return 1;

    int rc = b->set(opt->mime, data, len);
    free(owned);
    return rc == 0 ? 0 : 1;
}

static int do_add(const clipboard_backend *b, const options *opt, verb_t verb)
{
    const void *data;
    size_t len;
    void *owned;
    if (collect_input(opt, verb, &data, &len, &owned) != 0)
        return 1;

    const char *sep = opt->separator ? opt->separator : "\n";
    int rc = clip_add(b, opt->mime, data, len, sep, opt->max_mem);
    free(owned);
    return rc == 0 ? 0 : 1;
}

static int do_paste(const clipboard_backend *b, const options *opt)
{
    void *data = NULL;
    size_t len = 0;
    int status = b->get(opt->mime, &data, &len);
    if (status == CLIP_GET_NO_TEXT) {
        fprintf(stderr, "cppaste: clipboard has no text content\n");
        return 1;
    }
    if (status != CLIP_GET_OK)
        return 1;                       /* infrastructure error, already reported */

    int rc = 0;
    if (len && write_all(STDOUT_FILENO, data, len) != 0)
        rc = 1;

    /* Default: append a trailing newline only if the data lacks one, so terminal
     * output is tidy and round-trips do not accumulate blank lines. -n suppresses
     * it entirely (use it for exact binary output). */
    if (rc == 0 && !opt->no_newline &&
        !(len && ((const char *)data)[len - 1] == '\n')) {
        if (write_all(STDOUT_FILENO, "\n", 1) != 0)
            rc = 1;
    }

    free(data);
    return rc;
}

static int do_clear(const clipboard_backend *b)
{
    return b->clear() == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    /* A reader closing early (e.g. `cppaste | head -1`) is a clean exit. */
    signal(SIGPIPE, SIG_IGN);

    verb_t verb;
    if (dispatch_verb(prog_basename(argv[0]), &verb) != 0) {
        fprintf(stderr, "invoke as cpclip, cpadd, cppaste, or cpclear\n");
        return EXIT_USAGE;
    }

    options opt = {0};
    opt.max_mem = CLIP_DEFAULT_MAXMEM;      /* --maxmem may override below */
    parse_result pr = parse_args(verb, argc, argv, &opt);
    if (pr == PARSE_HELP || pr == PARSE_VERSION)
        return 0;
    if (pr != PARSE_OK)
        return EXIT_USAGE;

    clip_opt_foreground = opt.foreground;

    const clipboard_backend *b = resolve_backend(opt.backend);
    if (!b)
        return 1;

    switch (verb) {
    case VERB_COPY:  return do_copy(b, &opt, verb);
    case VERB_ADD:   return do_add(b, &opt, verb);
    case VERB_PASTE: return do_paste(b, &opt);
    case VERB_CLEAR: return do_clear(b);
    }
    return 1;
}
