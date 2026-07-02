#pragma once

#include <wlr/types/wlr_scene.h>
#include "client.h"
#include "monitor.h"
#include "common.h"

/* Forward declarations from peachwm.c */
extern struct wl_list clients;
extern struct wl_list fstack;
extern Monitor *selmon;

/* The scene layer trees — defined in peachwm.c.
 * We declare the full size so callers can index with
 * any Lyr* constant. */
#define NUM_LAYERS 9
extern struct wlr_scene_tree *layers[];

/* Scene layer indices */
enum {
	LyrBg,          /* 0 */
	LyrBottom,      /* 1 */
	LyrTile,        /* 2 */
	LyrFloat,       /* 3 */
	LyrScratch,     /* 4 */
	LyrTop,         /* 5 */
	LyrFS,          /* 6 */
	LyrOverlay,     /* 7 */
	LyrBlock,       /* 8 */
	/* NUM_LAYERS = 9 */
};

/* ----------- Extracted scratchpad functions ----------- */

/* Reparent + resize every scratchpad client of @m to a centred 80% box */
void scratchpad_arrange(Monitor *m);

/* Toggle the scratchpad workspace on/off */
void togglescratchpad(const Arg *arg);

/* Move focused client into/out-of scratchpad.
 * When inside scratchpad, pops it out to the current workspace.
 * When outside, moves it into the scratchpad. */
void swapdirscratchpad(const Arg *arg);

/* Cycle focus among scratchpad clients (used by focusdir). */
void scratchpad_focusdir_cycle(const Arg *arg);
