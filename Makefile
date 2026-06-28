.POSIX:
.SUFFIXES:

VERSION = `git describe --tags --dirty 2>/dev/null || echo 0.2`
PREFIX  = /usr/local
CC      = cc
PKG_CONFIG = pkg-config

# Uncomment to build without XWayland support
#XWAYLAND =
#XLIBS =
XWAYLAND = -DXWAYLAND
XLIBS = xcb xcb-icccm

LUA_PKG = lua5.4
PKGS    = wayland-server xkbcommon libinput $(LUA_PKG) $(XLIBS)

CFLAGS   = `$(PKG_CONFIG) --cflags $(PKGS) wlroots-0.20` \
	-I. -Iinclude -Isrc -Iparser -Iprotocols \
	-DWLR_USE_UNSTABLE -D_POSIX_C_SOURCE=200809L -DVERSION=\"$(VERSION)\" \
	$(XWAYLAND) -g -Wall -Wextra -Wno-unused-parameter -O1 -std=c11 $(CFLAGS)
LDLIBS   = `$(PKG_CONFIG) --libs $(PKGS) wlroots-0.20` -lm $(LIBS)

SMSG_CFLAGS = `$(PKG_CONFIG) --cflags wayland-client` -Wall -Wextra -Wno-unused-parameter
SMSG_LDLIBS = `$(PKG_CONFIG) --libs wayland-client`

SCANNER   = `$(PKG_CONFIG) --variable=wayland_scanner wayland-scanner`
WLPROTO   = `$(PKG_CONFIG) --variable=pkgdatadir wayland-protocols`

PROTO_OBJS = protocols/peachwm-ipc-unstable-v2-protocol.o \
	protocols/ext-workspace-v1-protocol.o

all: peachwm smsg/smsg

# Compositor

peachwm: src/peachwm.o src/util.o parser/parser.o \
	src/wlr_ext_workspace_v1.o $(PROTO_OBJS)
	$(CC) $^ $(LDFLAGS) $(LDLIBS) -o $@

src/peachwm.o: src/peachwm.c include/client.h include/ipc.h \
	include/ext-workspace.h src/wlr_ext_workspace_v1.h parser/parser.h \
	protocols/peachwm-ipc-unstable-v2-protocol.h \
	protocols/ext-workspace-v1-protocol.h \
	protocols/cursor-shape-v1-protocol.h \
	protocols/pointer-constraints-unstable-v1-protocol.h \
	protocols/wlr-layer-shell-unstable-v1-protocol.h \
	protocols/wlr-output-power-management-unstable-v1-protocol.h \
	protocols/xdg-shell-protocol.h
	$(CC) $(CFLAGS) -o $@ -c $<

src/util.o: src/util.c src/util.h
	$(CC) $(CFLAGS) -o $@ -c $<

src/wlr_ext_workspace_v1.o: src/wlr_ext_workspace_v1.c \
	src/wlr_ext_workspace_v1.h protocols/ext-workspace-v1-protocol.h
	$(CC) $(CFLAGS) -o $@ -c $<

parser/parser.o: parser/parser.c parser/parser.h
	$(CC) $(CFLAGS) -o $@ -c $<

$(PROTO_OBJS): protocols/%-protocol.o: protocols/%-protocol.c
	$(CC) $(CFLAGS) -o $@ -c $<

# Protocol headers and glue code

protocols/cursor-shape-v1-protocol.h:
	$(SCANNER) enum-header $(WLPROTO)/staging/cursor-shape/cursor-shape-v1.xml $@

protocols/pointer-constraints-unstable-v1-protocol.h:
	$(SCANNER) enum-header $(WLPROTO)/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml $@

protocols/wlr-layer-shell-unstable-v1-protocol.h:
	$(SCANNER) enum-header protocols/wlr-layer-shell-unstable-v1.xml $@

protocols/wlr-output-power-management-unstable-v1-protocol.h:
	$(SCANNER) server-header protocols/wlr-output-power-management-unstable-v1.xml $@

protocols/xdg-shell-protocol.h:
	$(SCANNER) server-header $(WLPROTO)/stable/xdg-shell/xdg-shell.xml $@

protocols/peachwm-ipc-unstable-v2-protocol.h:
	$(SCANNER) server-header protocols/peachwm-ipc-unstable-v2.xml $@

protocols/peachwm-ipc-unstable-v2-protocol.c:
	$(SCANNER) private-code protocols/peachwm-ipc-unstable-v2.xml $@

protocols/ext-workspace-v1-protocol.h:
	$(SCANNER) server-header protocols/ext-workspace-v1.xml $@

protocols/ext-workspace-v1-protocol.c:
	$(SCANNER) private-code protocols/ext-workspace-v1.xml $@

# smsg IPC client

smsg/peachwm-ipc-unstable-v2-protocol.h:
	$(SCANNER) client-header protocols/peachwm-ipc-unstable-v2.xml $@

smsg/peachwm-ipc-unstable-v2-protocol.c:
	$(SCANNER) private-code protocols/peachwm-ipc-unstable-v2.xml $@

smsg/peachwm-ipc-unstable-v2-protocol.o: smsg/peachwm-ipc-unstable-v2-protocol.c
	$(CC) $(SMSG_CFLAGS) -o $@ -c $<

smsg/smsg.o: smsg/smsg.c smsg/peachwm-ipc-unstable-v2-protocol.h
	$(CC) $(SMSG_CFLAGS) -o $@ -c $<

smsg/smsg: smsg/smsg.o smsg/peachwm-ipc-unstable-v2-protocol.o
	$(CC) $^ $(SMSG_CFLAGS) $(SMSG_LDLIBS) -o $@

clean:
	rm -f peachwm smsg/smsg src/*.o parser/*.o smsg/*.o \
		protocols/*.o protocols/*-protocol.h protocols/*-protocol.c \
		smsg/*-protocol.h smsg/*-protocol.c

install: peachwm smsg/smsg
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f peachwm $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/peachwm
	cp -f smsg/smsg $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/smsg
	mkdir -p $(DESTDIR)/etc/peachwm
	cp -r example/* $(DESTDIR)/etc/peachwm

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/peachwm
	rm -f $(DESTDIR)$(PREFIX)/bin/smsg
	rm -rf $(DESTDIR)/etc/peachwm
