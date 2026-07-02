/*
 * Layout algorithms extracted from peachwm.c
 * Dwindle, Master/Stack, Monocle, and associated utilities.
 */
#include <stddef.h>
#include <stdlib.h>

#include "util.h"
#include "layout.h"
#include "parser/parser.h"

/* macros (duplicated from peachwm.c) */
#define MAX(A, B)     ((A) > (B) ? (A) : (B))
#define LENGTH(X)     (sizeof X / sizeof X[0])
#define TAGMASK       ((1u << TAGCOUNT) - 1)

static inline int visibleon(Client *c, Monitor *m) {
	return m && c->mon == m
		? (c->isscratchpad ? m->scratchpad_visible
		                   : (int)(c->tags & m->tagset[m->seltags]))
		: 0;
}
#define VISIBLEON(C, M) visibleon((C), (M))

/* globals from peachwm.c */
extern struct wl_list clients;
extern struct wl_list fstack;
extern struct wl_list mons;
extern Monitor *selmon;
extern Config cfg;

/* functions from peachwm.c */
void       resize(Client *c, struct wlr_box geo, int interact);
Client    *focustop(Monitor *m);
void       focusclient(Client *c, int lift);
void       printstatus(void);
void       client_set_suspended(Client *c, int suspended);

/* ================================================================
 * layout table
 * ================================================================ */

const Layout layouts[] = {
	{"><>", NULL},
	{"[T]", dwindle},
	{"[M]", master},
	{"[]",  monocle},
};
const unsigned int layout_count = LENGTH(layouts);

/* ================================================================
 * helpers
 * ================================================================ */

/*
 * Returns the 0-based index of the lowest set tag bit for monitor m.
 * For single-tag views this is the exact tag. For multi-tag views it
 * picks the lowest bit.
 */
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

/* Helper: get current layout for the active tag on monitor m */
const Layout *
curlayout(Monitor *m)
{
	int ti = current_tag_idx(m);
	return m->lt[ti][m->sellt[ti]];
}

/* ================================================================
 * dwindle tree helpers
 * ================================================================ */

static DwindleNode *
dwindle_new_node(void)
{
	DwindleNode *n = ecalloc(1, sizeof(*n));
	n->split_ratio = 1.0f;
	return n;
}

void
dwindle_free_tree(DwindleNode *n)
{
	if (!n)
		return;
	dwindle_free_tree(n->children[0]);
	dwindle_free_tree(n->children[1]);
	free(n);
}

DwindleNode *
dwindle_find_leaf(DwindleNode *n, Client *c)
{
	if (!n)
		return NULL;
	if (!n->is_node)
		return n->client == c ? n : NULL;
	DwindleNode *r = dwindle_find_leaf(n->children[0], c);
	return r ? r : dwindle_find_leaf(n->children[1], c);
}

static DwindleNode *
dwindle_first_leaf(DwindleNode *n)
{
	if (!n)
		return NULL;
	while (n->is_node)
		n = n->children[0];
	return n;
}

/*
 * Recursively distribute geometry downward from node n.
 * gap is inner gap pixels between the two halves.
 */
void
dwindle_recalc(DwindleNode *n, int gap)
{
	if (!n)
		return;

	if (!n->is_node) {
		if (n->client && !n->client->isfullscreen)
			resize(n->client, n->box, 0);
		return;
	}

	/* Wider than tall -> split left/right; taller -> split top/bottom. */
	n->split_top = (n->box.height > n->box.width);

	if (!n->split_top) {
		int w1 = MAX(1, (int)(n->box.width / 2.0f * n->split_ratio) - gap / 2);
		n->children[0]->box =
			(struct wlr_box){n->box.x, n->box.y, w1, n->box.height};
		n->children[1]->box =
			(struct wlr_box){n->box.x + w1 + gap, n->box.y,
			                 MAX(1, n->box.width - w1 - gap), n->box.height};
	} else {
		int h1 = MAX(1, (int)(n->box.height / 2.0f * n->split_ratio) - gap / 2);
		n->children[0]->box =
			(struct wlr_box){n->box.x, n->box.y, n->box.width, h1};
		n->children[1]->box =
			(struct wlr_box){n->box.x, n->box.y + h1 + gap, n->box.width,
			                 MAX(1, n->box.height - h1 - gap)};
	}

	dwindle_recalc(n->children[0], gap);
	dwindle_recalc(n->children[1], gap);
}

/*
 * Insert new_c into the tree, bisecting the focused client's node.
 * It falls back to the first leaf if focused is NULL or not in the tree.
 */
static void
dwindle_insert(DwindleNode **root, Client *new_c, Client *focused)
{
	DwindleNode *new_leaf = dwindle_new_node();
	new_leaf->client = new_c;
	new_leaf->is_node = 0;

	if (!*root) {
		*root = new_leaf;
		return;
	}

	DwindleNode *opening_on = focused ? dwindle_find_leaf(*root, focused) : NULL;
	if (!opening_on)
		opening_on = dwindle_first_leaf(*root);

	DwindleNode *new_parent = dwindle_new_node();
	new_parent->is_node = 1;
	new_parent->box = opening_on->box;
	new_parent->parent = opening_on->parent;
	new_parent->split_top = (opening_on->box.height > opening_on->box.width);
	new_parent->children[0] = opening_on;
	new_parent->children[1] = new_leaf;

	opening_on->parent = new_parent;
	new_leaf->parent = new_parent;

	if (new_parent->parent) {
		if (new_parent->parent->children[0] == opening_on)
			new_parent->parent->children[0] = new_parent;
		else
			new_parent->parent->children[1] = new_parent;
	} else {
		*root = new_parent;
	}
}

/*
 * Remove client c from the tree, promoting its sibling upward.
 */
static void
dwindle_remove(DwindleNode **root, Client *c)
{
	DwindleNode *leaf = dwindle_find_leaf(*root, c);
	if (!leaf)
		return;

	DwindleNode *parent = leaf->parent;
	if (!parent) {
		free(leaf);
		*root = NULL;
		return;
	}

	DwindleNode *sibling =
		(parent->children[0] == leaf) ? parent->children[1] : parent->children[0];
	DwindleNode *grandparent = parent->parent;

	sibling->parent = grandparent;
	if (grandparent) {
		if (grandparent->children[0] == parent)
			grandparent->children[0] = sibling;
		else
			grandparent->children[1] = sibling;
	} else {
		*root = sibling;
	}

	free(leaf);
	free(parent);
}

/* Remove c from every monitor's per-tag tree.  Called from unmapnotify(). */
void
dwindle_remove_client(Client *c)
{
	Monitor *m = c->mon;
	if (!m)
		return;
	for (int i = 0; i < TAGCOUNT; i++)
		dwindle_remove(&m->dwindle_root[i], c);
}

/* ================================================================
 * dwindle
 * ================================================================ */

void
dwindle(Monitor *m)
{
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
	DwindleNode **root = &m->dwindle_root[ti];

	/* prune leaves whose clients are no longer tiled here */
	{
		DwindleNode *stack[512];
		Client *stale[512];
		int sp = 0, sc = 0;

		if (*root)
			stack[sp++] = *root;

		while (sp > 0) {
			DwindleNode *nd = stack[--sp];
			if (!nd->is_node) {
				int found = 0;
				wl_list_for_each(c, &clients, link) {
					if (c == nd->client && VISIBLEON(c, m) && !c->isfloating) {
						found = 1;
						break;
					}
				}
				if (!found && sc < 512)
					stale[sc++] = nd->client;
			} else {
				if (nd->children[1])
					stack[sp++] = nd->children[1];
				if (nd->children[0])
					stack[sp++] = nd->children[0];
			}
		}

		for (int i = 0; i < sc; i++)
			dwindle_remove(root, stale[i]);
	}

	/*
	 * Insert any newly visible client, splitting the focused node.
	 * Use m->dwindle_focus[ti] so it actually splits what the user
	 * was looking at when they spawned the window, rather than whatever
	 * focustop() happens to return.
	 */
	Client *focused = m->dwindle_focus[ti];

	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;
		if (!dwindle_find_leaf(*root, c)) {
			dwindle_insert(root, c, focused);
			focused = c;
		}
	}

	/* assign root box and recurse */
	if (*root) {
		(*root)->box = (struct wlr_box){
			m->w.x + e,
			m->w.y + e,
			MAX(1, m->w.width - 2 * e),
			MAX(1, m->w.height - 2 * e),
		};
		dwindle_recalc(*root, e);
	}
}

/* ================================================================
 * master / stack
 * ================================================================ */

static void
master_arrange(Monitor *m, int ti)
{
	Client *c, *master_c = NULL;
	Client *stack[256];
	int nstack = 0, n = 0;

	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, m) && !c->isfloating) {
			if (c == m->master_master[ti])
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
				m->master_master[ti] = c;
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

	int master_w = MAX(1, (int)(aw * m->mfact));

	if (m->master_side[ti] == 0) {
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

/* Clear master references to client c on all tags of its monitor. */
void
master_remove_client(Client *c)
{
	Monitor *m = c->mon;
	if (!m)
		return;
	for (int i = 0; i < TAGCOUNT; i++) {
		if (m->master_master[i] == c)
			m->master_master[i] = NULL;
	}
}

/* ================================================================
 * monocle
 * ================================================================ */

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

	/* Stack all windows at the same position and raise the focused one to
	 * top. */
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

/* ================================================================
 * swaptiled  (tile-drag swap)
 * ================================================================ */

void
swaptiled(Client *a, Client *b)
{
	Monitor *m;
	if (!a || !b || a == b)
		return;

	wl_list_for_each(m, &mons, link) {
		for (int i = 0; i < TAGCOUNT; i++) {
			DwindleNode **root = &m->dwindle_root[i];
			if (!*root)
				continue;
			DwindleNode *la = dwindle_find_leaf(*root, a);
			DwindleNode *lb = dwindle_find_leaf(*root, b);
			if (!la || !lb)
				continue;
			la->client = b;
			lb->client = a;
			int e = (m->gaps && cfg.appearance.gaps)
				? (int)cfg.appearance.gaps
				: 0;
			(*root)->box = (struct wlr_box){
				m->w.x + e,
				m->w.y + e,
				MAX(1, m->w.width - 2 * e),
				MAX(1, m->w.height - 2 * e),
			};
			dwindle_recalc(*root, e);
		}
	}

	focusclient(a, 1);
	printstatus();
}
