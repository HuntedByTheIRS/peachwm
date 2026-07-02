#ifndef PEACHWM_LAYOUT_H
#define PEACHWM_LAYOUT_H

#include "monitor.h"
#include "client.h"

/* dwindle binary tree node */
struct DwindleNode {
	DwindleNode *children[2];
	DwindleNode *parent;
	Client *client;
	struct wlr_box box;
	float split_ratio;
	int split_top;
	int is_node;
};

/* layout table and count */
extern const Layout layouts[];
extern const unsigned int layout_count;

/* layout entry points (called via Layout.arrange function pointer) */
void dwindle(Monitor *m);
void master(Monitor *m);
void monocle(Monitor *m);

/* helpers used across modules */
int current_tag_idx(Monitor *m);
const Layout *curlayout(Monitor *m);

/* dwindle tree helpers (exposed for swapdir) */
DwindleNode *dwindle_find_leaf(DwindleNode *n, Client *c);
void dwindle_recalc(DwindleNode *n, int gap);

/* lifecycle helpers (called from peachwm.c unmap/cleanup) */
void dwindle_free_tree(DwindleNode *n);
void dwindle_remove_client(Client *c);
void master_remove_client(Client *c);

/* tile drag swap (called from buttonpress handler) */
void swaptiled(Client *a, Client *b);

#endif /* PEACHWM_LAYOUT_H */
