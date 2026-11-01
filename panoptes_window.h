#ifndef CG_PANOPTES_WINDOW_H
#define CG_PANOPTES_WINDOW_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct cg_panoptes_window_manager;
struct cg_server;
struct cg_view;

struct cg_panoptes_window_manager *panoptes_window_manager_create(struct cg_server *server,
								  const char *xwayland_display);
void panoptes_window_map(struct cg_view *view);
void panoptes_window_update(struct cg_view *view);
void panoptes_window_unmap(struct cg_view *view);
bool panoptes_window_resize_outputs(struct cg_server *server);
void panoptes_window_handle_request_configure(struct wl_listener *listener, void *data);

#endif
