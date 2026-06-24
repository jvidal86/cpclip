# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

```sh
make           # builds cpclip + cpadd/cppaste/cpclear symlinks (in the repo root)
make test      # E2E matrix (tests/run.sh) on every available backend; no extra deps
make test-unit # Criterion unit tests (needs libcriterion-dev)
make check     # both: test-unit then test
make clean     # removes the binary, symlinks, and build/ (objects + generated glue)
make install   # installs binary, symlinks, and man pages (PREFIX overridable)
```

Always build with `-Wall -Wextra` (already in `CFLAGS`) and fix all warnings before committing.

Dependencies: `libx11-dev`, `libxfixes-dev`, `libwayland-dev` (provides `wayland-scanner`), and a C11 compiler. The ext-data-control protocol XML is vendored in `protocol/`, so the build does not need the system `wayland-protocols` package.

License: GPL-2.0-or-later (`LICENSE`); every `src/*.c|h` carries an `SPDX-License-Identifier` header. Packaging lives in `debian/`, `nfpm.yaml`, and `packaging/`; CI/release workflows in `.github/workflows/`.

## Layout

```
src/    hand-written C sources and headers
doc/    DESIGN.md, IMPLEMENTATION.md
man/    cpclip.1 (full) + cpadd/cppaste/cpclear.1 (.so redirects)
tests/  run.sh — the test matrix
build/  objects + generated wayland-scanner glue (.gitignored)
```

The binary `cpclip` and its `cpadd`/`cppaste`/`cpclear` symlinks are built into the repo root. The Makefile uses `VPATH = src` so sources are found without `src/` prefixes; the ext-data-control protocol glue is generated into `build/` (found via `-Ibuild`).

## Status

All phases complete (0 skeleton+null → 1 X11 → 2 Wayland → 3 add → 4 polish/tests). `make test` runs 36 assertions across null + X11 + Wayland.

## Testing

`make test` runs `tests/run.sh`, which auto-detects available backends (null always; x11 if `$DISPLAY`; wayland if `$WAYLAND_DISPLAY`), saves/restores the live clipboard, and asserts round-trip, NUL/binary safety, `add` composition, type coherence, newline handling, SIGPIPE, empty/large input (incl. X11 INCR send), `--foreground`, and the CLI surface. The null backend needs no display server:

```sh
echo hi | ./cpclip --backend null && ./cppaste --backend null   # → hi
```

The dev box is KDE Plasma (KWin) Wayland + XWayland with no X-side clipboard manager.

Unit tests use **Criterion** (`make test-unit`, CI job `unit-tests`, dev-dep
`libcriterion-dev`) and cover the pure modules only — `parse_size`
(`src/parse_util.c`), `read_all_fd`/`write_all` (`src/io_util.c`), and `clip_add`
(`src/ops_add.c`, via an in-memory mock backend in `tests/test_ops_add.c`). The
backends are intentionally left to the E2E suite (they need a display server).
Criterion is test-only and does not affect the shipped packages.

## Architecture

One binary (`cpclip`) dispatched by `argv[0]` to four verbs — `cpclip`, `cpadd`, `cppaste`, `cpclear` — installed as symlinks. See `doc/DESIGN.md §4`.

**Backend interface** (`src/backend.h`): a struct of four function pointers (`name`, `set`, `get`, `clear`). All clipboard payloads are `(void *, size_t)` — never C strings — for binary safety. `get` returns a tri-state status (`CLIP_GET_OK` / `CLIP_GET_ERROR` / `CLIP_GET_NO_TEXT`) so `add` can tell an empty clipboard (copy) from a non-text selection (refuse). `main.c` calls `resolve_backend()`, which returns one static vtable; the rest of the code is backend-agnostic.

**Key files (`src/`):**
- `main.c` — `argv[0]` dispatch, per-command flag parsing, `collect_input()`, `resolve_backend()`
- `backend.h` — vtable, `CLIP_GET_*` codes, `clip_opt_foreground` extern
- `backend_null.c` — file-backed fake (models type negotiation); `--backend null`
- `backend_x11.c` — Xlib + Xfixes; forks a persistent owner; INCR receive **and** send
- `backend_wayland.c` — `ext-data-control` (no input serial/surface, like wl-copy); runs the mandatory dispatch loop
- `ops_add.c` — `clip_add()`: read current → concat with separator → write; backend-agnostic
- `io_util.c` — `read_all_fd()` (grow-buffer reader + optional TTY mirror), `write_all()`
- `proc_util.c` — shared fork/readiness-handshake/detach plumbing for both forking backends

**`cpadd` composition**: not a display-server primitive. `ops_add.c` does `get` → concat → `set`, fully completing `get` before `set`. Empty clipboard behaves like `cpclip` (no leading separator); a non-text selection is refused, not clobbered.

**Fork/persist model**: on `set`, real backends fork a child that holds clipboard ownership and answers `SelectionRequest` (X11) or `ext_data_control_source.send` (Wayland) events. The parent waits for a pipe byte from the child (readiness handshake) before exiting. `clip_opt_foreground` (`-f`) suppresses the fork.

**Wayland gotcha**: the backgrounded owner MUST run `wl_display_dispatch` — without it copy "succeeds" but every paste returns empty.

**TTY mirror**: `cpclip`/`cpadd` echo stdin to stdout only when `isatty(STDOUT_FILENO)`.

## Behavioral invariants (do not break)

- All clipboard payloads binary-safe — no `strlen`/`strcpy` on payload bytes.
- Fork-readiness handshake must close before parent exits — no "paste in the gap" race.
- `SIGPIPE` ignored — `cppaste | head -1` is a clean exit, not an error.
- `cpadd` on empty clipboard == `cpclip` with no leading separator; non-text → clean refusal.
- MIME types advertised must all be serveable.
- Backend error messages are verb-neutral (`x11:` / `wayland:` / `null:`) since `get` is shared.
- `clear` honestly relinquishes ownership; a clipboard manager may restore from history (expected, no manager-specific code).
