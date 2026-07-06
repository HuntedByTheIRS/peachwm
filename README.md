# PeachWM

<!--toc:start-->
- [PeachWM](#peachwm)
  - [peachwm](#peachwm-1)
  - [Features](#features)
  - [Dependencies](#dependencies)
  - [SceneFX](#scenefx)
    - [Effects](#effects)
    - [⚠️ SceneFX version note](#️-scenefx-version-note)
  - [Configuration](#configuration)
  - [Installation](#installation)
  - [Roadmap](#roadmap)
<!--toc:end-->

## peachwm

~~peachwm is a fork of [swindle](https://github.com/kantiankant/swindle)~~
PeachWM is now a independent wayland compositor based on WLRoots
that is designed to be similar to MangoWC, while remaining lightweight.

PeachWM is maintained by [HuntedByTheIRS](https://github.com/HuntedByTheIRS).

## Features

- Lua configuration
- Dwindle, Master, Monocle, and Floating layouts
- XWayland support
- Lightweight

## Dependencies

- libinput
- wayland
- wlroots-0.20 (compiled with the libinput backend)
- [scenefx](https://github.com/wlrfx/scenefx)
(for rendering effects) — **see note below**
- xkbcommon
- wayland-protocols (compile-time only)
- pkg-config (compile-time only)
- a C23 compiler — Clang 18+ (default) or GCC 14+
- LLD linker (for linking; part of LLVM)

Install these (and their `-devel` versions if your distro has separate
development packages). The project requires **Clang** and the **LLD linker**
(both part of LLVM; install the `clang` and `lld` packages for your distro).

For a development build with debug symbols and sanitizers, run `make` (which
uses the `all` target). For a production install, use `make release` to get an
optimized, stripped binary with LTO and linker hardening.

Then run `doas/sudo make install` to install to `/usr/local/bin` by default.

## SceneFX

PeachWM uses [scenefx](https://github.com/wlrfx/scenefx) for rendering
eye-candy effects on screen. These effects are now considered stable and
no longer experimental.

### Effects

- **Rounded corners** — smooth, configurable corner rounding on windows
- **Shadows** — drop shadows for windows
- **Blur** — background blur behind transparent windows
- More effects are planned and on the way.

### ⚠️ SceneFX version note

PeachWM requires a scenefx build targeting **wlroots 0.20**. The latest
official scenefx release ([0.4.1](https://github.com/wlrfx/scenefx/releases/tag/0.4.1))
targets wlroots 0.19 and is **not compatible**.

The Makefile looks for `scenefx-0.5` via pkg-config. This version does
**not exist as an official release** — you must build scenefx from the
master branch to get a wlroots 0.20 compatible build.

**Building scenefx from source:**

```bash
git clone https://github.com/wlrfx/scenefx.git
cd scenefx
meson setup build
ninja -C build
sudo ninja -C build install
```

The master branch `meson.build` already sets the project version to `0.5.0`
and produces `libscenefx-0.5.so` with the `scenefx-0.5` pkg-config target.
No version patching needed.

**Note:** The default meson install prefix is `/usr/local/`. On some distros
(notably Arch Linux), `pkg-config` does not search `/usr/local/lib/pkgconfig`
and the linker does not search `/usr/local/lib` by default. If `make` fails
with `Package scenefx-0.5 was not found`, symlink the installed files:

```bash
sudo ln -sf /usr/local/lib/pkgconfig/scenefx-0.5.pc /usr/lib/pkgconfig/scenefx-0.5.pc
sudo ln -sf /usr/local/lib/libscenefx-0.5.so /usr/lib/libscenefx-0.5.so
sudo ldconfig
```

**CachyOS users:** Install `scenefx-wlroots20-git` from the
[CachyOS repo](https://github.com/CachyOS/CachyOS-PKGBUILDS) instead.

## Configuration

Read example/config.lua. It should give
you a basic idea of how configuring peachwm works.

> Its good to move/copy /etc/peachwm/config.lua to $HOME/.config/peachwm/,
but will start without this step if you choose not to.

## Installation

```bash
make release -j$(nproc)
sudo make install
mkdir -p $HOME/.config/peachwm/
cp /etc/peachwm/config.lua $HOME/.config/peachwm/config.lua
```

## Roadmap

- [x] No longer require exiting peachwm upon input config changes
- [x] Allow status bar requests (swaymsg like)
- [x] Add smartgaps
- [x] Implement scratchpad workspaces
- [ ] Add animations via [scenefx](https://github.com/wlrfx/scenefx)
