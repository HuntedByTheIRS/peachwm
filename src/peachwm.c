/*
 * See LICENSE file for copyright and license details.
 * PeachWM is a fork of swindle, which is also GPLv3 licensed.
 */
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <libinput.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <scenefx/render/fx_renderer/fx_renderer.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_drm.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <scenefx/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <xkbcommon/xkbcommon.h>
#ifdef XWAYLAND
#include <wlr/xwayland.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#endif

#include "parser/parser.h"
#include "util.h"
#include "client.h"
#include "monitor.h"
#include "layout.h"
#include "scratchpad.h"
#include "ext_workspace.h"
#include "ipc.h"
#include "ipc_socket.h"
#include "common.h"

/* macros */
#define CLEANMASK(mask) (mask & ~WLR_MODIFIER_CAPS)
#define END(A) ((A) + LENGTH(A))
#define LISTEN(E, L, H) wl_signal_add((E), ((L)->notify = (H), (L)))
#define LISTEN_STATIC(E, H)                                                    \
  do {                                                                         \
    struct wl_listener *_l = ecalloc(1, sizeof(*_l));                          \
    _l->notify = (H);                                                          \
    wl_signal_add((E), _l);                                                    \
  } while (0)

/* enums */
enum { CurNormal, CurPressed, CurMove, CurResize, CurTileDrag }; /* cursor */

typedef struct {
  unsigned int mod;
  unsigned int button;
  void (*func)(const Arg *);
  const Arg arg;
} Button;

typedef struct {
  uint32_t mod;
  xkb_keysym_t keysym;
  void (*func)(const Arg *);
  const Arg arg;
} Key;

typedef struct {
  struct wlr_keyboard_group *wlr_group;

  int nsyms;
  const xkb_keysym_t *keysyms; /* invalid if nsyms == 0 */
  uint32_t mods;               /* invalid if nsyms == 0 */
  struct wl_event_source *key_repeat_source;

  struct wl_listener modifiers;
  struct wl_listener key;
  struct wl_listener destroy;
} KeyboardGroup;

typedef struct {
  struct wlr_input_device *device;
  struct wl_list link;
  struct wl_listener destroy;
} PointerDevice;

#ifndef TAGCOUNT
#define TAGCOUNT 9
#endif

typedef struct {
  const char *name;
  float mfact;
  int nmaster;
  float scale;
  const Layout *lt;
  enum wl_output_transform rr;
  int x, y;
} MonitorRule;

typedef struct {
  struct wlr_pointer_constraint_v1 *constraint;
  struct wl_listener destroy;
} PointerConstraint;

typedef struct {
  const char *id;
  const char *title;
  uint32_t tags;
  int isfloating;
  int monitor;
} Rule;

typedef struct {
  struct wlr_scene_tree *scene;

  struct wlr_session_lock_v1 *lock;
  struct wl_listener new_surface;
  struct wl_listener unlock;
  struct wl_listener destroy;
} SessionLock;

/* function declarations */
static void applybounds(Client *c, struct wlr_box *bbox);
static void applyrules(Client *c);
static void arrangelayer(Monitor *m, struct wl_list *list,
                         struct wlr_box *usable_area, int exclusive);
static void arrangelayers(Monitor *m);
static void axisnotify(struct wl_listener *listener, void *data);
static void buttonpress(struct wl_listener *listener, void *data);
static void chvt(const Arg *arg);
static void checkidleinhibitor(struct wlr_surface *exclude);
static void cleanup(void);
static void cleanupmon(struct wl_listener *listener, void *data);
static void cleanuplisteners(void);
static void closemon(Monitor *m);
static void commitlayersurfacenotify(struct wl_listener *listener, void *data);
static void commitnotify(struct wl_listener *listener, void *data);
static void commitpopup(struct wl_listener *listener, void *data);
static void createdecoration(struct wl_listener *listener, void *data);
static void createidleinhibitor(struct wl_listener *listener, void *data);
static void createkeyboard(struct wlr_keyboard *keyboard);
static KeyboardGroup *createkeyboardgroup(void);
static void createlayersurface(struct wl_listener *listener, void *data);
static void createlocksurface(struct wl_listener *listener, void *data);
static void createmon(struct wl_listener *listener, void *data);
static void createnotify(struct wl_listener *listener, void *data);
static void createpointer(struct wlr_pointer *pointer);
static void createpointerconstraint(struct wl_listener *listener, void *data);
static void createpopup(struct wl_listener *listener, void *data);
static void cursorconstrain(struct wlr_pointer_constraint_v1 *constraint);
static void cursorframe(struct wl_listener *listener, void *data);
static void cursorwarptohint(void);
static void destroydecoration(struct wl_listener *listener, void *data);
static void destroydragicon(struct wl_listener *listener, void *data);
static void destroyidleinhibitor(struct wl_listener *listener, void *data);
static void destroylayersurfacenotify(struct wl_listener *listener, void *data);
static void destroylock(SessionLock *lock, int unlocked);
static void destroylocksurface(struct wl_listener *listener, void *data);
static void destroynotify(struct wl_listener *listener, void *data);
static void destroypointerconstraint(struct wl_listener *listener, void *data);
static void destroysessionlock(struct wl_listener *listener, void *data);
static void destroykeyboardgroup(struct wl_listener *listener, void *data);
static Monitor *dirtomon(enum wlr_direction dir);
static void focusmon(const Arg *arg);
void focusdir(const Arg *arg);
static void swapdir(const Arg *arg);
static void fullscreennotify(struct wl_listener *listener, void *data);
static void gpureset(struct wl_listener *listener, void *data);
static void handlesig(int signo);
static void inputdevice(struct wl_listener *listener, void *data);
static int keybinding(uint32_t mods, xkb_keysym_t sym);
static void keypress(struct wl_listener *listener, void *data);
static void keypressmod(struct wl_listener *listener, void *data);
static int keyrepeat(void *data);
void killclient(const Arg *arg);
static void locksession(struct wl_listener *listener, void *data);
static void mapnotify(struct wl_listener *listener, void *data);
static void maximizenotify(struct wl_listener *listener, void *data);
static void motionabsolute(struct wl_listener *listener, void *data);
static void motionnotify(uint32_t time, struct wlr_input_device *device,
                         double sx, double sy, double sx_unaccel,
                         double sy_unaccel);
static void motionrelative(struct wl_listener *listener, void *data);
static void moveresize(const Arg *arg);
static void outputmgrapply(struct wl_listener *listener, void *data);
static void outputmgrapplyortest(struct wlr_output_configuration_v1 *config,
                                 int test);
static void outputmgrtest(struct wl_listener *listener, void *data);
static void pointerfocus(Client *c, struct wlr_surface *surface, double sx,
                         double sy, uint32_t time);
static void powermgrsetmode(struct wl_listener *listener, void *data);
void quit(const Arg *arg);
static void rendermon(struct wl_listener *listener, void *data);
static void requestdecorationmode(struct wl_listener *listener, void *data);
static void requeststartdrag(struct wl_listener *listener, void *data);
static void requestmonstate(struct wl_listener *listener, void *data);
void resize(Client *c, struct wlr_box geo, int interact);
static void run(char *startup_cmd);
static void setcursor(struct wl_listener *listener, void *data);
static void setcursorshape(struct wl_listener *listener, void *data);
void setfloating(Client *c, int floating);
static void setfullscreen(Client *c, int fullscreen);
static void setlayout(const Arg *arg);
static void setmon(Client *c, Monitor *m, uint32_t newtags);
static void setpsel(struct wl_listener *listener, void *data);
static void setsel(struct wl_listener *listener, void *data);
static void setup(void);
static void spawn(const Arg *arg);
static void startdrag(struct wl_listener *listener, void *data);
void tag(const Arg *arg);
static void tagmon(const Arg *arg);
void togglefloating(const Arg *arg);
void togglefullscreen(const Arg *arg);
static void togglegaps(const Arg *arg);
static void toggletag(const Arg *arg);
void toggleview(const Arg *arg);
static void unlocksession(struct wl_listener *listener, void *data);
static void unmaplayersurfacenotify(struct wl_listener *listener, void *data);
static void unmapnotify(struct wl_listener *listener, void *data);
static void updatemons(struct wl_listener *listener, void *data);
static void updatetitle(struct wl_listener *listener, void *data);
static void urgent(struct wl_listener *listener, void *data);
void view(const Arg *arg);
static void virtualkeyboard(struct wl_listener *listener, void *data);
static void virtualpointer(struct wl_listener *listener, void *data);
static Monitor *xytomon(double x, double y);
static void xytonode(double x, double y, struct wlr_surface **psurface,
                     Client **pc, LayerSurface **pl, double *nx, double *ny);
void do_reload(void);

/* variables */
static pid_t child_pid = -1;
static int locked;
static void *exclusive_focus;
struct wl_display *dpy;
static struct wl_event_loop *event_loop;
static struct wlr_backend *backend;
static struct wlr_scene *scene;
struct wlr_scene_tree *layers[NUM_LAYERS];
static struct wlr_scene_tree *drag_icon;
/* Map from ZWLR_LAYER_SHELL_* constants to Lyr* enum */
static const int layermap[] = {LyrBg, LyrBottom, LyrTop, LyrOverlay};
static struct wlr_renderer *drw;
static struct wlr_allocator *alloc;
static struct wlr_compositor *compositor;
static struct wlr_session *session;

static struct wlr_xdg_shell *xdg_shell;
static struct wlr_xdg_activation_v1 *activation;
static struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
struct wl_list clients; /* tiling order */
struct wl_list fstack;  /* focus order */
static struct wlr_idle_notifier_v1 *idle_notifier;
static struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
static struct wlr_layer_shell_v1 *layer_shell;
static struct wlr_output_manager_v1 *output_mgr;
static struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;
static struct wlr_virtual_pointer_manager_v1 *virtual_pointer_mgr;
static struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
static struct wlr_output_power_manager_v1 *power_mgr;

static struct wlr_pointer_constraints_v1 *pointer_constraints;
static struct wlr_relative_pointer_manager_v1 *relative_pointer_mgr;
static struct wlr_pointer_constraint_v1 *active_constraint;

static struct wlr_cursor *cursor;
static struct wlr_xcursor_manager *cursor_mgr;

static struct wlr_scene_rect *root_bg;
static struct wlr_session_lock_manager_v1 *session_lock_mgr;
static struct wlr_scene_rect *locked_bg;
static struct wlr_session_lock_v1 *cur_lock;

struct wlr_seat *seat;
static KeyboardGroup *kb_group;
static struct wl_list pointer_devices;
static unsigned int cursor_mode;
static Client *grabc;
static int grabcx, grabcy; /* client-relative */

static struct wlr_output_layout *output_layout;
static struct wlr_box sgeom;
struct wl_list mons;
Monitor *selmon;

/* global event handlers */
static struct wl_listener cursor_axis = {.notify = axisnotify};
static struct wl_listener cursor_button = {.notify = buttonpress};
static struct wl_listener cursor_frame = {.notify = cursorframe};
static struct wl_listener cursor_motion = {.notify = motionrelative};
static struct wl_listener cursor_motion_absolute = {.notify = motionabsolute};
static struct wl_listener gpu_reset = {.notify = gpureset};
static struct wl_listener layout_change = {.notify = updatemons};
static struct wl_listener new_idle_inhibitor = {.notify = createidleinhibitor};
static struct wl_listener new_input_device = {.notify = inputdevice};
static struct wl_listener new_virtual_keyboard = {.notify = virtualkeyboard};
static struct wl_listener new_virtual_pointer = {.notify = virtualpointer};
static struct wl_listener new_pointer_constraint = {
    .notify = createpointerconstraint};
static struct wl_listener new_output = {.notify = createmon};
static struct wl_listener new_xdg_toplevel = {.notify = createnotify};
static struct wl_listener new_xdg_popup = {.notify = createpopup};
static struct wl_listener new_xdg_decoration = {.notify = createdecoration};
static struct wl_listener new_layer_surface = {.notify = createlayersurface};
static struct wl_listener output_mgr_apply = {.notify = outputmgrapply};
static struct wl_listener output_mgr_test = {.notify = outputmgrtest};
static struct wl_listener output_power_mgr_set_mode = {.notify =
                                                           powermgrsetmode};
static struct wl_listener request_activate = {.notify = urgent};
static struct wl_listener request_cursor = {.notify = setcursor};
static struct wl_listener request_set_psel = {.notify = setpsel};
static struct wl_listener request_set_sel = {.notify = setsel};
static struct wl_listener request_set_cursor_shape = {.notify = setcursorshape};
static struct wl_listener request_start_drag = {.notify = requeststartdrag};
static struct wl_listener start_drag = {.notify = startdrag};
static struct wl_listener new_session_lock = {.notify = locksession};

#ifdef XWAYLAND
static void activatex11(struct wl_listener *listener, void *data);
static void associatex11(struct wl_listener *listener, void *data);
static void configurex11(struct wl_listener *listener, void *data);
static void createnotifyx11(struct wl_listener *listener, void *data);
static void dissociatex11(struct wl_listener *listener, void *data);
static void sethints(struct wl_listener *listener, void *data);
static void xwaylandready(struct wl_listener *listener, void *data);
static struct wl_listener new_xwayland_surface = {.notify = createnotifyx11};
static struct wl_listener xwayland_ready = {.notify = xwaylandready};
static struct wlr_xwayland *xwayland;
#endif

/* Runtime config */

Config cfg;
static WatchState *cfg_watch_src;
static const char *custom_cfg_path;
static char config_path[1024];

/* function implementations */

static int
config_get_corner_radius(void)
{
	return cfg.effects.windows.corner_radius > 0 ? cfg.effects.windows.corner_radius : 0;
}

/* Apply a workspace default layout for the given tag index (0-based) on monitor
 * m */
static void apply_workspace_layout(Monitor *m, int tag_idx) {
  if (tag_idx < 0 || tag_idx >= TAGCOUNT)
    return;
  const char *layout_name = cfg.workspace_layouts[tag_idx];
  if (!layout_name[0])
    return;

  /* ensure_cold will be called by whichever write path triggers first */
  for (int li = 0; li < (int)layout_count; li++) {
    if (layouts[li].symbol && !strcmp(layout_name, layouts[li].symbol)) {
      m->cold->lt[tag_idx][m->cold->sellt[tag_idx]] = &layouts[li];
      break;
    }
    if (!strcmp(layout_name, "dwindle") && layouts[li].arrange == dwindle) {
      m->cold->lt[tag_idx][m->cold->sellt[tag_idx]] = &layouts[li];
      break;
    }
    if (!strcmp(layout_name, "master") && layouts[li].arrange == master) {
      m->cold->lt[tag_idx][m->cold->sellt[tag_idx]] = &layouts[li];
      break;
    }
    if (!strcmp(layout_name, "monocle") && layouts[li].arrange == monocle) {
      m->cold->lt[tag_idx][m->cold->sellt[tag_idx]] = &layouts[li];
      break;
    }
  }
}

static void applybounds(Client *c, struct wlr_box *bbox) {
  /* set minimum possible */
  c->geom.width = MAX(1 + 2 * (int)c->bw, c->geom.width);
  c->geom.height = MAX(1 + 2 * (int)c->bw, c->geom.height);

  if (c->geom.x >= bbox->x + bbox->width)
    c->geom.x = bbox->x + bbox->width - c->geom.width;
  if (c->geom.y >= bbox->y + bbox->height)
    c->geom.y = bbox->y + bbox->height - c->geom.height;
  if (c->geom.x + c->geom.width <= bbox->x)
    c->geom.x = bbox->x;
  if (c->geom.y + c->geom.height <= bbox->y)
    c->geom.y = bbox->y;
}

static void applyrules(Client *c) {
  /* rule matching */
  const char *appid, *title;
  uint32_t newtags = 0;
  int i, ri;
  Monitor *mon = selmon, *m;

  appid = client_get_appid(c);
  title = client_get_title(c);

  for (ri = 0; ri < cfg.nrules; ri++) {
    const CfgRule *r = &cfg.rules[ri];
    if ((!r->title[0] || strstr(title, r->title)) &&
        (!r->app_id[0] || strstr(appid, r->app_id))) {
      c->isfloating = r->floating;
      newtags |= r->tags;
      i = 0;
      wl_list_for_each(m, &mons, link) {
        if (r->monitor == i++)
          mon = m;
      }
    }
  }

  c->isfloating |= client_is_float_type(c);
  setmon(c, mon, newtags);
}

void arrange(Monitor *m) {
  Client *c;

  if (!m->wlr_output->enabled)
    return;

  wl_list_for_each(c, &clients, link) {
    if (c->mon == m) {
      wlr_scene_node_set_enabled(&c->scene->node, VISIBLEON(c, m));
      client_set_suspended(c, !VISIBLEON(c, m));
    }
  }

  wlr_scene_node_set_enabled(&m->fullscreen_bg->node,
                             (c = focustop(m)) && c->isfullscreen);

  int ti = current_tag_idx(m);
  strncpy(m->cold->ltsymbol[ti], curlayout(m)->symbol, LENGTH(m->cold->ltsymbol[ti]));
  m->cold->ltsymbol[ti][LENGTH(m->cold->ltsymbol[ti]) - 1] = '\0';

  /* We move all clients (except fullscreen and unmanaged) to LyrTile while
   * in floating layout to avoid "real" floating clients be always on top */
  wl_list_for_each(c, &clients, link) {
    if (c->mon != m || c->scene->node.parent == layers[LyrFS])
      continue;

    if (c->isscratchpad) {
      wlr_scene_node_reparent(&c->scene->node, layers[LyrScratch]);
    } else {
      wlr_scene_node_reparent(
          &c->scene->node,
          (!curlayout(m)->arrange && c->isfloating)  ? layers[LyrTile]
          : (curlayout(m)->arrange && c->isfloating) ? layers[LyrFloat]
                                                        : c->scene->node.parent);
    }
  }

  if (curlayout(m)->arrange)
    curlayout(m)->arrange(m);
  motionnotify(0, nullptr, 0, 0, 0, 0);
  checkidleinhibitor(nullptr);
}

static void arrangelayer(Monitor *m, struct wl_list *list, struct wlr_box *usable_area,
                  int exclusive) {
  LayerSurface *l;
  struct wlr_box full_area = m->m;

  wl_list_for_each(l, list, link) {
    struct wlr_layer_surface_v1 *layer_surface = l->layer_surface;

    if (!layer_surface->initialized)
      continue;

    if (exclusive != (layer_surface->current.exclusive_zone > 0))
      continue;

    wlr_scene_layer_surface_v1_configure(l->scene_layer, &full_area,
                                         usable_area);
    wlr_scene_node_set_position(&l->popups->node, l->scene->node.x,
                                l->scene->node.y);
  }
}

static void arrangelayers(Monitor *m) {
  int i;
  struct wlr_box usable_area = m->m;
  LayerSurface *l;
  uint32_t layers_above_shell[] = {
      ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
      ZWLR_LAYER_SHELL_V1_LAYER_TOP,
  };
  if (!m->wlr_output->enabled)
    return;

  /* Arrange exclusive surfaces from top->bottom */
  for (i = 3; i >= 0; i--)
    arrangelayer(m, &m->layers[i], &usable_area, 1);

  if (!wlr_box_equal(&usable_area, &m->w)) {
    m->w = usable_area;
    arrange(m);
  }

  /* Arrange non-exclusive surfaces from top->bottom */
  for (i = 3; i >= 0; i--)
    arrangelayer(m, &m->layers[i], &usable_area, 0);

  /* Find topmost keyboard interactive layer, if such a layer exists */
  for (i = 0; i < (int)LENGTH(layers_above_shell); i++) {
    wl_list_for_each_reverse(l, &m->layers[layers_above_shell[i]], link) {
      if (locked || !l->layer_surface->current.keyboard_interactive ||
          !l->mapped)
        continue;
      /* Deactivate the focused client. */
      focusclient(nullptr, 0);
      exclusive_focus = l;
      client_notify_enter(l->layer_surface->surface,
                          wlr_seat_get_keyboard(seat));
      return;
    }
  }
}

static void dispatch_action(const char *action,
                            const char (*args)[CFG_MAX_STRLEN], int nargs);

static void axisnotify(struct wl_listener *listener, [[maybe_unused]] void *data) {
  /* This event is forwarded by the cursor when a pointer emits an axis event,
   * for example when you move the scroll wheel. */
  struct wlr_pointer_axis_event *event = data;

  /* Check scroll bindings first (trackpad vs mouse wheel separation) */
  struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
  uint32_t mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;

  for (int i = 0; i < cfg.nscrolls; i++) {
    const CfgScroll *s = &cfg.scrolls[i];
    if (CLEANMASK(mods) == CLEANMASK(s->mods) &&
        (s->source == -1 || s->source == (int)event->source) &&
        (s->orientation == -1 || s->orientation == (int)event->orientation)) {
      if (s->action[0] == '\0')
        break;
      dispatch_action(s->action, s->args, s->nargs);
      return;
    }
  }

  wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
  wlr_seat_pointer_notify_axis(seat, event->time_msec, event->orientation,
                               event->delta, event->delta_discrete,
                               event->source, event->relative_direction);
}

static void buttonpress(struct wl_listener *listener, [[maybe_unused]] void *data) {
  struct wlr_pointer_button_event *event = data;
  struct wlr_keyboard *keyboard;
  uint32_t mods;
  Client *c;

  wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

  switch (event->state) {
  case WL_POINTER_BUTTON_STATE_PRESSED:
    cursor_mode = CurPressed;
    selmon = xytomon(cursor->x, cursor->y);
    if (locked)
      break;

    /* Change focus if the button was _pressed_ over a client */
    xytonode(cursor->x, cursor->y, nullptr, &c, nullptr, nullptr, nullptr);
    if (c && (!client_is_unmanaged(c) || client_wants_focus(c)))
      focusclient(c, 1);

    keyboard = wlr_seat_get_keyboard(seat);
    mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
    for (int i = 0; i < cfg.nbuttons; i++) {
      const CfgButton *b = &cfg.buttons[i];
      if (CLEANMASK(mods) == CLEANMASK(b->mods) && event->button == b->button) {
        dispatch_action(b->action, b->args, b->nargs);
        return;
      }
    }
    break;
  case WL_POINTER_BUTTON_STATE_RELEASED:
    /* If you released any buttons, we exit interactive move/resize mode. */
    if (!locked && cursor_mode != CurNormal && cursor_mode != CurPressed) {
      wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
      if (cursor_mode == CurTileDrag) {
        /* Find whatever tiled client is under the cursor and swap. */
        Client *target = nullptr;
        xytonode(cursor->x, cursor->y, nullptr, &target, nullptr, nullptr, nullptr);
        if (target && target != grabc && !target->isfloating &&
            !target->isfullscreen &&
            curlayout(target->mon)->arrange)
          swaptiled(grabc, target);
        cursor_mode = CurNormal;
        grabc = nullptr;
        return;
      }
      cursor_mode = CurNormal;
      /* Drop the window off on its new monitor */
      selmon = xytomon(cursor->x, cursor->y);
      setmon(grabc, selmon, 0);
      grabc = nullptr;
      return;
    }
    cursor_mode = CurNormal;
    break;
  }
  /* If the event wasn't handled by the compositor, notify the client with
   * pointer focus that a button press has occurred */
  wlr_seat_pointer_notify_button(seat, event->time_msec, event->button,
                                 event->state);
}

static void chvt(const Arg *arg) {
  if (!session) {
    fprintf(stderr, "peachwm: chvt: no session (not running on DRM)\n");
    return;
  }
  wlr_session_change_vt(session, arg->ui);
}

static void checkidleinhibitor(struct wlr_surface *exclude) {
  int inhibited = 0, unused_lx, unused_ly;
  struct wlr_idle_inhibitor_v1 *inhibitor;
  wl_list_for_each(inhibitor, &idle_inhibit_mgr->inhibitors, link) {
    struct wlr_surface *surface =
        wlr_surface_get_root_surface(inhibitor->surface);
    struct wlr_scene_tree *tree = surface->data;
    if (exclude != surface &&
        (cfg.bypass_surface_visibility ||
         (!tree ||
          wlr_scene_node_coords(&tree->node, &unused_lx, &unused_ly)))) {
      inhibited = 1;
      break;
    }
  }

  wlr_idle_notifier_v1_set_inhibited(idle_notifier, inhibited);
}

static void cleanup(void) {
  config_watch_stop(cfg_watch_src);
  cfg_watch_src = nullptr;
  ipc_socket_finish();
  cleanuplisteners();

#ifdef XWAYLAND
  /* SIGKILL XWayland first so the internal waitpid in
   * wlr_xwayland_destroy can't block. */
  if (xwayland && xwayland->server && xwayland->server->pid > 0)
    kill(xwayland->server->pid, SIGKILL);
  wlr_xwayland_destroy(xwayland);
  xwayland = nullptr;
#endif

  wl_display_destroy_clients(dpy);
  if (child_pid > 0) {
    kill(-child_pid, SIGTERM);
    close(STDOUT_FILENO);
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};
    int ret;
    for (int i = 0; i < 50; i++) {
      ret = waitpid(child_pid, nullptr, WNOHANG);
      if (ret == child_pid || ret == -1)
        goto child_done;
      nanosleep(&ts, nullptr);
    }
    kill(-child_pid, SIGKILL);
    waitpid(child_pid, nullptr, 0);
  }
child_done:
  wlr_xcursor_manager_destroy(cursor_mgr);

  destroykeyboardgroup(&kb_group->destroy, nullptr);

  /* If it's not destroyed manually, it will cause a use-after-free of wlr_seat.
   * Destroy it until it's fixed on the wlroots side */
  wlr_backend_destroy(backend);

  wl_display_destroy(dpy);
  /* Destroy after the wayland display (when the monitors are already destroyed)
     to avoid destroying them with an invalid scene output. */
  wlr_scene_node_destroy(&scene->tree.node);
}

static void cleanupmon(struct wl_listener *listener, void *data) {
  Monitor *m = wl_container_of(listener, m, destroy);
  LayerSurface *l, *tmp;
  size_t i;

  /* m->layers[i] are intentionally not unlinked */
  for (i = 0; i < LENGTH(m->layers); i++) {
    wl_list_for_each_safe(l, tmp, &m->layers[i], link)
        wlr_layer_surface_v1_destroy(l->layer_surface);
  }

  wl_list_remove(&m->destroy.link);
  wl_list_remove(&m->frame.link);
  wl_list_remove(&m->link);
  wl_list_remove(&m->request_state.link);
  if (m->lock_surface)
    destroylocksurface(&m->destroy_lock_surface, nullptr);
  m->wlr_output->data = nullptr;
  wlr_output_layout_remove(output_layout, m->wlr_output);
  wlr_scene_output_destroy(m->scene_output);

  closemon(m);
  ext_workspace_cleanupmon(m);
  ipc_socket_send_output_event();
  if (m->cold) {
    for (int i = 0; i < TAGCOUNT; i++)
      dwindle_free_tree(m->cold->dwindle_root[i]);
    free(m->cold);
  }
  wlr_scene_node_destroy(&m->fullscreen_bg->node);
  free(m);
}

static void cleanuplisteners(void) {
  wl_list_remove(&cursor_axis.link);
  wl_list_remove(&cursor_button.link);
  wl_list_remove(&cursor_frame.link);
  wl_list_remove(&cursor_motion.link);
  wl_list_remove(&cursor_motion_absolute.link);
  wl_list_remove(&gpu_reset.link);
  wl_list_remove(&new_idle_inhibitor.link);
  wl_list_remove(&layout_change.link);
  wl_list_remove(&new_input_device.link);
  wl_list_remove(&new_virtual_keyboard.link);
  wl_list_remove(&new_virtual_pointer.link);
  wl_list_remove(&new_pointer_constraint.link);
  wl_list_remove(&new_output.link);
  wl_list_remove(&new_xdg_toplevel.link);
  wl_list_remove(&new_xdg_decoration.link);
  wl_list_remove(&new_xdg_popup.link);
  wl_list_remove(&new_layer_surface.link);
  wl_list_remove(&output_mgr_apply.link);
  wl_list_remove(&output_mgr_test.link);
  wl_list_remove(&output_power_mgr_set_mode.link);
  wl_list_remove(&request_activate.link);
  wl_list_remove(&request_cursor.link);
  wl_list_remove(&request_set_psel.link);
  wl_list_remove(&request_set_sel.link);
  wl_list_remove(&request_set_cursor_shape.link);
  wl_list_remove(&request_start_drag.link);
  wl_list_remove(&start_drag.link);
  wl_list_remove(&new_session_lock.link);
#ifdef XWAYLAND
  wl_list_remove(&new_xwayland_surface.link);
  wl_list_remove(&xwayland_ready.link);
#endif
}

static void closemon(Monitor *m) {
  /* update selmon if needed and
   * move closed monitor's clients to the focused one */
  Client *c;
  int i = 0, nmons = wl_list_length(&mons);
  if (!nmons) {
    selmon = nullptr;
  } else if (m == selmon) {
    do /* don't switch to disabled mons */
      selmon = wl_container_of(mons.next, selmon, link);
    while (!selmon->wlr_output->enabled && i++ < nmons);

    if (!selmon->wlr_output->enabled)
      selmon = nullptr;
  }

  wl_list_for_each(c, &clients, link) {
    if (c->mon == m) {
      if (c->isfloating && c->geom.x > m->m.width)
        resize(c,
               (struct wlr_box){.x = c->geom.x - m->w.width,
                                .y = c->geom.y,
                                .width = c->geom.width,
                                .height = c->geom.height},
               0);
      setmon(c, selmon, c->tags);
    }
  }
  focusclient(focustop(selmon), 1);
  printstatus();
}

// ── Compositor Lifecycle ────────────────────────────────────

static void commitlayersurfacenotify(struct wl_listener *listener, void *data) {
  LayerSurface *l = wl_container_of(listener, l, surface_commit);
  struct wlr_layer_surface_v1 *layer_surface = l->layer_surface;
  struct wlr_scene_tree *scene_layer =
      layers[layermap[layer_surface->current.layer]];
  struct wlr_layer_surface_v1_state old_state;

  if (l->layer_surface->initial_commit) {
    client_set_scale(layer_surface->surface, l->mon->wlr_output->scale);

    /* Temporarily set the layer's current state to pending
     * so that we can easily arrange it */
    old_state = l->layer_surface->current;
    l->layer_surface->current = l->layer_surface->pending;
    arrangelayers(l->mon);
    l->layer_surface->current = old_state;
    return;
  }

  if (layer_surface->current.committed == 0 &&
      l->mapped == layer_surface->surface->mapped)
    return;
  l->mapped = layer_surface->surface->mapped;

  if (scene_layer != l->scene->node.parent) {
    wlr_scene_node_reparent(&l->scene->node, scene_layer);
    wl_list_remove(&l->link);
    wl_list_insert(&l->mon->layers[layer_surface->current.layer], &l->link);
    wlr_scene_node_reparent(
        &l->popups->node,
        (layer_surface->current.layer < ZWLR_LAYER_SHELL_V1_LAYER_TOP
             ? layers[LyrTop]
             : scene_layer));
  }

  arrangelayers(l->mon);
}

static void commitnotify(struct wl_listener *listener, void *data) {
  Client *c = wl_container_of(listener, c, commit);

  if (c->surface.xdg->initial_commit) {
    /*
     * Get the monitor this client will be rendered on
     * Note that if the user set a rule in which the client is placed on
     * a different monitor based on its title, this will likely select
     * a wrong monitor.
     */
    applyrules(c);
    if (c->mon) {
      client_set_scale(client_surface(c), c->mon->wlr_output->scale);
    }
    setmon(c, nullptr, 0); /* Make sure to reapply rules in mapnotify() */

    wlr_xdg_toplevel_set_wm_capabilities(
        c->surface.xdg->toplevel, WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
    if (c->decoration)
      requestdecorationmode(&c->set_decoration_mode, c->decoration);
    wlr_xdg_toplevel_set_size(c->surface.xdg->toplevel, 0, 0);
    return;
  }

  resize(c, c->geom, (c->isfloating && !c->isfullscreen));

  /* mark a pending resize as completed */
  if (c->resize && c->resize <= c->surface.xdg->current.configure_serial)
    c->resize = 0;
}

static void commitpopup(struct wl_listener *listener, void *data) {
  struct wlr_surface *surface = data;
  struct wlr_xdg_popup *popup = wlr_xdg_popup_try_from_wlr_surface(surface);
  LayerSurface *l = nullptr;
  Client *c = nullptr;
  struct wlr_box box;
  int type = -1;

  if (!popup->base->initial_commit)
    return;

  type = toplevel_from_wlr_surface(popup->base->surface, &c, &l);
  if (!popup->parent || type < 0)
    return;
  popup->base->surface->data =
      wlr_scene_xdg_surface_create(popup->parent->data, popup->base);
  if ((l && !l->mon) || (c && !c->mon)) {
    wlr_xdg_popup_destroy(popup);
    return;
  }
  box = type == LayerShell ? l->mon->m : c->mon->w;
  box.x -= (type == LayerShell ? l->scene->node.x : c->geom.x);
  box.y -= (type == LayerShell ? l->scene->node.y : c->geom.y);
  wlr_xdg_popup_unconstrain_from_box(popup, &box);
  wl_list_remove(&listener->link);
  free(listener);
}

static void createdecoration(struct wl_listener *listener, void *data) {
  struct wlr_xdg_toplevel_decoration_v1 *deco = data;
  Client *c = deco->toplevel->base->data;
  c->decoration = deco;

  LISTEN(&deco->events.request_mode, &c->set_decoration_mode,
         requestdecorationmode);
  LISTEN(&deco->events.destroy, &c->destroy_decoration, destroydecoration);

  requestdecorationmode(&c->set_decoration_mode, deco);
}

static void createidleinhibitor(struct wl_listener *listener, [[maybe_unused]] void *data) {
  struct wlr_idle_inhibitor_v1 *idle_inhibitor = data;
  LISTEN_STATIC(&idle_inhibitor->events.destroy, destroyidleinhibitor);

  checkidleinhibitor(nullptr);
}

static void createkeyboard(struct wlr_keyboard *keyboard) {
  /* Set the keymap to match the group keymap */
  wlr_keyboard_set_keymap(keyboard, kb_group->wlr_group->keyboard.keymap);

  /* Add the new keyboard to the group */
  wlr_keyboard_group_add_keyboard(kb_group->wlr_group, keyboard);
}

static KeyboardGroup *createkeyboardgroup(void) {
  KeyboardGroup *group = ecalloc(1, sizeof(*group));
  struct xkb_context *context;
  struct xkb_keymap *keymap;

  group->wlr_group = wlr_keyboard_group_create();
  group->wlr_group->data = group;

  /* Prepare an XKB keymap and assign it to the keyboard group. */
  context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (!(keymap = xkb_keymap_new_from_names(
            context, &(struct xkb_rule_names){.options = nullptr},
            XKB_KEYMAP_COMPILE_NO_FLAGS)))
    die("failed to compile keymap");

  wlr_keyboard_set_keymap(&group->wlr_group->keyboard, keymap);
  xkb_keymap_unref(keymap);
  xkb_context_unref(context);

  wlr_keyboard_set_repeat_info(&group->wlr_group->keyboard,
                               cfg.input.repeat_rate, cfg.input.repeat_delay);

  /* Set up listeners for keyboard events */
  LISTEN(&group->wlr_group->keyboard.events.key, &group->key, keypress);
  LISTEN(&group->wlr_group->keyboard.events.modifiers, &group->modifiers,
         keypressmod);

  group->key_repeat_source =
      wl_event_loop_add_timer(event_loop, keyrepeat, group);

  /* A seat can only have one keyboard, but this is a limitation of the
   * Wayland protocol - not wlroots. We assign all connected keyboards to the
   * same wlr_keyboard_group, which provides a single wlr_keyboard interface for
   * all of them. Set this combined wlr_keyboard as the seat keyboard.
   */
  wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
  return group;
}

static void createlayersurface(struct wl_listener *listener, void *data) {
  struct wlr_layer_surface_v1 *layer_surface = data;
  LayerSurface *l;
  struct wlr_surface *surface = layer_surface->surface;
  struct wlr_scene_tree *scene_layer =
      layers[layermap[layer_surface->pending.layer]];

  if (!layer_surface->output &&
      !(layer_surface->output = selmon ? selmon->wlr_output : nullptr)) {
    wlr_layer_surface_v1_destroy(layer_surface);
    return;
  }

  l = layer_surface->data = ecalloc(1, sizeof(*l));
  l->type = LayerShell;
  LISTEN(&surface->events.commit, &l->surface_commit, commitlayersurfacenotify);
  LISTEN(&surface->events.unmap, &l->unmap, unmaplayersurfacenotify);
  LISTEN(&layer_surface->events.destroy, &l->destroy,
         destroylayersurfacenotify);

  l->layer_surface = layer_surface;
  l->mon = layer_surface->output->data;
  l->scene_layer =
      wlr_scene_layer_surface_v1_create(scene_layer, layer_surface);
  l->scene = l->scene_layer->tree;
  l->popups = surface->data = wlr_scene_tree_create(
      layer_surface->current.layer < ZWLR_LAYER_SHELL_V1_LAYER_TOP
          ? layers[LyrTop]
          : scene_layer);
  l->scene->node.data = l->popups->node.data = l;

  wl_list_insert(&l->mon->layers[layer_surface->pending.layer], &l->link);
  wlr_surface_send_enter(surface, layer_surface->output);
}

static void createlocksurface(struct wl_listener *listener, void *data) {
  SessionLock *lock = wl_container_of(listener, lock, new_surface);
  struct wlr_session_lock_surface_v1 *lock_surface = data;
  Monitor *m = lock_surface->output->data;
  struct wlr_scene_tree *scene_tree = lock_surface->surface->data =
      wlr_scene_subsurface_tree_create(lock->scene, lock_surface->surface);
  m->lock_surface = lock_surface;

  wlr_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
  wlr_session_lock_surface_v1_configure(lock_surface, m->m.width, m->m.height);

  LISTEN(&lock_surface->events.destroy, &m->destroy_lock_surface,
         destroylocksurface);

  if (m == selmon)
    client_notify_enter(lock_surface->surface, wlr_seat_get_keyboard(seat));
}

static void createmon(struct wl_listener *listener, void *data) {
  /* This event is raised by the backend when a new output (aka a display or
   * monitor) becomes available. */
  struct wlr_output *wlr_output = data;
  size_t i;
  struct wlr_output_state state;
  Monitor *m;

  if (!wlr_output_init_render(wlr_output, alloc, drw))
    return;

  m = wlr_output->data = ecalloc(1, sizeof(*m));
  m->wlr_output = wlr_output;

  for (i = 0; i < LENGTH(m->layers); i++)
    wl_list_init(&m->layers[i]);

  wlr_output_state_init(&state);
  /* Initialize monitor state using configured rules */
  m->tagset[0] = m->tagset[1] = 1;
  m->gaps = (cfg.appearance.gaps > 0) ? 1 : 0;
  {
    int ri;
    for (ri = 0; ri < cfg.nmonitors; ri++) {
      const CfgMonitorRule *r = &cfg.monitors[ri];
      if (!r->name[0] || strstr(wlr_output->name, r->name)) {
        m->m.x = r->x;
        m->m.y = r->y;
        /* Allocate cold state lazily via the shared helper */
        ensure_cold(m);
        m->cold->mfact = r->mfact;
        m->cold->nmaster = r->nmaster;
        /* Determine monitor's default layout */
        const Layout *mon_layout = &layouts[0];
        for (int li = 0; li < (int)layout_count; li++) {
          if (layouts[li].symbol && !strcmp(r->layout, layouts[li].symbol)) {
            mon_layout = &layouts[li];
            break;
          }
          if (!strcmp(r->layout, "dwindle") && layouts[li].arrange == dwindle) {
            mon_layout = &layouts[li];
            break;
          }
          if (!strcmp(r->layout, "master") && layouts[li].arrange == master) {
            mon_layout = &layouts[li];
            break;
          }
          if (!strcmp(r->layout, "monocle") && layouts[li].arrange == monocle) {
            mon_layout = &layouts[li];
            break;
          }
        }
        const Layout *alt_layout = &layouts[layout_count > 1 && mon_layout != &layouts[1]];
        /* Propagate monitor layout to all tags */
        for (int ti = 0; ti < TAGCOUNT; ti++) {
          m->cold->lt[ti][0] = mon_layout;
          m->cold->lt[ti][1] = alt_layout;
          m->cold->sellt[ti] = 0;
        }
        /* Apply per-workspace default layout for all tags */
        for (int ti = 0; ti < TAGCOUNT; ti++)
          apply_workspace_layout(m, ti);
        strncpy(m->cold->ltsymbol[0], curlayout(m)->symbol, LENGTH(m->cold->ltsymbol[0]));
        m->cold->ltsymbol[0][LENGTH(m->cold->ltsymbol[0]) - 1] = '\0';
        wlr_output_state_set_scale(&state, r->scale);
        wlr_output_state_set_transform(&state, r->transform);
        break;
      }
    }
  }

  /* The mode is a tuple of (width, height, refresh rate), and each
   * monitor supports only a specific set of modes. We just pick the
   * monitor's preferred mode; a more sophisticated compositor would let
   * the user configure it. */
  wlr_output_state_set_mode(&state, wlr_output_preferred_mode(wlr_output));

  /* Set up event listeners */
  LISTEN(&wlr_output->events.frame, &m->frame, rendermon);
  LISTEN(&wlr_output->events.destroy, &m->destroy, cleanupmon);
  LISTEN(&wlr_output->events.request_state, &m->request_state, requestmonstate);

  wlr_output_state_set_enabled(&state, 1);
  wlr_output_commit_state(wlr_output, &state);
  wlr_output_state_finish(&state);

  wl_list_insert(&mons, &m->link);
  ext_workspace_createmon(m);
  printstatus();
  ipc_socket_send_output_event();

  /* The xdg-protocol specifies:
   *
   * If the fullscreened surface is not opaque, the compositor must make
   * sure that other screen content not part of the same surface tree (made
   * up of subsurfaces, popups or similarly coupled surfaces) are not
   * visible below the fullscreened surface.
   *
   */

  /* updatemons() will resize and set correct position */
  m->fullscreen_bg =
      wlr_scene_rect_create(layers[LyrFS], 0, 0, cfg.appearance.fullscreen_bg);
  wlr_scene_node_set_enabled(&m->fullscreen_bg->node, 0);

  /* Adds this to the output layout in the order it was configured.
   *
   * The output layout utility automatically adds a wl_output global to the
   * display, which Wayland clients can see to find out information about the
   * output (such as DPI, scale factor, manufacturer, etc).
   */
  m->scene_output = wlr_scene_output_create(scene, wlr_output);
  if (m->m.x == -1 && m->m.y == -1)
    wlr_output_layout_add_auto(output_layout, wlr_output);
  else
    wlr_output_layout_add(output_layout, wlr_output, m->m.x, m->m.y);
}

static void createnotify(struct wl_listener *listener, void *data) {
  /* This event is raised when a client creates a new toplevel (application
   * window). */
  struct wlr_xdg_toplevel *toplevel = data;
  Client *c = nullptr;

  /* Allocate a Client for this surface */
  c = toplevel->base->data = ecalloc(1, sizeof(*c));
  c->surface.xdg = toplevel->base;
  c->bw = cfg.appearance.border_px;

  LISTEN(&toplevel->base->surface->events.commit, &c->commit, commitnotify);
  LISTEN(&toplevel->base->surface->events.map, &c->map, mapnotify);
  LISTEN(&toplevel->base->surface->events.unmap, &c->unmap, unmapnotify);
  LISTEN(&toplevel->events.destroy, &c->destroy, destroynotify);
  LISTEN(&toplevel->events.request_fullscreen, &c->fullscreen,
         fullscreennotify);
  LISTEN(&toplevel->events.request_maximize, &c->maximize, maximizenotify);
  LISTEN(&toplevel->events.set_title, &c->set_title, updatetitle);
}

static void pointer_device_destroy(struct wl_listener *listener, void *data) {
  PointerDevice *pdev = wl_container_of(listener, pdev, destroy);
  wl_list_remove(&pdev->link);
  wl_list_remove(&pdev->destroy.link);
  free(pdev);
}

static void createpointer(struct wlr_pointer *pointer) {
  struct libinput_device *device;
  PointerDevice *pdev = ecalloc(1, sizeof(*pdev));
  pdev->device = &pointer->base;
  pdev->destroy.notify = pointer_device_destroy;
  wl_signal_add(&pointer->base.events.destroy, &pdev->destroy);
  wl_list_insert(&pointer_devices, &pdev->link);

  if (wlr_input_device_is_libinput(&pointer->base) &&
      (device = wlr_libinput_get_device_handle(&pointer->base))) {

    if (libinput_device_config_tap_get_finger_count(device)) {
      libinput_device_config_tap_set_enabled(device, cfg.input.tap_to_click);
      libinput_device_config_tap_set_drag_enabled(device,
                                                  cfg.input.tap_and_drag);
      libinput_device_config_tap_set_drag_lock_enabled(device,
                                                       cfg.input.drag_lock);
      libinput_device_config_tap_set_button_map(device,
                                                cfg.input.tap_button_map);
    }

    if (libinput_device_config_scroll_has_natural_scroll(device))
      libinput_device_config_scroll_set_natural_scroll_enabled(
          device, cfg.input.natural_scrolling);

    if (libinput_device_config_dwt_is_available(device))
      libinput_device_config_dwt_set_enabled(device,
                                             cfg.input.disable_while_typing);

    if (libinput_device_config_left_handed_is_available(device))
      libinput_device_config_left_handed_set(device, cfg.input.left_handed);

    if (libinput_device_config_middle_emulation_is_available(device))
      libinput_device_config_middle_emulation_set_enabled(
          device, cfg.input.middle_button_emulation);

    if (libinput_device_config_scroll_get_methods(device) !=
        LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
      libinput_device_config_scroll_set_method(device, cfg.input.scroll_method);

    if (libinput_device_config_click_get_methods(device) !=
        LIBINPUT_CONFIG_CLICK_METHOD_NONE)
      libinput_device_config_click_set_method(device, cfg.input.click_method);

    if (libinput_device_config_send_events_get_modes(device))
      libinput_device_config_send_events_set_mode(device,
                                                  cfg.input.send_events_mode);

    if (libinput_device_config_accel_is_available(device)) {
      libinput_device_config_accel_set_profile(device, cfg.input.accel_profile);
      libinput_device_config_accel_set_speed(device, cfg.input.accel_speed);
    }
  }

  wlr_cursor_attach_input_device(cursor, &pointer->base);
}

static void createpointerconstraint(struct wl_listener *listener, void *data) {
  PointerConstraint *pointer_constraint =
      ecalloc(1, sizeof(*pointer_constraint));
  pointer_constraint->constraint = data;
  LISTEN(&pointer_constraint->constraint->events.destroy,
         &pointer_constraint->destroy, destroypointerconstraint);
}

static void createpopup(struct wl_listener *listener, void *data) {
  /* This event is raised when a client (either xdg-shell or layer-shell)
   * creates a new popup. */
  struct wlr_xdg_popup *popup = data;
  LISTEN_STATIC(&popup->base->surface->events.commit, commitpopup);
}

static void cursorconstrain(struct wlr_pointer_constraint_v1 *constraint) {
  if (active_constraint == constraint)
    return;

  if (active_constraint)
    wlr_pointer_constraint_v1_send_deactivated(active_constraint);

  active_constraint = constraint;
  wlr_pointer_constraint_v1_send_activated(constraint);
}

static void cursorframe(struct wl_listener *listener, void *data) {
  /* This event is forwarded by the cursor when a pointer emits a frame
   * event. Frame events are sent after regular pointer events to group
   * multiple events together. For instance, two axis events may happen at the
   * same time, in which case a frame event won't be sent in between. */
  /* Notify the client with pointer focus of the frame event. */
  wlr_seat_pointer_notify_frame(seat);
}

static void cursorwarptohint(void) {
  if (!active_constraint)
    return;
  Client *c = nullptr;
  double sx = active_constraint->current.cursor_hint.x;
  double sy = active_constraint->current.cursor_hint.y;

  (void)toplevel_from_wlr_surface(active_constraint->surface, &c, nullptr);
  if (c && active_constraint->current.cursor_hint.enabled) {
    wlr_cursor_warp(cursor, nullptr, sx + c->geom.x + c->bw,
                    sy + c->geom.y + c->bw);
    wlr_seat_pointer_warp(active_constraint->seat, sx, sy);
  }
}

static void destroydecoration(struct wl_listener *listener, void *data) {
  Client *c = wl_container_of(listener, c, destroy_decoration);

  wl_list_remove(&c->destroy_decoration.link);
  wl_list_remove(&c->set_decoration_mode.link);
}

static void destroydragicon(struct wl_listener *listener, void *data) {
  /* Focus enter isn't sent during drag, so refocus the focused node. */
  focusclient(focustop(selmon), 1);
  motionnotify(0, nullptr, 0, 0, 0, 0);
  wl_list_remove(&listener->link);
  free(listener);
}

static void destroyidleinhibitor(struct wl_listener *listener, void *data) {
  /* `data` is the wlr_surface of the idle inhibitor being destroyed,
   * at this point the idle inhibitor is still in the list of the manager */
  checkidleinhibitor(wlr_surface_get_root_surface(data));
  wl_list_remove(&listener->link);
  free(listener);
}

static void destroylayersurfacenotify(struct wl_listener *listener, void *data) {
  LayerSurface *l = wl_container_of(listener, l, destroy);

  wl_list_remove(&l->link);
  wl_list_remove(&l->destroy.link);
  wl_list_remove(&l->unmap.link);
  wl_list_remove(&l->surface_commit.link);
  wlr_scene_node_destroy(&l->scene->node);
  wlr_scene_node_destroy(&l->popups->node);
  free(l);
}

static void destroylock(SessionLock *lock, int unlock) {
  wlr_seat_keyboard_notify_clear_focus(seat);
  if ((locked = !unlock))
    goto destroy;

  wlr_scene_node_set_enabled(&locked_bg->node, 0);

  focusclient(focustop(selmon), 0);
  motionnotify(0, nullptr, 0, 0, 0, 0);

destroy:
  wl_list_remove(&lock->new_surface.link);
  wl_list_remove(&lock->unlock.link);
  wl_list_remove(&lock->destroy.link);

  wlr_scene_node_destroy(&lock->scene->node);
  cur_lock = nullptr;
  free(lock);
}

static void destroylocksurface(struct wl_listener *listener, [[maybe_unused]] void *data) {
  Monitor *m = wl_container_of(listener, m, destroy_lock_surface);
  struct wlr_session_lock_surface_v1 *surface, *lock_surface = m->lock_surface;

  m->lock_surface = nullptr;
  wl_list_remove(&m->destroy_lock_surface.link);

  if (lock_surface->surface != seat->keyboard_state.focused_surface)
    return;

  if (locked && cur_lock && !wl_list_empty(&cur_lock->surfaces)) {
    surface = wl_container_of(cur_lock->surfaces.next, surface, link);
    client_notify_enter(surface->surface, wlr_seat_get_keyboard(seat));
  } else if (!locked) {
    focusclient(focustop(selmon), 1);
  } else {
    wlr_seat_keyboard_clear_focus(seat);
  }
}

static void destroynotify(struct wl_listener *listener, void *data) {
  /* Called when the xdg_toplevel is destroyed. */
  Client *c = wl_container_of(listener, c, destroy);
  wl_list_remove(&c->destroy.link);
  wl_list_remove(&c->set_title.link);
  wl_list_remove(&c->fullscreen.link);
#ifdef XWAYLAND
  if (c->type != XDGShell) {
    wl_list_remove(&c->activate.link);
    wl_list_remove(&c->associate.link);
    wl_list_remove(&c->configure.link);
    wl_list_remove(&c->dissociate.link);
    wl_list_remove(&c->set_hints.link);
  } else
#endif
  {
    wl_list_remove(&c->commit.link);
    wl_list_remove(&c->map.link);
    wl_list_remove(&c->unmap.link);
    wl_list_remove(&c->maximize.link);
  }
  free(c);
}

static void destroypointerconstraint(struct wl_listener *listener, void *data) {
  PointerConstraint *pointer_constraint =
      wl_container_of(listener, pointer_constraint, destroy);

  if (active_constraint == pointer_constraint->constraint) {
    cursorwarptohint();
    active_constraint = nullptr;
  }

  wl_list_remove(&pointer_constraint->destroy.link);
  free(pointer_constraint);
}

static void destroysessionlock(struct wl_listener *listener, void *data) {
  SessionLock *lock = wl_container_of(listener, lock, destroy);
  destroylock(lock, 0);
}

static void destroykeyboardgroup(struct wl_listener *listener, void *data) {
  KeyboardGroup *group = wl_container_of(listener, group, destroy);
  wl_event_source_remove(group->key_repeat_source);
  wl_list_remove(&group->key.link);
  wl_list_remove(&group->modifiers.link);
  wl_list_remove(&group->destroy.link);
  wlr_keyboard_group_destroy(group->wlr_group);
  free(group);
}

static Monitor *dirtomon(enum wlr_direction dir) {
  struct wlr_output *next;
  if (!wlr_output_layout_get(output_layout, selmon->wlr_output))
    return selmon;
  if ((next = wlr_output_layout_adjacent_output(
           output_layout, dir, selmon->wlr_output, selmon->m.x, selmon->m.y)))
    return next->data;
  if ((next = wlr_output_layout_farthest_output(
           output_layout, dir ^ (WLR_DIRECTION_LEFT | WLR_DIRECTION_RIGHT),
           selmon->wlr_output, selmon->m.x, selmon->m.y)))
    return next->data;
  return selmon;
}

static bool warp_focus = true;

void focusclient(Client *c, int lift) {
  struct wlr_surface *old = seat->keyboard_state.focused_surface;
  int unused_lx, unused_ly, old_client_type;
  Client *old_c = nullptr;
  LayerSurface *old_l = nullptr;

  if (locked)
    return;

  /* Raise client in stacking order if requested */
  if (c && lift)
    wlr_scene_node_raise_to_top(&c->scene->node);

  if (c && client_surface(c) == old)
    return;

  if ((old_client_type = toplevel_from_wlr_surface(old, &old_c, &old_l)) ==
      XDGShell) {
    struct wlr_xdg_popup *popup, *tmp;
    wl_list_for_each_safe(popup, tmp, &old_c->surface.xdg->popups, link)
        wlr_xdg_popup_destroy(popup);
  }

  /* Put the new client atop the focus stack and select its monitor */
  if (c && !client_is_unmanaged(c)) {
    wl_list_remove(&c->flink);
    wl_list_insert(&fstack, &c->flink);
    selmon = c->mon;
    c->isurgent = 0;

    /* Track focused client for dwindle insertion anchor */
    if (c->mon && !c->isfloating && !c->isfullscreen && c->mon->cold)
      c->mon->cold->dwindle_focus[current_tag_idx(c->mon)] = c;

    /* Don't change border color if there is an exclusive focus or we are
     * handling a drag operation */
    if (!exclusive_focus && !seat->drag)
      client_set_border_color(c, cfg.appearance.focus_color);
  }

  /* Deactivate old client if focus is changing */
  if (old && (!c || client_surface(c) != old)) {
    /* If an overlay is focused, don't focus or activate the client,
     * but only update its position in fstack to render its border with
     * cfg.appearance.focus_color and focus it after the overlay is closed. */
    if (old_client_type == LayerShell &&
        wlr_scene_node_coords(&old_l->scene->node, &unused_lx, &unused_ly) &&
        old_l->layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP) {
      return;
    } else if (old_c && old_c == exclusive_focus && client_wants_focus(old_c)) {
      return;
      /* Don't deactivate old client if the new one wants focus, as this causes
       * issues with winecfg and probably other clients */
    } else if (old_c && !client_is_unmanaged(old_c) &&
               (!c || !client_wants_focus(c))) {
      client_set_border_color(old_c, cfg.appearance.border_color);

      client_activate_surface(old, 0);
    }
  }
  printstatus();

  if (!c) {
    /* With no client, all we have left is to clear focus */
    wlr_seat_keyboard_notify_clear_focus(seat);
    return;
  }

  /* Change cursor surface */
  motionnotify(0, nullptr, 0, 0, 0, 0);

  /* Have a client, so focus its top-level wlr_surface */
  client_notify_enter(client_surface(c), wlr_seat_get_keyboard(seat));

  /* Activate the new client */
  client_activate_surface(client_surface(c), 1);

  if (cfg.sloppyfocus && warp_focus)
    wlr_cursor_warp(cursor, nullptr, c->geom.x + c->geom.width / 2,
        c->geom.y + c->geom.height / 2);
}

static void focusmon(const Arg *arg) {
  int i = 0, nmons = wl_list_length(&mons);
  if (nmons) {
    do /* don't switch to disabled mons */
      selmon = dirtomon(arg->i);
    while (!selmon->wlr_output->enabled && i++ < nmons);
  }
  focusclient(focustop(selmon), 1);
}

/* Return the centre point of a client's geometry. */
static void client_center(Client *c, int *cx, int *cy) {
  *cx = c->geom.x + c->geom.width / 2;
  *cy = c->geom.y + c->geom.height / 2;
}

/* focusdir is basically spatial focus in a cardinal direction.
 * arg->i: WLR_DIRECTION_LEFT, RIGHT, UP, DOWN.
 * It finds the nearest tiled visible client whose centre lies in the
 * requested half-plane relative to the focused client's centre
 *
 * In monocle layout, this cycles through clients instead. */
void focusdir(const Arg *arg) {
  Client *c, *best = nullptr;
  Client *sel = focustop(selmon);
  int sx, sy, cx, cy;
  double bestdist = 1e18, dist;

  if (!sel || (sel->isfullscreen && !client_has_children(sel)))
    return;

  /* Scratchpad monocle cycling */
  if (sel->isscratchpad && selmon && selmon->scratchpad_visible) {
    scratchpad_focusdir_cycle(arg);
    return;
  }

  /* In monocle layout, cycle through clients in order */
  if (curlayout(selmon)->arrange == monocle) {
    Client *first = nullptr, *next = nullptr, *prev = nullptr;
    int found = 0;

    wl_list_for_each(c, &clients, link) {
      if (!VISIBLEON(c, selmon) || c->isfloating || c->isfullscreen)
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

    if (arg->i == WLR_DIRECTION_DOWN || arg->i == WLR_DIRECTION_RIGHT) {
      if (next) {
        focusclient(next, 1);
        arrange(selmon);
      } else if (first) {
        focusclient(first, 1);
        arrange(selmon);
      }
    } else {
      if (prev) {
        focusclient(prev, 1);
        arrange(selmon);
      } else {
        /* wrap to last */
        wl_list_for_each(c, &clients, link) if (VISIBLEON(c, selmon) &&
                                                !c->isfloating &&
                                                !c->isfullscreen) prev = c;
        if (prev) {
          focusclient(prev, 1);
          arrange(selmon);
        }
      }
    }
    return;
  }

  client_center(sel, &sx, &sy);

  wl_list_for_each(c, &clients, link) {
    if (c == sel || !VISIBLEON(c, selmon))
      continue;

    client_center(c, &cx, &cy);

    /* the candidate must be in the requested direction. */
    switch (arg->i) {
    case WLR_DIRECTION_LEFT:
      if (cx >= sx)
        continue;
      break;
    case WLR_DIRECTION_RIGHT:
      if (cx <= sx)
        continue;
      break;
    case WLR_DIRECTION_UP:
      if (cy >= sy)
        continue;
      break;
    case WLR_DIRECTION_DOWN:
      if (cy <= sy)
        continue;
      break;
    default:
      return;
    }

    dist = (double)(cx - sx) * (cx - sx) + (double)(cy - sy) * (cy - sy);
    if (dist < bestdist) {
      bestdist = dist;
      best = c;
    }
  }

  if (best)
    focusclient(best, 1);
}

/* We probably should change the name of this: it sounds like it
 * will focus the topmost client of this mon, when actually will
 * only return that client */
Client *focustop(Monitor *m) {
  Client *c;
  wl_list_for_each(c, &fstack, flink) {
    if (VISIBLEON(c, m))
      return c;
  }
  return nullptr;
}

static void fullscreennotify(struct wl_listener *listener, void *data) {
  Client *c = wl_container_of(listener, c, fullscreen);
  setfullscreen(c, client_wants_fullscreen(c));
}

static void gpureset(struct wl_listener *listener, void *data) {
  struct wlr_renderer *old_drw = drw;
  struct wlr_allocator *old_alloc = alloc;
  struct Monitor *m;
  if (!(drw = fx_renderer_create(backend)))
    die("couldn't recreate renderer");

  if (!(alloc = wlr_allocator_autocreate(backend, drw)))
    die("couldn't recreate allocator");

  wl_list_remove(&gpu_reset.link);
  wl_signal_add(&drw->events.lost, &gpu_reset);

  wlr_compositor_set_renderer(compositor, drw);

  wl_list_for_each(m, &mons, link) {
    wlr_output_init_render(m->wlr_output, alloc, drw);
  }

  wlr_allocator_destroy(old_alloc);
  wlr_renderer_destroy(old_drw);
}

static void handlesig(int signo) {
  if (signo == SIGCHLD)
    while (waitpid(-1, nullptr, WNOHANG) > 0)
      ;
  else if (signo == SIGINT || signo == SIGTERM)
    quit(nullptr);
}

static void inputdevice(struct wl_listener *listener, void *data) {
  /* This event is raised by the backend when a new input device becomes
   * available. */
  struct wlr_input_device *device = data;
  uint32_t caps;

  switch (device->type) {
  case WLR_INPUT_DEVICE_KEYBOARD:
    createkeyboard(wlr_keyboard_from_input_device(device));
    break;
  case WLR_INPUT_DEVICE_POINTER:
    createpointer(wlr_pointer_from_input_device(device));
    break;
  default:
    /* TODO handle other input device types */
    break;
  }

  /* We need to let the wlr_seat know what our capabilities are, which is
   * communiciated to the client. We always have a cursor, even if
   * there are no pointer devices, so we always include that capability. */
  /* TODO do we actually require a cursor? */
  caps = WL_SEAT_CAPABILITY_POINTER;
  if (!wl_list_empty(&kb_group->wlr_group->devices))
    caps |= WL_SEAT_CAPABILITY_KEYBOARD;
  wlr_seat_set_capabilities(seat, caps);
}

// ── Input Handling ──────────────────────────────────────────

/* dispatch_action maps a CfgKeybind/CfgButton action string to the
 * corresponding compositor function and calls it. args[0] is used where
 * needed (spawn argv, tag mask, direction, etc). */
static void dispatch_action(const char *action,
                            const char (*args)[CFG_MAX_STRLEN], int nargs) {
  Arg arg = {0};
  const char *a0 = nargs > 0 ? args[0] : nullptr;

  if (!strcmp(action, "spawn")) {
    /* Build a NULL-terminated argv on the stack.
     * CFG_MAX_ARGS is 16, which is plenty. */
    const char *argv[CFG_MAX_ARGS + 1];
    int i;
    for (i = 0; i < nargs && i < CFG_MAX_ARGS; i++)
      argv[i] = args[i];
    argv[i] = nullptr;
    arg.v = argv;
    spawn(&arg);
    return;
  }
  if (!strcmp(action, "killclient")) {
    killclient(nullptr);
    return;
  }
  if (!strcmp(action, "togglefloating")) {
    togglefloating(nullptr);
    return;
  }
  if (!strcmp(action, "togglefullscreen")) {
    togglefullscreen(nullptr);
    return;
  }
  if (!strcmp(action, "togglegaps")) {
    togglegaps(nullptr);
    return;
  }
  if (!strcmp(action, "quit")) {
    quit(nullptr);
    return;
  }

  if (!strcmp(action, "view")) {
    if (a0 && strcmp(a0, "all") == 0)
      arg.ui = ~0u;
    else if (a0)
      arg.ui = (uint32_t)atoi(a0);
    view(&arg);
    return;
  }
  if (!strcmp(action, "toggleview")) {
    if (a0)
      arg.ui = (uint32_t)atoi(a0);
    toggleview(&arg);
    return;
  }
  if (!strcmp(action, "tag")) {
    if (a0)
      arg.ui = (uint32_t)atoi(a0);
    tag(&arg);
    return;
  }
  if (!strcmp(action, "toggletag")) {
    if (a0)
      arg.ui = (uint32_t)atoi(a0);
    toggletag(&arg);
    return;
  }

  if (!strcmp(action, "focusdir")) {
    if (a0 && !strcmp(a0, "left"))
      arg.i = WLR_DIRECTION_LEFT;
    else if (a0 && !strcmp(a0, "right"))
      arg.i = WLR_DIRECTION_RIGHT;
    else if (a0 && !strcmp(a0, "up"))
      arg.i = WLR_DIRECTION_UP;
    else if (a0 && !strcmp(a0, "down"))
      arg.i = WLR_DIRECTION_DOWN;
    focusdir(&arg);
    return;
  }
  if (!strcmp(action, "swapdir")) {
    int dir = 0;
    if (a0 && !strcmp(a0, "left"))
      dir = WLR_DIRECTION_LEFT;
    else if (a0 && !strcmp(a0, "right"))
      dir = WLR_DIRECTION_RIGHT;
    else if (a0 && !strcmp(a0, "up"))
      dir = WLR_DIRECTION_UP;
    else if (a0 && !strcmp(a0, "down"))
      dir = WLR_DIRECTION_DOWN;

    /* If focused window is floating, move it instead of swapping */
    Client *sel = focustop(selmon);
    if (sel && sel->isfloating && !sel->isfullscreen && dir) {
      int pixels = (nargs > 1 && args[1][0]) ? atoi(args[1]) : 5;
      struct wlr_box new_geo = sel->geom;
      switch (dir) {
      case WLR_DIRECTION_LEFT:   new_geo.x -= pixels; break;
      case WLR_DIRECTION_RIGHT:  new_geo.x += pixels; break;
      case WLR_DIRECTION_UP:     new_geo.y -= pixels; break;
      case WLR_DIRECTION_DOWN:   new_geo.y += pixels; break;
      }
      resize(sel, new_geo, 1);
      return;
    }

    arg.i = dir;
    swapdir(&arg);
    return;
  }
  if (!strcmp(action, "focusmon")) {
    if (a0 && !strcmp(a0, "left"))
      arg.i = WLR_DIRECTION_LEFT;
    else if (a0 && !strcmp(a0, "right"))
      arg.i = WLR_DIRECTION_RIGHT;
    focusmon(&arg);
    return;
  }
  if (!strcmp(action, "setlayout")) {
    /* arg is index into layouts[]; "0" = floating, "1" = dwindle and so
     * on and so forth */

    if (a0)
      arg.ui = (uint32_t)atoi(a0);
    setlayout(&arg);
    return;
  }
  if (!strcmp(action, "setlayout_dwindle")) {
    arg.ui = 1;
    setlayout(&arg);
    return;
  }
  if (!strcmp(action, "setlayout_master")) {
    arg.ui = 2;
    setlayout(&arg);
    return;
  }
  if (!strcmp(action, "setlayout_monocle")) {
    arg.ui = 3;
    setlayout(&arg);
    return;
  }
  if (!strcmp(action, "switchlayout")) {
    uint32_t i;
    for (i = 0; i < layout_count; i++)
      if (curlayout(selmon) == &layouts[i])
        break;
    arg.ui = (i + 1) % layout_count;
    setlayout(&arg);
    return;
  }
  if (!strcmp(action, "tagmon")) {
    if (a0 && !strcmp(a0, "left"))
      arg.i = WLR_DIRECTION_LEFT;
    else if (a0 && !strcmp(a0, "right"))
      arg.i = WLR_DIRECTION_RIGHT;
    tagmon(&arg);
    return;
  }
  if (!strcmp(action, "moveresize")) {
    if (a0 && !strcmp(a0, "move"))
      arg.ui = CurMove;
    else if (a0 && !strcmp(a0, "resize"))
      arg.ui = CurResize;
    moveresize(&arg);
    return;
  }
  if (!strcmp(action, "chvt")) {
    if (a0)
      arg.ui = (uint32_t)atoi(a0);
    chvt(&arg);
    return;
  }
  if (!strcmp(action, "togglescratchpad")) {
    togglescratchpad(nullptr);
    return;
  }
  if (!strcmp(action, "swapdirscratchpad")) {
    swapdirscratchpad(nullptr);
    return;
  }

  fprintf(stderr, "peachwm: unknown action '%s'\n", action);
}

static int keybinding(uint32_t mods, xkb_keysym_t sym) {
  for (int i = 0; i < cfg.nkeybinds; i++) {
    const CfgKeybind *k = &cfg.keybinds[i];
    if (CLEANMASK(mods) == CLEANMASK(k->mods) &&
        xkb_keysym_to_lower(sym) == xkb_keysym_to_lower(k->key)) {
      dispatch_action(k->action, k->args, k->nargs);
      return 1;
    }
  }
  return 0;
}

static void keypress(struct wl_listener *listener, void *data) {
  int i;
  /* This event is raised when a key is pressed or released. */
  KeyboardGroup *group = wl_container_of(listener, group, key);
  struct wlr_keyboard_key_event *event = data;

  /* Translate libinput keycode -> xkbcommon */
  uint32_t keycode = event->keycode + 8;
  /* Get a list of keysyms based on the keymap for this keyboard */
  const xkb_keysym_t *syms;
  int nsyms = xkb_state_key_get_syms(group->wlr_group->keyboard.xkb_state,
                                     keycode, &syms);

  /*
   * Also get the base-level (unshifted) keysyms for this key
   * This is necessary for binds like { mods={"logo","shift"}, key="1" }:
   * when Shift is held, xkb_state_key_get_syms returns "exclam" (0x21),
   * not "1" (0x31), so the config match would silently fail
   * Querying level 0 of the active layout gives it the unshifted sym
   * regardless of modifier state, which matches what the user wrote in
   * the config
   */
  const xkb_keysym_t *base_syms;
  int base_nsyms = xkb_keymap_key_get_syms_by_level(
      xkb_state_get_keymap(group->wlr_group->keyboard.xkb_state), keycode,
      xkb_state_key_get_layout(group->wlr_group->keyboard.xkb_state, keycode),
      0, /* level 0 = unshifted */
      &base_syms);

  int handled = 0;
  uint32_t mods = wlr_keyboard_get_modifiers(&group->wlr_group->keyboard);

  wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

  if (session && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    for (i = 0; i < nsyms; i++) {
      if (syms[i] >= XKB_KEY_XF86Switch_VT_1 &&
          syms[i] <= XKB_KEY_XF86Switch_VT_12) {
        wlr_session_change_vt(session, syms[i] - XKB_KEY_XF86Switch_VT_1 + 1);
        return;
      }
    }
    for (i = 0; i < base_nsyms; i++) {
      if (base_syms[i] >= XKB_KEY_XF86Switch_VT_1 &&
          base_syms[i] <= XKB_KEY_XF86Switch_VT_12) {
        wlr_session_change_vt(session,
                              base_syms[i] - XKB_KEY_XF86Switch_VT_1 + 1);
        return;
      }
    }
  }

  /* Try state-aware syms first, then base syms (catches shifted digit binds).
   */
  if (!locked && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    for (i = 0; i < nsyms; i++)
      handled = keybinding(mods, syms[i]) || handled;
    if (!handled) {
      for (i = 0; i < base_nsyms; i++)
        handled = keybinding(mods, base_syms[i]) || handled;
    }
  }

  if (handled && group->wlr_group->keyboard.repeat_info.delay > 0) {
    group->mods = mods;
    group->keysyms = base_syms;
    group->nsyms = base_nsyms;
    wl_event_source_timer_update(group->key_repeat_source,
                                 group->wlr_group->keyboard.repeat_info.delay);
  } else {
    group->nsyms = 0;
    wl_event_source_timer_update(group->key_repeat_source, 0);
  }

  if (handled)
    return;

  wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
  /* Pass unhandled keycodes along to the client. */
  wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode,
                               event->state);
}

static void keypressmod(struct wl_listener *listener, void *data) {
  /* This event is raised when a modifier key, such as shift or alt, is
   * pressed. We simply communicate this to the client. */
  KeyboardGroup *group = wl_container_of(listener, group, modifiers);

  wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
  /* Send modifiers to the client. */
  wlr_seat_keyboard_notify_modifiers(seat,
                                     &group->wlr_group->keyboard.modifiers);
}

static int keyrepeat(void *data) {
  KeyboardGroup *group = data;
  int i;
  if (!group->nsyms || group->wlr_group->keyboard.repeat_info.rate <= 0)
    return 0;

  wl_event_source_timer_update(group->key_repeat_source,
                               1000 /
                                   group->wlr_group->keyboard.repeat_info.rate);

  for (i = 0; i < group->nsyms; i++)
    keybinding(group->mods, group->keysyms[i]);

  return 0;
}

void killclient(const Arg *arg) {
  Client *sel = focustop(selmon);
  if (sel)
    client_send_close(sel);
}

// ── Session Lock ────────────────────────────────────────────

static void locksession(struct wl_listener *listener, void *data) {
  struct wlr_session_lock_v1 *session_lock = data;
  SessionLock *lock;
  wlr_scene_node_set_enabled(&locked_bg->node, 1);
  if (cur_lock) {
    wlr_session_lock_v1_destroy(session_lock);
    return;
  }
  lock = session_lock->data = ecalloc(1, sizeof(*lock));
  focusclient(nullptr, 0);

  lock->scene = wlr_scene_tree_create(layers[LyrBlock]);
  cur_lock = lock->lock = session_lock;
  locked = 1;

  LISTEN(&session_lock->events.new_surface, &lock->new_surface,
         createlocksurface);
  LISTEN(&session_lock->events.destroy, &lock->destroy, destroysessionlock);
  LISTEN(&session_lock->events.unlock, &lock->unlock, unlocksession);

  wlr_session_lock_v1_send_locked(session_lock);
}

static void
apply_surface_corners(struct wlr_scene_node *node, int radius)
{
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
		if (buffer) {
			wlr_scene_buffer_set_corner_radii(buffer,
				corner_radii_all(radius > 0 ? radius : 0));
		}
	} else if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			apply_surface_corners(child, radius);
		}
	}
}

static void mapnotify(struct wl_listener *listener, void *data) {
  /* Called when the surface is mapped, or ready to display on-screen. */
  Client *p = nullptr;
  Client *w, *c = wl_container_of(listener, c, map);
  Monitor *m;

  /* Create scene tree for this client and its border */
  c->scene = client_surface(c)->data = wlr_scene_tree_create(layers[LyrTile]);
  /* Enabled later by a call to arrange() */
  wlr_scene_node_set_enabled(&c->scene->node, client_is_unmanaged(c));
  c->scene->node.data = c;

  client_get_geometry(c, &c->geom);

  /* Handle unmanaged clients first so we can return prior create borders */
  if (client_is_unmanaged(c)) {
    /* Unmanaged clients always are floating */
    wlr_scene_node_reparent(&c->scene->node, layers[LyrFloat]);
    wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
    client_set_size(c, c->geom.width, c->geom.height);
    if (client_wants_focus(c)) {
      focusclient(c, 1);
      exclusive_focus = c;
    }
    goto unset_fullscreen;
  }

  /* Create single border background rect */
  c->border_bg = wlr_scene_rect_create(c->scene, 0, 0,
      c->isurgent ? cfg.appearance.urgent_color : cfg.appearance.border_color);
  c->border_bg->node.data = c;

  {
    int r = config_get_corner_radius();
    c->corner_radius = r;
  }

  /* Create drop shadow */
  if (cfg.effects.windows.shadows.shadows && !client_is_unmanaged(c)) {
    float sc[4];
    parse_color_hex(cfg.effects.windows.shadows.shadow_color, sc);
    sc[3] *= cfg.effects.windows.shadows.shadow_opacity;
    sc[0] *= sc[3]; sc[1] *= sc[3]; sc[2] *= sc[3];
    int sr = c->corner_radius;
    int sw = MAX(1, (int)(c->geom.width - 2 * c->bw) +
                       2 * cfg.effects.windows.shadows.shadow_expand);
    int sh = MAX(1, (int)(c->geom.height - 2 * c->bw) +
                       2 * cfg.effects.windows.shadows.shadow_expand);
    c->shadow = wlr_scene_shadow_create(c->scene, sw, sh, sr,
        (float)cfg.effects.windows.shadows.shadow_radius, sc);
    if (c->shadow) {
      wlr_scene_node_set_position(&c->shadow->node,
          cfg.effects.windows.shadows.shadow_offset_x
              - cfg.effects.windows.shadows.shadow_expand,
          cfg.effects.windows.shadows.shadow_offset_y
              - cfg.effects.windows.shadows.shadow_expand);
      wlr_scene_node_place_below(&c->shadow->node, &c->border_bg->node);
    }
  }

  c->scene_surface =
      c->type == XDGShell
          ? wlr_scene_xdg_surface_create(c->scene, c->surface.xdg)
          : wlr_scene_subsurface_tree_create(c->scene, client_surface(c));
  c->scene_surface->node.data = c;

  wlr_scene_node_raise_to_top(&c->border_bg->node);

  /* Initialize client geometry with room for border */
  client_set_tiled(c, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT |
                          WLR_EDGE_RIGHT);
  c->geom.width += 2 * c->bw;
  c->geom.height += 2 * c->bw;

  /* Insert this client into client lists. */
  wl_list_insert(&clients, &c->link);
  wl_list_insert(&fstack, &c->flink);

  /* Set initial monitor, tags, floating status, and focus:
   * we always consider floating, clients that have parent and thus
   * we set the same tags and monitor as its parent.
   * If there is no parent, apply rules */
  if ((p = client_get_parent(c))) {
    c->isfloating = 1;
    setmon(c, p->mon, p->tags);
  } else {
    applyrules(c);
  }
  /* Auto-send to scratchpad if scratchpad is visible */
  if (c->mon && c->mon->scratchpad_visible && !c->isscratchpad &&
      !c->isfullscreen) {
    dwindle_remove_client(c);
    master_remove_client(c);
    c->isscratchpad = 1;
    c->isfloating = 1;
    setfloating(c, 1);
    /* Show it and hide previous scratchpad client */
    if (c->mon->scratchpad_current && c->mon->scratchpad_current != c) {
      wlr_scene_node_set_enabled(
          &c->mon->scratchpad_current->scene->node, 0);
      client_set_suspended(c->mon->scratchpad_current, 1);
    }
    scratchpad_arrange(c->mon);
    c->mon->scratchpad_current = c;
    focusclient(c, 1);
  }
  printstatus();
  ipc_socket_send_window_event("new");

unset_fullscreen:
  m = c->mon ? c->mon : xytomon(c->geom.x, c->geom.y);
  wl_list_for_each(w, &clients, link) {
    if (w != c && w != p && w->isfullscreen && m == w->mon &&
        (w->tags & c->tags))
      setfullscreen(w, 0);
  }
}

static void maximizenotify(struct wl_listener *listener, void *data) {
  /* This event is raised when a client would like to maximize itself,
   * typically because the user clicked on the maximize button on
   * client-side decorations. peachwm doesn't support maximization, but
   * to conform to xdg-shell protocol we still must send a configure.
   * Since xdg-shell protocol v5 we should ignore request of unsupported
   * capabilities, just schedule a empty configure when the client uses <5
   * protocol version
   * wlr_xdg_surface_schedule_configure() is used to send an empty reply. */
  Client *c = wl_container_of(listener, c, maximize);
  if (c->surface.xdg->initialized &&
      wl_resource_get_version(c->surface.xdg->toplevel->resource) <
          XDG_TOPLEVEL_STATE_TILED_BOTTOM_SINCE_VERSION)
    wlr_xdg_surface_schedule_configure(c->surface.xdg);
}

static void motionabsolute(struct wl_listener *listener, void *data) {
  /* This event is forwarded by the cursor when a pointer emits an _absolute_
   * motion event, from 0..1 on each axis. This happens, for example, when
   * wlroots is running under a Wayland window rather than KMS+DRM, and you
   * move the mouse over the window. You could enter the window from any edge,
   * so we have to warp the mouse there. Also, some hardware emits these events.
   */
  struct wlr_pointer_motion_absolute_event *event = data;
  double lx, ly, dx, dy;

  if (!event->time_msec) /* this is 0 with virtual pointers */
    wlr_cursor_warp_absolute(cursor, &event->pointer->base, event->x, event->y);

  wlr_cursor_absolute_to_layout_coords(cursor, &event->pointer->base, event->x,
                                       event->y, &lx, &ly);
  dx = lx - cursor->x;
  dy = ly - cursor->y;
  motionnotify(event->time_msec, &event->pointer->base, dx, dy, dx, dy);
}

static void motionnotify(uint32_t time, struct wlr_input_device *device, double dx,
                  double dy, double dx_unaccel, double dy_unaccel) {
  double sx = 0, sy = 0, sx_confined, sy_confined;
  Client *c = nullptr, *w = nullptr;
  LayerSurface *l = nullptr;
  struct wlr_surface *surface = nullptr;
  struct wlr_pointer_constraint_v1 *constraint;

  /* Find the client under the pointer and send the event along. */
  xytonode(cursor->x, cursor->y, &surface, &c, nullptr, &sx, &sy);

  if (cursor_mode == CurPressed && !seat->drag &&
      surface != seat->pointer_state.focused_surface &&
      toplevel_from_wlr_surface(seat->pointer_state.focused_surface, &w, &l) >=
          0) {
    c = w;
    surface = seat->pointer_state.focused_surface;
    sx = cursor->x - (l ? l->scene->node.x : w->geom.x);
    sy = cursor->y - (l ? l->scene->node.y : w->geom.y);
  }

  /* time is 0 in internal calls meant to restore pointer focus. */
  if (time) {
    wlr_relative_pointer_manager_v1_send_relative_motion(
        relative_pointer_mgr, seat, (uint64_t)time * 1000, dx, dy, dx_unaccel,
        dy_unaccel);

    wl_list_for_each(constraint, &pointer_constraints->constraints, link)
        cursorconstrain(constraint);

    if (active_constraint && cursor_mode != CurResize &&
        cursor_mode != CurMove) {
      (void)toplevel_from_wlr_surface(active_constraint->surface, &c, nullptr);
      if (c &&
          active_constraint->surface == seat->pointer_state.focused_surface) {
        sx = cursor->x - c->geom.x - c->bw;
        sy = cursor->y - c->geom.y - c->bw;
        if (wlr_region_confine(&active_constraint->region, sx, sy, sx + dx,
                               sy + dy, &sx_confined, &sy_confined)) {
          dx = sx_confined - sx;
          dy = sy_confined - sy;
        }

        if (active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
          return;
      }
    }

    wlr_cursor_move(cursor, device, dx, dy);
    wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

    /* Update selmon (even while dragging a window) */
    if (cfg.sloppyfocus)
      selmon = xytomon(cursor->x, cursor->y);
  }

  /* Update drag icon's position */
  wlr_scene_node_set_position(&drag_icon->node, (int)round(cursor->x),
                              (int)round(cursor->y));

  /* If we are currently grabbing the mouse, handle and return */
  if (cursor_mode == CurMove) {
    /* Move the grabbed client to the new position. */
    resize(grabc,
           (struct wlr_box){.x = (int)round(cursor->x) - grabcx,
                            .y = (int)round(cursor->y) - grabcy,
                            .width = grabc->geom.width,
                            .height = grabc->geom.height},
           1);
    return;
  } else if (cursor_mode == CurResize) {
    resize(grabc,
           (struct wlr_box){.x = grabc->geom.x,
                            .y = grabc->geom.y,
                            .width = (int)round(cursor->x) - grabc->geom.x,
                            .height = (int)round(cursor->y) - grabc->geom.y},
           1);
    return;
  }

  /* If there's no client surface under the cursor, set the cursor image to a
   * default. This is what makes the cursor image appear when you move it
   * off of a client or over its border. */
  if (!surface && !seat->drag)
    wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");

  pointerfocus(c, surface, sx, sy, time);
}

static void motionrelative(struct wl_listener *listener, void *data) {
  /* This event is forwarded by the cursor when a pointer emits a _relative_
   * pointer motion event (i.e. a delta) */
  struct wlr_pointer_motion_event *event = data;
  /* The cursor doesn't move unless we tell it to. The cursor automatically
   * handles constraining the motion to the output layout, as well as any
   * special configuration applied for the specific input device which
   * generated the event. You can pass NULL for the device if you want to move
   * the cursor around without any input. */
  motionnotify(event->time_msec, &event->pointer->base, event->delta_x,
               event->delta_y, event->unaccel_dx, event->unaccel_dy);
}

static void moveresize(const Arg *arg) {
  if (cursor_mode != CurNormal && cursor_mode != CurPressed)
    return;
  xytonode(cursor->x, cursor->y, nullptr, &grabc, nullptr, nullptr, nullptr);
  if (!grabc || client_is_unmanaged(grabc) || grabc->isfullscreen)
    return;

  if (arg->ui == CurMove && !grabc->isfloating &&
      curlayout(grabc->mon)->arrange) {
    cursor_mode = CurTileDrag;
    grabcx = (int)round(cursor->x) - grabc->geom.x;
    grabcy = (int)round(cursor->y) - grabc->geom.y;
    wlr_cursor_set_xcursor(cursor, cursor_mgr, "grabbing");
    return;
  }

  /* Float the window and tell motionnotify to grab it */
  setfloating(grabc, 1);
  switch (cursor_mode = arg->ui) {
  case CurMove:
    grabcx = (int)round(cursor->x) - grabc->geom.x;
    grabcy = (int)round(cursor->y) - grabc->geom.y;
    wlr_cursor_set_xcursor(cursor, cursor_mgr, "all-scroll");
    break;
  case CurResize:
    /* Doesn't work for X11 output - the next absolute motion event
     * returns the cursor to where it started */
    wlr_cursor_warp_closest(cursor, nullptr, grabc->geom.x + grabc->geom.width,
                            grabc->geom.y + grabc->geom.height);
    wlr_cursor_set_xcursor(cursor, cursor_mgr, "se-resize");
    break;
  }
}

static void outputmgrapply(struct wl_listener *listener, void *data) {
  struct wlr_output_configuration_v1 *config = data;
  outputmgrapplyortest(config, 0);
}

static void outputmgrapplyortest(struct wlr_output_configuration_v1 *config,
                          int test) {
  /*
   * Called when a client such as wlr-randr requests a change in output
   * configuration. This is only one way that the layout can be changed,
   * so any Monitor information should be updated by updatemons() after an
   * output_layout.change event, not here.
   */
  struct wlr_output_configuration_head_v1 *config_head;
  int ok = 1;

  wl_list_for_each(config_head, &config->heads, link) {
    struct wlr_output *wlr_output = config_head->state.output;
    Monitor *m = wlr_output->data;
    struct wlr_output_state state;

    /* Ensure displays previously disabled by wlr-output-power-management-v1
     * are properly handled*/
    m->asleep = 0;

    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, config_head->state.enabled);
    if (!config_head->state.enabled)
      goto apply_or_test;

    if (config_head->state.mode)
      wlr_output_state_set_mode(&state, config_head->state.mode);
    else
      wlr_output_state_set_custom_mode(&state,
                                       config_head->state.custom_mode.width,
                                       config_head->state.custom_mode.height,
                                       config_head->state.custom_mode.refresh);

    wlr_output_state_set_transform(&state, config_head->state.transform);
    wlr_output_state_set_scale(&state, config_head->state.scale);
    wlr_output_state_set_adaptive_sync_enabled(
        &state, config_head->state.adaptive_sync_enabled);

  apply_or_test:
    ok &= test ? wlr_output_test_state(wlr_output, &state)
               : wlr_output_commit_state(wlr_output, &state);

    /* Don't move monitors if position wouldn't change. This avoids
     * wlroots marking the output as manually configured.
     * wlr_output_layout_add does not like disabled outputs */
    if (!test && wlr_output->enabled &&
        (m->m.x != config_head->state.x || m->m.y != config_head->state.y))
      wlr_output_layout_add(output_layout, wlr_output, config_head->state.x,
                            config_head->state.y);

    wlr_output_state_finish(&state);
  }

  if (ok)
    wlr_output_configuration_v1_send_succeeded(config);
  else
    wlr_output_configuration_v1_send_failed(config);
  wlr_output_configuration_v1_destroy(config);

  updatemons(nullptr, nullptr);
}

static void outputmgrtest(struct wl_listener *listener, void *data) {
  struct wlr_output_configuration_v1 *config = data;
  outputmgrapplyortest(config, 1);
}

static void pointerfocus(Client *c, struct wlr_surface *surface, double sx, double sy,
                  uint32_t time) {
  struct timespec now;

  if (surface != seat->pointer_state.focused_surface && cfg.sloppyfocus &&
      time && c && !client_is_unmanaged(c)) {
    warp_focus = false;
    focusclient(c, 0);
    warp_focus = true;
  }

  /* If surface is NULL, clear pointer focus */
  if (!surface) {
    wlr_seat_pointer_notify_clear_focus(seat);
    return;
  }

  if (!time) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    time = now.tv_sec * 1000 + now.tv_nsec / 1000000;
  }

  /* Let the client know that the mouse cursor has entered one
   * of its surfaces, and make keyboard focus follow if desired.
   * wlroots makes this a no-op if surface is already focused */
  wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
  wlr_seat_pointer_notify_motion(seat, time, sx, sy);
}

void printstatus(void) {
  Monitor *m = nullptr;
  Client *c;
  uint32_t occ, urg, sel;

  wl_list_for_each(m, &mons, link) {
    occ = urg = 0;
    wl_list_for_each(c, &clients, link) {
      if (c->mon != m)
        continue;
      occ |= c->tags;
      if (c->isurgent)
        urg |= c->tags;
    }
    if ((c = focustop(m))) {
      printf("%s title %s\n", m->wlr_output->name, client_get_title(c));
      printf("%s appid %s\n", m->wlr_output->name, client_get_appid(c));
      printf("%s fullscreen %d\n", m->wlr_output->name, c->isfullscreen);
      printf("%s floating %d\n", m->wlr_output->name, c->isfloating);
      sel = c->tags;
    } else {
      printf("%s title \n", m->wlr_output->name);
      printf("%s appid \n", m->wlr_output->name);
      printf("%s fullscreen \n", m->wlr_output->name);
      printf("%s floating \n", m->wlr_output->name);
      sel = 0;
    }

    printf("%s selmon %u\n", m->wlr_output->name, m == selmon);
    printf("%s tags %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 "\n",
           m->wlr_output->name, occ, m->tagset[m->seltags], sel, urg);
    int _ti = current_tag_idx(m);
    printf("%s layout %s\n", m->wlr_output->name, m->cold->ltsymbol[_ti]);
    ext_workspace_printstatus(m);
  }
  fflush(stdout);
  ipc_printstatus();
  ipc_socket_send_workspace_event("focus");
}

static void powermgrsetmode(struct wl_listener *listener, void *data) {
  struct wlr_output_power_v1_set_mode_event *event = data;
  struct wlr_output_state state;
  wlr_output_state_init(&state);
  Monitor *m = event->output->data;

  if (!m)
    return;

  wlr_output_state_set_enabled(&state, event->mode);
  wlr_output_commit_state(m->wlr_output, &state);

  m->asleep = !event->mode;
  updatemons(nullptr, nullptr);
}

void quit(const Arg *arg) { wl_display_terminate(dpy); }

static void rendermon(struct wl_listener *listener, void *data) {
  /* This function is called every time an output is ready to display a frame,
   * generally at the output's refresh rate (e.g. 60Hz). */
  Monitor *m = wl_container_of(listener, m, frame);
  Client *c;
  struct wlr_output_state pending = {0};
  struct timespec now;

  /* Render if no XDG clients have an outstanding resize and are visible on
   * this monitor. */
  wl_list_for_each(c, &clients, link) {
    if (c->resize && !c->isfloating && client_is_rendered_on_mon(c, m) &&
        !client_is_stopped(c))
      goto skip;
  }

  /* Apply corner radius to all client surfaces on this monitor */
  {
    Client *c;
    wl_list_for_each(c, &clients, link) {
      if (c->mon == m && c->scene_surface && c->corner_radius > 0 && !c->isfullscreen) {
        apply_surface_corners(&c->scene_surface->node, c->corner_radius);
      }
    }
  }

  wlr_scene_output_commit(m->scene_output, nullptr);

skip:
  /* Let clients know a frame has been rendered */
  clock_gettime(CLOCK_MONOTONIC, &now);
  wlr_scene_output_send_frame_done(m->scene_output, &now);
  wlr_output_state_finish(&pending);
}

static void requestdecorationmode(struct wl_listener *listener, void *data) {
  Client *c = wl_container_of(listener, c, set_decoration_mode);
  if (c->surface.xdg->initialized)
    wlr_xdg_toplevel_decoration_v1_set_mode(
        c->decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void requeststartdrag(struct wl_listener *listener, void *data) {
  struct wlr_seat_request_start_drag_event *event = data;

  if (wlr_seat_validate_pointer_grab_serial(seat, event->origin, event->serial))
    wlr_seat_start_pointer_drag(seat, event->drag, event->serial);
  else
    wlr_data_source_destroy(event->drag->source);
}

static void requestmonstate(struct wl_listener *listener, void *data) {
  struct wlr_output_event_request_state *event = data;
  wlr_output_commit_state(event->output, event->state);
  updatemons(nullptr, nullptr);
}

void resize(Client *c, struct wlr_box geo, int interact) {
  struct wlr_box *bbox;

  if (!c->mon || !client_surface(c)->mapped)
    return;

  bbox = interact ? &sgeom : &c->mon->w;

  client_set_bounds(c, geo.width, geo.height);
  c->geom = geo;
  applybounds(c, bbox);

  /* Update scene-graph, including border */
  wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
  wlr_scene_node_set_position(&c->scene_surface->node, c->bw, c->bw);

  int effective_r = c->corner_radius;
  if (effective_r > 0) {
    if (!c->mon->gaps)
      effective_r = 0;
    else if (cfg.appearance.smart_gaps && !c->isfloating
        && c->geom.x == c->mon->w.x && c->geom.y == c->mon->w.y
        && c->geom.x + c->geom.width == c->mon->w.x + c->mon->w.width
        && c->geom.y + c->geom.height == c->mon->w.y + c->mon->w.height)
      effective_r = 0;
  }

  /* Update single border rect with clipped region hole */
  if (c->border_bg) {
    int bw_i = (int)c->bw;
    int cw = (int)c->geom.width;
    int ch = (int)c->geom.height;

    wlr_scene_rect_set_size(c->border_bg, cw, ch);
    wlr_scene_node_set_position(&c->border_bg->node, 0, 0);

    if (effective_r > 0 && !c->isfullscreen) {
      int r = effective_r;
      struct clipped_region cr = {
          .area = {bw_i, bw_i, cw - 2*bw_i, ch - 2*bw_i},
          .corners = corner_radii_all(r),
      };
      wlr_scene_rect_set_clipped_region(c->border_bg, cr);
      wlr_scene_rect_set_corner_radii(c->border_bg, corner_radii_all(r));
      wlr_scene_node_set_enabled(&c->border_bg->node, true);
      apply_surface_corners(&c->scene_surface->node, r);
    } else {
      struct clipped_region cr = {
          .area = {bw_i, bw_i, cw - 2*bw_i, ch - 2*bw_i},
          .corners = corner_radii_new(1, 1, 1, 1), /* non-zero to keep GLES2 shader clip active */
      };
      wlr_scene_rect_set_clipped_region(c->border_bg, cr);
      wlr_scene_rect_set_corner_radius(c->border_bg, 0);
      wlr_scene_node_set_enabled(&c->border_bg->node, true);
      apply_surface_corners(&c->scene_surface->node, 0);
    }
  }

  /* Update drop shadow */
  if (c->shadow) {
    bool shadow_visible = cfg.effects.windows.shadows.shadows
        && (cfg.effects.windows.shadows.fullscreen_shadows || !c->isfullscreen)
        && (!cfg.effects.windows.shadows.only_floating || c->isfloating);
    if (!cfg.effects.windows.shadows.nogaps_shadows && !c->mon->gaps)
      shadow_visible = false;
    else if (!cfg.effects.windows.shadows.nogaps_shadows
             && cfg.appearance.smart_gaps && !c->isfloating
             && c->geom.x == c->mon->w.x && c->geom.y == c->mon->w.y
             && c->geom.x + c->geom.width == c->mon->w.x + c->mon->w.width
             && c->geom.y + c->geom.height == c->mon->w.y + c->mon->w.height)
      shadow_visible = false;
    wlr_scene_node_set_enabled(&c->shadow->node, shadow_visible);

    if (shadow_visible) {
      int sw = MAX(1, (int)(c->geom.width - 2 * (int)c->bw) + 2 * cfg.effects.windows.shadows.shadow_expand);
      int sh = MAX(1, (int)(c->geom.height - 2 * (int)c->bw) + 2 * cfg.effects.windows.shadows.shadow_expand);
      wlr_scene_shadow_set_size(c->shadow, sw, sh);
      wlr_scene_node_set_position(&c->shadow->node,
          cfg.effects.windows.shadows.shadow_offset_x - cfg.effects.windows.shadows.shadow_expand + (int)c->bw,
          cfg.effects.windows.shadows.shadow_offset_y - cfg.effects.windows.shadows.shadow_expand + (int)c->bw);

      int effective_r_for_shadow = effective_r > 0 ? effective_r : c->corner_radius;
      wlr_scene_shadow_set_corner_radius(c->shadow, effective_r_for_shadow);
      wlr_scene_shadow_set_blur_sigma(c->shadow,
          (float)cfg.effects.windows.shadows.shadow_radius);

      float sc[4];
      parse_color_hex(cfg.effects.windows.shadows.shadow_color, sc);
      sc[3] *= cfg.effects.windows.shadows.shadow_opacity;
      sc[0] *= sc[3]; sc[1] *= sc[3]; sc[2] *= sc[3];
      wlr_scene_shadow_set_color(c->shadow, sc);

      if (cfg.effects.windows.shadows.shadow_clip) {
        int cr_val = effective_r_for_shadow;
        struct clipped_region cr = {
          .area = {
              cfg.effects.windows.shadows.shadow_expand - cfg.effects.windows.shadows.shadow_offset_x,
              cfg.effects.windows.shadows.shadow_expand - cfg.effects.windows.shadows.shadow_offset_y,
              (int)(c->geom.width - 2 * (int)c->bw),
              (int)(c->geom.height - 2 * (int)c->bw)
          },
          .corners = corner_radii_new(cr_val, cr_val, cr_val, cr_val),
        };
        wlr_scene_shadow_set_clipped_region(c->shadow, cr);
      }
    }
  }

  /* this is a no-op if size hasn't changed */
  c->resize =
      client_set_size(c, c->geom.width - 2 * c->bw, c->geom.height - 2 * c->bw);
}

static void run(char *startup_cmd) {
  /* Add a Unix socket to the Wayland display. */
  const char *socket = wl_display_add_socket_auto(dpy);
  if (!socket)
    die("startup: display_add_socket_auto");
  setenv("WAYLAND_DISPLAY", socket, 1);

  /* Start the backend. This will enumerate outputs and inputs, become the DRM
   * master, etc */
  if (!wlr_backend_start(backend))
    die("startup: backend_start");

  /* Now that the socket exists and the backend is started, run the startup
   * command */
  if (startup_cmd) {
    int piperw[2];
    if (pipe(piperw) < 0)
      die("startup: pipe:");
    if ((child_pid = fork()) < 0)
      die("startup: fork:");
    if (child_pid == 0) {
      setsid();
      dup2(piperw[0], STDIN_FILENO);
      close(piperw[0]);
      close(piperw[1]);
      execl("/bin/sh", "/bin/sh", "-c", startup_cmd, nullptr);
      die("startup: execl:");
    }
    dup2(piperw[1], STDOUT_FILENO);
    close(piperw[1]);
    close(piperw[0]);
  }

  /* Mark stdout as non-blocking as to avoid the startup script
   * causing peachwm to freeze when a user neither closes stdin
   * nor consumes standard input in their startup script */

  if (fd_set_nonblock(STDOUT_FILENO) < 0)
    close(STDOUT_FILENO);

  printstatus();

  /* Launch autostart commands now that the socket is live and the backend
   * is running. Commands run sequentially in a detached child process so
   * the compositor never blocks. */
  config_autostart_run(&cfg);

  /* At this point the outputs are initialized, choose initial selmon based on
   * cursor position, and set default cursor image */
  selmon = xytomon(cursor->x, cursor->y);

  wlr_cursor_warp_closest(cursor, nullptr, cursor->x, cursor->y);
  wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");

  /* Run the Wayland event loop. This does not return until you exit the
   * compositor. Starting the backend rigged up all of the necessary event
   * loop configuration to listen to libinput events, DRM events, generate
   * frame events at the refresh rate, and so on. */
  wl_display_run(dpy);
}

static void setcursor(struct wl_listener *listener, void *data) {
  /* This event is raised by the seat when a client provides a cursor image */
  struct wlr_seat_pointer_request_set_cursor_event *event = data;
  /* If we're "grabbing" the cursor, don't use the client's image, we will
   * restore it after "grabbing" sending a leave event, followed by a enter
   * event, which will result in the client requesting set the cursor surface */
  if (cursor_mode != CurNormal && cursor_mode != CurPressed)
    return;
  /* This can be sent by any client, so we check to make sure this one
   * actually has pointer focus first. If so, we can tell the cursor to
   * use the provided surface as the cursor image. It will set the
   * hardware cursor on the output that it's currently on and continue to
   * do so as the cursor moves between outputs. */
  if (event->seat_client == seat->pointer_state.focused_client)
    wlr_cursor_set_surface(cursor, event->surface, event->hotspot_x,
                           event->hotspot_y);
}

static void setcursorshape(struct wl_listener *listener, void *data) {
  struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
  if (cursor_mode != CurNormal && cursor_mode != CurPressed)
    return;
  /* This can be sent by any client, so we check to make sure this one
   * actually has pointer focus first. If so, we can tell the cursor to
   * use the provided cursor shape. */
  if (event->seat_client == seat->pointer_state.focused_client)
    wlr_cursor_set_xcursor(cursor, cursor_mgr,
                           wlr_cursor_shape_v1_name(event->shape));
}

void setfloating(Client *c, int floating) {
  Client *p = client_get_parent(c);
  c->isfloating = floating;
  /* If in floating layout do not change the client's layer */
  if (!c->mon || !client_surface(c)->mapped ||
      !curlayout(c->mon)->arrange)
    return;
  if (c->isscratchpad) {
    wlr_scene_node_reparent(&c->scene->node, layers[LyrScratch]);
  } else {
    wlr_scene_node_reparent(
        &c->scene->node, layers[c->isfullscreen || (p && p->isfullscreen) ? LyrFS
                                : c->isfloating ? LyrFloat
                                                : LyrTile]);
  }
  arrange(c->mon);
  printstatus();
}

static void setfullscreen(Client *c, int fullscreen) {
  c->isfullscreen = fullscreen;
  if (!c->mon || !client_surface(c)->mapped)
    return;
	c->bw = fullscreen ? 0 : cfg.appearance.border_px;
	if (fullscreen) {
		c->corner_radius = 0;
		if (c->border_bg) {
			wlr_scene_node_set_enabled(&c->border_bg->node, false);
		}
	} else {
		c->corner_radius = config_get_corner_radius();
		if (c->border_bg) {
			wlr_scene_node_set_enabled(&c->border_bg->node, true);
		}
	}
	client_set_fullscreen(c, fullscreen);
  if (c->isscratchpad) {
    wlr_scene_node_reparent(&c->scene->node, layers[LyrScratch]);
  } else {
    wlr_scene_node_reparent(&c->scene->node, layers[c->isfullscreen ? LyrFS
                                                    : c->isfloating ? LyrFloat
                                                                    : LyrTile]);
  }

  if (fullscreen) {
    c->prev = c->geom;
    resize(c, c->mon->m, 0);
  } else {
    /* restore previous size instead of arrange for floating windows since
     * client positions are set by the user and cannot be recalculated */
    resize(c, c->prev, 0);
  }
  arrange(c->mon);
  printstatus();
}

static void setlayout(const Arg *arg) {
  if (!selmon)
    return;
  ensure_cold(selmon);
  int ti = current_tag_idx(selmon);
  /* arg->ui is an index into layouts[]. If out of range, just toggle. */
  if (arg && arg->ui < layout_count) {
    if (selmon->cold->lt[ti][selmon->cold->sellt[ti]] != &layouts[arg->ui])
      selmon->cold->sellt[ti] ^= 1;
    selmon->cold->lt[ti][selmon->cold->sellt[ti]] = &layouts[arg->ui];
  } else {
    selmon->cold->sellt[ti] ^= 1;
  }
  strncpy(selmon->cold->ltsymbol[ti], selmon->cold->lt[ti][selmon->cold->sellt[ti]]->symbol,
          LENGTH(selmon->cold->ltsymbol[ti]));
  selmon->cold->ltsymbol[ti][LENGTH(selmon->cold->ltsymbol[ti]) - 1] = '\0';

  /* Convert windows when switching to master: set focused client as master */
  if (curlayout(selmon)->arrange == master) {
    Client *sel = focustop(selmon);
    if (sel && VISIBLEON(sel, selmon) && !sel->isfloating && !sel->isfullscreen)
      selmon->cold->master_master[ti] = sel;
    else
      selmon->cold->master_master[ti] = nullptr;
    /* Convert to floating: float all tiled clients */
  } else if (!curlayout(selmon)->arrange) {
    Client *c;
    wl_list_for_each(c, &clients, link) {
      if (VISIBLEON(c, selmon) && !c->isfloating && !c->isfullscreen)
        c->isfloating = 1;
    }
  }

  arrange(selmon);
  printstatus();
}

/* arg > 1.0 will set mfact absolutely */

static void setmon(Client *c, Monitor *m, uint32_t newtags) {
  Monitor *oldmon = c->mon;

  if (oldmon == m)
    return;
  c->mon = m;
  c->prev = c->geom;

  /* Scene graph sends surface leave/enter events on move and resize */
  if (oldmon)
    arrange(oldmon);
  if (m) {
    /* Make sure window actually overlaps with the monitor */
    resize(c, c->geom, 0);
    c->tags = newtags
                  ? newtags
                  : m->tagset[m->seltags]; /* assign tags of target monitor */
    setfullscreen(c, c->isfullscreen);     /* This will call arrange(c->mon) */
    setfloating(c, c->isfloating);
  }
  focusclient(focustop(selmon), 1);
}

static void setpsel(struct wl_listener *listener, void *data) {
  /* This event is raised by the seat when a client wants to set the selection,
   * usually when the user copies something. wlroots allows compositors to
   * ignore such requests if they so choose, but we always honor them
   */
  struct wlr_seat_request_set_primary_selection_event *event = data;
  wlr_seat_set_primary_selection(seat, event->source, event->serial);
}

static void setsel(struct wl_listener *listener, void *data) {
  /* This event is raised by the seat when a client wants to set the selection,
   * usually when the user copies something. wlroots allows compositors to
   * ignore such requests if they so choose, but we always honor them
   */
  struct wlr_seat_request_set_selection_event *event = data;
  wlr_seat_set_selection(seat, event->source, event->serial);
}

// ── Config Reload ───────────────────────────────────────────

/* Re-apply input settings from newcfg to all connected input devices */
static void reapply_input_config(const Config *newcfg) {
	PointerDevice *pdev;
	struct libinput_device *libinput_device;

	wlr_keyboard_set_repeat_info(&kb_group->wlr_group->keyboard,
	                             newcfg->input.repeat_rate,
	                             newcfg->input.repeat_delay);

	wl_list_for_each(pdev, &pointer_devices, link) {
		if (!wlr_input_device_is_libinput(pdev->device))
			continue;
		libinput_device = wlr_libinput_get_device_handle(pdev->device);
		if (!libinput_device)
			continue;

		if (libinput_device_config_tap_get_finger_count(libinput_device)) {
			libinput_device_config_tap_set_enabled(libinput_device,
				newcfg->input.tap_to_click);
			libinput_device_config_tap_set_drag_enabled(libinput_device,
				newcfg->input.tap_and_drag);
			libinput_device_config_tap_set_drag_lock_enabled(libinput_device,
				newcfg->input.drag_lock);
			libinput_device_config_tap_set_button_map(libinput_device,
				newcfg->input.tap_button_map);
		}

		if (libinput_device_config_scroll_has_natural_scroll(libinput_device))
			libinput_device_config_scroll_set_natural_scroll_enabled(
				libinput_device, newcfg->input.natural_scrolling);

		if (libinput_device_config_dwt_is_available(libinput_device))
			libinput_device_config_dwt_set_enabled(libinput_device,
				newcfg->input.disable_while_typing);

		if (libinput_device_config_left_handed_is_available(libinput_device))
			libinput_device_config_left_handed_set(libinput_device,
				newcfg->input.left_handed);

		if (libinput_device_config_middle_emulation_is_available(libinput_device))
			libinput_device_config_middle_emulation_set_enabled(libinput_device,
				newcfg->input.middle_button_emulation);

		if (libinput_device_config_scroll_get_methods(libinput_device) !=
		    LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
			libinput_device_config_scroll_set_method(libinput_device,
				newcfg->input.scroll_method);

		if (libinput_device_config_click_get_methods(libinput_device) !=
		    LIBINPUT_CONFIG_CLICK_METHOD_NONE)
			libinput_device_config_click_set_method(libinput_device,
				newcfg->input.click_method);

		if (libinput_device_config_send_events_get_modes(libinput_device))
			libinput_device_config_send_events_set_mode(libinput_device,
				newcfg->input.send_events_mode);

		if (libinput_device_config_accel_is_available(libinput_device)) {
			libinput_device_config_accel_set_profile(libinput_device,
				newcfg->input.accel_profile);
			libinput_device_config_accel_set_speed(libinput_device,
				newcfg->input.accel_speed);
		}
	}
}

/* Re-apply monitor configuration from global cfg to existing monitors */

static void
reapply_monitor_config(void)
{
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		for (int ri = 0; ri < cfg.nmonitors; ri++) {
			const CfgMonitorRule *r = &cfg.monitors[ri];
			if (!r->name[0] || strstr(m->wlr_output->name, r->name)) {
		ensure_cold(m);
		m->cold->mfact = r->mfact;
		m->cold->nmaster = r->nmaster;
		/* Map layout name */
		const Layout *mon_layout = &layouts[0];
		for (int li = 0; li < (int)layout_count; li++) {
			if (layouts[li].symbol && !strcmp(r->layout, layouts[li].symbol)) {
				mon_layout = &layouts[li];
				break;
			}
			if (!strcmp(r->layout, "dwindle") && layouts[li].arrange == dwindle) {
				mon_layout = &layouts[li];
				break;
			}
			if (!strcmp(r->layout, "master") && layouts[li].arrange == master) {
				mon_layout = &layouts[li];
				break;
			}
			if (!strcmp(r->layout, "monocle") && layouts[li].arrange == monocle) {
				mon_layout = &layouts[li];
				break;
			}
		}
		const Layout *alt_layout = &layouts[layout_count > 1 && mon_layout != &layouts[1]];
		for (int ti = 0; ti < TAGCOUNT; ti++) {
			m->cold->lt[ti][0] = mon_layout;
			m->cold->lt[ti][1] = alt_layout;
			m->cold->sellt[ti] = 0;
		}
				/* Apply scale and transform */
				struct wlr_output_state state;
				wlr_output_state_init(&state);
				wlr_output_state_set_scale(&state, r->scale);
				wlr_output_state_set_transform(&state, r->transform);
				wlr_output_commit_state(m->wlr_output, &state);
				wlr_output_state_finish(&state);
				/* Update position */
				if (r->x != -1 && r->y != -1)
					wlr_output_layout_add(output_layout, m->wlr_output, r->x, r->y);
				break;
			}
		}
	}
}

/* Re-apply appearance settings to root background, fullscreen backgrounds,
 * and all client borders */

static void
reapply_client_appearance(void)
{
	Client *c;
	Monitor *m;
	wlr_scene_rect_set_color(root_bg, cfg.appearance.root_color);
	wl_list_for_each(m, &mons, link) {
		wlr_scene_rect_set_color(m->fullscreen_bg, cfg.appearance.fullscreen_bg);
	}
	wl_list_for_each(c, &clients, link) {
		c->bw = !c->isfullscreen ? cfg.appearance.border_px : 0;
		if (!client_is_unmanaged(c)) {
		int r = c->isfullscreen ? 0 : config_get_corner_radius();
		c->corner_radius = r;
		if (c->border_bg) {
			if (r > 0 && !c->isfullscreen) {
				wlr_scene_rect_set_corner_radii(c->border_bg, corner_radii_all(r));
				wlr_scene_node_set_enabled(&c->border_bg->node, true);
			} else if (c->isfullscreen) {
				wlr_scene_node_set_enabled(&c->border_bg->node, false);
			} else {
				wlr_scene_rect_set_corner_radius(c->border_bg, 0);
				wlr_scene_node_set_enabled(&c->border_bg->node, true);
			}
		}
			/* Shadow re-apply on config reload */
			if (cfg.effects.windows.shadows.shadows) {
				if (!c->shadow) {
					float sc[4];
					parse_color_hex(cfg.effects.windows.shadows.shadow_color, sc);
					sc[3] *= cfg.effects.windows.shadows.shadow_opacity;
					sc[0] *= sc[3]; sc[1] *= sc[3]; sc[2] *= sc[3];
					int sr = c->corner_radius;
					int sw = MAX(1, (int)(c->geom.width - 2 * (int)c->bw)
					    + 2 * cfg.effects.windows.shadows.shadow_expand);
					int sh = MAX(1, (int)(c->geom.height - 2 * (int)c->bw)
					    + 2 * cfg.effects.windows.shadows.shadow_expand);
					c->shadow = wlr_scene_shadow_create(c->scene, sw, sh, sr,
					    (float)cfg.effects.windows.shadows.shadow_radius, sc);
					if (c->shadow) {
						wlr_scene_node_set_position(&c->shadow->node,
						    cfg.effects.windows.shadows.shadow_offset_x
						        - cfg.effects.windows.shadows.shadow_expand,
						    cfg.effects.windows.shadows.shadow_offset_y
						        - cfg.effects.windows.shadows.shadow_expand);
					wlr_scene_node_place_below(&c->shadow->node,
					    &c->border_bg->node);
					}
				}
			} else {
				if (c->shadow) {
					wlr_scene_node_destroy(&c->shadow->node);
					c->shadow = NULL;
				}
			}
		}
		if (!client_is_unmanaged(c)) {
			float *color = c->isurgent ? cfg.appearance.urgent_color
			             : c == focustop(c->mon) ? cfg.appearance.focus_color
			                                     : cfg.appearance.border_color;
			client_set_border_color(c, color);
		}
	}
}

/* Re-apply window rules from global cfg to all existing clients */

static void
reapply_client_rules(void)
{
	Client *c;
	wl_list_for_each(c, &clients, link) {
		if (client_is_unmanaged(c))
			continue;
		const char *appid = client_get_appid(c);
		const char *title = client_get_title(c);
		for (int ri = 0; ri < cfg.nrules; ri++) {
			const CfgRule *r = &cfg.rules[ri];
			if ((!r->title[0] || strstr(title, r->title)) &&
			    (!r->app_id[0] || strstr(appid, r->app_id))) {
				c->isfloating = r->floating | client_is_float_type(c);
				if (r->tags) {
					dwindle_remove_client(c);
					master_remove_client(c);
					c->tags = r->tags;
				}
				break;
			}
		}
	}
}

/* Config hot reload callback is fired by the inotify watcher */

static void
on_config_reload(const Config *newcfg, void *ud)
{
	Monitor *m;
	(void)ud;
	reapply_input_config(newcfg);
	cfg = *newcfg;
	reapply_monitor_config();
	reapply_client_appearance();
	reapply_client_rules();
	wl_list_for_each(m, &mons, link) {
		for (int ti = 0; ti < TAGCOUNT; ti++)
			apply_workspace_layout(m, ti);
		arrange(m);
	}
	printstatus();
}

/* Triggered by the IPC 'reload' command - re-reads config from disk */

void
do_reload(void)
{
	Config fresh;
	if (config_load(config_path, &fresh) == 0) {
		fprintf(stderr, "peachwm: config reloaded via IPC\n");
		reapply_input_config(&fresh);
		cfg = fresh;
		reapply_monitor_config();
		reapply_client_appearance();
		reapply_client_rules();
		Monitor *m;
		wl_list_for_each(m, &mons, link) {
			for (int ti = 0; ti < TAGCOUNT; ti++)
				apply_workspace_layout(m, ti);
			arrange(m);
		}
		printstatus();
	} else {
		fprintf(stderr, "peachwm: IPC config reload failed\n");
	}
}

static void setup(void) {
  int drm_fd, i, sig[] = {SIGCHLD, SIGINT, SIGTERM, SIGPIPE};
  struct sigaction sa = {.sa_flags = SA_RESTART, .sa_handler = handlesig};
  sigemptyset(&sa.sa_mask);

  for (i = 0; i < (int)LENGTH(sig); i++)
    sigaction(sig[i], &sa, nullptr);

  /* Load config before anything else so all settings are ready */
  {
    char cfgpath[1024];
    const char *path;
    int debug_override =
        (cfg.log_level == WLR_DEBUG); /* set by -d flag in main() */
    if (custom_cfg_path)
      path = custom_cfg_path;
    else {
      config_get_path(cfgpath, sizeof(cfgpath));
      path = cfgpath;
    }
    if (config_load(path, &cfg) < 0)
      die("failed to load config from '%s'", path);
    if (debug_override)
      cfg.log_level = WLR_DEBUG;
    strncpy(config_path, path, sizeof(config_path) - 1);
    config_path[sizeof(config_path) - 1] = '\0';
  }

  wlr_log_init(cfg.log_level, nullptr);

  /* The Wayland display is managed by libwayland. It handles accepting
   * clients from the Unix socket, managing Wayland globals, and so on and so
   * forth */
  dpy = wl_display_create();
  event_loop = wl_display_get_event_loop(dpy);
  ipc_init();
  ipc_socket_init();
  workspaces_init();

  /* Start the inotify config watcher */

  {
    char cfgpath[1024];
    const char *path = custom_cfg_path;
    if (!path) {
      config_get_path(cfgpath, sizeof(cfgpath));
      path = cfgpath;
    }
    cfg_watch_src =
        config_watch_start(event_loop, path, on_config_reload, nullptr);
  }

  /* The backend is a wlroots feature which abstracts the underlying input and
   * output hardware. The autocreate option will choose the most suitable
   * backend based on the current environment, such as opening an X11 window
   * if an X11 server is running. */
  if (!(backend = wlr_backend_autocreate(event_loop, &session)))
    die("couldn't create backend");

  /* Initialize the scene graph used to lay out windows */
  scene = wlr_scene_create();
  root_bg =
      wlr_scene_rect_create(&scene->tree, 0, 0, cfg.appearance.root_color);
  for (i = 0; i < NUM_LAYERS; i++)
    layers[i] = wlr_scene_tree_create(&scene->tree);
  drag_icon = wlr_scene_tree_create(&scene->tree);
  wlr_scene_node_place_below(&drag_icon->node, &layers[LyrBlock]->node);

  /* Autocreates a renderer, either Pixman, GLES2 or Vulkan for us. The user
   * can also specify a renderer using the WLR_RENDERER env var.
   * The renderer is responsible for defining the various pixel formats it
   * supports for shared memory, this configures that for clients. */
  if (!(drw = fx_renderer_create(backend)))
    die("couldn't create renderer");
  wl_signal_add(&drw->events.lost, &gpu_reset);

  /* Create shm, drm and linux_dmabuf interfaces by ourselves.
   * The simplest way is to call:
   *      wlr_renderer_init_wl_display(drw);
   * but we need to create the linux_dmabuf interface manually to integrate it
   * with wlr_scene. */
  wlr_renderer_init_wl_shm(drw, dpy);

  if (wlr_renderer_get_texture_formats(drw, WLR_BUFFER_CAP_DMABUF)) {
    wlr_drm_create(dpy, drw);
    wlr_scene_set_linux_dmabuf_v1(
        scene, wlr_linux_dmabuf_v1_create_with_renderer(dpy, 5, drw));
  }

  if ((drm_fd = wlr_renderer_get_drm_fd(drw)) >= 0 && drw->features.timeline &&
      backend->features.timeline)
    wlr_linux_drm_syncobj_manager_v1_create(dpy, 1, drm_fd);

  /* Autocreates an allocator for us.
   * The allocator is the bridge between the renderer and the backend. It
   * handles the buffer creation, allowing wlroots to render onto the
   * screen */
  if (!(alloc = wlr_allocator_autocreate(backend, drw)))
    die("couldn't create allocator");

  /* This creates some hands-off wlroots interfaces. The compositor is
   * necessary for clients to allocate surfaces and the data device manager
   * handles the clipboard. Each of these wlroots interfaces has room for you
   * to dig your fingers in and play with their behavior if you want. Note that
   * the clients cannot set the selection directly without compositor approval,
   * see the setsel() function. */
  compositor = wlr_compositor_create(dpy, 6, drw);
  wlr_subcompositor_create(dpy);
  wlr_data_device_manager_create(dpy);
  wlr_export_dmabuf_manager_v1_create(dpy);
  wlr_screencopy_manager_v1_create(dpy);
  wlr_data_control_manager_v1_create(dpy);
  wlr_ext_data_control_manager_v1_create(dpy, 1);
  wlr_primary_selection_v1_device_manager_create(dpy);
  wlr_viewporter_create(dpy);
  wlr_single_pixel_buffer_manager_v1_create(dpy);
  wlr_fractional_scale_manager_v1_create(dpy, 1);
  wlr_presentation_create(dpy, backend, 2);
  wlr_alpha_modifier_v1_create(dpy);

  /* Initializes the interface used to implement urgency hints */
  activation = wlr_xdg_activation_v1_create(dpy);
  wl_signal_add(&activation->events.request_activate, &request_activate);

  wlr_scene_set_gamma_control_manager_v1(
      scene, wlr_gamma_control_manager_v1_create(dpy));

  power_mgr = wlr_output_power_manager_v1_create(dpy);
  wl_signal_add(&power_mgr->events.set_mode, &output_power_mgr_set_mode);

  /* Creates an output layout, which is a wlroots utility for working with an
   * arrangement of screens in a physical layout. */
  output_layout = wlr_output_layout_create(dpy);
  wl_signal_add(&output_layout->events.change, &layout_change);

  wlr_xdg_output_manager_v1_create(dpy, output_layout);

  /* Configure a listener to be notified when new outputs are available on the
   * backend. */
  wl_list_init(&mons);
  wl_list_init(&pointer_devices);
  wl_signal_add(&backend->events.new_output, &new_output);

  /* Set up our client lists, the xdg-shell and the layer-shell. The xdg-shell
   * is a Wayland protocol which is used for application windows. For more
   * detail on shells, refer to the article:
   *
   * https://drewdevault.com/2018/07/29/Wayland-shells.html
   */
  wl_list_init(&clients);
  wl_list_init(&fstack);

  xdg_shell = wlr_xdg_shell_create(dpy, 6);
  wl_signal_add(&xdg_shell->events.new_toplevel, &new_xdg_toplevel);
  wl_signal_add(&xdg_shell->events.new_popup, &new_xdg_popup);

  layer_shell = wlr_layer_shell_v1_create(dpy, 3);
  wl_signal_add(&layer_shell->events.new_surface, &new_layer_surface);

  idle_notifier = wlr_idle_notifier_v1_create(dpy);

  idle_inhibit_mgr = wlr_idle_inhibit_v1_create(dpy);
  wl_signal_add(&idle_inhibit_mgr->events.new_inhibitor, &new_idle_inhibitor);

  session_lock_mgr = wlr_session_lock_manager_v1_create(dpy);
  wl_signal_add(&session_lock_mgr->events.new_lock, &new_session_lock);
  locked_bg = wlr_scene_rect_create(layers[LyrBlock], sgeom.width, sgeom.height,
                                    (float[4]){0.1f, 0.1f, 0.1f, 1.0f});
  wlr_scene_node_set_enabled(&locked_bg->node, 0);

  /* Use decoration protocols to negotiate server-side decorations */
  wlr_server_decoration_manager_set_default_mode(
      wlr_server_decoration_manager_create(dpy),
      WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
  xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(dpy);
  wl_signal_add(&xdg_decoration_mgr->events.new_toplevel_decoration,
                &new_xdg_decoration);

  pointer_constraints = wlr_pointer_constraints_v1_create(dpy);
  wl_signal_add(&pointer_constraints->events.new_constraint,
                &new_pointer_constraint);

  relative_pointer_mgr = wlr_relative_pointer_manager_v1_create(dpy);

  /*
   * Creates a cursor, which is a wlroots utility for tracking the cursor
   * image shown on screen.
   */
  cursor = wlr_cursor_create();
  wlr_cursor_attach_output_layout(cursor, output_layout);

  /* Creates an xcursor manager, another wlroots utility which loads up
   * Xcursor themes to source cursor images from and makes sure that cursor
   * images are available at all scale factors on the screen (necessary for
   * HiDPI support). Scaled cursors will be loaded with each output. */
  cursor_mgr = wlr_xcursor_manager_create(nullptr, 24);
  setenv("XCURSOR_SIZE", "24", 1);

  /*
   * wlr_cursor *only* displays an image on screen. It does not move around
   * when the pointer moves. However, we can attach input devices to it, and
   * it will generate aggregate events for all of them. In these events, we
   * can choose how we want to process them, forwarding them to clients and
   * moving the cursor around. More detail on this process is described in
   * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html
   *
   * And more comments are sprinkled throughout the notify functions above.
   */
  wl_signal_add(&cursor->events.motion, &cursor_motion);
  wl_signal_add(&cursor->events.motion_absolute, &cursor_motion_absolute);
  wl_signal_add(&cursor->events.button, &cursor_button);
  wl_signal_add(&cursor->events.axis, &cursor_axis);
  wl_signal_add(&cursor->events.frame, &cursor_frame);

  cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(dpy, 1);
  wl_signal_add(&cursor_shape_mgr->events.request_set_shape,
                &request_set_cursor_shape);

  /*
   * Configures a seat, which is a single "seat" at which a user sits and
   * operates the computer. This conceptually includes up to one keyboard,
   * pointer, touch, and drawing tablet device. We also rig up a listener to
   * let us know when new input devices are available on the backend.
   */
  wl_signal_add(&backend->events.new_input, &new_input_device);
  virtual_keyboard_mgr = wlr_virtual_keyboard_manager_v1_create(dpy);
  wl_signal_add(&virtual_keyboard_mgr->events.new_virtual_keyboard,
                &new_virtual_keyboard);
  virtual_pointer_mgr = wlr_virtual_pointer_manager_v1_create(dpy);
  wl_signal_add(&virtual_pointer_mgr->events.new_virtual_pointer,
                &new_virtual_pointer);

  seat = wlr_seat_create(dpy, "seat0");
  wl_signal_add(&seat->events.request_set_cursor, &request_cursor);
  wl_signal_add(&seat->events.request_set_selection, &request_set_sel);
  wl_signal_add(&seat->events.request_set_primary_selection, &request_set_psel);
  wl_signal_add(&seat->events.request_start_drag, &request_start_drag);
  wl_signal_add(&seat->events.start_drag, &start_drag);

  kb_group = createkeyboardgroup();
  wl_list_init(&kb_group->destroy.link);

  output_mgr = wlr_output_manager_v1_create(dpy);
  wl_signal_add(&output_mgr->events.apply, &output_mgr_apply);
  wl_signal_add(&output_mgr->events.test, &output_mgr_test);

  /* Make sure XWayland clients don't connect to the parent X server,
   * e.g when running in the x11 backend or the wayland backend and the
   * compositor has Xwayland support */
  unsetenv("DISPLAY");
#ifdef XWAYLAND
  /*
   * Initialise the XWayland X server.
   * It will be started when the first X client is started.
   */
  if ((xwayland = wlr_xwayland_create(dpy, compositor, 1))) {
    wl_signal_add(&xwayland->events.ready, &xwayland_ready);
    wl_signal_add(&xwayland->events.new_surface, &new_xwayland_surface);

    setenv("DISPLAY", xwayland->display_name, 1);
  } else {
    fprintf(stderr,
            "failed to setup XWayland X server, continuing without it\n");
  }
#endif
}

static void spawn(const Arg *arg) {
  if (fork() == 0) {
    dup2(STDERR_FILENO, STDOUT_FILENO);
    setsid();
    execvp(((char **)arg->v)[0], (char **)arg->v);
    die("peachwm: execvp %s failed:", ((char **)arg->v)[0]);
  }
}

static void startdrag(struct wl_listener *listener, void *data) {
  struct wlr_drag *drag = data;
  if (!drag->icon)
    return;

  drag->icon->data = &wlr_scene_drag_icon_create(drag_icon, drag->icon)->node;
  LISTEN_STATIC(&drag->icon->events.destroy, destroydragicon);
}

void tag(const Arg *arg) {
  Client *sel = focustop(selmon);
  if (!sel || (arg->ui & TAGMASK) == 0 || sel->isscratchpad)
    return;

  /* Remove from the source tag's layout trees */
  dwindle_remove_client(sel);
  master_remove_client(sel);
  sel->tags = arg->ui & TAGMASK;
  focusclient(focustop(selmon), 1);
  arrange(selmon);
  printstatus();
}

static void tagmon(const Arg *arg) {
  Client *sel = focustop(selmon);
  if (sel)
    setmon(sel, dirtomon(arg->i), 0);
}

void togglefloating(const Arg *arg) {
  Client *sel = focustop(selmon);
  /* return if fullscreen or scratchpad (strictly floating) */
  if (sel && !sel->isfullscreen && !sel->isscratchpad)
    setfloating(sel, !sel->isfloating);
}

void togglefullscreen(const Arg *arg) {
  Client *sel = focustop(selmon);
  if (sel)
    setfullscreen(sel, !sel->isfullscreen);
}

static void togglegaps(const Arg *arg) {
  selmon->gaps = !selmon->gaps;
  arrange(selmon);
}



/* swapdir is essentially focusdir but swap */

static void swapdir(const Arg *arg) {
  Client *c, *other = nullptr;
  Client *sel = focustop(selmon);
  int sx, sy, cx, cy;
  double bestdist = 1e18, dist;

  if (!sel || sel->isfloating || sel->isfullscreen)
    return;

  client_center(sel, &sx, &sy);

  wl_list_for_each(c, &clients, link) {
    if (c == sel || !VISIBLEON(c, selmon) || c->isfloating || c->isfullscreen)
      continue;

    client_center(c, &cx, &cy);

    switch (arg->i) {
    case WLR_DIRECTION_LEFT:
      if (cx >= sx)
        continue;
      break;
    case WLR_DIRECTION_RIGHT:
      if (cx <= sx)
        continue;
      break;
    case WLR_DIRECTION_UP:
      if (cy >= sy)
        continue;
      break;
    case WLR_DIRECTION_DOWN:
      if (cy <= sy)
        continue;
      break;
    default:
      return;
    }

    dist = (double)(cx - sx) * (cx - sx) + (double)(cy - sy) * (cy - sy);
    if (dist < bestdist) {
      bestdist = dist;
      other = c;
    }
  }

  if (!other)
    return;

  if (curlayout(selmon)->arrange == dwindle) {
    int ti = current_tag_idx(selmon);
    DwindleNode **root = &selmon->cold->dwindle_root[ti];
    if (*root) {
      DwindleNode *leaf_sel = dwindle_find_leaf(*root, sel);
      DwindleNode *leaf_other = dwindle_find_leaf(*root, other);
      if (leaf_sel && leaf_other) {
        leaf_sel->client = other;
        leaf_other->client = sel;
        int e = (selmon->gaps && cfg.appearance.gaps) ? (int)cfg.appearance.gaps
                                                      : 0;
        (*root)->box = (struct wlr_box){
            selmon->w.x + e,
            selmon->w.y + e,
            MAX(1, selmon->w.width - 2 * e),
            MAX(1, selmon->w.height - 2 * e),
        };
        dwindle_recalc(*root, e);
      }
    }
  } else if (curlayout(selmon)->arrange == master) {
    int ti = current_tag_idx(selmon);
    int side = selmon->cold->master_side[ti];

    if (sel == selmon->cold->master_master[ti]) {
      /* master moving away from master side → flip */
      if ((side == 0 && arg->i == WLR_DIRECTION_RIGHT) ||
          (side == 1 && arg->i == WLR_DIRECTION_LEFT)) {
        selmon->cold->master_side[ti] = !side;
        arrange(selmon);
        printstatus();
        focusclient(sel, 1);
        return;
      }
    } else {
      /* stack moving toward master side → promote */
      if ((side == 0 && arg->i == WLR_DIRECTION_LEFT) ||
          (side == 1 && arg->i == WLR_DIRECTION_RIGHT)) {
        selmon->cold->master_master[ti] = sel;
        arrange(selmon);
        printstatus();
        focusclient(sel, 1);
        return;
      }
    }
    /* fall through to geometry swap for other directions */
    struct wlr_box tmp = sel->geom;
    resize(sel, other->geom, 0);
    resize(other, tmp, 0);
  } else {
    struct wlr_box tmp = sel->geom;
    resize(sel, other->geom, 0);
    resize(other, tmp, 0);
  }

  focusclient(sel, 1);
  printstatus();
}

static void toggletag(const Arg *arg) {
  uint32_t newtags;
  Client *sel = focustop(selmon);
  if (!sel || sel->isscratchpad || !(newtags = sel->tags ^ (arg->ui & TAGMASK)))
    return;

  dwindle_remove_client(sel);
  master_remove_client(sel);
  sel->tags = newtags;
  focusclient(focustop(selmon), 1);
  arrange(selmon);
  printstatus();
}

void toggleview(const Arg *arg) {
  uint32_t newtagset;
  if (!(newtagset =
            selmon ? selmon->tagset[selmon->seltags] ^ (arg->ui & TAGMASK) : 0))
    return;

  selmon->tagset[selmon->seltags] = newtagset;
  focusclient(focustop(selmon), 1);
  arrange(selmon);
  printstatus();
}

static void unlocksession(struct wl_listener *listener, void *data) {
  SessionLock *lock = wl_container_of(listener, lock, unlock);
  destroylock(lock, 1);
}

static void unmaplayersurfacenotify(struct wl_listener *listener, void *data) {
  LayerSurface *l = wl_container_of(listener, l, unmap);

  l->mapped = 0;
  wlr_scene_node_set_enabled(&l->scene->node, 0);
  if (l == exclusive_focus)
    exclusive_focus = nullptr;
  if (l->layer_surface->output && (l->mon = l->layer_surface->output->data))
    arrangelayers(l->mon);
  if (l->layer_surface->surface == seat->keyboard_state.focused_surface)
    focusclient(focustop(selmon), 1);
  motionnotify(0, nullptr, 0, 0, 0, 0);
}

static void unmapnotify(struct wl_listener *listener, void *data) {
  /* Called when the surface is unmapped, and should no longer be shown. */
  Client *c = wl_container_of(listener, c, unmap);
  if (c == grabc) {
    cursor_mode = CurNormal;
    grabc = nullptr;
  }

  if (client_is_unmanaged(c)) {
    if (c == exclusive_focus) {
      exclusive_focus = nullptr;
      focusclient(focustop(selmon), 1);
    }
  } else {
    Monitor *oldmon = c->mon;
    wl_list_remove(&c->link);
    dwindle_remove_client(c);
    master_remove_client(c);
    wl_list_remove(&c->flink);
    /* Clear any stale scratchpad references to this client */
    if (oldmon) {
      if (oldmon->scratchpad_current == c)
        oldmon->scratchpad_current = nullptr;
      if (oldmon->scratchpad_prev_focus == c)
        oldmon->scratchpad_prev_focus = nullptr;
    }
    if (c->isscratchpad && oldmon && oldmon->scratchpad_visible) {
      /* Closing a scratchpad client: focus another scratchpad client */
      Client *s;
      int found = 0;
      wl_list_for_each(s, &fstack, flink) {
        if (s->isscratchpad && s->mon == oldmon) {
          focusclient(s, 1);
          found = 1;
          break;
        }
      }
      if (!found)
        focusclient(focustop(selmon), 1);
      c->mon = nullptr;
      arrange(oldmon);
    } else {
      setmon(c, nullptr, 0);
    }
  }

  wlr_scene_node_destroy(&c->scene->node);
  printstatus();
  ipc_socket_send_window_event("close");
  motionnotify(0, nullptr, 0, 0, 0, 0);
}

static void updatemons(struct wl_listener *listener, void *data) {
  /*
   * Called whenever the output layout changes: adding or removing a
   * monitor, changing an output's mode or position, etc. This is where
   * the change officially happens and we update geometry, window
   * positions, focus, and the stored configuration in wlroots'
   * output-manager implementation.
   */
  struct wlr_output_configuration_v1 *config =
      wlr_output_configuration_v1_create();
  Client *c;
  struct wlr_output_configuration_head_v1 *config_head;
  Monitor *m;

  /* First remove from the layout the disabled monitors */
  wl_list_for_each(m, &mons, link) {
    if (m->wlr_output->enabled || m->asleep)
      continue;
    config_head =
        wlr_output_configuration_head_v1_create(config, m->wlr_output);
    config_head->state.enabled = 0;
    /* Remove this output from the layout to avoid cursor enter inside it */
    wlr_output_layout_remove(output_layout, m->wlr_output);
    closemon(m);
    m->m = m->w = (struct wlr_box){0};
  }
  /* Insert outputs that need to */
  wl_list_for_each(m, &mons, link) {
    if (m->wlr_output->enabled &&
        !wlr_output_layout_get(output_layout, m->wlr_output))
      wlr_output_layout_add_auto(output_layout, m->wlr_output);
  }

  /* Now that we update the output layout we can get its box */
  wlr_output_layout_get_box(output_layout, nullptr, &sgeom);

  wlr_scene_node_set_position(&root_bg->node, sgeom.x, sgeom.y);
  wlr_scene_rect_set_size(root_bg, sgeom.width, sgeom.height);

  /* Make sure the clients are hidden when peachwm is locked */
  wlr_scene_node_set_position(&locked_bg->node, sgeom.x, sgeom.y);
  wlr_scene_rect_set_size(locked_bg, sgeom.width, sgeom.height);

  wl_list_for_each(m, &mons, link) {
    if (!m->wlr_output->enabled)
      continue;
    config_head =
        wlr_output_configuration_head_v1_create(config, m->wlr_output);

    /* Get the effective monitor geometry to use for surfaces */
    wlr_output_layout_get_box(output_layout, m->wlr_output, &m->m);
    m->w = m->m;
    wlr_scene_output_set_position(m->scene_output, m->m.x, m->m.y);

    wlr_scene_node_set_position(&m->fullscreen_bg->node, m->m.x, m->m.y);
    wlr_scene_rect_set_size(m->fullscreen_bg, m->m.width, m->m.height);

    if (m->lock_surface) {
      struct wlr_scene_tree *scene_tree = m->lock_surface->surface->data;
      wlr_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
      wlr_session_lock_surface_v1_configure(m->lock_surface, m->m.width,
                                            m->m.height);
    }

    /* Calculate the effective monitor geometry to use for clients */
    arrangelayers(m);
    /* Don't move clients to the left output when plugging monitors */
    arrange(m);
    /* make sure fullscreen clients have the right size */
    if ((c = focustop(m)) && c->isfullscreen)
      resize(c, m->m, 0);

    config_head->state.x = m->m.x;
    config_head->state.y = m->m.y;

    if (!selmon) {
      selmon = m;
    }
  }

  if (selmon && selmon->wlr_output->enabled) {
    wl_list_for_each(c, &clients, link) {
      if (!c->mon && client_surface(c)->mapped)
        setmon(c, selmon, c->tags);
    }
    focusclient(focustop(selmon), 1);
    if (selmon->lock_surface) {
      client_notify_enter(selmon->lock_surface->surface,
                          wlr_seat_get_keyboard(seat));
      client_activate_surface(selmon->lock_surface->surface, 1);
    }
  }

  wlr_cursor_move(cursor, nullptr, 0, 0);

  wlr_output_manager_v1_set_configuration(output_mgr, config);
}

static void updatetitle(struct wl_listener *listener, void *data) {
  Client *c = wl_container_of(listener, c, set_title);
  if (c == focustop(c->mon)) {
    printstatus();
    ipc_socket_send_window_event("title");
  }
}

static void urgent(struct wl_listener *listener, void *data) {
  struct wlr_xdg_activation_v1_request_activate_event *event = data;
  Client *c = nullptr;
  (void)toplevel_from_wlr_surface(event->surface, &c, nullptr);
  if (!c || c == focustop(selmon))
    return;

  c->isurgent = 1;
  printstatus();

  if (client_surface(c)->mapped)
    client_set_border_color(c, cfg.appearance.urgent_color);
}

void view(const Arg *arg) {
  if (!selmon || (arg->ui & TAGMASK) == selmon->tagset[selmon->seltags])
    return;
  selmon->seltags ^= 1; /* toggle sel tagset */
  if (arg->ui & TAGMASK)
    selmon->tagset[selmon->seltags] = arg->ui & TAGMASK;
  focusclient(focustop(selmon), 1);
  arrange(selmon);
  printstatus();
}

static void virtualkeyboard(struct wl_listener *listener, void *data) {
  struct wlr_virtual_keyboard_v1 *kb = data;
  /* virtual keyboards shouldn't share keyboard group */
  KeyboardGroup *group = createkeyboardgroup();
  /* Set the keymap to match the group keymap */
  wlr_keyboard_set_keymap(&kb->keyboard, group->wlr_group->keyboard.keymap);
  LISTEN(&kb->keyboard.base.events.destroy, &group->destroy,
         destroykeyboardgroup);

  /* Add the new keyboard to the group */
  wlr_keyboard_group_add_keyboard(group->wlr_group, &kb->keyboard);
}

static void virtualpointer(struct wl_listener *listener, void *data) {
  struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
  struct wlr_input_device *device = &event->new_pointer->pointer.base;

  wlr_cursor_attach_input_device(cursor, device);
  if (event->suggested_output)
    wlr_cursor_map_input_to_output(cursor, device, event->suggested_output);
}

Monitor *xytomon(double x, double y) {
  struct wlr_output *o = wlr_output_layout_output_at(output_layout, x, y);
  return o ? o->data : nullptr;
}

static void xytonode(double x, double y, struct wlr_surface **psurface, Client **pc,
              LayerSurface **pl, double *nx, double *ny) {
  struct wlr_scene_node *node, *pnode;
  struct wlr_surface *surface = nullptr;
  Client *c = nullptr;
  LayerSurface *l = nullptr;
  int layer;

  for (layer = NUM_LAYERS - 1; !surface && layer >= 0; layer--) {
    if (!(node = wlr_scene_node_at(&layers[layer]->node, x, y, nx, ny)))
      continue;

    if (node->type == WLR_SCENE_NODE_BUFFER)
      surface =
          wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node))
              ->surface;
    /* Walk the tree to find a node that knows the client */
    for (pnode = node; pnode && !c; pnode = &pnode->parent->node)
      c = pnode->data;
    if (c && c->type == LayerShell) {
      c = nullptr;
      l = pnode->data;
    }
  }

  if (psurface)
    *psurface = surface;
  if (pc)
    *pc = c;
  if (pl)
    *pl = l;
}

#ifdef XWAYLAND
static void activatex11(struct wl_listener *listener, void *data) {
  Client *c = wl_container_of(listener, c, activate);

  /* Only "managed" windows can be activated */
  if (!client_is_unmanaged(c))
    wlr_xwayland_surface_activate(c->surface.xwayland, 1);
}

static void associatex11(struct wl_listener *listener, void *data) {
  Client *c = wl_container_of(listener, c, associate);

  LISTEN(&client_surface(c)->events.map, &c->map, mapnotify);
  LISTEN(&client_surface(c)->events.unmap, &c->unmap, unmapnotify);
}

static void configurex11(struct wl_listener *listener, void *data) {
  Client *c = wl_container_of(listener, c, configure);
  struct wlr_xwayland_surface_configure_event *event = data;
  if (!client_surface(c) || !client_surface(c)->mapped) {
    wlr_xwayland_surface_configure(c->surface.xwayland, event->x, event->y,
                                   event->width, event->height);
    return;
  }
  if (client_is_unmanaged(c)) {
    wlr_scene_node_set_position(&c->scene->node, event->x, event->y);
    wlr_xwayland_surface_configure(c->surface.xwayland, event->x, event->y,
                                   event->width, event->height);
    return;
  }
  if ((c->isfloating && c != grabc) || !curlayout(c->mon)->arrange) {
    resize(c,
           (struct wlr_box){.x = event->x - c->bw,
                            .y = event->y - c->bw,
                            .width = event->width + c->bw * 2,
                            .height = event->height + c->bw * 2},
           0);
  } else {
    arrange(c->mon);
  }
}

static void createnotifyx11(struct wl_listener *listener, void *data) {
  struct wlr_xwayland_surface *xsurface = data;
  Client *c;

  /* Allocate a Client for this surface */
  c = xsurface->data = ecalloc(1, sizeof(*c));
  c->surface.xwayland = xsurface;
  c->type = X11;
  c->bw = client_is_unmanaged(c) ? 0 : cfg.appearance.border_px;

  /* Listen to the various events it can emit */
  LISTEN(&xsurface->events.associate, &c->associate, associatex11);
  LISTEN(&xsurface->events.destroy, &c->destroy, destroynotify);
  LISTEN(&xsurface->events.dissociate, &c->dissociate, dissociatex11);
  LISTEN(&xsurface->events.request_activate, &c->activate, activatex11);
  LISTEN(&xsurface->events.request_configure, &c->configure, configurex11);
  LISTEN(&xsurface->events.request_fullscreen, &c->fullscreen,
         fullscreennotify);
  LISTEN(&xsurface->events.set_hints, &c->set_hints, sethints);
  LISTEN(&xsurface->events.set_title, &c->set_title, updatetitle);
}

static void dissociatex11(struct wl_listener *listener, void *data) {
  Client *c = wl_container_of(listener, c, dissociate);
  wl_list_remove(&c->map.link);
  wl_list_remove(&c->unmap.link);
}

static void sethints(struct wl_listener *listener, void *data) {
  Client *c = wl_container_of(listener, c, set_hints);
  struct wlr_surface *surface = client_surface(c);
  if (c == focustop(selmon) || !c->surface.xwayland->hints)
    return;

  c->isurgent = xcb_icccm_wm_hints_get_urgency(c->surface.xwayland->hints);
  printstatus();

  if (c->isurgent && surface && surface->mapped)
    client_set_border_color(c, cfg.appearance.urgent_color);
}

static void xwaylandready(struct wl_listener *listener, void *data) {
  struct wlr_xcursor *xcursor;
  struct wlr_xcursor_image *image;

  /* assign the one and only seat */
  wlr_xwayland_set_seat(xwayland, seat);

  /* Set the default XWayland cursor to match the rest of peachwm. */
  if ((xcursor = wlr_xcursor_manager_get_xcursor(cursor_mgr, "default", 1))) {
    image = xcursor->images[0];
    wlr_xwayland_set_cursor(xwayland, wlr_xcursor_image_get_buffer(image),
                            image->hotspot_x, image->hotspot_y);
  }
}
#endif

// ── main() ──────────────────────────────────────────────────

int main(int argc, char *argv[]) {
  char *startup_cmd = nullptr;
  int c;

  while ((c = getopt(argc, argv, "c:s:hdv")) != -1) {
    switch (c) {
    case 'c': {
      struct stat st;
      if (stat(optarg, &st) < 0) {
        if (errno == ENOENT)
          die("error: config file '%s' does not exist", optarg);
        die("error: config file '%s': %s", optarg, strerror(errno));
      }
      if (S_ISDIR(st.st_mode))
        die("error: config file '%s' is a directory, expected a regular file",
            optarg);
      if (access(optarg, R_OK) < 0)
        die("error: config file '%s' is not readable (permission denied)",
            optarg);
      custom_cfg_path = optarg;
      break;
    }
    case 's':
      startup_cmd = optarg;
      break;
    case 'd':
      cfg.log_level = WLR_DEBUG; /* override before setup() */
      break;
    case 'v':
      die("peachwm " VERSION);
      break;
    default:
      goto usage;
    }
  }
  if (optind < argc)
    goto usage;

  /* Wayland requires XDG_RUNTIME_DIR for creating its communications socket */
  if (!getenv("XDG_RUNTIME_DIR"))
    die("XDG_RUNTIME_DIR must be set");
  setup();
  run(startup_cmd);
  cleanup();
  return EXIT_SUCCESS;

usage:
  die("Usage: %s [-v] [-d] [-s startup command] [-c config file]", argv[0]);
}
