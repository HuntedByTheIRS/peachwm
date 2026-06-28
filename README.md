# PeachWM

<!--toc:start-->
- [peachwm](#peachwm)
- [Features](#features)
- [Dependencies](#dependencies)
- [Configuration](#configuration)
- [Installation](#installation)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
<!--toc:end-->

## peachwm

peachwm is a fork of [swindle](https://github.com/kantiankant/swindle)
that is designed to be similar to MangoWC, while remaining lightweight.

## Features

- Lua configuration
- Dwindle, Master, Monocle, and Floating layouts
- XWayland support
- Lightweight

## Dependencies

- libinput
- wayland
- wlroots-0.20 (compiled with the libinput backend)
- xkbcommon
- wayland-protocols (compile-time only)
- pkg-config (compile-time only)
- a C compiler (clang recommended)

Install these (and their `-devel` versions if your distro has separate
development packages) and run `make` followed by `doas/sudo make install`,
if you wish to install it (installs to /usr/local/bin/ by default).

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

- [ ] No longer require exiting peachwm upon input config changes
- [ ] Allow status bar requests
- [ ] Add smartgaps
- [ ] Implement scratchpad workspaces
- [ ] Add animations via [scenefx](https://github.com/wlrfx/scenefx)
