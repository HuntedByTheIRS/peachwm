/*
 * See LICENSE file for copyright and license details.
 * PeachWM is a fork of swindle, which is also GPLv3 licensed.
 *
 * Scratchpad workspace — a hidden layer of clients that can be toggled
 * as a group, with monocle-style cycling within the scratchpad.
 */
#include <wlr/types/wlr_output_layout.h>
#include <scenefx/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "client.h"
#include "layout.h"
#include "monitor.h"
#include "scratchpad.h"

/* ------------------------------------------------------------------ */
/*  Extern functions from peachwm.c / layout.c / ipc client           */
/* ------------------------------------------------------------------ */

void arrange(Monitor *m);
void focusclient(Client *c, int lift);
Client *focustop(Monitor *m);
void printstatus(void);
void resize(Client *c, struct wlr_box geo, int interact);
void setfloating(Client *c, int floating);
void client_set_suspended(Client *c, int suspended);
void dwindle_remove_client(Client *c);
void master_remove_client(Client *c);

/* ------------------------------------------------------------------ */
/*  scratchpad_arrange  — centre & resize all scratchpad clients       */
/* ------------------------------------------------------------------ */

void scratchpad_arrange(Monitor *m) {
	Client *c;
	int sw = m->w.width * 0.8;
	int sh = m->w.height * 0.8;
	int sx = m->w.x + (m->w.width - sw) / 2;
	int sy = m->w.y + (m->w.height - sh) / 2;

	wl_list_for_each(c, &clients, link) {
		if (c->mon != m || !c->isscratchpad)
			continue;
		wlr_scene_node_reparent(&c->scene->node, layers[LyrScratch]);
		resize(c, (struct wlr_box){sx, sy, sw, sh}, 0);
	}
}

/* ------------------------------------------------------------------ */
/*  togglescratchpad  — show / hide the scratchpad workspace           */
/* ------------------------------------------------------------------ */

void togglescratchpad(const Arg *arg) {
	Monitor *m = selmon;
	Client *c, *focus = nullptr;

	if (!m)
		return;

	if (!m->scratchpad_visible) {
		/* Save current focus before showing scratchpad */
		m->scratchpad_prev_focus = focustop(m);
	}

	m->scratchpad_visible = !m->scratchpad_visible;

	if (m->scratchpad_visible) {
		/* Find the most recently focused scratchpad client */
		scratchpad_arrange(m);
		wl_list_for_each(c, &fstack, flink) {
			if (c->mon == m && c->isscratchpad) {
				focus = c;
				break;
			}
		}
		/* Show only that one (monocle-like) */
		wl_list_for_each(c, &clients, link) {
			if (c->mon == m && c->isscratchpad) {
				wlr_scene_node_set_enabled(&c->scene->node, c == focus);
				client_set_suspended(c, c != focus);
			}
		}
		m->scratchpad_current = focus;
		if (focus)
			focusclient(focus, 1);
	} else {
		/* Hide all scratchpad clients */
		wl_list_for_each(c, &clients, link) {
			if (c->mon == m && c->isscratchpad) {
				wlr_scene_node_set_enabled(&c->scene->node, 0);
				client_set_suspended(c, 1);
			}
		}
		m->scratchpad_current = nullptr;
		/* Restore focus to previous client */
		if (m->scratchpad_prev_focus && VISIBLEON(m->scratchpad_prev_focus, m))
			focusclient(m->scratchpad_prev_focus, 1);
		else
			focusclient(focustop(m), 1);
	}
	printstatus();
}

/* ------------------------------------------------------------------ */
/*  swapdirscratchpad  — move client into / out of scratchpad          */
/* ------------------------------------------------------------------ */

void swapdirscratchpad(const Arg *arg) {
	Client *sel = focustop(selmon);

	if (!sel || sel->isfullscreen || !selmon)
		return;

	if (sel->isscratchpad) {
		/* Move FROM scratchpad to current workspace */
		Client *next = nullptr, *n;
		sel->isscratchpad = 0;
		sel->isfloating = 0;
		sel->tags = selmon->tagset[selmon->seltags];
		if (selmon->scratchpad_visible) {
			/* Find another scratchpad client to show */
			wl_list_for_each(n, &fstack, flink) {
				if (n != sel && n->isscratchpad && n->mon == selmon) {
					scratchpad_arrange(selmon);
					next = n;
					break;
				}
			}
			if (!next)
				selmon->scratchpad_current = nullptr;
			else
				selmon->scratchpad_current = next;
		} else {
			selmon->scratchpad_current = nullptr;
		}
		/* setfloating reparents + arranges; let it manage visibility */
		setfloating(sel, 0);
		if (next)
			focusclient(next, 1);
		else
			focusclient(focustop(selmon), 1);
	} else {
		/* Move TO scratchpad */
		Client *old = selmon->scratchpad_current;
		dwindle_remove_client(sel);
		master_remove_client(sel);
		sel->isscratchpad = 1;
		sel->isfloating = 1;
		setfloating(sel, 1);
		if (selmon->scratchpad_visible) {
			/* Hide previous scratchpad client if different */
			if (old && old != sel) {
				wlr_scene_node_set_enabled(&old->scene->node, 0);
				client_set_suspended(old, 1);
			}
			scratchpad_arrange(selmon);
			wlr_scene_node_set_enabled(&sel->scene->node, 1);
			client_set_suspended(sel, 0);
			selmon->scratchpad_current = sel;
			focusclient(sel, 1);
		} else {
			wlr_scene_node_set_enabled(&sel->scene->node, 0);
			client_set_suspended(sel, 1);
			arrange(selmon);
			focusclient(focustop(selmon), 1);
		}
	}

	printstatus();
}

/* ------------------------------------------------------------------ */
/*  scratchpad_focusdir_cycle  — cycle focus among scratchpad clients  */
/*  Called from focusdir() when the focused client is a scratchpad and  */
/*  the scratchpad workspace is visible.                                */
/* ------------------------------------------------------------------ */

void scratchpad_focusdir_cycle(const Arg *arg) {
	Client *sel = focustop(selmon);
	Client *c, *first = nullptr, *next = nullptr, *prev = nullptr;
	int found = 0;

	wl_list_for_each(c, &clients, link) {
		if (!c->isscratchpad || c->mon != selmon)
			continue;
		if (!first)
			first = c;
		if (found) {
			next = c;
			break;
		}
		if (c == sel) {
			found = 1;
			continue;
		}
		prev = c;
	}

	Client *target = nullptr;
	if (arg->i == WLR_DIRECTION_DOWN || arg->i == WLR_DIRECTION_RIGHT) {
		if (next)
			target = next;
		else if (first)
			target = first;
	} else {
		if (prev)
			target = prev;
		else {
			/* wrap to last */
			wl_list_for_each(c, &clients, link)
				if (c->isscratchpad && c->mon == selmon)
					target = c;
		}
	}

	if (target && target != sel) {
		if (selmon->scratchpad_current)
			wlr_scene_node_set_enabled(
				&selmon->scratchpad_current->scene->node, 0);
		wlr_scene_node_set_enabled(&target->scene->node, 1);
		client_set_suspended(target, 0);
		selmon->scratchpad_current = target;
		focusclient(target, 1);
	}
}
