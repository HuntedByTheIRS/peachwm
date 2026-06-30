/*
 * See LICENSE file for copyright and license details.
 * PeachWM IPC Socket - swaymsg-compatible Unix domain socket IPC
 *
 * This is included directly in peachwm.c (similar to ipc.h / client.h).
 * It provides a swaymsg-compatible JSON-based IPC over a Unix domain socket,
 * allowing external tools like waybar to query peachwm state.
 *
 * Protocol (sway/i3-compatible):
 *   Header:  "i3-ipc" magic (6 bytes)
 *            uint32_t payload_length (little-endian)
 *            uint32_t message_type   (little-endian)
 *   Payload: JSON string (UTF-8)
 *
 * Message types:
 *   0 = COMMAND      1 = GET_WORKSPACES   2 = SUBSCRIBE
 *   3 = GET_OUTPUTS  4 = GET_TREE         5 = GET_VERSION
 *
 * Events (type has 0x80000000 bit set):
 *   0x80000000 = workspace   0x80000001 = output
 *   0x80000002 = mode        0x80000003 = window
 */

#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Protocol constants                                                  */
/* ------------------------------------------------------------------ */

#define IPC_MAGIC        "i3-ipc"
#define IPC_MAGIC_LEN    6
#define IPC_HEADER_SIZE  14  /* magic(6) + len(4) + type(4) */

enum ipc_message_type {
	IPC_COMMAND        = 0,
	IPC_GET_WORKSPACES = 1,
	IPC_SUBSCRIBE      = 2,
	IPC_GET_OUTPUTS    = 3,
	IPC_GET_TREE       = 4,
	IPC_GET_VERSION    = 5,
};

/* Event types use the high bit (0x80000000) which exceeds INT_MAX.
 * Use uint32_t constants instead of enum members to stay C11-clean. */
#define IPC_EVENT_WORKSPACE  ((uint32_t)0x80000000u)
#define IPC_EVENT_OUTPUT     ((uint32_t)0x80000001u)
#define IPC_EVENT_MODE       ((uint32_t)0x80000002u)
#define IPC_EVENT_WINDOW     ((uint32_t)0x80000003u)

enum ipc_sub_flag {
	IPC_SUB_WORKSPACE = 1 << 0,
	IPC_SUB_WINDOW    = 1 << 1,
	IPC_SUB_OUTPUT    = 1 << 2,
	IPC_SUB_MODE      = 1 << 3,
};

/* ------------------------------------------------------------------ */
/* Per-client state                                                    */
/* ------------------------------------------------------------------ */

struct ipc_client {
	struct wl_list link;
	int fd;
	uint32_t subscribed;
	struct wl_event_source *src;
	/* partial-read state */
	uint8_t hdr_buf[IPC_HEADER_SIZE];
	size_t hdr_len;
	char *payload;
	size_t payload_size;   /* total expected payload length */
	size_t payload_len;    /* amount read so far */
	size_t payload_alloc;
	uint32_t msg_type;
	int header_done;
};

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

static int ipc_listen_fd = -1;
static struct wl_event_source *ipc_listen_src;
static struct wl_list ipc_clients;
static struct wl_event_loop *ipc_event_loop;
static char ipc_socket_path[256];

/* ------------------------------------------------------------------ */
/* Simple JSON builder                                                 */
/* ------------------------------------------------------------------ */

struct json_writer {
	char *buf;
	size_t len;
	size_t cap;
	int depth;
	int need_comma[32];
};

static void
json_init(struct json_writer *w)
{
	w->cap = 4096;
	w->buf = ecalloc(1, w->cap);
	w->len = 0;
	w->depth = -1;
	memset(w->need_comma, 0, sizeof(w->need_comma));
}

static void
json_finish(struct json_writer *w)
{
	free(w->buf);
}

static void
json_grow(struct json_writer *w, size_t needed)
{
	if (w->len + needed <= w->cap)
		return;
	while (w->cap < w->len + needed)
		w->cap *= 2;
	w->buf = realloc(w->buf, w->cap);
	if (!w->buf)
		die("json_grow: realloc:");
}

static void
json_raw(struct json_writer *w, const char *s, size_t n)
{
	json_grow(w, n);
	memcpy(w->buf + w->len, s, n);
	w->len += n;
}

static void
json_puts(struct json_writer *w, const char *s)
{
	json_raw(w, s, strlen(s));
}

__attribute__((__format__(__printf__, 2, 3)))
static void
json_printf(struct json_writer *w, const char *fmt, ...)
{
	va_list ap;
	int n;
	json_grow(w, 128);
	va_start(ap, fmt);
	n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, ap);
	va_end(ap);
	if ((size_t)n >= w->cap - w->len) {
		json_grow(w, (size_t)n + 1);
		va_start(ap, fmt);
		n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, ap);
		va_end(ap);
	}
	w->len += (size_t)n;
}

static void
json_comma(struct json_writer *w)
{
	if (w->need_comma[w->depth + 1])
		json_puts(w, ",");
	w->need_comma[w->depth + 1] = 1;
}

static void
json_object_start(struct json_writer *w)
{
	json_comma(w);
	json_puts(w, "{");
	++w->depth;
	w->need_comma[w->depth + 1] = 0;
}

static void
json_object_end(struct json_writer *w)
{
	json_puts(w, "}");
	--w->depth;
}

static void
json_array_start(struct json_writer *w)
{
	json_comma(w);
	json_puts(w, "[");
	++w->depth;
	w->need_comma[w->depth + 1] = 0;
}

static void
json_array_end(struct json_writer *w)
{
	json_puts(w, "]");
	--w->depth;
}

static void
json_key(struct json_writer *w, const char *key)
{
	json_comma(w);
	json_printf(w, "\"%s\":", key);
}

static void
json_string(struct json_writer *w, const char *s)
{
	json_comma(w);
	json_puts(w, "\"");
	for (; *s; s++) {
		switch (*s) {
		case '"':  json_puts(w, "\\\""); break;
		case '\\': json_puts(w, "\\\\"); break;
		case '\n': json_puts(w, "\\n");  break;
		case '\r': json_puts(w, "\\r");  break;
		case '\t': json_puts(w, "\\t");  break;
		default:
			if ((unsigned char)*s < 0x20)
				json_printf(w, "\\u%04x",
					    (unsigned char)*s);
			else
				json_raw(w, s, 1);
			break;
		}
	}
	json_puts(w, "\"");
}

static void
json_integer(struct json_writer *w, int64_t val)
{
	json_comma(w);
	json_printf(w, "%" PRId64, val);
}

static void
json_bool(struct json_writer *w, int val)
{
	json_comma(w);
	json_puts(w, val ? "true" : "false");
}

static void
json_float(struct json_writer *w, double val)
{
	json_comma(w);
	json_printf(w, "%.2f", val);
}

#define json_key_string(w, k, v)  do { \
	json_key(w, k); json_string(w, v); } while(0)
#define json_key_int(w, k, v)     do { \
	json_key(w, k); json_integer(w, v); } while(0)
#define json_key_bool(w, k, v)    do { \
	json_key(w, k); json_bool(w, v); } while(0)
#define json_key_float(w, k, v)   do { \
	json_key(w, k); json_float(w, v); } while(0)

/* ------------------------------------------------------------------ */
/* Helper: tag index (0-based) -> workspace name                      */
/* ------------------------------------------------------------------ */

static const char *
ipc_tag_name(int idx)
{
	static const char *names[] = {
		"1", "2", "3", "4", "5", "6", "7", "8", "9",
	};
	if (idx >= 0 && idx < (int)LENGTH(names))
		return names[idx];
	return "?";
}

/* ------------------------------------------------------------------ */
/* Send a raw message (header + payload) to a client fd                */
/* ------------------------------------------------------------------ */

static int
ipc_send_raw(int fd, uint32_t type, const char *payload, size_t len)
{
	uint8_t hdr[IPC_HEADER_SIZE];
	uint32_t nlen = (uint32_t)len;

	memcpy(hdr, IPC_MAGIC, IPC_MAGIC_LEN);
	memcpy(hdr + IPC_MAGIC_LEN, &nlen, sizeof(nlen));
	memcpy(hdr + IPC_MAGIC_LEN + 4, &type, sizeof(type));

	struct iovec iov[2];
	iov[0].iov_base = hdr;
	iov[0].iov_len = IPC_HEADER_SIZE;
	iov[1].iov_base = (void *)payload;
	iov[1].iov_len = len;

	struct msghdr msg = {0};
	msg.msg_iov = iov;
	msg.msg_iovlen = 2;

	ssize_t ret;
	do {
		ret = sendmsg(fd, &msg, MSG_NOSIGNAL);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
		return -1;
	return 0;
}

static int
ipc_send_json(int fd, uint32_t type, struct json_writer *w)
{
	return ipc_send_raw(fd, type, w->buf, w->len);
}

/* ------------------------------------------------------------------ */
/* Broadcast an event to all subscribed clients                        */
/* ------------------------------------------------------------------ */

static void
ipc_broadcast(uint32_t event_type, struct json_writer *w)
{
	struct ipc_client *c;
	uint32_t flag;

	switch (event_type) {
	case IPC_EVENT_WORKSPACE: flag = IPC_SUB_WORKSPACE; break;
	case IPC_EVENT_OUTPUT:    flag = IPC_SUB_OUTPUT;    break;
	case IPC_EVENT_WINDOW:    flag = IPC_SUB_WINDOW;    break;
	case IPC_EVENT_MODE:      flag = IPC_SUB_MODE;      break;
	default:                  flag = ~0u;               break;
	}

	wl_list_for_each(c, &ipc_clients, link) {
		if (c->subscribed & flag)
			ipc_send_json(c->fd, event_type, w);
	}
}

/* ------------------------------------------------------------------ */
/* JSON response builders                                              */
/* ------------------------------------------------------------------ */

static void
ipc_build_workspace(struct json_writer *w, Monitor *m, uint32_t tag_bit,
		    int tag_idx)
{
	Client *c;
	int is_urgent = 0;
	int visible = !!(m->tagset[m->seltags] & tag_bit);
	int num = 0;

	/* compute tag number (1-9) */
	{
		uint32_t t = tag_bit;
		while (t) { num++; t >>= 1; }
	}

	wl_list_for_each(c, &clients, link) {
		if (c->mon != m)
			continue;
		if (c->tags & tag_bit) {
			if (c->isurgent)
				is_urgent = 1;
		}
	}

	json_object_start(w);
	json_key_int(w, "id", (int64_t)tag_bit +
		     ((int64_t)(uintptr_t)m << 32));
	json_key_int(w, "num", num);
	json_key_string(w, "name", ipc_tag_name(tag_idx));
	json_key_bool(w, "visible", visible);
	json_key_bool(w, "focused", visible && m == selmon);
	json_key_bool(w, "urgent", !!is_urgent);

	json_key(w, "rect");
	json_object_start(w);
	json_key_int(w, "x", m->m.x);
	json_key_int(w, "y", m->m.y);
	json_key_int(w, "width", m->m.width);
	json_key_int(w, "height", m->m.height);
	json_object_end(w);

	json_key_string(w, "output", m->wlr_output->name);
	json_key_string(w, "layout", m->ltsymbol[tag_idx]);
	json_key_string(w, "representation", "");
	json_key_string(w, "type", "workspace");

	json_key(w, "focus");
	json_array_start(w);
	c = focustop(m);
	if (c && (c->tags & tag_bit))
		json_integer(w, (int64_t)(uintptr_t)c);
	json_array_end(w);

	json_key(w, "floating_nodes");
	json_array_start(w);
	wl_list_for_each(c, &clients, link) {
		if (c->mon == m && (c->tags & tag_bit) && c->isfloating &&
		    !c->isfullscreen)
			json_integer(w, (int64_t)(uintptr_t)c);
	}
	json_array_end(w);

	json_key(w, "nodes");
	json_array_start(w);
	wl_list_for_each(c, &clients, link) {
		if (c->mon == m && (c->tags & tag_bit) && !c->isfloating &&
		    !c->isfullscreen)
			json_integer(w, (int64_t)(uintptr_t)c);
	}
	json_array_end(w);
	json_object_end(w);
}

static void
ipc_build_output(struct json_writer *w, Monitor *m)
{
	struct wlr_output *o = m->wlr_output;
	struct wlr_output_mode *mode = NULL;
	int ti;

	if (o->current_mode)
		mode = o->current_mode;
	else
		mode = wlr_output_preferred_mode(o);

	/* Determine current workspace name = first visible tag name */
	const char *ws_name = "";
	for (ti = 0; ti < TAGCOUNT; ti++) {
		if (m->tagset[m->seltags] & (1u << ti)) {
			ws_name = ipc_tag_name(ti);
			break;
		}
	}

	json_object_start(w);
	json_key_string(w, "name", o->name ? o->name : "");
	json_key_string(w, "make", o->make ? o->make : "");
	json_key_string(w, "model", o->model ? o->model : "");
	json_key_string(w, "serial", o->serial ? o->serial : "");
	json_key_bool(w, "active", o->enabled);
	json_key_bool(w, "primary",
		      m == wl_container_of(mons.next, m, link));
	json_key_float(w, "scale", o->scale);
	json_key_string(w, "subpixel_hinting", "rgb");
	json_key_string(w, "transform",
			o->transform == WL_OUTPUT_TRANSFORM_NORMAL ?
			"normal" : "90");
	json_key_string(w, "current_workspace", ws_name);

	json_key(w, "modes");
	json_array_start(w);
	if (mode) {
		json_object_start(w);
		json_key_int(w, "width", mode->width);
		json_key_int(w, "height", mode->height);
		json_key_int(w, "refresh", mode->refresh);
		json_object_end(w);
	}
	json_array_end(w);

	if (mode) {
		json_key(w, "current_mode");
		json_object_start(w);
		json_key_int(w, "width", mode->width);
		json_key_int(w, "height", mode->height);
		json_key_int(w, "refresh", mode->refresh);
		json_object_end(w);
	}

	json_key(w, "rect");
	json_object_start(w);
	json_key_int(w, "x", m->m.x);
	json_key_int(w, "y", m->m.y);
	json_key_int(w, "width", m->m.width);
	json_key_int(w, "height", m->m.height);
	json_object_end(w);
	json_object_end(w);
}

static void
ipc_build_client(struct json_writer *w, Client *c)
{
	int floating = c->isfloating ||
		!curlayout(c->mon)->arrange;

	json_object_start(w);
	json_key_int(w, "id", (int64_t)(uintptr_t)c);
	json_key_string(w, "type", "con");
	json_key_string(w, "orientation", "none");
	json_key_int(w, "percent", -1);
	json_key_bool(w, "urgent", !!c->isurgent);
	json_key_bool(w, "focused", c == focustop(selmon));
	json_key_string(w, "layout", "none");
	json_key_string(w, "app_id", client_get_appid(c));
	json_key_string(w, "name", client_get_title(c));
	json_key_string(w, "title", client_get_title(c));
	json_key_bool(w, "fullscreen_mode", !!c->isfullscreen);
	json_key_bool(w, "floating", !!floating);

	json_key(w, "window_properties");
	json_object_start(w);
	json_key_string(w, "class", client_get_appid(c));
	json_key_string(w, "instance", client_get_appid(c));
	json_key_string(w, "title", client_get_title(c));
	json_key_bool(w, "transient", 0);
	json_object_end(w);

	json_key(w, "marks");
	json_array_start(w);
	json_array_end(w);

	json_key(w, "rect");
	json_object_start(w);
	json_key_int(w, "x", c->geom.x);
	json_key_int(w, "y", c->geom.y);
	json_key_int(w, "width", c->geom.width);
	json_key_int(w, "height", c->geom.height);
	json_object_end(w);

	json_key(w, "window_rect");
	json_object_start(w);
	json_key_int(w, "x", (int)c->bw);
	json_key_int(w, "y", (int)c->bw);
	json_key_int(w, "width",
		     c->geom.width - 2 * (int)c->bw);
	json_key_int(w, "height",
		     c->geom.height - 2 * (int)c->bw);
	json_object_end(w);

	json_key(w, "deco_rect");
	json_object_start(w);
	json_key_int(w, "x", 0);
	json_key_int(w, "y", 0);
	json_key_int(w, "width", 0);
	json_key_int(w, "height", 0);
	json_object_end(w);

	json_key(w, "geometry");
	json_object_start(w);
	json_key_int(w, "x", c->geom.x);
	json_key_int(w, "y", c->geom.y);
	json_key_int(w, "width", c->geom.width);
	json_key_int(w, "height", c->geom.height);
	json_object_end(w);

	json_key(w, "border");
	json_object_start(w);
	json_key_int(w, "top", (int)c->bw);
	json_key_int(w, "bottom", (int)c->bw);
	json_key_int(w, "right", (int)c->bw);
	json_key_int(w, "left", (int)c->bw);
	json_object_end(w);

	json_key_int(w, "current_border_width", (int64_t)c->bw);

	json_key_int(w, "workspace", current_tag_idx(c->mon) + 1);

	json_key(w, "nodes");
	json_array_start(w);
	json_array_end(w);

	json_key(w, "floating_nodes");
	json_array_start(w);
	json_array_end(w);
	json_object_end(w);
}

/* ------------------------------------------------------------------ */
/* Command handlers                                                    */
/* ------------------------------------------------------------------ */

static void
ipc_handle_get_workspaces(struct ipc_client *client)
{
	struct json_writer w;
	Monitor *m;
	int i;

	json_init(&w);
	json_array_start(&w);
	wl_list_for_each(m, &mons, link) {
		for (i = 0; i < TAGCOUNT; i++) {
			uint32_t tag_bit = 1u << i;
			if (m->tagset[m->seltags] & tag_bit)
				ipc_build_workspace(&w, m, tag_bit, i);
		}
	}
	json_array_end(&w);
	ipc_send_json(client->fd, IPC_GET_WORKSPACES, &w);
	json_finish(&w);
}

static void
ipc_handle_get_outputs(struct ipc_client *client)
{
	struct json_writer w;
	Monitor *m;

	json_init(&w);
	json_array_start(&w);
	wl_list_for_each(m, &mons, link) {
		ipc_build_output(&w, m);
	}
	json_array_end(&w);
	ipc_send_json(client->fd, IPC_GET_OUTPUTS, &w);
	json_finish(&w);
}

static void
ipc_handle_get_version(struct ipc_client *client)
{
	struct json_writer w;

	json_init(&w);
	json_object_start(&w);
	json_key_string(&w, "variant", "peachwm");
	json_key_string(&w, "human_readable",
			"peachwm " VERSION);
	json_key_int(&w, "major", 0);
	json_key_int(&w, "minor", 2);
	json_key_int(&w, "patch", 0);
	json_key_string(&w, "loaded_config_file_name", "");
	json_object_end(&w);
	ipc_send_json(client->fd, IPC_GET_VERSION, &w);
	json_finish(&w);
}

static void
ipc_handle_get_tree(struct ipc_client *client)
{
	struct json_writer w;
	Monitor *m;
	Client *c;
	int i;

	json_init(&w);

	/* root node */
	json_object_start(&w);
	json_key_string(&w, "type", "root");
	json_key_string(&w, "name", "root");

	json_key(&w, "nodes");
	json_array_start(&w);

	wl_list_for_each(m, &mons, link) {
		/* output node */
		json_object_start(&w);
		json_key_string(&w, "type", "output");
		json_key_string(&w, "name", m->wlr_output->name);
		json_key_bool(&w, "active", m->wlr_output->enabled);

		json_key(&w, "rect");
		json_object_start(&w);
		json_key_int(&w, "x", m->m.x);
		json_key_int(&w, "y", m->m.y);
		json_key_int(&w, "width", m->m.width);
		json_key_int(&w, "height", m->m.height);
		json_object_end(&w);

		json_key(&w, "nodes");
		json_array_start(&w);

		for (i = 0; i < TAGCOUNT; i++) {
			uint32_t tag_bit = 1u << i;
			if (!(m->tagset[m->seltags] & tag_bit))
				continue;

			/* workspace node */
			json_object_start(&w);
			json_key_string(&w, "type", "workspace");
			json_key_string(&w, "name",
					ipc_tag_name(i));
			json_key_string(&w, "layout", m->ltsymbol[i]);

			json_key(&w, "rect");
			json_object_start(&w);
			json_key_int(&w, "x", m->m.x);
			json_key_int(&w, "y", m->m.y);
			json_key_int(&w, "width", m->m.width);
			json_key_int(&w, "height", m->m.height);
			json_object_end(&w);

			/* tiled nodes */
			json_key(&w, "nodes");
			json_array_start(&w);
			wl_list_for_each(c, &clients, link) {
				if (c->mon == m &&
				    (c->tags & tag_bit) &&
				    !c->isfloating &&
				    !c->isfullscreen)
					ipc_build_client(&w, c);
			}
			json_array_end(&w);

			/* floating nodes */
			json_key(&w, "floating_nodes");
			json_array_start(&w);
			wl_list_for_each(c, &clients, link) {
				if (c->mon == m &&
				    (c->tags & tag_bit) &&
				    (c->isfloating ||
				     c->isfullscreen))
					ipc_build_client(&w, c);
			}
			json_array_end(&w);

			json_object_end(&w); /* workspace */
		}
		json_array_end(&w); /* output nodes */

		json_key(&w, "floating_nodes");
		json_array_start(&w);
		json_array_end(&w);

		json_object_end(&w); /* output */
	}
	json_array_end(&w); /* root nodes */

	json_key(&w, "floating_nodes");
	json_array_start(&w);
	json_array_end(&w);

	json_object_end(&w); /* root */

	ipc_send_json(client->fd, IPC_GET_TREE, &w);
	json_finish(&w);
}

static void
ipc_handle_command(struct ipc_client *client, const char *payload,
		   size_t len)
{
	struct json_writer w;
	int success = 0;
	char *cmd = NULL;

	if (len > 0) {
		cmd = ecalloc(1, len + 1);
		memcpy(cmd, payload, len);
		cmd[len] = '\0';
	}

	if (cmd) {
		if (!strncmp(cmd, "focus workspace", 15)) {
			const char *ws = cmd + 15;
			while (*ws == ' ') ws++;
			if (*ws) {
				int num = atoi(ws);
				if (num >= 1 && num <= TAGCOUNT) {
					view(&(Arg){.ui =
						1u << (num - 1)});
					success = 1;
				}
			}
		} else if (!strcmp(cmd, "focus left")) {
			focusdir(&(Arg){.i = WLR_DIRECTION_LEFT});
			success = 1;
		} else if (!strcmp(cmd, "focus right")) {
			focusdir(&(Arg){.i = WLR_DIRECTION_RIGHT});
			success = 1;
		} else if (!strcmp(cmd, "focus up")) {
			focusdir(&(Arg){.i = WLR_DIRECTION_UP});
			success = 1;
		} else if (!strcmp(cmd, "focus down")) {
			focusdir(&(Arg){.i = WLR_DIRECTION_DOWN});
			success = 1;
		} else if (!strncmp(cmd, "move workspace", 14)) {
			const char *ws = cmd + 14;
			while (*ws == ' ') ws++;
			if (*ws) {
				int num = atoi(ws);
				if (num >= 1 && num <= TAGCOUNT) {
					tag(&(Arg){.ui =
						1u << (num - 1)});
					success = 1;
				}
			}
		} else if (!strcmp(cmd, "floating toggle") ||
			   !strcmp(cmd, "floating enable") ||
			   !strcmp(cmd, "floating disable")) {
			togglefloating(NULL);
			success = 1;
		} else if (!strcmp(cmd, "fullscreen toggle") ||
			   !strcmp(cmd, "fullscreen enable") ||
			   !strcmp(cmd, "fullscreen disable")) {
			togglefullscreen(NULL);
			success = 1;
		} else if (!strcmp(cmd, "kill") ||
			   !strcmp(cmd, "close")) {
			killclient(NULL);
			success = 1;
		} else if (!strncmp(cmd, "workspace", 9)) {
			const char *ws = cmd + 9;
			while (*ws == ' ') ws++;
			if (*ws) {
				int num = atoi(ws);
				if (num >= 1 && num <= TAGCOUNT) {
					view(&(Arg){.ui =
						1u << (num - 1)});
					success = 1;
				}
			}
		} else if (!strcmp(cmd, "exit") ||
			   !strcmp(cmd, "quit")) {
			quit(NULL);
			success = 1;
		} else if (!strcmp(cmd, "reload")) {
			do_reload();
			success = 1;
		}
	}

	free(cmd);

	json_init(&w);
	json_array_start(&w);
	json_object_start(&w);
	json_key_bool(&w, "success", success);
	if (!success)
		json_key_string(&w, "error",
				"Unknown or unsupported command");
	json_object_end(&w);
	json_array_end(&w);
	ipc_send_json(client->fd, IPC_COMMAND, &w);
	json_finish(&w);
}

static void
ipc_handle_subscribe(struct ipc_client *client, const char *payload,
		     size_t len)
{
	struct json_writer w;
	(void)len;

	client->subscribed = 0;

	if (strstr(payload, "\"workspace\""))
		client->subscribed |= IPC_SUB_WORKSPACE;
	if (strstr(payload, "\"window\""))
		client->subscribed |= IPC_SUB_WINDOW;
	if (strstr(payload, "\"output\""))
		client->subscribed |= IPC_SUB_OUTPUT;
	if (strstr(payload, "\"mode\""))
		client->subscribed |= IPC_SUB_MODE;

	json_init(&w);
	json_array_start(&w);
	json_object_start(&w);
	json_key_bool(&w, "success", 1);
	json_object_end(&w);
	json_array_end(&w);
	ipc_send_json(client->fd, IPC_SUBSCRIBE, &w);
	json_finish(&w);
}

/* ------------------------------------------------------------------ */
/* Client I/O                                                          */
/* ------------------------------------------------------------------ */

static void
ipc_client_close(struct ipc_client *c)
{
	wl_list_remove(&c->link);
	if (c->src) {
		wl_event_source_remove(c->src);
		c->src = NULL;
	}
	close(c->fd);
	free(c->payload);
	free(c);
}

static int
ipc_client_handle_readable(int fd, uint32_t mask, void *data)
{
	struct ipc_client *c = data;
	ssize_t n;
	size_t want;
	(void)fd;

	if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
		ipc_client_close(c);
		return 0;
	}

	if (!(mask & WL_EVENT_READABLE))
		return 0;

	if (!c->header_done) {
		want = IPC_HEADER_SIZE - c->hdr_len;
		n = read(c->fd, c->hdr_buf + c->hdr_len, want);
		if (n <= 0) {
			if (n == 0 ||
			    (errno != EAGAIN && errno != EWOULDBLOCK)) {
				ipc_client_close(c);
				return 0;
			}
			return 0;
		}
		c->hdr_len += (size_t)n;
		if (c->hdr_len < IPC_HEADER_SIZE)
			return 0;

		/* Validate magic */
		if (memcmp(c->hdr_buf, IPC_MAGIC, IPC_MAGIC_LEN) != 0) {
			ipc_client_close(c);
			return 0;
		}

		/* Parse header */
		memcpy(&c->payload_size,
		       c->hdr_buf + IPC_MAGIC_LEN, sizeof(uint32_t));
		memcpy(&c->msg_type,
		       c->hdr_buf + IPC_MAGIC_LEN + 4, sizeof(uint32_t));

		if (c->payload_size > 0) {
			if (c->payload_size > c->payload_alloc) {
				free(c->payload);
				c->payload_alloc = c->payload_size + 1;
				c->payload = ecalloc(1, c->payload_alloc);
			}
			c->payload_len = 0;
		}
		c->header_done = 1;
	}

	/* Read payload */
	if (c->header_done && c->payload_size > 0) {
		want = c->payload_size - c->payload_len;
		if (want > 0) {
			n = read(c->fd,
				 c->payload + c->payload_len, want);
			if (n <= 0) {
				if (n == 0 ||
				    (errno != EAGAIN &&
				     errno != EWOULDBLOCK)) {
					ipc_client_close(c);
					return 0;
				}
				return 0;
			}
			c->payload_len += (size_t)n;
		}
		if (c->payload_len < c->payload_size)
			return 0;
	}

	/* Full message received – dispatch */
	switch (c->msg_type) {
	case IPC_COMMAND:
		ipc_handle_command(c, c->payload, c->payload_size);
		break;
	case IPC_GET_WORKSPACES:
		ipc_handle_get_workspaces(c);
		break;
	case IPC_GET_OUTPUTS:
		ipc_handle_get_outputs(c);
		break;
	case IPC_GET_VERSION:
		ipc_handle_get_version(c);
		break;
	case IPC_GET_TREE:
		ipc_handle_get_tree(c);
		break;
	case IPC_SUBSCRIBE:
		ipc_handle_subscribe(c, c->payload, c->payload_size);
		break;
	default:
		ipc_send_raw(c->fd, c->msg_type, "{}", 2);
		break;
	}

	/* Reset for next message */
	c->hdr_len = 0;
	c->header_done = 0;
	c->payload_len = 0;
	c->payload_size = 0;

	return 0;
}

/* ------------------------------------------------------------------ */
/* Accept new connections                                              */
/* ------------------------------------------------------------------ */

static int
ipc_handle_accept(int fd, uint32_t mask, void *data)
{
	struct sockaddr_un un;
	socklen_t len = sizeof(un);
	int cfd;
	struct ipc_client *c;
	struct wl_event_source *src;
	int flags;

	(void)data;

	if (!(mask & WL_EVENT_READABLE))
		return 0;

	cfd = accept(fd, (struct sockaddr *)&un, &len);
	if (cfd < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			wlr_log(WLR_ERROR, "ipc: accept: %s",
				strerror(errno));
		return 0;
	}

	/* Set non-blocking and close-on-exec */
	flags = fcntl(cfd, F_GETFL);
	if (flags < 0 || fcntl(cfd, F_SETFL, flags | O_NONBLOCK) < 0) {
		close(cfd);
		return 0;
	}

	c = ecalloc(1, sizeof(*c));
	c->fd = cfd;
	c->hdr_len = 0;
	c->header_done = 0;
	c->payload_size = 0;
	c->payload_len = 0;
	c->payload_alloc = 0;
	c->payload = NULL;
	c->subscribed = 0;

	src = wl_event_loop_add_fd(ipc_event_loop, cfd,
				   WL_EVENT_READABLE,
				   ipc_client_handle_readable, c);
	if (!src) {
		close(cfd);
		free(c);
		return 0;
	}
	c->src = src;

	wl_list_insert(&ipc_clients, &c->link);

	wlr_log(WLR_DEBUG, "ipc: client connected (fd %d)", cfd);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

static void
ipc_socket_init(void)
{
	const char *runtime_dir;
	struct sockaddr_un un;
	int fd, flags;
	char sock_path[256];
	struct passwd *pw;

	wl_list_init(&ipc_clients);
	ipc_event_loop = event_loop;

	runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (runtime_dir && runtime_dir[0]) {
		snprintf(sock_path, sizeof(sock_path),
			 "%s/peachwm.sock", runtime_dir);
	} else {
		pw = getpwuid(getuid());
		if (pw)
			snprintf(sock_path, sizeof(sock_path),
				 "/tmp/peachwm-%s.sock", pw->pw_name);
		else
			snprintf(sock_path, sizeof(sock_path),
				 "/tmp/peachwm-%d.sock", getuid());
	}

	unlink(sock_path);

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		wlr_log(WLR_ERROR, "ipc: socket: %s", strerror(errno));
		return;
	}

	memset(&un, 0, sizeof(un));
	un.sun_family = AF_UNIX;
	strncpy(un.sun_path, sock_path, sizeof(un.sun_path) - 1);

	if (bind(fd, (struct sockaddr *)&un, sizeof(un)) < 0) {
		wlr_log(WLR_ERROR, "ipc: bind(%s): %s",
			sock_path, strerror(errno));
		close(fd);
		return;
	}

	chmod(sock_path, 0700);

	if (listen(fd, 8) < 0) {
		wlr_log(WLR_ERROR, "ipc: listen: %s", strerror(errno));
		close(fd);
		unlink(sock_path);
		return;
	}

	flags = fcntl(fd, F_GETFL);
	if (flags < 0 ||
	    fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		wlr_log(WLR_ERROR, "ipc: fcntl(O_NONBLOCK): %s",
			strerror(errno));
		close(fd);
		unlink(sock_path);
		return;
	}

	ipc_listen_fd = fd;
	strncpy(ipc_socket_path, sock_path,
		sizeof(ipc_socket_path) - 1);
	ipc_socket_path[sizeof(ipc_socket_path) - 1] = '\0';

	ipc_listen_src = wl_event_loop_add_fd(
		ipc_event_loop, fd, WL_EVENT_READABLE,
		ipc_handle_accept, NULL);
	if (!ipc_listen_src) {
		wlr_log(WLR_ERROR,
			"ipc: wl_event_loop_add_fd failed");
		close(fd);
		unlink(sock_path);
		ipc_listen_fd = -1;
		ipc_socket_path[0] = '\0';
		return;
	}

	wlr_log(WLR_DEBUG, "ipc: listening on %s", sock_path);
}

static void
ipc_socket_finish(void)
{
	struct ipc_client *c, *tmp;

	wl_list_for_each_safe(c, tmp, &ipc_clients, link)
		ipc_client_close(c);

	if (ipc_listen_src) {
		wl_event_source_remove(ipc_listen_src);
		ipc_listen_src = NULL;
	}
	if (ipc_listen_fd >= 0) {
		close(ipc_listen_fd);
		if (ipc_socket_path[0])
			unlink(ipc_socket_path);
		ipc_listen_fd = -1;
		ipc_socket_path[0] = '\0';
	}
}

/* ------------------------------------------------------------------ */
/* Event helpers called from peachwm.c                                 */
/* ------------------------------------------------------------------ */

static void
ipc_socket_send_workspace_event(const char *change)
{
	struct json_writer w;

	if (wl_list_empty(&ipc_clients))
		return;

	json_init(&w);
	json_object_start(&w);
	json_key_string(&w, "change", change ? change : "focus");

	if (selmon) {
		int ti = current_tag_idx(selmon);
		uint32_t tag_bit = 1u << ti;
		if (selmon->tagset[selmon->seltags] & tag_bit) {
			json_key(&w, "current");
			ipc_build_workspace(&w, selmon, tag_bit, ti);
		}
	}

	json_object_end(&w);
	ipc_broadcast((uint32_t)IPC_EVENT_WORKSPACE, &w);
	json_finish(&w);
}

static void
ipc_socket_send_window_event(const char *change)
{
	struct json_writer w;

	if (wl_list_empty(&ipc_clients))
		return;

	json_init(&w);
	json_object_start(&w);
	json_key_string(&w, "change", change ? change : "focus");

	if (selmon) {
		Client *c = focustop(selmon);
		if (c) {
			json_key(&w, "container");
			ipc_build_client(&w, c);
		}
	}

	json_object_end(&w);
	ipc_broadcast((uint32_t)IPC_EVENT_WINDOW, &w);
	json_finish(&w);
}

static void
ipc_socket_send_output_event(void)
{
	struct json_writer w;

	if (wl_list_empty(&ipc_clients))
		return;

	json_init(&w);
	json_object_start(&w);
	json_key_string(&w, "change", "unspecified");
	json_object_end(&w);
	ipc_broadcast((uint32_t)IPC_EVENT_OUTPUT, &w);
	json_finish(&w);
}
