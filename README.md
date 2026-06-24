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

## Install

### One-liner (recommended)

Downloads the latest release, verifies its SHA-256, and installs system-wide
(`wget -qO- … | sh` works too):

```sh
curl -fsSL https://raw.githubusercontent.com/jvidal86/cpclip/main/install.sh | sh
```

It installs into **`/usr/local`** (using `sudo` only for that step):

- `/usr/local/bin/cpclip` plus the `cpadd`, `cppaste`, `cpclear` symlinks
- `/usr/local/share/man/man1/cp*.1`

`/usr/local/bin` is on the default `PATH`, so `cpclip` works immediately.

**Per-user, no sudo** — install into `~/.local` by setting `PREFIX` on the
piped shell (note the placement, so the variable reaches the script, not `curl`):

```sh
curl -fsSL https://raw.githubusercontent.com/jvidal86/cpclip/main/install.sh | PREFIX=$HOME/.local sh
```

Pin a specific version with `CPCLIP_VERSION=v0.1.1`.

### Native package

Download a `.deb` / `.rpm` / `.apk` from a
[Release](https://github.com/jvidal86/cpclip/releases) and install it with your
package manager — it resolves the `libx11` / `libxfixes` / `libwayland-client`
runtime dependencies for you.

### From source

```sh
make               # builds cpclip + the cpadd/cppaste/cpclear symlinks
make test          # runs the test matrix on whatever backends are available
sudo make install  # installs to /usr/local (override with PREFIX= and DESTDIR=)
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

Because that owner holds the whole payload in memory, a single `cpclip`/`cpadd`
is capped at **10 MiB** by default (this is a text tool). Raise or remove it with
`--maxmem` — e.g. `--maxmem 200M`, or `--maxmem 0` to disable the cap.

See [`doc/DESIGN.md`](doc/DESIGN.md) for the architecture and
[`doc/IMPLEMENTATION.md`](doc/IMPLEMENTATION.md) for the phased build.

## Dependencies

`libx11`, `libxfixes`, `libwayland-client`, plus `wayland-scanner` and a C11
compiler. The ext-data-control protocol XML is vendored in `protocol/`, so the
build does not depend on the system `wayland-protocols` version.

## Not supported

Primary/secondary selections, clipboard history/watch mode, macOS/Windows.

## License

GPL-2.0-or-later. See [`LICENSE`](LICENSE).
