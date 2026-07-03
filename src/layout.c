#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "layout.h"
#include "parser/parser.h"
#include "common.h"

extern struct wl_list clients;
extern struct wl_list fstack;
extern struct wl_list mons;
extern Monitor *selmon;
extern Config cfg;

void resize(Client *c, struct wlr_box geo, int interact);
Client *focustop(Monitor *m);
void focusclient(Client *c, int lift);
void printstatus(void);
void client_set_suspended(Client *c, int suspended);

const Layout layouts[] = {
	{"><>", nullptr},
	{"[T]", dwindle},
	{"[M]", master},
	{"[]",  monocle},
};
const unsigned int layout_count = LENGTH(layouts);

void
ensure_cold(Monitor *m)
{
	if (m->cold)
		return;
	m->cold = ecalloc(1, sizeof(MonitorCold));
	for (int i = 0; i < TAGCOUNT; i++) {
		m->cold->lt[i][0] = &layouts[0];
		m->cold->lt[i][1] = &layouts[0];
	}
	m->cold->mfact = 0.55f;
	m->cold->nmaster = 1;
}

int
current_tag_idx(Monitor *m)
{
	uint32_t tags = m->tagset[m->seltags] & TAGMASK;
	if (!tags)
		return 0;
	int idx = 0;
	while (!(tags & 1u)) {
		tags >>= 1;
		idx++;
	}
	return idx < TAGCOUNT ? idx : 0;
}

const Layout *
curlayout(Monitor *m)
{
	int ti = current_tag_idx(m);
	ensure_cold(m);
	return m->cold->lt[ti][m->cold->sellt[ti]];
}

static int
dwindle_new_node(DwindleTree *tree)
{
	if (tree->node_count >= tree->node_cap) {
		int new_cap = tree->node_cap ? tree->node_cap * 2 : 8;
		if (tree->nodes) {
			tree->nodes = realloc(tree->nodes,
			                      new_cap * sizeof(DwindleNode));
			memset(&tree->nodes[tree->node_cap], 0,
			       (new_cap - tree->node_cap) * sizeof(DwindleNode));
		} else {
			tree->nodes = ecalloc(new_cap, sizeof(DwindleNode));
		}
		tree->node_cap = new_cap;
	}
	int idx = tree->node_count++;
	tree->nodes[idx].children[0] = -1;
	tree->nodes[idx].children[1] = -1;
	tree->nodes[idx].parent = -1;
	tree->nodes[idx].split_ratio = 1.0f;
	return idx;
}

void
dwindle_free_tree(DwindleTree *tree)
{
	free(tree->nodes);
	tree->nodes = nullptr;
	tree->node_count = 0;
	tree->node_cap = 0;
}

int
dwindle_find_leaf(const DwindleTree *tree, Client *c)
{
	for (int i = 0; i < tree->node_count; i++)
		if (!tree->nodes[i].is_node && tree->nodes[i].client == c)
			return i;
	return -1;
}

static int
dwindle_first_leaf(const DwindleTree *tree)
{
	for (int i = 0; i < tree->node_count; i++)
		if (!tree->nodes[i].is_node)
			return i;
	return -1;
}

void
dwindle_recalc(DwindleTree *tree, int gap)
{
	for (int i = 0; i < tree->node_count; i++) {
		DwindleNode *n = &tree->nodes[i];
		if (!n->is_node)
			continue;
		if (n->children[0] < 0 || n->children[1] < 0)
			continue;

		n->split_top = (n->box.height > n->box.width);

		if (!n->split_top) {
			int w1 = MAX(1, (int)(n->box.width / 2.0f * n->split_ratio) - gap / 2);
			tree->nodes[n->children[0]].box =
				(struct wlr_box){n->box.x, n->box.y, w1,
				                 n->box.height};
			tree->nodes[n->children[1]].box =
				(struct wlr_box){n->box.x + w1 + gap, n->box.y,
				                 MAX(1, n->box.width - w1 - gap),
				                 n->box.height};
		} else {
			int h1 = MAX(1, (int)(n->box.height / 2.0f * n->split_ratio) - gap / 2);
			tree->nodes[n->children[0]].box =
				(struct wlr_box){n->box.x, n->box.y, n->box.width,
				                 h1};
			tree->nodes[n->children[1]].box =
				(struct wlr_box){n->box.x, n->box.y + h1 + gap,
				                 n->box.width,
				                 MAX(1, n->box.height - h1 - gap)};
		}
	}

	for (int i = 0; i < tree->node_count; i++) {
		DwindleNode *n = &tree->nodes[i];
		if (!n->is_node && n->client && !n->client->isfullscreen)
			resize(n->client, n->box, 0);
	}
}

static void
dwindle_insert(DwindleTree *tree, Client *new_c, Client *focused)
{
	int new_leaf_idx = dwindle_new_node(tree);
	tree->nodes[new_leaf_idx].client = new_c;
	tree->nodes[new_leaf_idx].is_node = 0;

	if (tree->node_count == 1)
		return;

	int leaf_idx = focused ? dwindle_find_leaf(tree, focused) : -1;
	if (leaf_idx < 0)
		leaf_idx = dwindle_first_leaf(tree);

	int new_parent_idx = dwindle_new_node(tree);
	DwindleNode *parent = &tree->nodes[new_parent_idx];
	DwindleNode *opening = &tree->nodes[leaf_idx];

	parent->is_node = 1;
	parent->box = opening->box;
	parent->split_top = (opening->box.height > opening->box.width);
	parent->children[0] = leaf_idx;
	parent->children[1] = new_leaf_idx;

	int gp_idx = opening->parent;
	parent->parent = gp_idx;
	opening->parent = new_parent_idx;
	tree->nodes[new_leaf_idx].parent = new_parent_idx;

	if (gp_idx >= 0) {
		DwindleNode *gp = &tree->nodes[gp_idx];
		if (gp->children[0] == leaf_idx)
			gp->children[0] = new_parent_idx;
		else
			gp->children[1] = new_parent_idx;
	}
}

static void
dwindle_remove(DwindleTree *tree, Client *c)
{
	int leaf_idx = dwindle_find_leaf(tree, c);
	if (leaf_idx < 0)
		return;

	int parent_idx = tree->nodes[leaf_idx].parent;
	if (parent_idx < 0) {
		tree->node_count = 0;
		return;
	}

	DwindleNode *parent = &tree->nodes[parent_idx];
	int sibling_idx = (parent->children[0] == leaf_idx)
		? parent->children[1]
		: parent->children[0];
	int gp_idx = parent->parent;

	int old_n = tree->node_count;
	int remap[512];
	int ni = 0;

	for (int i = 0; i < old_n; i++)
		remap[i] = (i == leaf_idx || i == parent_idx) ? -1 : ni++;

	int write = 0;
	for (int read = 0; read < old_n; read++) {
		if (remap[read] >= 0)
			tree->nodes[write++] = tree->nodes[read];
	}
	tree->node_count = ni;

	for (int i = 0; i < ni; i++) {
		DwindleNode *n = &tree->nodes[i];
		if (n->children[0] >= 0)
			n->children[0] = remap[n->children[0]];
		if (n->children[1] >= 0)
			n->children[1] = remap[n->children[1]];
		if (n->parent >= 0)
			n->parent = remap[n->parent];
	}

	int new_sib = remap[sibling_idx];
	int new_gp = gp_idx >= 0 ? remap[gp_idx] : -1;

	tree->nodes[new_sib].parent = new_gp;
	if (new_gp >= 0) {
		DwindleNode *gp = &tree->nodes[new_gp];
		if (gp->children[0] < 0)
			gp->children[0] = new_sib;
		else if (gp->children[1] < 0)
			gp->children[1] = new_sib;
	}
}

void
dwindle_remove_client(Client *c)
{
	Monitor *m = c->mon;
	if (!m || !m->cold)
		return;
	for (int i = 0; i < TAGCOUNT; i++)
		dwindle_remove(&m->cold->dwindle_tree[i], c);
}

void
dwindle(Monitor *m)
{
	ensure_cold(m);
	Client *c;
	int n = 0, e;

	wl_list_for_each(c, &clients, link)
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen)
			n++;

	if (n == 0)
		return;

	e = (m->gaps && !(cfg.appearance.smart_gaps && n == 1))
		? (int)cfg.appearance.gaps
		: 0;

	int ti = current_tag_idx(m);
	DwindleTree *tree = &m->cold->dwindle_tree[ti];

	if (tree->node_count > 0) {
		Client *stale[512];
		int sc = 0;

		for (int i = 0; i < tree->node_count; i++) {
			DwindleNode *nd = &tree->nodes[i];
			if (nd->is_node)
				continue;
			int found = 0;
			wl_list_for_each(c, &clients, link) {
				if (c == nd->client && VISIBLEON(c, m) && !c->isfloating) {
					found = 1;
					break;
				}
			}
			if (!found && sc < 512)
				stale[sc++] = nd->client;
		}

		for (int i = 0; i < sc; i++)
			dwindle_remove(tree, stale[i]);
	}

	Client *focused = m->cold->dwindle_focus[ti];

	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;
		if (dwindle_find_leaf(tree, c) < 0) {
			dwindle_insert(tree, c, focused);
			focused = c;
		}
	}

	if (tree->node_count > 0) {
		int root_idx = -1;
		for (int i = 0; i < tree->node_count; i++) {
			if (tree->nodes[i].parent < 0) {
				root_idx = i;
				break;
			}
		}
		if (root_idx >= 0) {
			tree->nodes[root_idx].box = (struct wlr_box){
				m->w.x + e,
				m->w.y + e,
				MAX(1, m->w.width - 2 * e),
				MAX(1, m->w.height - 2 * e),
			};
			dwindle_recalc(tree, e);
		}
	}
}

static void
master_arrange(Monitor *m, int ti)
{
	ensure_cold(m);
	Client *c, *master_c = nullptr;
	Client *stack[256];
	int nstack = 0, n = 0;

	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, m) && !c->isfloating) {
			if (c == m->cold->master_master[ti])
				master_c = c;
			if (!c->isfullscreen)
				n++;
		}
	}

	if (n == 0)
		return;

	if (!master_c) {
		wl_list_for_each(c, &clients, link) {
			if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen) {
				m->cold->master_master[ti] = c;
				master_c = c;
				break;
			}
		}
	}

	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen &&
		    c != master_c && nstack < 256)
			stack[nstack++] = c;
	}

	int e = (m->gaps && !(cfg.appearance.smart_gaps && n == 1))
		? (int)cfg.appearance.gaps
		: 0;
	int aw = MAX(1, m->w.width - 2 * e);
	int ah = MAX(1, m->w.height - 2 * e);

	if (n == 1) {
		if (master_c && !master_c->isfullscreen)
			resize(master_c,
			       (struct wlr_box){m->w.x + e, m->w.y + e, aw, ah}, 0);
		return;
	}

	int master_w = MAX(1, (int)(aw * m->cold->mfact));

	if (m->cold->master_side[ti] == 0) {
		int stack_x = m->w.x + e + master_w + e;
		int stack_w = MAX(1, aw - master_w - e);

		if (master_c && !master_c->isfullscreen)
			resize(master_c,
			       (struct wlr_box){m->w.x + e, m->w.y + e, master_w, ah},
			       0);

		int sh = MAX(1, (ah - (nstack - 1) * e) / nstack);
		for (int i = 0; i < nstack; i++)
			resize(stack[i],
			       (struct wlr_box){stack_x, m->w.y + e + i * (sh + e),
			                        stack_w, sh},
			       0);
	} else {
		int stack_w = MAX(1, aw - master_w - e);
		int sh = MAX(1, (ah - (nstack - 1) * e) / nstack);

		for (int i = 0; i < nstack; i++)
			resize(stack[i],
			       (struct wlr_box){m->w.x + e, m->w.y + e + i * (sh + e),
			                        stack_w, sh},
			       0);

		if (master_c && !master_c->isfullscreen)
			resize(master_c,
			       (struct wlr_box){m->w.x + e + stack_w + e, m->w.y + e,
			                        master_w, ah},
			       0);
	}
}

void
master(Monitor *m)
{
	int ti = current_tag_idx(m);
	master_arrange(m, ti);
}

void
master_remove_client(Client *c)
{
	Monitor *m = c->mon;
	if (!m || !m->cold)
		return;
	for (int i = 0; i < TAGCOUNT; i++) {
		if (m->cold->master_master[i] == c)
			m->cold->master_master[i] = nullptr;
	}
}

void
monocle(Monitor *m)
{
	Client *c, *sel = focustop(m);
	int n = 0, e;

	wl_list_for_each(c, &clients, link)
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen)
			n++;

	if (n == 0)
		return;

	e = (m->gaps && !(cfg.appearance.smart_gaps && n == 1))
		? (int)cfg.appearance.gaps
		: 0;

	int aw = MAX(1, m->w.width - 2 * e);
	int ah = MAX(1, m->w.height - 2 * e);

	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;
		resize(c, (struct wlr_box){m->w.x + e, m->w.y + e, aw, ah}, 0);
		wlr_scene_node_set_enabled(&c->scene->node, 0);
	}
	if (sel) {
		wlr_scene_node_set_enabled(&sel->scene->node, 1);
		wlr_scene_node_raise_to_top(&sel->scene->node);
	}
}

void
swaptiled(Client *a, Client *b)
{
	Monitor *m;
	if (!a || !b || a == b)
		return;

	wl_list_for_each(m, &mons, link) {
		if (!m->cold)
			continue;
		for (int i = 0; i < TAGCOUNT; i++) {
			DwindleTree *tree = &m->cold->dwindle_tree[i];
			if (!tree->nodes || tree->node_count == 0)
				continue;
			int la = dwindle_find_leaf(tree, a);
			int lb = dwindle_find_leaf(tree, b);
			if (la < 0 || lb < 0)
				continue;
			tree->nodes[la].client = b;
			tree->nodes[lb].client = a;
			int e = (m->gaps && cfg.appearance.gaps)
				? (int)cfg.appearance.gaps
				: 0;
			int root_idx = -1;
			for (int j = 0; j < tree->node_count; j++) {
				if (tree->nodes[j].parent < 0) {
					root_idx = j;
					break;
				}
			}
			if (root_idx >= 0) {
				tree->nodes[root_idx].box = (struct wlr_box){
					m->w.x + e,
					m->w.y + e,
					MAX(1, m->w.width - 2 * e),
					MAX(1, m->w.height - 2 * e),
				};
			}
			dwindle_recalc(tree, e);
		}
	}

	focusclient(a, 1);
	printstatus();
}
