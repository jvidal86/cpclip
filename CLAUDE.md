# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

```sh
make          # builds cpclip + cpadd/cppaste/cpclear symlinks
make clean    # remove binaries, objects, and generated protocol files
make install  # install to /usr/local/bin (PREFIX overridable)
```

Always build with `-Wall -Wextra` (already in `CFLAGS`) and fix all warnings before committing.

Current phase: **Phase 1** (X11 backend active). Phase 2 (Wayland) lines are commented out in the Makefile.

Dependencies: `libx11-dev`, `libxfixes-dev` (X11). Install Wayland libs (`libwayland-dev`, `wayland-protocols`) when Phase 2 begins.

## Testing

No automated test suite yet (Phase 4). Test manually per-phase using the exit tests in `IMPLEMENTATION.md`. For the null backend (no display server needed):

```sh
echo hi | ./cpclip --backend null && ./cppaste --backend null
# → hi
```

For X11, use Xvfb; for Wayland, use `cage` or `weston --headless`. The dev box is Wayland+XWayland with no X clipboard manager — test X11 under `Xvfb`.

## Architecture

One binary (`cpclip`) dispatched by `argv[0]` to four verbs — `cpclip`, `cpadd`, `cppaste`, `cpclear` — installed as symlinks. See `DESIGN.md §4`.

**Backend interface** (`backend.h`): a struct of four function pointers (`name`, `set`, `get`, `clear`). All clipboard payloads are `(void *, size_t)` — never C strings — for binary safety. `main.c` calls `resolve_backend()` which returns a pointer to one static vtable; the rest of the code is backend-agnostic.

**Key files:**
- `main.c` — `argv[0]` dispatch, per-command flag parsing, `collect_input()`, `resolve_backend()`
- `backend.h` — vtable definition and `clip_opt_foreground` extern
- `backend_null.c` — in-process buffer; used for wiring tests and `--backend null`
- `backend_x11.c` — Xlib + Xfixes; forks a persistent child to hold clipboard ownership
- `ops_add.c` — `clip_add()`: read current → concat with separator → write result; backend-agnostic
- `io_util.c` — `read_all_fd()` (grow-buffer stdin reader with optional TTY mirror), `write_all()`

**`cpadd` composition**: not a display-server primitive. `ops_add.c` does `get` → concat → `set`. Must fully complete `get` before `set` (avoid reading your own buffer). Empty clipboard behaves like `cpclip` (no leading separator).

**Fork/persist model**: on `set`, real backends fork a child that holds clipboard ownership and answers `SelectionRequest` (X11) or `wl_data_source.send` (Wayland) events. The parent waits for a pipe byte from the child (readiness handshake) before exiting. `clip_opt_foreground` (`-f`) suppresses the fork for scripting/demos.

**TTY mirror**: `cpclip`/`cpadd` echo stdin to stdout only when `isatty(STDOUT_FILENO)` — interactive use shows output and copies it; piped use stays silent.

## Behavioral invariants (do not break)

- All clipboard payloads binary-safe — no `strlen`/`strcpy` on payload bytes.
- Fork-readiness handshake must close before parent exits — no "paste in the gap" race.
- `SIGPIPE` ignored — `cppaste | head -1` is a clean exit, not an error.
- `cpadd` on empty clipboard == `cpclip` with no leading separator.
- MIME types advertised must all be serveable — advertising without serving causes clipboard managers to store empty entries.
