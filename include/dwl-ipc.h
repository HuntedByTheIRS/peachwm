#include "dwl-ipc-unstable-v2-protocol.h"

/* Forward declarations for dwl.c static functions */

static void arrange(Monitor *m);
static void focusclient(Client *c, int lift);
static Client *focustop(Monitor *m);
static void printstatus(void);

/* globals  */

static struct wl_global *ipc_global = NULL;
/* All bound manager objects */
static struct wl_list ipc_managers; /* IpcManager.link */

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

/* helpers  */

static IpcOutput *
ipc_output_for_mon(IpcManager *mgr, Monitor *m)
{
	IpcOutput *o;
	wl_list_for_each(o, &mgr->outputs, link)
		if (o->mon == m)
			return o;
	return NULL;
}

/* Send the full state of monitor m to one IpcOutput resource */
static void
ipc_send_output_state(IpcOutput *ipc_out)
{
	Monitor *m = ipc_out->mon;
	struct wl_resource *res = ipc_out->resource;
	Client *c;
	uint32_t occ = 0, urg = 0, sel_tags = 0;
	int tag, focused;

	/* Compute occupancy/urgency (same logic as printstatus, I think) */
	wl_list_for_each(c, &clients, link) {
		if (c->mon != m)
			continue;
		occ |= c->tags;
		if (c->isurgent)
			urg |= c->tags;
	}

	c = focustop(m);
	sel_tags = c ? c->tags : 0;

	/* active */
	zdwl_ipc_output_v2_send_active(res, m == selmon ? 1 : 0);

	/* per-tag state */
	for (tag = 0; tag < TAGCOUNT; tag++) {
		uint32_t mask   = 1u << tag;
		uint32_t state  = ZDWL_IPC_OUTPUT_V2_TAG_STATE_NONE;
		uint32_t clients_on_tag = 0;

		if (m->tagset[m->seltags] & mask)
			state = ZDWL_IPC_OUTPUT_V2_TAG_STATE_ACTIVE;
		if (urg & mask)
			state = ZDWL_IPC_OUTPUT_V2_TAG_STATE_URGENT;
		if (occ & mask)
			clients_on_tag = 1; /* report ≥1, not exact count */

		focused = (sel_tags & mask) ? 1 : 0;
		zdwl_ipc_output_v2_send_tag(res, tag, state, clients_on_tag, focused);
	}

	/* layout index */
	{
		uint32_t i;
		for (i = 0; i < LENGTH(layouts); i++)
			if (&layouts[i] == m->lt[m->sellt])
				break;
		zdwl_ipc_output_v2_send_layout(res, i);
	}

	/* layout symbol */
	zdwl_ipc_output_v2_send_layout_symbol(res, m->ltsymbol);

	/* title/appid/fullscreen/floating */
	if (c) {
		zdwl_ipc_output_v2_send_title(res, client_get_title(c));
		zdwl_ipc_output_v2_send_appid(res, client_get_appid(c));
		if (wl_resource_get_version(res) >= 2) {
			zdwl_ipc_output_v2_send_fullscreen(res, c->isfullscreen ? 1 : 0);
			zdwl_ipc_output_v2_send_floating(res, c->isfloating ? 1 : 0);
		}
	} else {
		zdwl_ipc_output_v2_send_title(res, "");
		zdwl_ipc_output_v2_send_appid(res, "");
		if (wl_resource_get_version(res) >= 2) {
			zdwl_ipc_output_v2_send_fullscreen(res, 0);
			zdwl_ipc_output_v2_send_floating(res, 0);
		}
	}

	/* frame — signals end of this batch of events */
	zdwl_ipc_output_v2_send_frame(res);
}

/* Called instead of (or alongside) the old printstatus() */
static void
ipc_printstatus(void)
{
	IpcManager *mgr;
	Monitor *m;

	wl_list_for_each(mgr, &ipc_managers, link) {
		wl_list_for_each(m, &mons, link) {
			IpcOutput *out = ipc_output_for_mon(mgr, m);
			if (out)
				ipc_send_output_state(out);
		}
	}
}

/* zdwl_ipc_output_v2 requests */

static void
ipc_output_handle_release(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
ipc_output_handle_set_tags(struct wl_client *client, struct wl_resource *resource,
		uint32_t tagmask, uint32_t toggle_tagset)
{
	IpcOutput *out = wl_resource_get_user_data(resource);
	if (!out) return;
	Monitor *m = out->mon;
	if (!m) return;

	if (toggle_tagset)
		m->seltags ^= 1;
	m->tagset[m->seltags] = tagmask & TAGMASK;
	focusclient(focustop(m), 1);
	arrange(m);
	printstatus();
}

static void
ipc_output_handle_set_client_tags(struct wl_client *client,
		struct wl_resource *resource, uint32_t and_tags, uint32_t xor_tags)
{
	IpcOutput *out = wl_resource_get_user_data(resource);
	if (!out) return;
	Monitor *m = out->mon;
	Client *sel;
	uint32_t newtags;
	if (!m) return;

	sel = focustop(m);
	if (!sel) return;

	newtags = (sel->tags & and_tags) ^ xor_tags;
	if (!newtags) return;
	sel->tags = newtags;
	focusclient(focustop(m), 1);
	arrange(m);
	printstatus();
}

static void
ipc_output_handle_set_layout(struct wl_client *client,
		struct wl_resource *resource, uint32_t idx)
{
	IpcOutput *out = wl_resource_get_user_data(resource);
	if (!out) return;
	Monitor *m = out->mon;
	if (!m || idx >= LENGTH(layouts)) return;

	m->lt[m->sellt] = &layouts[idx];
	strncpy(m->ltsymbol, layouts[idx].symbol, LENGTH(m->ltsymbol));
	arrange(m);
	printstatus();
}

static const struct zdwl_ipc_output_v2_interface ipc_output_impl = {
	.release          = ipc_output_handle_release,
	.set_tags         = ipc_output_handle_set_tags,
	.set_client_tags  = ipc_output_handle_set_client_tags,
	.set_layout       = ipc_output_handle_set_layout,
};

static void
ipc_output_destroy(struct wl_resource *resource)
{
	IpcOutput *out = wl_resource_get_user_data(resource);
	wl_list_remove(&out->link);
	free(out);
}

/* zdwl_ipc_manager_v2 requests */

static void
ipc_manager_handle_release(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
ipc_manager_handle_get_output(struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *output_resource)
{
	IpcManager *mgr = wl_resource_get_user_data(resource);
	struct wlr_output *wlr_out = wlr_output_from_resource(output_resource);
	Monitor *m = (wlr_out && wlr_out->data) ? wlr_out->data : NULL;

	IpcOutput *out = ecalloc(1, sizeof(*out));
	out->mon = m;
	out->resource = wl_resource_create(client, &zdwl_ipc_output_v2_interface,
			wl_resource_get_version(resource), id);
	if (!out->resource) {
		free(out);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(out->resource, &ipc_output_impl,
			out, ipc_output_destroy);
	wl_list_insert(&mgr->outputs, &out->link);

	if (m)
		ipc_send_output_state(out);
}

static const struct zdwl_ipc_manager_v2_interface ipc_manager_impl = {
	.release    = ipc_manager_handle_release,
	.get_output = ipc_manager_handle_get_output,
};

static void
ipc_manager_destroy(struct wl_resource *resource)
{
	IpcManager *mgr = wl_resource_get_user_data(resource);
	IpcOutput *out, *tmp;
	wl_list_for_each_safe(out, tmp, &mgr->outputs, link)
		wl_resource_destroy(out->resource);
	wl_list_remove(&mgr->link);
	free(mgr);
}

/* global bind */

static void
ipc_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	IpcManager *mgr = ecalloc(1, sizeof(*mgr));
	wl_list_init(&mgr->outputs);
	mgr->resource = wl_resource_create(client, &zdwl_ipc_manager_v2_interface,
			version, id);
	if (!mgr->resource) {
		free(mgr);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(mgr->resource, &ipc_manager_impl,
			mgr, ipc_manager_destroy);
	wl_list_insert(&ipc_managers, &mgr->link);

	/* Announce tags and layouts */
	zdwl_ipc_manager_v2_send_tags(mgr->resource, TAGCOUNT);
	for (uint32_t i = 0; i < LENGTH(layouts); i++)
		zdwl_ipc_manager_v2_send_layout(mgr->resource, layouts[i].symbol);
}

static void
ipc_init(void)
{
	wl_list_init(&ipc_managers);
	ipc_global = wl_global_create(dpy, &zdwl_ipc_manager_v2_interface,
			2, NULL, ipc_bind);
}
