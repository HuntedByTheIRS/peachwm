#pragma once

#include <stdint.h>
#include <stddef.h>

/* Default tag count — define before including to override */
#ifndef TAGCOUNT
#define TAGCOUNT 9
#endif

/* Forward declarations for types used below */
struct Client;
struct Monitor;

/* Argument union used by keybind functions */
typedef union {
	int i;
	uint32_t ui;
	float f;
	const void *v;
} Arg;

/* Compute array length */
#define LENGTH(X) (sizeof(X) / sizeof((X)[0]))

/* Bitmask covering all tags */
#define TAGMASK ((1u << TAGCOUNT) - 1)

/* Convenience min/max */
#define MAX(A, B) ((A) > (B) ? (A) : (B))
#define MIN(A, B) ((A) < (B) ? (A) : (B))

extern struct Client **client_arr;
extern int nclients;
extern struct Client **fstack_arr;
extern int nfstack;

/* Convenience wrapper for the function below */
#define VISIBLEON(C, M) visibleon((C), (M))

/* Determine whether client @c is visible on monitor @m */
static inline int visibleon(struct Client *c, struct Monitor *m) {
	return m && c->mon == m
		? (c->isscratchpad ? m->scratchpad_visible
		                   : (int)(c->tags & m->tagset[m->seltags]))
		: 0;
}
