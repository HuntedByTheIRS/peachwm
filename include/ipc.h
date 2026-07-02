#pragma once
#include "peachwm-ipc-unstable-v2-protocol.h"
#include "monitor.h"
#include "common.h"

/* Forward declarations from peachwm.c */

extern struct wl_display *dpy;
extern struct wl_list mons;
extern Monitor *selmon;
extern struct wl_list clients;
extern const Layout layouts[];
extern const unsigned int layout_count;

/* Functions from peachwm.c called by IPC */
void arrange(Monitor *m);
void focusclient(Client *c, int lift);
Client *focustop(Monitor *m);
void printstatus(void);
const Layout *curlayout(Monitor *m);
int current_tag_idx(Monitor *m);

/* IPC-specific globals */
extern struct wl_global *ipc_global;
extern struct wl_list ipc_managers;

/* Types */
typedef struct {
	struct wl_resource     *resource;
	struct wl_list          link;     /* ipc_managers */
	struct wl_list          outputs;  /* IpcOutput.link */
} IpcManager;

typedef struct {
	struct wl_resource     *resource;
	Monitor                *mon;
	struct wl_list          link;     /* IpcManager.outputs */
} IpcOutput;

/* Public IPC API */
void ipc_printstatus(void);
void ipc_init(void);
