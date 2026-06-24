#!/usr/bin/env bash
#
# tests/run.sh — the cpclip test matrix.
#
# Runs the same CLI assertions against every backend available in this session
# (null always; x11 if $DISPLAY; wayland if $WAYLAND_DISPLAY). Real backends
# touch the live clipboard, so we save it on entry and restore it on exit.
#
# Usage: tests/run.sh        (or: make test)
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2
[ -x ./cpclip ] || make >/dev/null || exit 2
export PATH="$ROOT:$PATH"

pass=0; fail=0
check() { # desc expected actual
    if [ "$2" = "$3" ]; then
        printf '  PASS: %s\n' "$1"; pass=$((pass + 1))
    else
        printf '  FAIL: %s\n    exp: %q\n    got: %q\n' "$1" "$2" "$3"; fail=$((fail + 1))
    fi
}

# --- which backends can we exercise here? --------------------------------
# A display backend is usable only if it can actually open a connection. This
# skips, e.g., wayland on a compositor without ext-data-control (an old Weston
# in CI) rather than failing every assertion.
backend_works() {
    local err
    err=$(cppaste --backend "$1" 2>&1 >/dev/null)
    case "$err" in
        *"cannot connect"*|*"cannot open display"*|*"ext-data-control"*|*"no wl_seat"*)
            return 1 ;;
    esac
    return 0
}

backends=(null)
if [ -n "${DISPLAY:-}" ]; then
    if backend_works x11; then backends+=(x11)
    else echo "note: \$DISPLAY set but x11 backend unusable here — skipping"; fi
fi
if [ -n "${WAYLAND_DISPLAY:-}" ]; then
    if backend_works wayland; then backends+=(wayland)
    else echo "note: \$WAYLAND_DISPLAY set but wayland backend unusable (compositor lacks ext-data-control?) — skipping"; fi
fi
echo "Testing backends: ${backends[*]}"

# --- save/restore the user's real clipboard ------------------------------
real_backend=""
for b in wayland x11; do
    for x in "${backends[@]}"; do [ "$x" = "$b" ] && real_backend="$b"; done
    [ -n "$real_backend" ] && break
done
BACKUP="$(mktemp)"
[ -n "$real_backend" ] && cppaste -n --backend "$real_backend" >"$BACKUP" 2>/dev/null

cleanup() {
    pkill -x cpclip 2>/dev/null
    sleep 0.2
    if [ -n "$real_backend" ] && [ -s "$BACKUP" ]; then
        cpclip --backend "$real_backend" <"$BACKUP" >/dev/null 2>&1
    fi
    rm -f "$BACKUP"
}
trap cleanup EXIT

settle() { [ "$1" = null ] && return; sleep 0.2; }

# --- core matrix, per backend --------------------------------------------
for b in "${backends[@]}"; do
    echo "== backend: $b =="
    cpclear --backend "$b"; settle "$b"

    echo hi | cpclip --backend "$b" >/dev/null; settle "$b"
    check "[$b] copy/paste" "hi" "$(cppaste -n --backend "$b")"

    printf 'a\0b' | cpclip --backend "$b" >/dev/null; settle "$b"
    check "[$b] NUL binary-safe" "610062" "$(cppaste -n --backend "$b" | xxd -p)"

    cpclear --backend "$b"; settle "$b"
    printf 'one'   | cpclip --backend "$b" >/dev/null; settle "$b"
    printf 'two'   | cpadd  --backend "$b" >/dev/null; settle "$b"
    printf 'three' | cpadd  --backend "$b" >/dev/null; settle "$b"
    check "[$b] add composition" "$(printf 'one\ntwo\nthree')" "$(cppaste -n --backend "$b")"

    printf 'noeol' | cpclip --backend "$b" >/dev/null; settle "$b"
    check "[$b] default appends newline" "$(printf 'noeol\n')" "$(cppaste --backend "$b")"
    check "[$b] -n suppresses newline"   "noeol"               "$(cppaste -n --backend "$b")"

    printf 'l1\nl2\n' | cpclip --backend "$b" >/dev/null; settle "$b"
    cppaste --backend "$b" | head -1 >/dev/null
    check "[$b] early reader (SIGPIPE) clean exit" "0" "$?"

    printf '' | cpclip --backend "$b" >/dev/null; settle "$b"
    check "[$b] empty input round-trips" "" "$(cppaste -n --backend "$b")"

    # add-on-empty needs a backend whose clear truly empties (a clipboard
    # manager may restore history — see DESIGN.md §7).
    cpclear --backend "$b"; settle "$b"
    if [ -z "$(cppaste -n --backend "$b")" ]; then
        printf 'solo' | cpadd --backend "$b" >/dev/null; settle "$b"
        check "[$b] add-on-empty == copy" "solo" "$(cppaste -n --backend "$b")"
    else
        echo "  SKIP: [$b] add-on-empty (clipboard manager restores after clear)"
    fi
done

# --- type coherence (null models a non-text selection deterministically) --
echo "== type coherence (null) =="
cpclear --backend null
printf 'PNGDATA' | cpclip -t image/png --backend null >/dev/null
check "cppaste on non-text errors" "cppaste: clipboard has no text content" \
      "$(cppaste --backend null 2>&1 >/dev/null)"
check "cpadd refuses non-text" "cpadd: current selection is not text; refusing to overwrite it" \
      "$(echo x | cpadd --backend null 2>&1 >/dev/null)"
check "cpadd did not clobber" "PNGDATA" "$(cppaste -t image/png -n --backend null)"
check "exact-type paste works" "PNGDATA" "$(cppaste -t image/png -n --backend null)"

# --- memory cap (--maxmem, deterministic on null) ------------------------
echo "== memory cap (--maxmem, null) =="
cpclear --backend null
head -c $((11 * 1024 * 1024)) /dev/zero | cpclip --backend null 2>/dev/null
check "default 10 MiB cap rejects 11 MiB" "1" "$?"
head -c $((11 * 1024 * 1024)) /dev/zero | cpclip --backend null --maxmem 20M
check "--maxmem 20M accepts 11 MiB" "0" "$?"
head -c $((11 * 1024 * 1024)) /dev/zero | cpclip --backend null --maxmem 0
check "--maxmem 0 disables the cap" "0" "$?"
check "invalid --maxmem is a usage error" "2" \
      "$(echo x | cpclip --backend null --maxmem nope >/dev/null 2>&1; echo $?)"
check "cppaste rejects --maxmem" "cppaste: -m/--maxmem applies only to cpclip/cpadd/cuclip/cuadd" \
      "$(cppaste --maxmem 5M --backend null 2>&1)"

# --- CLI surface: dispatch, flags, exit codes ----------------------------
echo "== CLI surface =="
check "unknown invocation name" "invoke as cpclip, cpadd, cppaste, cpclear, cuclip, or cuadd" \
      "$(ln -sf cpclip bogus; ./bogus 2>&1; rm -f bogus)"
check "cppaste rejects --separator" "cppaste: --separator applies only to cpadd/cuadd" \
      "$(cppaste --separator x --backend null 2>&1)"
check "cpclear rejects -t" "cpclear: -t/--type does not apply to cpclear" \
      "$(cpclear -t text/plain --backend null 2>&1)"
check "--help exits 0" "0" "$(cpclip --help >/dev/null; echo $?)"
check "usage error exits 2" "2" "$(cpclip -n --backend null >/dev/null 2>&1; echo $?)"

# --- cuclip / cuadd (cut: copy without TTY mirror) -----------------------
echo "== cuclip / cuadd: basic round-trip =="
cpclear --backend null
printf 'cutval' | cuclip --backend null
check "cuclip copies to clipboard" "cutval" "$(cppaste -n --backend null)"

printf 'a\0b' | cuclip --backend null
check "cuclip NUL binary-safe" "610062" "$(cppaste -n --backend null | xxd -p)"

printf '' | cuclip --backend null
check "cuclip empty input round-trips" "" "$(cppaste -n --backend null)"

printf 'IMGDATA' | cuclip -t image/png --backend null
check "cuclip -t type flag" "IMGDATA" "$(cppaste -t image/png -n --backend null)"

cpclear --backend null
head -c $((11 * 1024 * 1024)) /dev/zero | cuclip --backend null 2>/dev/null
check "cuclip default 10 MiB cap rejects 11 MiB" "1" "$?"
head -c $((11 * 1024 * 1024)) /dev/zero | cuclip --backend null --maxmem 20M
check "cuclip --maxmem 20M accepts 11 MiB" "0" "$?"

echo "== cuclip / cuadd: stdout silence =="
check "cuclip silent (non-TTY stdout)" "" "$(printf 'hello' | cuclip --backend null)"
check "cuadd silent (non-TTY stdout)"  "" "$(printf 'world' | cuadd  --backend null)"

echo "== cuclip / cuadd: add composition =="
cpclear --backend null
printf 'one'   | cuclip --backend null
printf 'two'   | cuadd  --backend null
printf 'three' | cuadd  --backend null
check "cuadd composition" "$(printf 'one\ntwo\nthree')" "$(cppaste -n --backend null)"

cpclear --backend null
printf 'A' | cuclip --backend null
printf 'B' | cuadd --separator '|' --backend null
check "cuadd --separator" "A|B" "$(cppaste -n --backend null)"

cpclear --backend null
printf 'solo' | cuadd --backend null
check "cuadd on empty clipboard == copy" "solo" "$(cppaste -n --backend null)"

echo "== cuclip / cuadd: non-text refusal =="
cpclear --backend null
printf 'PNGDATA' | cuclip -t image/png --backend null
check "cuadd refuses non-text" \
      "cuadd: current selection is not text; refusing to overwrite it" \
      "$(printf x | cuadd --backend null 2>&1 >/dev/null)"
check "cuadd did not clobber" "PNGDATA" "$(cppaste -t image/png -n --backend null)"

echo "== cuclip / cuadd: TTY mirror suppression =="
if command -v python3 >/dev/null 2>&1; then
    _pty_run() {  # $1 = binary name; prints whatever it writes to a PTY stdout
        python3 - "$1" <<'PYEOF'
import os, pty, subprocess, select, sys
bin_name = sys.argv[1]
master, slave = pty.openpty()
proc = subprocess.Popen(
    [bin_name, '--backend', 'null'],
    stdin=subprocess.PIPE, stdout=slave, stderr=subprocess.DEVNULL,
    close_fds=True)
os.close(slave)
proc.stdin.write(b'ttytest\n')
proc.stdin.close()
proc.wait()
data = b''
while select.select([master], [], [], 0.2)[0]:
    try: data += os.read(master, 4096)
    except OSError: break
os.close(master)
# Strip carriage returns injected by the terminal line discipline
print(data.decode('utf-8', errors='replace').replace('\r', '').rstrip('\n'), end='')
PYEOF
    }
    check "cuclip no TTY echo"   ""         "$(_pty_run ./cuclip)"
    check "cuadd  no TTY echo"   ""         "$(_pty_run ./cuadd)"
    check "cpclip echoes in TTY" "ttytest"  "$(_pty_run ./cpclip)"  # control
else
    echo "  SKIP: TTY mirror tests (python3 not available)"
fi

# --- large input + X11 INCR send -----------------------------------------
if printf '%s\n' "${backends[@]}" | grep -qx x11; then
    echo "== X11 large input / INCR send =="
    for sz in 1048576 24000000; do          # under, then over the ~16MB ceiling
        head -c "$sz" /dev/urandom >"$BACKUP.in"
        cpclip --backend x11 --maxmem 0 <"$BACKUP.in" >/dev/null 2>&1; sleep 0.4
        cppaste -n --backend x11 >"$BACKUP.out" 2>/dev/null
        if cmp -s "$BACKUP.in" "$BACKUP.out"; then
            echo "  PASS: x11 $sz bytes round-trips"; pass=$((pass + 1))
        else
            echo "  FAIL: x11 $sz bytes (out=$(wc -c <"$BACKUP.out"))"; fail=$((fail + 1))
        fi
        pkill -x cpclip 2>/dev/null; sleep 0.2
    done
    rm -f "$BACKUP.in" "$BACKUP.out"

    echo "== --foreground serves then yields =="
    printf 'fgtest' | cpclip -f --backend x11 >/dev/null 2>&1 &
    fgpid=$!
    sleep 0.4
    check "[x11] -f foreground owner serves" "fgtest" "$(cppaste -n --backend x11)"
    kill "$fgpid" 2>/dev/null
fi

echo
echo "==== $pass passed, $fail failed ===="
[ "$fail" -eq 0 ]
