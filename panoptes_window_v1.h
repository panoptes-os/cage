#ifndef CG_PANOPTES_WINDOW_V1_H
#define CG_PANOPTES_WINDOW_V1_H

struct cg_panoptes_window_manager;
struct cg_server;
struct cg_view;

/* Serves panoptes_window_manager_v1. xwayland_display is the DISPLAY of
 * Cage's Xwayland, or NULL without one. */
struct cg_panoptes_window_manager *panoptes_window_manager_create(struct cg_server *server,
								  const char *xwayland_display);

/* Announce a view that just mapped. Call after it took focus, so its
 * initial state carries its focus. */
void panoptes_window_map(struct cg_view *view);

/* Send whatever changed about a mapped view since clients last heard. A
 * no-op for a view that is not listed. */
void panoptes_window_update(struct cg_view *view);

/* Tell clients a view is gone. Call before it stops being mapped. */
void panoptes_window_unmap(struct cg_view *view);

#endif
