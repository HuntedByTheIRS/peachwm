#pragma once

#include <wlr/types/wlr_output.h>
#include <scenefx/types/wlr_scene.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include "wlr_ext_workspace_v1.h"

struct Client;
struct LayerSurface;

#ifndef TAGCOUNT
#define TAGCOUNT 9
#endif

typedef struct Monitor Monitor;

typedef struct {
	const char *symbol;
	void (*arrange)(Monitor *);
} Layout;

struct DwindleNode {
	int children[2];
	int parent;
	struct Client *client;
	struct wlr_box box;
	float split_ratio;
	int split_top;
	int is_node;
};
typedef struct DwindleNode DwindleNode;

typedef struct {
	struct DwindleNode *nodes;
	int node_count;
	int node_cap;
} DwindleTree;

typedef struct MonitorCold {
	const Layout *lt[TAGCOUNT][2];
	unsigned int sellt[TAGCOUNT];
	float mfact;
	int nmaster;
	char ltsymbol[TAGCOUNT][16];
	DwindleTree dwindle_tree[TAGCOUNT];
	struct Client *dwindle_focus[TAGCOUNT];
	struct Client *master_master[TAGCOUNT];
	int master_side[TAGCOUNT];
} MonitorCold;

struct Monitor {
	struct wl_list link;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;
	struct wlr_scene_rect *fullscreen_bg;
	struct wl_listener frame;
	struct wl_listener destroy;
	struct wl_listener request_state;
	struct wl_listener destroy_lock_surface;
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct wlr_box m;
	struct wlr_box w;
	struct wl_list layers[4];
	struct MonitorCold *cold;
	int gaps;
	unsigned int seltags;
	struct wlr_ext_workspace_group_handle_v1 *ext_group;
	uint32_t tagset[2];
	int asleep;
	int scratchpad_visible;
	struct Client *scratchpad_prev_focus;
	struct Client *scratchpad_current;
};
