sloppy_focus = true
bypass_surface_visibility = false
log_level = "error" -- "silent", "error", "info", "debug"

appearance = {
	border_px = 2,
	gaps = 10, -- 0 = no gaps. simple as.
	smart_gaps = false,

	root_color = 0x222222ff,
	border_color = 0x00000000,
	focus_color = 0xe8e8e8ff,
	urgent_color = 0xff0000ff,

	fullscreen_bg = 0x000000ff,
}

-- Note: the entire input section requires you to restart
-- the compositor once changed
input = {
	repeat_rate = 50,
	repeat_delay = 150,
	tap_to_click = true,
	tap_and_drag = true,
	drag_lock = true,
	natural_scrolling = false,
	disable_while_typing = true,
	left_handed = false,
	middle_button_emulation = false,
	scroll_method = "2fg", -- "2fg", "edge", "button"
	click_method = "button_areas", -- "button_areas", "clickfinger"
	accel_profile = "adaptive", -- "adaptive", "flat"
	accel_speed = 0.0,
}

rules = {
	--   { app_id = "Gimp",    floating = true,  monitor = -1 },
	--   { app_id = "firefox", tags = 1 << 8,    floating = false, monitor = -1 },
}

monitors = {
	-- catch-all rule; name = nil means match anything
	{ name = nil, mfact = 0.55, nmaster = 1, scale = 1.0, layout = "dwindle", x = -1, y = -1 },

	-- HiDPI laptop example:
	-- { name = "eDP-1", mfact = 0.5, nmaster = 1, scale = 2.0,
	--   layout = "dwindle", x = -1, y = -1 },
}

-- autostart: it starts stuff in sequence
autostart = {
	"dbus-update-activation-environment --systemd WAYLAND_DISPLAY XDG_SESSION_TYPE XDG_CURRENT_DESKTOP",
	"systemctl --user import-environment WAYLAND_DISPLAY XDG_SESSION_TYPE XDG_CURRENT_DESKTOP",
	"waybar",
}

keybinds = {
	{ mods = { "logo" }, key = "q", action = "spawn", args = { "alacritty" } },
	{ mods = { "logo" }, key = "space", action = "spawn", args = { "wofi", "--show", "drun" } },
	{ mods = { "logo" }, key = "w", action = "killclient" },
	{ mods = { "logo" }, key = "v", action = "togglefloating" },
	{ mods = { "logo" }, key = "f", action = "togglefullscreen" },
	{ mods = { "logo" }, key = "g", action = "togglegaps" },
	{ mods = { "logo" }, key = "h", action = "focusdir", args = { "left" } },
	{ mods = { "logo" }, key = "j", action = "focusdir", args = { "down" } },
	{ mods = { "logo" }, key = "k", action = "focusdir", args = { "up" } },
	{ mods = { "logo" }, key = "l", action = "focusdir", args = { "right" } },
	{ mods = { "logo", "shift" }, key = "H", action = "swapdir", args = { "left" } },
	{ mods = { "logo", "shift" }, key = "J", action = "swapdir", args = { "down" } },
	{ mods = { "logo", "shift" }, key = "K", action = "swapdir", args = { "up" } },
	{ mods = { "logo", "shift" }, key = "L", action = "swapdir", args = { "right" } },
	{ mods = { "logo" }, key = "Tab", action = "switchlayout" },
	{ mods = { "logo" }, key = "0", action = "view", args = { "all" } },
	{ mods = { "logo" }, key = "comma", action = "focusmon", args = { "left" } },
	{ mods = { "logo" }, key = "period", action = "focusmon", args = { "right" } },
	{ mods = { "logo", "shift" }, key = "less", action = "tagmon", args = { "left" } },
	{ mods = { "logo", "shift" }, key = "greater", action = "tagmon", args = { "right" } },
	{ mods = { "logo", "shift" }, key = "e", action = "quit" },
}

-- layout switchers (also available: setlayout_dwindle, setlayout_master, setlayout_monocle)
-- uncomment to bind directly:
-- { mods = { "logo", "ctrl" }, key = "1", action = "setlayout_dwindle" },
-- { mods = { "logo", "ctrl" }, key = "2", action = "setlayout_master" },
-- { mods = { "logo", "ctrl" }, key = "3", action = "setlayout_monocle" },

for i = 1, 9 do
	local key = tostring(i)
	local mask = 1 << (i - 1)
	table.insert(keybinds, {
		mods = { "logo" },
		key = key,
		action = "view",
		args = { tostring(mask) },
	})
	table.insert(keybinds, {
		mods = { "logo", "ctrl" },
		key = key,
		action = "toggleview",
		args = { tostring(mask) },
	})
	table.insert(keybinds, {
		mods = { "logo", "shift" },
		key = key,
		action = "tag",
		args = { tostring(mask) },
	})
	table.insert(
		keybinds,
		{ mods = { "logo", "ctrl", "shift" }, key = key, action = "toggletag", args = { tostring(mask) } }
	)
end

buttons = {
	{ mods = { "logo" }, button = "left", action = "moveresize", args = { "move" } },
	{ mods = { "logo" }, button = "middle", action = "togglefloating" },
	{ mods = { "logo" }, button = "right", action = "moveresize", args = { "resize" } },
}

scrolls = {
	--   { source = "finger", orientation = "horizontal", mods = { "logo" }, action = "view", args = { "all" } },
	--   { source = "wheel", orientation = "vertical", action = "" },
}
