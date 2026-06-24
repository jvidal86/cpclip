# Changelog

All notable changes to cpclip are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.2.2] – 2026-06-25

### Added

- **MIME type sniffing** (`src/mime_sniff.c`): `cpclip` and `cuclip` now
  probe up to 8 KiB of input when no `-t` flag is given and advertise the
  correct MIME type automatically:
  - NUL byte present → `application/octet-stream`
  - Valid UTF-8 (including pure ASCII) → `text/plain;charset=utf-8`
  - No NUL but invalid UTF-8 → `text/plain` (legacy 8-bit encoding)

### Changed

- **`cppaste` binary fallback**: when the clipboard has no text type
  (e.g. content copied as `application/octet-stream`), `cppaste` now
  falls back to requesting `application/octet-stream` and outputs the raw
  bytes verbatim.  Binary content never gets the auto-appended trailing
  newline.

## [0.2.1] – 2026-06-24

### Security

- **Integer overflow in `clip_add`**: a malicious clipboard owner returning a
  very large payload could cause the combined-size arithmetic in `ops_add.c` to
  wrap, leading to a heap under-allocation and subsequent buffer overflow.  Fixed
  with `SIZE_MAX` guards before addition.
- **Integer overflow in `buf_append` (X11 backend)**: doubling loop could
  overflow `size_t`; now guarded with `ncap > SIZE_MAX / 2` check.
- **Unsafe `nitems * bytes_per_item` multiplication (X11 backend)**: XGetWindowProperty
  returns `nitems` and `format` independently; their product was not checked for
  overflow before use.  Fixed with `nitems > SIZE_MAX / bytes_per_item` guard.
- **`limit + 1` overflow in `read_all_fd`**: when `limit == SIZE_MAX` the `+1`
  wrapped to 0.  Fixed by computing `lim1` once with a conditional.
- **MIME length cap bypass in `null_set`**: no upper bound on the MIME string
  written to the store file; a caller could craft a 4 GB header.  Now capped at
  4095 bytes.
- **Bounds-check overflow in `null_get`**: `MIME_LEN_BYTES + mlen` could overflow
  on 32-bit `size_t`.  Rewritten as safe-subtraction form.
- **Missing `O_NOFOLLOW` on null-backend files**: open calls in `backend_null.c`
  did not pass `O_NOFOLLOW`, allowing a symlink planted in `/tmp` to redirect
  reads or writes.  Added to both `open()` calls.
- **EINTR not retried in readiness handshake**: `signal_owner_ready` and
  `wait_for_owner_ready` in `proc_util.c` did not loop on `EINTR`, causing a
  spurious "owner failed to start" error under signal pressure.  Fixed with
  `do { … } while (errno == EINTR)` loops.
- **Wayland wrong return code for non-text offer**: when the clipboard owner
  advertised no recognised text MIME type, `wl_get` returned `CLIP_GET_ERROR`
  (-1) instead of `CLIP_GET_NO_TEXT` (-2), causing `cpadd`/`cuadd` to silently
  clobber a non-text selection instead of refusing it.  Fixed.
- **Wayland offer leak beyond `MAX_OFFERS`**: if more than `MAX_OFFERS` data
  offers arrived, the selection offer could fall outside the tracked array and
  leak.  Fixed with `sel_destroyed` guard in `get_state_cleanup`; `MAX_OFFERS`
  also raised from 8 to 32.
- **No receive-size cap on Wayland get**: a malicious clipboard owner could
  trigger unbounded memory growth.  Added `RECV_MAX_BYTES` (512 MiB) cap,
  matching the existing X11 cap.
- **X11 INCR send integer truncation**: chunk size cast to `int` without range
  check; very large payloads could pass a negative `nelt` to `XChangeProperty`.
  Fixed with explicit `(size_t)INT_MAX` bound before cast.

## [0.2.0] – 2026-06-24

### Added

- **`cuclip` and `cuadd`** — silent-copy variants of `cpclip` and `cpadd`.
  They copy stdin to the clipboard but never echo it to stdout, even when
  stdout is a terminal.  Every flag accepted by `cpclip`/`cpadd` is supported
  identically (`-t`, `-s`, `-m`, `-f`, `-b`).
- Man pages for `cuclip(1)` and `cuadd(1)` (`.so` redirects to `cpclip(1)`).
- **Usage section in README** covering copy/paste, TTY pass-through, stdout/stderr
  redirection, silent copy with `cuclip`/`cuadd`, append with `cpadd`/`cuadd`,
  and paste options.
- 17 new test assertions for `cuclip`/`cuadd` in `tests/run.sh`, including a
  Python3 PTY-based test that verifies no terminal echo even when stdout is
  a real TTY.

### Changed

- `ops_add.c`: `clip_add()` now takes a `const char *prog` parameter so error
  messages name the correct invoking command (`cuadd:` vs `cpadd:`).
- `tests/run.sh`: updated expected error strings for `--separator` and
  `--maxmem` rejection messages to include the new verb names.
- README command table updated with "TTY echo" column; six commands listed.

## [0.1.3] – 2026-06-24

### Added

- Criterion unit tests (`make test-unit`) for `parse_size`, `read_all_fd`/`write_all`,
  and `clip_add` (via in-memory mock backend).  CI job `unit-tests` added.

## [0.1.2] – 2026-06-24

### Added

- `-m`/`--maxmem` flag: caps clipboard read+write at 10 MiB by default
  (`0` disables).  Applies to all verbs that read or write payload data.

## [0.1.1] – 2026-06-24

### Added

- `-V`/`--version` flag on all four commands.

## [0.1.0] – 2026-06-24

### Added

- Initial release: `cpclip`, `cpadd`, `cppaste`, `cpclear`.
- X11 (Xlib + Xfixes) and Wayland (`ext-data-control-v1`) backends.
- Null backend (`--backend null`) for display-server-free testing.
- Binary-safe clipboard payloads; fork+readiness-handshake ownership model.
- INCR send/receive for large X11 payloads.
- `curl | sh` installer and portable binary tarball.
