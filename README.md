# cpclip

[![CI](https://img.shields.io/github/actions/workflow/status/jvidal86/cpclip/ci.yml)](https://github.com/jvidal86/cpclip/actions/workflows/ci.yml)
[![Tests](https://img.shields.io/badge/tests-70%20passing-brightgreen)](https://github.com/jvidal86/cpclip/actions)
[![License](https://img.shields.io/github/license/jvidal86/cpclip)](LICENSE)
[![Built with Claude Code](https://img.shields.io/badge/built%20with-Claude%20Code-d97757?logo=anthropic&logoColor=white)](https://claude.ai/code)

A single CLI clipboard tool for both **X11 and Wayland**, with one identical
command interface. It captures process output to the clipboard and serves the
clipboard back to process input, and adds a composed `add` (append) operation
that neither display server provides natively.

One binary with six faces, dispatched on `argv[0]` (the BusyBox pattern):

| Command   | Action                                              | TTY echo |
|-----------|-----------------------------------------------------|----------|
| `cpclip`  | stdin (or `TEXT`) → clipboard (replace)             | yes      |
| `cpadd`   | stdin (or `TEXT`) → clipboard (append)              | yes      |
| `cppaste` | clipboard → stdout                                  | —        |
| `cpclear` | empty the clipboard                                 | —        |
| `cuclip`  | stdin (or `TEXT`) → clipboard (replace)             | never    |
| `cuadd`   | stdin (or `TEXT`) → clipboard (append)              | never    |

"TTY echo" means stdin is mirrored to stdout when stdout is a terminal.

## Install

### One-liner (recommended)

Downloads the latest release, verifies its SHA-256, and installs system-wide
(`wget -qO- … | sh` works too):

```sh
curl -fsSL https://raw.githubusercontent.com/jvidal86/cpclip/main/install.sh | sh
```

It installs into **`/usr/local`** (using `sudo` only for that step):

- `/usr/local/bin/cpclip` plus the `cpadd`, `cppaste`, `cpclear`, `cuclip`, `cuadd` symlinks
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

## Usage

### Copy and paste

```sh
echo "hello" | cpclip       # copy text to the clipboard
cppaste                      # paste it back
cpclear                      # empty the clipboard
```

### TTY pass-through

`cpclip` and `cpadd` mirror stdin to stdout **only when stdout is a terminal**,
so you see the output and copy it at the same time:

```sh
make 2>&1 | cpclip           # prints the build output and copies it
```

When stdout is not a terminal (pipe, redirection, script), the mirror is
suppressed automatically — nothing extra reaches the next stage:

```sh
make 2>&1 | cpclip | wc -l  # copies silently; wc counts what actually went through
```

### Redirecting stdout and stderr

Because the mirror only fires when stdout is a TTY, redirecting stdout
is enough to silence it:

```sh
make 2>&1 | cpclip > /dev/null      # copy silently, discard stdout
make 2>&1 | cpclip > build.log      # copy and also save to a file
make 2>&1 | cpclip > build.log 2>&1 # same, stderr to the log too
```

Redirect stderr to suppress backend error messages (e.g. when the clipboard
is unavailable and you don't want noise):

```sh
echo hi | cpclip 2>/dev/null
```

### Silent copy with cuclip / cuadd

`cuclip` and `cuadd` never echo, regardless of whether stdout is a terminal.
Use them when the intent is "copy only" and pass-through would be surprising
— in shell functions, scripts, or keybindings where fd state is unpredictable:

```sh
echo hi | cuclip             # always silent
echo more | cuadd            # append, always silent
```

They accept the same flags as their `cp*` counterparts (`-t`, `-f`, `--maxmem`,
`--separator`, `--backend`), and all shell redirections work the same way.

### Append

Build up a clipboard entry from several commands, then paste it all at once:

```sh
git rev-parse HEAD | cpclip  # first line — replaces the clipboard
date              | cpadd    # appended with a newline separator
uname -a          | cpadd    # appended again
cppaste                       # paste all three lines
```

Custom separator:

```sh
printf 'foo' | cpclip
printf 'bar' | cpadd --separator ' | '
cppaste -n                   # → foo | bar
```

### Paste options

```sh
cppaste                      # appends a newline if the content lacks one
cppaste -n                   # byte-exact: no trailing newline added
cppaste -n > image.png       # save binary clipboard content to a file
cppaste -t text/html         # request a specific MIME type
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
the storage. So the copy/append commands fork a background owner that serves the bytes on
demand (the parent returns only once the child confirms ownership), while
`cppaste` is a clean one-shot read. A running clipboard manager copies from
the new owner and provides persistence after it exits.

Because that owner holds the whole payload in memory, a single copy/append
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
