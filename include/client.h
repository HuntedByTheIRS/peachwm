#pragma once

#include <wayland-server-core.h>
#include <wlr/util/box.h>
#include "monitor.h"

/* Forward declarations for pointer-only types */
struct wlr_surface;
struct wlr_keyboard;
struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_scene_layer_surface_v1;
struct wlr_layer_surface_v1;
struct wlr_xdg_surface;
struct wlr_xwayland_surface;
struct wlr_xdg_toplevel_decoration_v1;
struct wlr_scene_shadow;

/* Client types */
enum { XDGShell, LayerShell, X11 };

typedef struct Client {
	/* Must keep this field first */
	unsigned int type; /* XDGShell or X11* */

	Monitor *mon;
	struct wlr_scene_tree *scene;
	struct wlr_scene_rect *border_bg; /* single border rect with clipped content hole */
	struct wlr_scene_tree *scene_surface;
	struct wl_list link;
	struct wl_list flink;
	struct wlr_box geom;   /* layout-relative, includes border */
	struct wlr_box prev;   /* layout-relative, includes border */
	struct wlr_box bounds; /* only width and height are used */
	union {
		struct wlr_xdg_surface *xdg;
		struct wlr_xwayland_surface *xwayland;
	} surface;
	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	struct wl_listener commit;
	struct wl_listener map;
	struct wl_listener maximize;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener set_title;
	struct wl_listener fullscreen;
	struct wl_listener set_decoration_mode;
	struct wl_listener destroy_decoration;
#ifdef XWAYLAND
	struct wl_listener activate;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener configure;
	struct wl_listener set_hints;
#endif
	unsigned int bw;
	int corner_radius;
	struct wlr_scene_shadow *shadow;
	uint32_t tags;
	int isfloating, isurgent, isfullscreen, isscratchpad;
	uint32_t resize; /* configure serial of a pending resize */
} Client;

typedef struct {
	/* Must keep this field first */
	unsigned int type; /* LayerShell */

	Monitor *mon;
	struct wlr_scene_tree *scene;
	struct wlr_scene_tree *popups;
	struct wlr_scene_layer_surface_v1 *scene_layer;
	struct wl_list link;
	int mapped;
	struct wlr_layer_surface_v1 *layer_surface;

	struct wl_listener destroy;
	struct wl_listener unmap;
	struct wl_listener surface_commit;
} LayerSurface;

/* Function declarations */
[[nodiscard]] int client_is_x11(Client *c);
struct wlr_surface *client_surface(Client *c);
[[nodiscard]] int toplevel_from_wlr_surface(struct wlr_surface *s, Client **pc, LayerSurface **pl);
void client_activate_surface(struct wlr_surface *s, int activated);
uint32_t client_set_bounds(Client *c, int32_t width, int32_t height);
[[nodiscard]] const char *client_get_appid(Client *c);
void client_get_clip(Client *c, struct wlr_box *clip);
void client_get_geometry(Client *c, struct wlr_box *geom);
[[nodiscard]] Client *client_get_parent(Client *c);
[[nodiscard]] int client_has_children(Client *c);
[[nodiscard]] const char *client_get_title(Client *c);
[[nodiscard]] int client_is_float_type(Client *c);
[[nodiscard]] int client_is_rendered_on_mon(Client *c, Monitor *m);
[[nodiscard]] int client_is_stopped(Client *c);
[[nodiscard]] int client_is_unmanaged(Client *c);
void client_notify_enter(struct wlr_surface *s, struct wlr_keyboard *kb);
void client_send_close(Client *c);
void client_set_border_color(Client *c, const float color[static 4]);
void client_set_fullscreen(Client *c, int fullscreen);
void client_set_scale(struct wlr_surface *s, float scale);
uint32_t client_set_size(Client *c, uint32_t width, uint32_t height);
void client_set_tiled(Client *c, uint32_t edges);
void client_set_suspended(Client *c, int suspended);
[[nodiscard]] int client_wants_focus(Client *c);
[[nodiscard]] int client_wants_fullscreen(Client *c);

