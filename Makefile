.POSIX:
.SUFFIXES:
.ONESHELL:
.SHELLFLAGS = -ec

VERSION = `git describe --tags --dirty 2>/dev/null || echo 0.2`
PREFIX  = /usr/local
PKG_CONFIG = pkg-config

# Auto-detect compiler: TCC > Clang > GCC (only if CC not set)
ifeq ($(origin CC),default)
  ifeq ($(shell which tcc 2>/dev/null),)
    ifeq ($(shell which clang 2>/dev/null),)
      CC = gcc
    else
      CC = clang
    endif
  else
    CC = tcc
  endif
endif

# Uncomment to build without XWayland support
#XWAYLAND =
#XLIBS =
XWAYLAND = -DXWAYLAND
XLIBS = xcb xcb-icccm

DISTRO  =
LUA_PKG = lua5.4
PKGS    = wayland-server xkbcommon libinput $(LUA_PKG) $(XLIBS)

CFLAGS   = `$(PKG_CONFIG) --cflags $(PKGS) wlroots-0.20` \
	-I. -Iinclude -Isrc -Iparser -Iprotocols \
	-DWLR_USE_UNSTABLE -D_POSIX_C_SOURCE=200809L -DVERSION=\"$(VERSION)\" \
	$(XWAYLAND) -g -Wall -Wextra -Wno-unused-parameter -O2 -std=c11
LDLIBS   = `$(PKG_CONFIG) --libs $(PKGS) wlroots-0.20` -lm $(LIBS)

SMSG_CFLAGS = `$(PKG_CONFIG) --cflags wayland-client` -Wall -Wextra -Wno-unused-parameter
SMSG_LDLIBS = `$(PKG_CONFIG) --libs wayland-client`

SCANNER   = $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)
WLPROTO   = $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)

PROTO_OBJS = protocols/peachwm-ipc-unstable-v2-protocol.o \
	protocols/ext-workspace-v1-protocol.o

all: peachwm peachmsg/peachmsg

release: CC = clang
release: CFLAGS += -Werror -Wpedantic -Wmissing-prototypes -Wstrict-prototypes \
	-Wold-style-definition -Wmissing-declarations -Wimplicit-fallthrough \
	-Wno-gnu-zero-variadic-macro-arguments \
	-march=native
release: peachwm peachmsg/peachmsg

debug: CC = clang
debug: CFLAGS += -Werror -Weverything \
	-Wno-padded -Wno-unsafe-buffer-usage \
	-Wno-c++-keyword -Wno-reserved-macro-identifier -Wno-reserved-identifier \
	-Wno-pre-c11-compat -Wno-implicit-void-ptr-cast -Wno-switch-default \
	-Wno-sign-conversion -Wno-used-but-marked-unused \
	-Wno-cast-align -Wno-disabled-macro-expansion \
	-Wno-double-promotion -Wno-bad-function-cast \
	-Wno-implicit-int-conversion -Wno-shadow \
	-Wno-declaration-after-statement \
	-Wno-switch-enum -Wno-implicit-int-enum-cast \
	-Wno-format-signedness -Wno-cast-qual \
	-Wno-implicit-int-float-conversion -Wno-unused-macros \
	-Wno-unreachable-code-break -Wno-missing-format-attribute \
	-Wno-format-nonliteral -Wno-float-conversion \
	-Wno-tentative-definition-compat -Wno-missing-variable-declarations \
	-Wno-gnu-zero-variadic-macro-arguments \
	-g3 -fno-omit-frame-pointer -fsanitize=address,undefined,leak \
	-fsanitize-trap=all
debug: LDLIBS += -fsanitize=address,undefined,leak
debug: peachwm peachmsg/peachmsg

# Compositor

peachwm: src/peachwm.o src/util.o parser/parser.o \
	src/wlr_ext_workspace_v1.o $(PROTO_OBJS)
	$(CC) $^ $(LDFLAGS) $(LDLIBS) -o $@

src/peachwm.o: src/peachwm.c include/client.h include/ipc.h \
	include/ipc_socket.h include/ext-workspace.h \
	src/wlr_ext_workspace_v1.h parser/parser.h \
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

$(PROTO_OBJS): protocols/%-protocol.o: protocols/%-protocol.c protocols/%-protocol.h
	$(CC) $(CFLAGS) -o $@ -c $<

# Protocol headers and glue code

CURSOR_SHAPE_XML = $(WLPROTO)/staging/cursor-shape/cursor-shape-v1.xml
POINTER_CONSTRAINTS_XML = $(WLPROTO)/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml
XDG_SHELL_XML = $(WLPROTO)/stable/xdg-shell/xdg-shell.xml

protocols/cursor-shape-v1-protocol.h: $(CURSOR_SHAPE_XML)
	$(SCANNER) server-header $< $@

protocols/pointer-constraints-unstable-v1-protocol.h: $(POINTER_CONSTRAINTS_XML)
	$(SCANNER) server-header $< $@

protocols/wlr-layer-shell-unstable-v1-protocol.h: protocols/wlr-layer-shell-unstable-v1.xml
	$(SCANNER) server-header $< $@

protocols/wlr-output-power-management-unstable-v1-protocol.h: protocols/wlr-output-power-management-unstable-v1.xml
	$(SCANNER) server-header $< $@

protocols/xdg-shell-protocol.h: $(XDG_SHELL_XML)
	$(SCANNER) server-header $< $@

protocols/peachwm-ipc-unstable-v2-protocol.h: protocols/peachwm-ipc-unstable-v2.xml
	$(SCANNER) server-header $< $@

protocols/peachwm-ipc-unstable-v2-protocol.c: protocols/peachwm-ipc-unstable-v2.xml
	$(SCANNER) private-code $< $@

protocols/ext-workspace-v1-protocol.h: protocols/ext-workspace-v1.xml
	$(SCANNER) server-header $< $@

protocols/ext-workspace-v1-protocol.c: protocols/ext-workspace-v1.xml
	$(SCANNER) private-code $< $@

# peachmsg IPC client

peachmsg/peachwm-ipc-unstable-v2-protocol.h: protocols/peachwm-ipc-unstable-v2.xml
	$(SCANNER) client-header $< $@

peachmsg/peachwm-ipc-unstable-v2-protocol.c: protocols/peachwm-ipc-unstable-v2.xml
	$(SCANNER) private-code $< $@

peachmsg/peachwm-ipc-unstable-v2-protocol.o: peachmsg/peachwm-ipc-unstable-v2-protocol.c peachmsg/peachwm-ipc-unstable-v2-protocol.h
	$(CC) $(SMSG_CFLAGS) -o $@ -c $<

peachmsg/peachmsg.o: peachmsg/peachmsg.c peachmsg/peachwm-ipc-unstable-v2-protocol.h
	$(CC) $(SMSG_CFLAGS) -o $@ -c $<

peachmsg/peachmsg: peachmsg/peachmsg.o peachmsg/peachwm-ipc-unstable-v2-protocol.o
	$(CC) $^ $(SMSG_CFLAGS) $(SMSG_LDLIBS) -o $@

clean:
	rm -f peachwm peachmsg/peachmsg src/*.o parser/*.o peachmsg/*.o \
		protocols/*.o protocols/*-protocol.h protocols/*-protocol.c \
		peachmsg/*-protocol.h peachmsg/*-protocol.c
	rm -f peachwm-*.tar.gz peachwm-*.pkg.tar.* peachwm_*.deb peachwm-*.rpm
	rm -rf _pkg packaging

install: peachwm peachmsg/peachmsg
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f peachwm $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/peachwm
	cp -f peachmsg/peachmsg $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/peachmsg
	mkdir -p $(DESTDIR)/etc/peachwm
	cp -r example/* $(DESTDIR)/etc/peachwm
	mkdir -p $(DESTDIR)$(PREFIX)/share/wayland-sessions
	cp -f peachwm.desktop $(DESTDIR)$(PREFIX)/share/wayland-sessions/peachwm.desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/peachwm
	rm -f $(DESTDIR)$(PREFIX)/bin/peachmsg
	rm -rf $(DESTDIR)/etc/peachwm
	rm -f $(DESTDIR)$(PREFIX)/share/wayland-sessions/peachwm.desktop

# Packaging
#   make package             - source tarball
#   DISTRO=arch make package - Arch Linux PKGBUILD (build with makepkg if available)
#   DISTRO=debian make package - Debian/Ubuntu .deb (build with dpkg-deb)
#   DISTRO=fedora make package - Fedora .rpm (build with rpmbuild)
#   DISTRO=opensuse make package - OpenSUSE .rpm (build with rpmbuild)
#   DISTRO=gentoo make package - Gentoo ebuild
#   DISTRO=void make package - Void Linux template

_pkg-tarball:
	$(MAKE) release
	rm -rf peachwm-$(VERSION)
	mkdir -p peachwm-$(VERSION)
	cp -r .gitignore Makefile LICENSE README.md include src parser protocols peachmsg example peachwm.desktop peachwm-$(VERSION)/
	cp peachwm peachwm-$(VERSION)/
	tar -czf peachwm-$(VERSION).tar.gz peachwm-$(VERSION)
	rm -rf peachwm-$(VERSION)

_pkg-arch: _pkg-tarball
	rm -rf _pkg/arch
	mkdir -p _pkg/arch
	ver="$(VERSION)"; cat > _pkg/arch/PKGBUILD <<- HEREDOC
		# Maintainer: PeachWM Maintainers
		pkgname=peachwm
		pkgver=$$ver
		pkgrel=1
		pkgdesc="PeachWM - A lightweight Wayland compositor"
		arch=('x86_64')
		url="https://github.com/HuntedByTheIRS/peachwm"
		license=('GPL3')
		depends=(
		  'libinput'
		  'wayland'
		  'wlroots0.20'
		  'libxkbcommon'
		  'lua'
		  'libxcb'
		  'xcb-util-wm'
		)
		makedepends=(
		  'clang'
		  'pkg-config'
		  'wayland-protocols'
		  'git'
		)
		source=("peachwm-$${ver}.tar.gz")
		sha256sums=('SKIP')
		build() {
		  cd "$${srcdir}/peachwm-$${ver}"
		  make release
		}
		package() {
		  cd "$${srcdir}/peachwm-$${ver}"
		  make DESTDIR="$${pkgdir}" PREFIX=/usr install
		}
	HEREDOC
	cp peachwm-$(VERSION).tar.gz _pkg/arch/
	if command -v makepkg >/dev/null 2>&1; then \
		cd _pkg/arch && makepkg -f && cp *.pkg.tar.* ../.. || \
			echo "makepkg failed (try building manually with the PKGBUILD at _pkg/arch/)"; \
	else \
		echo "PKGBUILD generated at _pkg/arch/PKGBUILD"; \
	fi

_pkg-debian: _pkg-tarball
	rm -rf _pkg/debian
	mkdir -p _pkg/debian/DEBIAN
	mkdir -p _pkg/debian/usr/bin
	mkdir -p _pkg/debian/usr/share/doc/peachwm
	mkdir -p _pkg/debian/etc/peachwm
	mkdir -p _pkg/debian/usr/share/wayland-sessions
	cp peachwm _pkg/debian/usr/bin/
	cp peachmsg/peachmsg _pkg/debian/usr/bin/
	cp README.md _pkg/debian/usr/share/doc/peachwm/
	cp -r example/* _pkg/debian/etc/peachwm/
	cp peachwm.desktop _pkg/debian/usr/share/wayland-sessions/peachwm.desktop
	ver="$(VERSION)"; date="$(shell date -R)"; cat > _pkg/debian/DEBIAN/control <<- HEREDOC
		Source: peachwm
		Section: x11
		Priority: optional
		Maintainer: PeachWM Maintainers
		Build-Depends: debhelper-compat (= 13), libwayland-dev, libwlroots-dev (>= 0.20), libxkbcommon-dev, libinput-dev, liblua5.4-dev, pkgconf, libxcb-dev, libxcb-icccm4-dev, wayland-protocols, clang
		Standards-Version: 4.6.2
		Homepage: https://github.com/HuntedByTheIRS/peachwm
		Package: peachwm
		Architecture: linux-any
		Depends: $${shlibs:Depends}, $${misc:Depends}
		Description: A lightweight Wayland compositor
		 PeachWM is a Wayland compositor focused on simplicity and
		 lightweight design. It features Lua configuration, multiple
		 layouts (Dwindle, Master, Monocle, Floating), and XWayland
		 support.
	HEREDOC
	cat > _pkg/debian/DEBIAN/copyright <<- HEREDOC
		Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
		Upstream-Name: peachwm
		Source: https://github.com/HuntedByTheIRS/peachwm
		Files: *
		Copyright: 2026 HuntedByTheIRS
		License: GPL-3+
	HEREDOC
	if command -v dpkg-deb >/dev/null 2>&1; then \
		dpkg-deb --build _pkg/debian peachwm_$(VERSION)_amd64.deb; \
	else \
		echo "Debian package structure at _pkg/debian/"; \
	fi

_pkg-fedora: _pkg-tarball
	rm -rf _pkg/fedora
	mkdir -p _pkg/fedora/SOURCES _pkg/fedora/SPECS
	cp peachwm-$(VERSION).tar.gz _pkg/fedora/SOURCES/
	ver="$(VERSION)"; date="$(shell date +'%a %b %d %Y')"; cat > _pkg/fedora/SPECS/peachwm.spec <<- HEREDOC
		Name:       peachwm
		Version:    $$ver
		Release:    1%{?dist}
		Summary:    A lightweight Wayland compositor
		License:    GPLv3
		URL:        https://github.com/HuntedByTheIRS/peachwm
		Source0:    peachwm-$${ver}.tar.gz
		BuildRequires: clang
		BuildRequires: pkgconfig
		BuildRequires: wayland-devel
		BuildRequires: wayland-protocols-devel
		BuildRequires: wlroots0.20-devel
		BuildRequires: libxkbcommon-devel
		BuildRequires: libinput-devel
		BuildRequires: lua-devel
		BuildRequires: libxcb-devel
		BuildRequires: xcb-util-wm-devel
		BuildRequires: git
		Requires: libinput
		Requires: wayland
		Requires: wlroots0.20
		Requires: libxkbcommon
		Requires: lua
		Requires: libxcb
		Requires: xcb-util-wm
		%description
		PeachWM is a Wayland compositor focused on simplicity and lightweight
		design. It features Lua configuration, multiple layouts (Dwindle,
		Master, Monocle, Floating), and XWayland support.
		%prep
		%setup -q
		%build
		make release
		%install
		make DESTDIR=%{buildroot} PREFIX=/usr install
		%files
		%license LICENSE
		%doc README.md
		%{_bindir}/peachwm
		%{_bindir}/peachmsg
		%{_datadir}/wayland-sessions/peachwm.desktop
		%config(noreplace) /etc/peachwm/*
		%changelog
		* $$date PeachWM Maintainers - $$ver-1
		- Package peachwm $$ver.
	HEREDOC
	if command -v rpmbuild >/dev/null 2>&1; then \
		rpmbuild --define "_topdir $(PWD)/_pkg/fedora" \
			-ba _pkg/fedora/SPECS/peachwm.spec && \
		find _pkg/fedora -name "*.rpm" -exec cp {} . \;; \
	else \
		echo "RPM spec at _pkg/fedora/SPECS/peachwm.spec"; \
	fi

_pkg-opensuse: _pkg-tarball
	rm -rf _pkg/opensuse
	mkdir -p _pkg/opensuse/SOURCES _pkg/opensuse/SPECS
	cp peachwm-$(VERSION).tar.gz _pkg/opensuse/SOURCES/
	ver="$(VERSION)"; date="$(shell date +'%a %b %d %Y')"; cat > _pkg/opensuse/SPECS/peachwm.spec <<- HEREDOC
		Name:       peachwm
		Version:    $$ver
		Release:    1
		Summary:    A lightweight Wayland compositor
		License:    GPL-3.0-or-later
		URL:        https://github.com/HuntedByTheIRS/peachwm
		Source0:    peachwm-$${ver}.tar.gz
		BuildRequires: clang
		BuildRequires: pkgconfig
		BuildRequires: wayland-devel
		BuildRequires: wayland-protocols-devel
		BuildRequires: wlroots0.20-devel
		BuildRequires: libxkbcommon-devel
		BuildRequires: libinput-devel
		BuildRequires: lua54-devel
		BuildRequires: libxcb-devel
		BuildRequires: xcb-util-wm-devel
		BuildRequires: git
		Requires: libinput
		Requires: wayland
		Requires: wlroots0.20
		Requires: libxkbcommon
		Requires: lua54
		Requires: libxcb
		Requires: xcb-util-wm
		%description
		PeachWM is a Wayland compositor focused on simplicity and lightweight
		design. It features Lua configuration, multiple layouts (Dwindle,
		Master, Monocle, Floating), and XWayland support.
		%prep
		%setup -q
		%build
		make release
		%install
		make DESTDIR=%{buildroot} PREFIX=/usr install
		%files
		%license LICENSE
		%doc README.md
		%{_bindir}/peachwm
		%{_bindir}/peachmsg
		%{_datadir}/wayland-sessions/peachwm.desktop
		%config(noreplace) /etc/peachwm/*
		%changelog
		* $$date PeachWM Maintainers - $$ver-1
		- Package peachwm $$ver.
	HEREDOC
	if command -v rpmbuild >/dev/null 2>&1; then \
		rpmbuild --define "_topdir $(PWD)/_pkg/opensuse" \
			-ba _pkg/opensuse/SPECS/peachwm.spec && \
		find _pkg/opensuse -name "*.rpm" -exec cp {} . \;; \
	else \
		echo "RPM spec at _pkg/opensuse/SPECS/peachwm.spec"; \
	fi

_pkg-gentoo: _pkg-tarball
	rm -rf _pkg/gentoo
	mkdir -p _pkg/gentoo
	ver="$(VERSION)"; cat > _pkg/gentoo/peachwm-$${ver}.ebuild <<- HEREDOC
		# Copyright 2026-2026 Gentoo Authors
		# Distributed under the terms of the GNU General Public License v2
		EAPI=8
		DESCRIPTION="A lightweight Wayland compositor"
		HOMEPAGE="https://github.com/HuntedByTheIRS/peachwm"
		SRC_URI=""
		if [[ $${PV} == 9999 ]]; then
			inherit git-r3
			EGIT_REPO_URI="https://github.com/HuntedByTheIRS/peachwm.git"
		else
			SRC_URI="https://github.com/HuntedByTheIRS/peachwm/archive/v$${PV}.tar.gz -> $${P}.tar.gz"
			KEYWORDS="~amd64"
		fi
		LICENSE="GPL-3"
		SLOT="0"
		IUSE="+X"
		RDEPEND="
			dev-libs/libinput:=
			dev-libs/wayland
			dev-libs/wayland-protocols
			gui-libs/wlroots:0.20
			x11-libs/libxkbcommon
			dev-lang/lua:5.4
			X? ( x11-libs/libxcb
				 x11-libs/xcb-util-wm )
		"
		DEPEND="$${RDEPEND}
			>=dev-libs/wayland-protocols-1.24
		"
		BDEPEND="
			virtual/pkgconfig
			sys-devel/clang
		"
		src_compile() {
			make release
		}
		src_install() {
			make DESTDIR="$${D}" PREFIX=/usr install
		}
	HEREDOC
	echo "Ebuild generated at _pkg/gentoo/peachwm-$(VERSION).ebuild"

_pkg-void: _pkg-tarball
	rm -rf _pkg/void
	mkdir -p _pkg/void
	ver="$(VERSION)"; cat > _pkg/void/template <<- HEREDOC
		# Template file for 'peachwm'
		pkgname=peachwm
		version=$$ver
		revision=1
		build_style=gnu-makefile
		make_build_args="release"
		make_install_args="DESTDIR=$${DESTDIR} PREFIX=/usr"
		hostmakedepends="clang pkg-config wayland-protocols git"
		makedepends="wayland-devel wlroots0.20-devel libxkbcommon-devel libinput-devel lua54-devel libxcb-devel xcb-util-wm-devel"
		depends="libinput wayland wlroots0.20 libxkbcommon lua54 libxcb xcb-util-wm"
		short_desc="PeachWM - A lightweight Wayland compositor"
		maintainer="PeachWM Maintainers"
		license="GPL-3.0-or-later"
		homepage="https://github.com/HuntedByTheIRS/peachwm"
		distfiles="https://github.com/HuntedByTheIRS/peachwm/archive/v$${version}.tar.gz"
		checksum=SKIP
	HEREDOC
	echo "Void template generated at _pkg/void/template"

package: _pkg-tarball
	@case "$(DISTRO)" in \
		"") \
			;; \
		arch) \
			$(MAKE) _pkg-arch ;; \
		debian) \
			$(MAKE) _pkg-debian ;; \
		fedora) \
			$(MAKE) _pkg-fedora ;; \
		opensuse) \
			$(MAKE) _pkg-opensuse ;; \
		gentoo) \
			$(MAKE) _pkg-gentoo ;; \
		void) \
			$(MAKE) _pkg-void ;; \
		*) \
			echo "Unknown DISTRO '$(DISTRO)'. Supported: arch, debian, fedora, opensuse, gentoo, void"; \
			exit 1 ;; \
	esac

test:
	@for target in all debug release; do \
		echo "=== Testing 'make $$target' ==="; \
		$(MAKE) $$target && $(MAKE) clean || { echo "FAIL: make $$target"; exit 1; }; \
	done
	@echo "=== Testing 'make package' ==="
	$(MAKE) package && $(MAKE) clean || { echo "FAIL: make package"; exit 1; }
	@for distro in arch debian fedora opensuse gentoo void; do \
		echo "=== Testing 'DISTRO=$$distro make package' ==="; \
		$(MAKE) package DISTRO=$$distro && $(MAKE) clean || { echo "FAIL: DISTRO=$$distro"; exit 1; }; \
	done
	@echo "=== All tests passed ==="
