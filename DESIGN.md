# `clip` — Design Document

A single CLI clipboard tool for both X11 and Wayland, with one identical
command interface. Captures process output to the clipboard and serves the
clipboard back to process input. Adds a composed `add` (append) operation that
neither display server provides natively.

---

## 1. Goals

- One binary, one CLI, two display-server backends (X11, Wayland).
- Core verbs: `copy`, `paste`, `add`, `clear`.
- `add` accumulates outputs across several commands into one clipboard entry.
- Behave like `wl-copy`: writes fork into the background and persist.
- Integrate with clipboard managers (e.g. KDE Klipper) for free, by
  implementing the selection protocols correctly — no manager-specific code.
- Binary-safe throughout (NUL bytes and arbitrary bytes survive).

## 2. Non-Goals

- **Primary selection** (middle-click / highlight). Dropped for leanness.
- **Secondary selection.** Does not exist on Wayland; never exposed.
- **Clipboard history / watch mode.** Would require the data-control protocol
  family; out of scope. Left as a future note only.
- macOS / Windows backends.

## 3. The Ownership Model (root concept)

X11 and Wayland have **no system-owned clipboard buffer**. The copying process
*is* the storage:

- On **copy**, the process announces ownership of the clipboard and advertises
  the MIME types it can serve. The bytes stay inside the process.
- On **paste** (CLI or GUI Ctrl+V), the requesting app asks the server who owns
  the clipboard, is handed a pipe to the owner, and the owner streams the bytes
  on demand.

Consequences that shape the whole design:

- A **copy must fork and persist** — if the process exits, the data is gone,
  because the process *was* the storage. This is why `wl-copy`/`xclip` linger.
- A **paste is a clean one-shot** — request, stream, exit. No persistence.
- **Reads are stateless and trivial; writes are stateful and carry all the
  complexity.** The GUI paste path is just a write you already did, collected
  later.
- A running **clipboard manager** (Klipper) immediately copies from the new
  owner into its own store and takes over, so on a real desktop `copy` *feels*
  fire-and-forget even though it is not. The manager is the durable buffer the
  display server does not provide.

## 4. CLI Surface

Four **separate commands**, not subverbs. One binary with four faces, dispatched
on `argv[0]` (the BusyBox / `gzip`-`gunzip`-`zcat` pattern). Installed as one
real binary plus three symlinks.

```
program | cpclip       # capture stdin → clipboard (replace)
program | cpadd        # capture stdin → clipboard (append)
cppaste                # clipboard → stdout
cpclear                # empty the clipboard

Per-command flags:
  cpclip   [TEXT]   -t/--type MIME   -f/--foreground   --backend NAME
  cpadd    [TEXT]   -t/--type MIME   --separator STR   -f/--foreground   --backend NAME
  cppaste           -t/--type MIME   -n/--no-newline   --backend NAME
  cpclear           --backend NAME

  --backend NAME   # x11 | wayland | auto (default auto)
  -h, --help       # all four
```

`[TEXT]` argument is optional; if omitted, the command reads stdin (the primary
pipe use). Each command rejects flags that do not apply to it.

**Terminal pass-through:** `cpclip` and `cpadd` mirror stdin to stdout **only
when stdout is a TTY** (`isatty(STDOUT_FILENO)`). So `program | cpclip` shows the
output *and* copies it interactively, while `program | cpclip | other` stays
silent and just copies — pipelines are not disturbed. `cppaste` is a pure
one-shot read, no mirroring.

Validated as portable: every command and flag maps cleanly onto both backends.
The only environment-divergent feature (primary selection) has been removed.

### 4.1 `argv[0]` dispatch

```c
const char *prog = basename(argv[0]);
if      (!strcmp(prog, "cpclip"))  verb = VERB_COPY;
else if (!strcmp(prog, "cpadd"))   verb = VERB_ADD;
else if (!strcmp(prog, "cppaste")) verb = VERB_PASTE;
else if (!strcmp(prog, "cpclear")) verb = VERB_CLEAR;
else { fprintf(stderr, "invoke as cpclip, cpadd, cppaste, or cpclear\n");
       return 2; }
```

Install: build `cpclip`, then `ln -sf cpclip cpadd`, `cppaste`, `cpclear`.
`argv[0]` is data, not decoration — a teaching point in itself.

## 5. Architecture

One abstraction, two backends. The CLI never knows which backend is live.

```
cpclip/
├── main.c            # argv[0] dispatch, CLI parse, backend detection
├── backend.h         # the backend interface (struct of fn pointers)
├── backend_x11.c     # Xlib + Xfixes
├── backend_wayland.c # wl_data_device (+ dispatch loop)
├── ops_add.c         # cpadd composition, backend-agnostic
├── io_util.c         # grow-buffer stdin read, TTY-mirror write, full write
└── Makefile          # wayland-scanner; builds cpclip + 3 symlinks
```

### 5.1 Backend interface

A hand-built vtable. This *is* the polymorphism lesson: it shows that
"polymorphism" is a pattern, not a language keyword.

```c
typedef struct {
    const char *(*name)(void);
    int (*set)  (const char *mime, const void *data, size_t len);
    int (*get)  (const char *mime, void **out, size_t *out_len);
    int (*clear)(void);
} clipboard_backend;
```

All data is `(void *, size_t)` — never C strings — for binary safety.

### 5.2 Backend detection

`$WAYLAND_DISPLAY` set → Wayland; else `$DISPLAY` set → X11; else error
("no display server; clipboard unavailable in this session"). `--backend`
overrides for testing.

### 5.3 `add` as composition

`add` is **not a primitive**. Neither server can append. It is:

```
read current selection  →  concatenate (with separator)  →  write result
```

Implemented once in `ops_add.c`; works on both backends automatically. It
inherits the **write** side's fork/persist behavior (it ends in a `set`), not
the read side's clean exit.

```c
// must FULLY read the old owner before becoming the new owner
int clip_add(clipboard_backend *b, const char *mime,
             const void *new_data, size_t new_len,
             const char *sep) {
    void  *cur = NULL; size_t cur_len = 0;
    b->get(mime, &cur, &cur_len);          // empty is fine (first add == copy)

    size_t sep_len = (cur_len && sep) ? strlen(sep) : 0;
    size_t total   = cur_len + sep_len + new_len;
    char  *buf     = malloc(total);

    memcpy(buf, cur, cur_len);
    if (sep_len) memcpy(buf + cur_len, sep, sep_len);
    memcpy(buf + cur_len + sep_len, new_data, new_len);

    int rc = b->set(mime, buf, total);     // becomes new owner
    free(cur); free(buf);
    return rc;
}
```

## 6. Behavioral Decisions (locked)

- **Binary-safe everywhere.** No `strlen`/`strcpy` on clipboard payloads.
- **stdin read** uses a grow-as-you-read loop (doubling buffer); size unknown.
- **TTY pass-through.** `cpclip`/`cpadd` mirror stdin to stdout only when
  `isatty(STDOUT_FILENO)` is true — printed *and* copied when interactive,
  silent when piped/redirected so pipelines are not disturbed. Raw bytes are
  mirrored as-is (binary streams may garble the TTY; the clipboard copy stays
  byte-exact — same caveat as real `tee`).
- **Fork readiness handshake.** On copy/add, the parent does not return until
  the child signals it has actually acquired ownership (child writes a byte to a
  pipe; parent reads it, then exits). Closes the "paste in the gap finds no
  owner" race.
- **SIGPIPE ignored** on the read side; a reader closing early
  (`cppaste | head -1`) is a clean exit, not an error.
- **MIME serving.** Advertise a small honest set and serve *every* one
  requested (`text/plain`, `text/plain;charset=utf-8`, `UTF8_STRING`, `STRING`,
  `TEXT`). A type advertised but not served makes managers store empty entries.
- **Paste type fallback.** If the requested MIME is not offered, consult the
  offered list, pick the best text type, else error cleanly. Do not assume
  `text/plain` is present.
- **`clear` semantics:** relinquish ownership entirely (clipboard becomes
  empty/unowned), documented as such.
- **`add` on empty clipboard** behaves exactly like `copy` — no leading
  separator.
- **`add` type coherence:** if the current selection is non-text and a text
  `add` is requested, error rather than silently clobber.

## 7. Per-Backend Notes

### X11 (Xlib + Xfixes)
- `XFixes` provides selection-owner-change events.
- A headless process can grab the selection directly (no input serial needed).
- Owner persists by staying alive and answering `SelectionRequest`; exits on
  `SelectionClear` (someone else took it).

### Wayland (libwayland-client)
- Regular clipboard only: `wl_data_device_manager` / `wl_data_device`.
- **`set` needs an input-event serial + a hidden `wl_surface`/seat** — the
  compositor validates ownership against recent input focus. This complexity
  lives entirely in the backend; the CLI is unaffected.
- **The backgrounded owner must run a dispatch loop** (`wl_display_dispatch`)
  to answer send requests. Fork without the loop ⇒ copy "succeeds" but every
  paste returns empty. *The* Wayland gotcha.
- Klipper integration is automatic: implement `wl_data_source` correctly and
  Klipper records the entry and provides post-exit persistence on its own.

## 8. Environment Matrix

| Path | CLI (TTY) | GUI |
|---|---|---|
| stdout → clipboard | `cmd \| cpclip` (forks, persists; mirrors to TTY if interactive) | same write, no extra code |
| clipboard → stdin | `cppaste \| cmd` (one-shot) | — |
| clipboard → paste | — | Ctrl+V / Ctrl+Shift+V hits the owner; tool implements nothing, only stays alive (or Klipper holds it) |

Ctrl+V vs Ctrl+Shift+V is the *receiving app's* convention (formatted vs
plain). Not our concern — but it is why a clean `text/plain` offer matters: the
app chooses which type to pull.

## 9. Language Choice

**C.** Reference tools (wl-clipboard, xclip, xsel) are C, so students can
cross-read real source. The load-bearing concepts — fork, file descriptors,
ownership protocol, manual MIME serving — are exactly what C makes explicit. The
struct-of-function-pointers backend doubles as a demystification of what C++
`virtual` does under the hood.

## 10. Known Risks

- Wayland dispatch-loop omission is the one bug that fails silently; everything
  else fails loudly or degrades gracefully.
- Fork-readiness handshake is subtle plumbing; get it right early.
- Type negotiation must not assume a fixed MIME; test against real apps.
