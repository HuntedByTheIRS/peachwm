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
	const Layout *lt[TAGCOUNT][2];  /* per-tag layout slots */
	int gaps;
	unsigned int seltags;
	struct wlr_ext_workspace_group_handle_v1 *ext_group;
	unsigned int sellt[TAGCOUNT];   /* per-tag layout toggle index */
	uint32_t tagset[2];
	float mfact;
	int nmaster;
	char ltsymbol[TAGCOUNT][16];    /* per-tag layout symbol */
	int asleep;
	DwindleNode *dwindle_root[TAGCOUNT]; /* one tree per tag */
	struct Client *dwindle_focus[TAGCOUNT];     /* insertion anchor, one per tag */
	/* master/stack layout state */
	struct Client *master_master[TAGCOUNT]; /* the master client (NULL if none) */
	int master_side[TAGCOUNT];       /* 0 = master on left, 1 = master on right */
	int scratchpad_visible;          /* whether scratchpad is shown */
	struct Client *scratchpad_prev_focus;   /* client focused before scratchpad opened */
	struct Client *scratchpad_current;      /* currently visible scratchpad client */
};

