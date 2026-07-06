# Fractional Scale — Manual QA Checklist

Run these tests on **real hardware** (nested compositor won't exercise the
wlroots output scaling path). Each item stands alone — you don't need to
complete them in order.

**Prerequisites**

- PeachWM built and running on bare metal (not inside another compositor)
- `wlr-randr` installed (`wlr-randr --help` works)
- `xeyes` / `xterm` installed (for the XWayland test)
- A second monitor for items 3 and 4 (any DPI, any resolution)

---

| # | Test | Setup | Action | Expected Result | Pass / Fail |
|---|------|-------|--------|-----------------|-------------|
| 1 | **Fractional scale rendering** | Set `scale = 1.5` on your monitor in `config.lua` (the catch-all rule or your monitor's named rule). Restart PeachWM. | Open a native Wayland terminal (e.g. kitty, foot, Alacritty). Look at text glyphs and UI element edges. | Text is crisp — no blurriness, no double-vision artifacts. Edges are not smeared. The surface's buffer scale is the next integer up (2), but the compositor's output scale is 1.5. | |
| 2 | **Runtime scale change** | Start PeachWM at scale 1.0. Open a terminal and a native Wayland GUI (e.g. `gtk4-demo` or `gnome-calculator`). | Run `wlr-randr --output <name> --scale 1.75`. Replace `<name>` with your output name (run `wlr-randr` with no args to list them). | All windows and panels (layer-shell bars like waybar) re-render at the new scale within one frame. No flicker, no frozen regions. Text remains sharp (not blurry). | |
| 3 | **Hotplug with mixed scale** | Start PeachWM at scale 1.0 on the built-in display. Have a second monitor connected at a **different** physical DPI (e.g. a 4K external at 2x scale config). | Plug the second monitor while PeachWM is running. Configure its scale via `wlr-randr --output <name> --scale 2.0`. Drag a window from the 1x monitor to the 2x monitor. | The window re-renders correctly on each monitor at its respective scale. No corruption, no leftover pixels, no visual glitches at the transition boundary. The window is larger on the 1x display (in logical pixels) and smaller on the 2x display, but physically similar in size. | |
| 4 | **Mixed-DPI side-by-side** | Two monitors active: one at scale 1.0, the other at scale 2.0. Use `wlr-randr` to set them before or after startup. | Move the cursor between monitors. Open a terminal on each. Run a panel bar (waybar) spanning both outputs. | **Cursor**: same physical size on both monitors (not tiny on HiDPI, not huge on LoDPI). **Windows**: text is sharp on both. **Panel**: bar area scales per-output; text and icons in the bar are not stretched or clipped. | |
| 5 | **Config reload (SIGHUP / IPC)** | Start PeachWM at `scale = 1.0`. Have a terminal and a layer-shell panel visible. | Edit `config.lua` to change the scale to 1.5 on your monitor. Reload the config: `killall -SIGHUP peachwm` (or send SIGHUP via IPC `peachmsg reload`). | All surfaces update their scale without restarting the compositor. The terminal and panel re-render at the new scale. Window positions remain stable (no unwanted movement). | |
| 6 | **XWayland on fractional scale** | Start PeachWM with `scale = 1.5`. Have an XWayland server running (bundled; no extra config needed). | Run `xeyes` from a terminal. Observe the window size and pupil tracking. Then run `xterm` and check text rendering. | The xeyes window renders at a correct physical size — not comically small or huge. Pupils track the cursor smoothly. xterm text is readable (may be slightly softer due to integer ceilf fallback, but not garbled). No visual artifacts. | |
| 7 | **No-change path (scale = 1.0)** | Start PeachWM with `scale = 1.0` (the default). | Run PeachWM with `WAYLAND_DEBUG=1` and grep for `fractional_scale` events: `WAYLAND_DEBUG=1 peachwm 2>&1 | grep -i scale`. Open and close a few windows. | No `wlr_fractional_scale_v1_notify_scale` calls after the initial commit. The threshold check (`fabsf(scale - current) > 0.001f`) prevents redundant notifications when scale hasn't actually changed. | |
| 8 | **Edge extremes** | For each extreme, start a **separate** PeachWM session. | **(a)** Set `scale = 0.5` in config, restart. Open a terminal, run a few apps, move windows around. **(b)** Set `scale = 3.0` in config, restart. Do the same. | The compositor does not crash on startup or during use. Windows are renderable at both extremes — text is readable at 0.5x (very small but not corrupted) and appropriately large at 3.0x. No assertion failures, no segfaults. | |

---

## Notes

- **Scale range clamp**: The parser clamps scale to `[0.25, 4.0]` with a warning
  for values above 3.0. Tests 1 through 6 stay within safe bounds.
- **XWayland limitation**: X11 has no fractional scale protocol. PeachWM sends
  `ceilf(scale)` as the integer buffer scale for XWayland surfaces. Some
  softness at non-integer scales is expected.
- **Threshold hysteresis**: Scale re-notification fires only when the change
  exceeds 0.001. This prevents log spam and redundant commits.
- **No automated tests here**: See `test_scale.c` for automated unit tests of
  the scale calculation and clamping logic.
