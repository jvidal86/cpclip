# cpclip

A single CLI clipboard tool for both **X11 and Wayland**, with one identical
command interface. It captures process output to the clipboard and serves the
clipboard back to process input, and adds a composed `add` (append) operation
that neither display server provides natively.

One binary with four faces, dispatched on `argv[0]` (the BusyBox pattern):

| Command   | Action                                              |
|-----------|-----------------------------------------------------|
| `cpclip`  | stdin (or `TEXT`) → clipboard (replace)             |
| `cpadd`   | stdin (or `TEXT`) → clipboard (append)              |
| `cppaste` | clipboard → stdout                                  |
| `cpclear` | empty the clipboard                                 |

```sh
make            # builds cpclip + the cpadd/cppaste/cpclear symlinks
make test       # runs the test matrix on whatever backends are available
sudo make install
```

## Examples

```sh
make 2>&1 | cpclip          # copy a command's output (and see it, if interactive)

git rev-parse HEAD | cpclip # accumulate several outputs into one entry
date              | cpadd
uname -a          | cpadd
cppaste

cppaste -n > image.png      # byte-exact binary output (no trailing newline)
```

## Backends

`--backend auto` (the default) picks **Wayland** when `$WAYLAND_DISPLAY` is set,
else **X11** when `$DISPLAY` is set. Force one with `--backend x11|wayland|null`.

- **X11** — Xlib + XFixes; owns the `CLIPBOARD` selection, serves the MIME set,
  and handles arbitrarily large payloads via the INCR protocol (both directions).
- **Wayland** — `ext-data-control` (no input serial or surface needed; what
  `wl-copy` uses). The backgrounded owner runs the required dispatch loop.
- **null** — an in-process file-backed fake for testing the wiring.

## How it works

X11 and Wayland have no system-owned clipboard buffer: the copying process *is*
the storage. So `cpclip`/`cpadd` fork a background owner that serves the bytes on
demand (the parent returns only once the child confirms ownership), while
`cppaste` is a clean one-shot read. A running clipboard manager copies from the
new owner and provides persistence after it exits.

See [`doc/DESIGN.md`](doc/DESIGN.md) for the architecture and
[`doc/IMPLEMENTATION.md`](doc/IMPLEMENTATION.md) for the phased build.

## Dependencies

`libx11`, `libxfixes`, `libwayland-client`, `wayland-protocols`, plus
`wayland-scanner` and a C11 compiler.

## Not supported

Primary/secondary selections, clipboard history/watch mode, macOS/Windows.
