# Makefile — builds cpclip plus the cpadd/cppaste/cpclear symlinks.
#
# Phase 0 ships the null backend only (pure C, no display-server libs). The
# X11 (Phase 1) and Wayland (Phase 2) sections are scaffolded below: uncomment
# them as each backend lands. See IMPLEMENTATION.md.

CC       ?= cc
CFLAGS   ?= -O2 -g -std=c11 -Wall -Wextra
CPPFLAGS += -D_GNU_SOURCE
LDFLAGS  ?=

PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin

BIN    = cpclip
LINKS  = cpadd cppaste cpclear

# --- Backends -------------------------------------------------------------
# Common objects + the null backend (Phase 0).
OBJS = main.o io_util.o ops_add.o backend_null.o
LIBS =

# Phase 1 — X11:
OBJS   += backend_x11.o
CFLAGS += $(shell pkg-config --cflags x11 xfixes)
LIBS   += $(shell pkg-config --libs x11 xfixes)

# Phase 2 — Wayland (uncomment):
# OBJS   += backend_wayland.o
# CFLAGS += $(shell pkg-config --cflags wayland-client)
# LIBS   += $(shell pkg-config --libs wayland-client)

HEADERS = backend.h io_util.h ops_add.h

all: $(BIN) $(LINKS)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(LINKS): $(BIN)
	ln -sf $(BIN) $@

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

# --- wayland-scanner scaffold (Phase 2) -----------------------------------
# The core clipboard (wl_data_device*) lives in libwayland's built-in protocol,
# so Phase 2's basic path needs no scanning. These rules are here for when we
# add data-control (history) from wayland-protocols' datadir (see "Future").
WL_SCANNER  := $(shell pkg-config --variable=wayland_scanner wayland-scanner 2>/dev/null)
WL_PROTODIR := $(shell pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null)

%-protocol.c: %.xml
	$(WL_SCANNER) private-code  < $< > $@
%-protocol.h: %.xml
	$(WL_SCANNER) client-header < $< > $@

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	for l in $(LINKS); do ln -sf $(BIN) $(DESTDIR)$(BINDIR)/$$l; done

clean:
	rm -f $(BIN) $(LINKS) *.o *-protocol.c *-protocol.h

.PHONY: all install clean
