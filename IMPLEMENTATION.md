# `clip` — Implementation Plan (Phased)

Build order is chosen so the tool *works* as early as possible and the hardest,
silent-failure-prone part (Wayland dispatch) comes after the architecture is
proven. Each phase ends in something testable.

See `DESIGN.md` for the architecture and locked behavioral decisions.

---

## Phase 0 — Skeleton & Scaffolding

**Goal:** the whole dispatch path runs end-to-end with a fake backend, before
any real clipboard code exists.

Tasks:
- Project layout and `Makefile` (compiles, links, runs `wayland-scanner`
  to generate protocol glue — even if unused yet). Build `cpclip`, then create
  `cpadd`/`cppaste`/`cpclear` as symlinks; add an `install` rule for the same.
- `backend.h` — the `clipboard_backend` struct (set/get/clear/name).
- `main.c` — **`argv[0]` dispatch** (`basename` → one of cpclip/cpadd/cppaste/
  cpclear); per-command flag parsing; `--help`. One unknown name ⇒ usage error.
- Backend detection: `$WAYLAND_DISPLAY` → wayland, `$DISPLAY` → x11, else
  error; `--backend` override.
- `backend_null.c` — a no-op backend that stores into a process-local buffer,
  so cpclip→cppaste round-trips *within one run* for testing the wiring.
- `io_util.c` — grow-buffer stdin reader, full-write helper, and the
  **TTY-mirror** branch (`isatty(STDOUT_FILENO)` ⇒ echo each chunk to stdout)
  used by cpclip/cpadd.

**Exit test:** `echo hi | cpclip --backend null && cppaste --backend null`
prints `hi` (invoke via the symlinks). Flag parsing, `argv[0]` dispatch, and
TTY-mirror gating verified. (No persistence yet — null backend is in-process
only.)

**Teaching beat:** students see the vtable dispatch with zero display-server
noise.

---

## Phase 1 — X11 Backend

**Goal:** a fully working single-environment tool. Easier write path than
Wayland (headless grab, no serial).

Libraries: `libx11-dev`, `libxfixes-dev`.

Tasks:
- `backend_x11.c`: connect (`XOpenDisplay`), create a hidden window to own the
  selection.
- `set`: take `CLIPBOARD` ownership; serve `SelectionRequest` events;
  advertise + serve the MIME set (`TARGETS`, `text/plain`,
  `text/plain;charset=utf-8`, `UTF8_STRING`, `STRING`, `TEXT`).
- `get`: request `TARGETS`, choose best text type, request it, read the
  property, return `(void*, size_t)`.
- `clear`: relinquish ownership (clipboard becomes unowned).
- **Fork-to-background** with the readiness handshake: child acquires ownership
  and signals the parent via a pipe; parent exits only after the signal.
- Exit the owner cleanly on `SelectionClear`.
- Ignore `SIGPIPE`.

**Exit test (under X11 / Xvfb):**
- `echo hello | cpclip` then `cppaste` → `hello`.
- `cpclip` then Ctrl+V in a real X11 app works.
- `printf 'a\0b' | cpclip` round-trips with the NUL intact (binary safety).
- `cppaste | head -1` does not error.
- `program | cpclip` on an interactive TTY shows the output *and* copies it;
  `program | cpclip | cat` stays silent (mirror gated on `isatty`).

---

## Phase 2 — Wayland Backend

**Goal:** feature-parity with X11. This is the hard phase; budget the most time
here.

Libraries: `libwayland-dev`, `wayland-protocols`.

Tasks:
- `Makefile`: `wayland-scanner` generates client glue from the protocol XML.
- `backend_wayland.c`: connect (`wl_display_connect`), bind globals
  (`wl_seat`, `wl_data_device_manager`), get the `wl_data_device`.
- `set`: create a `wl_data_source`, advertise the MIME set, **acquire an input
  serial via a hidden surface/seat**, call `set_selection`, then serve
  `wl_data_source.send` events by writing bytes to the supplied fd.
- **Run the dispatch loop** (`wl_display_dispatch`) in the backgrounded owner —
  without it copy "succeeds" but paste returns empty. Loud comment in code.
- `get`: handle `wl_data_device.data_offer` / `selection`, enumerate offered
  MIME types, pick best text type, `receive` into a pipe, read to EOF.
- `clear`: `set_selection(NULL)` to relinquish.
- Reuse the **same** fork/handshake and SIGPIPE handling from Phase 1.

**Exit test (under Wayland / `cage` or `weston --headless`):**
- Same four assertions as Phase 1.
- **Persistence handoff under Klipper:** `echo x | cpclip`, kill the owner
  process, then paste in an app → `x` still present (Klipper held it).

---

## Phase 3 — `cpadd` (Composition)

**Goal:** the headline feature. No backend-specific code — proves the
abstraction is clean.

Tasks:
- `ops_add.c`: `get` current → concat with `--separator` → `set` result.
- Ordering guarantee: fully complete the `get` before the `set` acquires
  ownership (avoid reading your own empty self).
- Empty clipboard ⇒ behaves like `cpclip` (no leading separator).
- Type coherence: non-text current selection + text `cpadd` ⇒ clean error.
- `cpadd` ends in a `set`, so it forks/persists like `cpclip` (and shares the
  same TTY-mirror gating).

**Exit test (both backends):**
```
printf 'one'   | cpclip
printf 'two'   | cpadd
printf 'three' | cpadd
cppaste            # → one\ntwo\nthree
```
And on a fresh clipboard, the first `cpadd` equals `cpclip` (no leading
newline).

**Teaching beat:** if both backends are correct, `cpadd` lights up on both for
free — the payoff of the backend interface.

---

## Phase 4 — Polish & Test Matrix

**Goal:** ship quality.

Tasks:
- `--foreground` mode (block instead of fork) for scripts and live demos.
- `--no-newline` on `cppaste`.
- `cpclear` end-to-end on both backends (relinquish ownership; empty result).
- Robust error messages: no display server; primary-selection-style requests
  rejected with a hint; non-text `cpadd`; MIME not serveable; wrong/unknown
  `argv[0]` invocation name.
- `man` pages for all four commands (one source, four names).
- Automated test matrix running the *same* CLI assertions under:
  - Xvfb (X11),
  - headless Wayland (`cage` / `weston --headless`),
  - and the Klipper persistence case on the KDE box.
- Edge cases: empty input, very large input (exercises grow-buffer),
  `cppaste` with empty clipboard, early-closing reader (SIGPIPE).

**Exit test:** full matrix green; identical CLI behavior across both backends.

---

## Future (explicitly out of scope now)

- `clip watch` / history monitoring — requires the data-control protocol family
  (`wlr-data-control` / `ext-data-control`); verify KWin/Plasma version support
  before attempting. Core copy/paste/add must not depend on it.
- Image/rich MIME beyond text.
- macOS / Windows backends behind the same CLI.

---

## Dependency Quick Reference

```
X11:     libx11-dev  libxfixes-dev
Wayland: libwayland-dev  wayland-protocols
Build:   gcc/clang, make, wayland-scanner
Test:    xvfb, cage or weston, (Klipper on KDE for persistence test)
```

## Suggested Build Order Recap

```
Phase 0  skeleton + null backend      → wiring works
Phase 1  X11 backend                  → real tool, one environment
Phase 2  Wayland backend              → both environments, hard part
Phase 3  add (composition)            → headline feature, free on both
Phase 4  polish + test matrix         → ship
```
