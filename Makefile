# Makefile — builds cpclip plus the cpadd/cppaste/cpclear symlinks.
#
# Layout: hand-written sources in src/, build output (objects + generated
# wayland-scanner glue) in build/, binary + symlinks in the root. See CLAUDE.md.

CC       ?= cc
CFLAGS   ?= -O2 -g -std=c11 -Wall -Wextra
CPPFLAGS ?=
LDFLAGS  ?=

# Version reported by `cpclip --version`. Derived from git for local builds;
# packaging passes VERSION=x explicitly (debian/rules from the changelog, the
# release workflow from the tag) so the binary matches the shipped package.
VERSION  ?= $(or $(shell git describe --tags --always --dirty 2>/dev/null | sed 's/^v//'),0.1.2)

PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin
MANDIR   ?= $(PREFIX)/share/man/man1

SRCDIR    = src
BUILDDIR  = build
VPATH     = $(SRCDIR)              # let make find sources without src/ prefixes

BIN    = cpclip
LINKS  = cpadd cppaste cpclear cuclip cuadd cpsize

# --- wayland-scanner: ext-data-control glue (generated into build/) -------
WL_SCANNER  := $(shell pkg-config --variable=wayland_scanner wayland-scanner)
# The protocol XML is vendored (protocol/) so the build does not depend on the
# system wayland-protocols version shipping this staging protocol — older
# distros (Ubuntu 22.04, Debian stable) predate it.
EXTDC_XML    = protocol/ext-data-control-v1.xml
EXTDC_HDR    = $(BUILDDIR)/ext-data-control-v1-client-protocol.h
EXTDC_SRC    = $(BUILDDIR)/ext-data-control-v1-protocol.c

# --- objects + libraries --------------------------------------------------
# Common + null (Phase 0), X11 (Phase 1), Wayland (Phase 2). proc_util holds the
# fork/handshake plumbing shared by the two forking backends.
OBJS = $(addprefix $(BUILDDIR)/, \
       main.o io_util.o ops_add.o proc_util.o parse_util.o mime_sniff.o \
       backend_null.o backend_x11.o backend_wayland.o \
       ext-data-control-v1-protocol.o)

# Mandatory build flags cpclip cannot compile/link without. Kept OUT of
# CFLAGS/CPPFLAGS so a caller overriding those (distro packaging injects
# hardening flags, CI adds -Werror) never drops the include paths or pkg-config
# flags via a command-line override of CFLAGS.
cpclip_flags := -D_GNU_SOURCE -I$(BUILDDIR) \
                $(shell pkg-config --cflags x11 xfixes wayland-client)
LIBS         := $(shell pkg-config --libs x11 xfixes wayland-client)

HEADERS = $(addprefix $(SRCDIR)/, backend.h io_util.h ops_add.h proc_util.h parse_util.h mime_sniff.h)

all: $(BIN) $(LINKS)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(LINKS): $(BIN)
	ln -sf $(BIN) $@

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# Hand-written sources (found in src/ via VPATH) -> build/*.o
$(BUILDDIR)/%.o: %.c $(HEADERS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(cpclip_flags) -c -o $@ $<

# Stamp the version into main.o only.
$(BUILDDIR)/main.o: cpclip_flags += -DCPCLIP_VERSION='"$(VERSION)"'

# The generated protocol source has a full build/ path, so build it explicitly
# rather than through the VPATH pattern rule.
$(BUILDDIR)/ext-data-control-v1-protocol.o: $(EXTDC_SRC) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(cpclip_flags) -c -o $@ $(EXTDC_SRC)

# The Wayland backend needs the generated header (found via -I$(BUILDDIR)).
$(BUILDDIR)/backend_wayland.o: $(EXTDC_HDR)

$(EXTDC_HDR): | $(BUILDDIR)
	$(WL_SCANNER) client-header $(EXTDC_XML) $@
$(EXTDC_SRC): | $(BUILDDIR)
	$(WL_SCANNER) private-code  $(EXTDC_XML) $@

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	for l in $(LINKS); do ln -sf $(BIN) $(DESTDIR)$(BINDIR)/$$l; done
	install -d $(DESTDIR)$(MANDIR)
	install -m 0644 man/$(BIN).1 $(DESTDIR)$(MANDIR)/$(BIN).1
	for l in $(LINKS); do install -m 0644 man/$$l.1 $(DESTDIR)$(MANDIR)/$$l.1; done

test: all
	./tests/run.sh

# --- unit tests (Criterion; test-only dependency: libcriterion-dev) --------
# Links only the pure modules under test (no main(), no backends, no display
# server). Kept separate from `make test` so the E2E suite stays dependency-free.
CRITERION_CFLAGS := $(shell pkg-config --cflags criterion 2>/dev/null)
CRITERION_LIBS   := $(shell pkg-config --libs criterion 2>/dev/null || echo -lcriterion)
UNIT_SRC  := $(wildcard tests/test_*.c)
UNIT_DEPS := $(addprefix $(BUILDDIR)/, io_util.o ops_add.o parse_util.o mime_sniff.o)

$(BUILDDIR)/test-unit: $(UNIT_SRC) $(UNIT_DEPS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I$(SRCDIR) $(CRITERION_CFLAGS) \
	      $(UNIT_SRC) $(UNIT_DEPS) $(CRITERION_LIBS) -o $@

test-unit: $(BUILDDIR)/test-unit
	./$(BUILDDIR)/test-unit

# Run both suites.
check: test-unit test

clean:
	rm -f $(BIN) $(LINKS)
	rm -rf $(BUILDDIR)

.PHONY: all install test test-unit check clean
