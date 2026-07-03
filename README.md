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
- [scenefx](https://github.com/wlrfx/scenefx) (for rendering effects) — **see note below**
- xkbcommon
- wayland-protocols (compile-time only)
- pkg-config (compile-time only)
- a C23 compiler — GCC 14+ or Clang 18+ (clang recommended)

Install these (and their `-devel` versions if your distro has separate
development packages) and run `make` followed by `doas/sudo make install`,
if you wish to install it (installs to /usr/local/bin/ by default).

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
**not exist as an official release** — it refers to a distro-patched
scenefx that has been updated to build against wlroots 0.20 and had its
SONAME bumped to `libscenefx-0.5.so` to avoid version conflicts.

**How to get a compatible build:**

- **Arch (CachyOS):** Install `scenefx-wlroots20-git` from the
  [CachyOS repo](https://github.com/CachyOS/CachyOS-PKGBUILDS) (used
  by CI — version `0.4.1.r32.g7829fdc-1` or later).
- **Other distros:** Build scenefx from
  [git master](https://github.com/wlrfx/scenefx) and bump the project
  version in `meson.build` to `0.5.0` before installing, or create a
  symlink/wrapper so `pkg-config` finds it as `scenefx-0.5`.

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
