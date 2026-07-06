## task-3-transparency-blur

### Field added
- File: include/client.h
- Line 39: `struct wlr_scene_blur *blur;`
- Placement: after `struct wlr_scene_shadow *shadow;` (line 38)

### Context
- Forward declaration not needed — definition provided by `<scenefx/types/wlr_scene.h>` through monitor.h include chain.
- Field initialized to NULL by ecalloc in createnotify (peachwm.c:1245).
- No `#include` directives added.
