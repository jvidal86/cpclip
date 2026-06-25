#!/bin/sh
# cpclip installer (POSIX sh — works piped to sh, bash, dash, ...).
#
#   curl -fsSL https://raw.githubusercontent.com/clicraft/cpclip/main/install.sh | sh
#   wget -qO-  https://raw.githubusercontent.com/clicraft/cpclip/main/install.sh | sh
#
# It downloads the portable binary tarball from the latest GitHub Release,
# verifies its SHA-256, and installs cpclip + the cpadd/cppaste/cpclear symlinks
# and man pages into PREFIX (default /usr/local; uses sudo only if needed).
#
# Environment:
#   PREFIX=/usr/local        install prefix (use $HOME/.local for a no-sudo install)
#   CPCLIP_VERSION=v0.1.0    pin a version (default: the latest release)
set -eu

REPO="clicraft/cpclip"
PREFIX="${PREFIX:-/usr/local}"
VERSION="${CPCLIP_VERSION:-latest}"

info() { printf 'cpclip-install: %s\n' "$*"; }
err()  { printf 'cpclip-install: error: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

[ "$(uname -s)" = "Linux" ] || err "cpclip is Linux-only"

arch="$(uname -m)"
case "$arch" in
    x86_64 | amd64) arch="amd64" ;;
    *) err "unsupported architecture '$arch' (only amd64 is published; build from source)" ;;
esac

# Downloader shims: fetch <url> -> stdout ; download <url> <file>
if have curl; then
    fetch()    { curl -fsSL "$1"; }
    download() { curl -fsSL -o "$2" "$1"; }
elif have wget; then
    fetch()    { wget -qO- "$1"; }
    download() { wget -qO "$2" "$1"; }
else
    err "need curl or wget"
fi

# Resolve the latest tag from the GitHub API unless a version was pinned.
if [ "$VERSION" = "latest" ]; then
    VERSION="$(fetch "https://api.github.com/repos/$REPO/releases/latest" \
        | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -1)"
    [ -n "$VERSION" ] || err "could not determine the latest version from the GitHub API"
fi
ver="${VERSION#v}"

tarball="cpclip-${ver}-linux-${arch}.tar.gz"
base="https://github.com/$REPO/releases/download/${VERSION}"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

info "downloading $tarball ($VERSION)"
download "$base/$tarball" "$tmp/$tarball" \
    || err "download failed: $base/$tarball (does this release include the tarball?)"

# Verify the checksum when we can.
if download "$base/SHA256SUMS" "$tmp/SHA256SUMS" 2>/dev/null && have sha256sum; then
    if ( cd "$tmp" && grep " $tarball\$" SHA256SUMS | sha256sum -c - >/dev/null 2>&1 ); then
        info "checksum OK"
    else
        err "checksum verification failed for $tarball"
    fi
else
    info "skipping checksum (sha256sum or SHA256SUMS unavailable)"
fi

tar -xzf "$tmp/$tarball" -C "$tmp" || err "failed to extract $tarball"
[ -f "$tmp/bin/cpclip" ] || err "tarball did not contain bin/cpclip"

# Use sudo only if the destination isn't writable.
sudo=""
if ! { [ -w "$PREFIX" ] || [ -w "$(dirname "$PREFIX")" ]; }; then
    have sudo || err "$PREFIX is not writable and sudo is unavailable; re-run with PREFIX=\$HOME/.local"
    sudo="sudo"
fi

info "installing to $PREFIX"
$sudo mkdir -p "$PREFIX/bin" "$PREFIX/share/man/man1"
$sudo cp -a "$tmp/bin/." "$PREFIX/bin/"
[ -d "$tmp/share/man/man1" ] && $sudo cp -a "$tmp/share/man/man1/." "$PREFIX/share/man/man1/"

info "installed cpclip $ver -> $PREFIX/bin/{cpclip,cpadd,cppaste,cpclear}"

# Best-effort warnings.
if have ldd; then
    missing="$(ldd "$PREFIX/bin/cpclip" 2>/dev/null | awk '/not found/{print $1}')"
    [ -n "$missing" ] && info "note: missing runtime libraries: $missing — install libx11 / libxfixes / libwayland-client"
fi
case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) info "note: $PREFIX/bin is not on your PATH" ;;
esac

exit 0
