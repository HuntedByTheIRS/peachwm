#pragma once

#include <wlr/types/wlr_output.h>
#include <scenefx/types/wlr_scene.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include "wlr_ext_workspace_v1.h"

/* forward declarations */
struct Client;
struct LayerSurface;
typedef struct DwindleNode DwindleNode;

#ifndef TAGCOUNT
#define TAGCOUNT 9
#endif

typedef struct Monitor Monitor;

typedef struct {
	const char *symbol;
	void (*arrange)(Monitor *);
} Layout;

/* Cold state — accessed only on layout changes, not every frame.
 * Allocated lazily on first dwindle/master layout use. */
typedef struct MonitorCold {
	const Layout *lt[TAGCOUNT][2];
	unsigned int sellt[TAGCOUNT];
	float mfact;
	int nmaster;
	char ltsymbol[TAGCOUNT][16];
	DwindleNode *dwindle_root[TAGCOUNT];
	struct Client *dwindle_focus[TAGCOUNT];
	struct Client *master_master[TAGCOUNT];
	int master_side[TAGCOUNT];
} MonitorCold;

struct Monitor {
	struct wl_list link;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;
	struct wlr_scene_rect *fullscreen_bg; /* See createmon() for info */
	struct wl_listener frame;
	struct wl_listener destroy;
	struct wl_listener request_state;
	struct wl_listener destroy_lock_surface;
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct wlr_box m;         /* monitor area, layout-relative */
	struct wlr_box w;         /* window area, layout-relative */
	struct wl_list layers[4]; /* LayerSurface.link */
	struct MonitorCold *cold; /* lazy-allocated layout state */
	int gaps;
	unsigned int seltags;
	struct wlr_ext_workspace_group_handle_v1 *ext_group;
	uint32_t tagset[2];
	int asleep;
	int scratchpad_visible;          /* whether scratchpad is shown */
	struct Client *scratchpad_prev_focus;   /* client focused before scratchpad opened */
	struct Client *scratchpad_current;      /* currently visible scratchpad client */
};

