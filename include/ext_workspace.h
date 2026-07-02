#pragma once

#include "monitor.h"

#define EXT_WORKSPACE_CAPS \
	(EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE | \
	 EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_DEACTIVATE)

struct ext_workspace {
	struct wl_list link;
	uint32_t tag;
	Monitor *m;
	struct wlr_ext_workspace_handle_v1 *handle;
};

/* Functions */
void ext_workspace_createmon(Monitor *m);
void ext_workspace_cleanupmon(Monitor *m);
void ext_workspace_printstatus(Monitor *m);
void workspaces_init(void);

