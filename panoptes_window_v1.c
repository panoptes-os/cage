/*
 * panoptes_window_v1: every managed toplevel, with its pid, app_id, title,
 * box, parent and focus, for a client that drives the kiosk's windows.
 *
 * Each listed view caches what its clients last heard. Hooks in view.c,
 * seat.c and the shells call panoptes_window_update, which compares the
 * cache against the view and sends only what changed, then done.
 */

#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

#include "panoptes-window-v1-protocol.h"
#include "panoptes_window_v1.h"
#include "seat.h"
#include "server.h"
#include "view.h"
#if CAGE_HAS_XWAYLAND
#include "xwayland.h"
#endif

#define PANOPTES_WINDOW_MANAGER_V1_VERSION 1

struct cg_panoptes_window_manager {
	struct cg_server *server;
	struct wl_global *global;
	struct wl_list resources; // panoptes_window_manager_v1 resources
	char *xwayland_display;
	struct wl_listener display_destroy;
};

/* One listed view: what its clients last heard, and each client's object
 * for it. */
struct cg_panoptes_window {
	struct cg_view *view;
	struct wl_list resources; // panoptes_window_v1 resources
	struct wl_listener commit;

	pid_t pid;
	char *app_id;
	char *title;
	struct wlr_box box;
	struct cg_panoptes_window *parent;
	bool focused;
};

static const char *
or_empty(const char *s)
{
	return s ? s : "";
}

/* Replace *cached with value; report whether they differed. */
static bool
refresh_string(char **cached, const char *value)
{
	if (strcmp(or_empty(*cached), value) == 0) {
		return false;
	}
	free(*cached);
	*cached = strdup(value);
	return true;
}

static struct wlr_box
view_box(struct cg_view *view)
{
	struct wlr_box box;
	view->impl->get_geometry(view, &box);
	box.x = view->lx;
	box.y = view->ly;
	return box;
}

static struct cg_panoptes_window *
listed_parent(struct cg_view *view)
{
	struct cg_view *parent = view->impl->get_parent(view);
	return parent ? parent->panoptes_window : NULL;
}

static bool
view_focused(struct cg_view *view)
{
	return seat_get_focus(view->server->seat) == view;
}

/* The object client holds for window, the newest if it holds several. */
static struct wl_resource *
resource_for_client(struct cg_panoptes_window *window, struct wl_client *client)
{
	if (!window) {
		return NULL;
	}
	struct wl_resource *resource;
	wl_resource_for_each (resource, &window->resources) {
		if (wl_resource_get_client(resource) == client) {
			return resource;
		}
	}
	return NULL;
}

static void
send_parent(struct cg_panoptes_window *window, struct wl_resource *resource)
{
	panoptes_window_v1_send_parent(resource, resource_for_client(window->parent, wl_resource_get_client(resource)));
}

static void
window_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
window_handle_activate(struct wl_client *client, struct wl_resource *resource)
{
	struct cg_panoptes_window *window = wl_resource_get_user_data(resource);
	if (!window) {
		return;
	}
	struct cg_view *view = window->view;
	wlr_scene_node_raise_to_top(&view->scene_tree->node);
	seat_set_focus(view->server->seat, view);
}

static void
window_handle_close(struct wl_client *client, struct wl_resource *resource)
{
	struct cg_panoptes_window *window = wl_resource_get_user_data(resource);
	if (!window) {
		return;
	}
	window->view->impl->close(window->view);
}

static const struct panoptes_window_v1_interface window_impl = {
	.destroy = window_handle_destroy,
	.activate = window_handle_activate,
	.close = window_handle_close,
};

static void
window_resource_destroy(struct wl_resource *resource)
{
	wl_list_remove(wl_resource_get_link(resource));
}

/* Create the client's object for window and send its state, all but
 * parent and done: the parent may not be announced yet. */
static struct wl_resource *
announce(struct cg_panoptes_window *window, struct wl_resource *manager_resource)
{
	struct wl_client *client = wl_resource_get_client(manager_resource);
	struct wl_resource *resource =
		wl_resource_create(client, &panoptes_window_v1_interface, wl_resource_get_version(manager_resource), 0);
	if (!resource) {
		wl_client_post_no_memory(client);
		return NULL;
	}
	wl_resource_set_implementation(resource, &window_impl, window, window_resource_destroy);
	wl_list_insert(&window->resources, wl_resource_get_link(resource));

	panoptes_window_manager_v1_send_window(manager_resource, resource);
	panoptes_window_v1_send_pid(resource, window->pid);
	panoptes_window_v1_send_app_id(resource, or_empty(window->app_id));
	panoptes_window_v1_send_title(resource, or_empty(window->title));
	panoptes_window_v1_send_geometry(resource, window->box.x, window->box.y, window->box.width, window->box.height);
	panoptes_window_v1_send_focused(resource, window->focused);
	return resource;
}

static void
handle_commit(struct wl_listener *listener, void *data)
{
	struct cg_panoptes_window *window = wl_container_of(listener, window, commit);
	panoptes_window_update(window->view);
}

void
panoptes_window_map(struct cg_view *view)
{
	struct cg_panoptes_window_manager *manager = view->server->panoptes_window_manager;
	if (!manager) {
		return;
	}
#if CAGE_HAS_XWAYLAND
	if (view->type == CAGE_XWAYLAND_VIEW && !xwayland_view_should_manage(view)) {
		return;
	}
#endif

	struct cg_panoptes_window *window = calloc(1, sizeof(*window));
	if (!window) {
		wlr_log(WLR_ERROR, "Failed to allocate a panoptes window");
		return;
	}
	window->view = view;
	wl_list_init(&window->resources);
	window->pid = view->impl->get_pid(view);
	window->app_id = strdup(or_empty(view->impl->get_app_id(view)));
	window->title = strdup(or_empty(view->impl->get_title(view)));
	window->box = view_box(view);
	window->parent = listed_parent(view);
	window->focused = view_focused(view);
	window->commit.notify = handle_commit;
	wl_signal_add(&view->wlr_surface->events.commit, &window->commit);
	view->panoptes_window = window;

	struct wl_resource *manager_resource;
	wl_resource_for_each (manager_resource, &manager->resources) {
		struct wl_resource *resource = announce(window, manager_resource);
		if (resource) {
			send_parent(window, resource);
			panoptes_window_v1_send_done(resource);
		}
	}

	/* An X11 dialog can map before its parent and then list none. */
	struct cg_view *other;
	wl_list_for_each (other, &view->server->views, link) {
		if (other->panoptes_window && other->impl->get_parent(other) == view) {
			panoptes_window_update(other);
		}
	}
}

void
panoptes_window_update(struct cg_view *view)
{
	struct cg_panoptes_window *window = view->panoptes_window;
	if (!window) {
		return;
	}
	struct wl_resource *resource;
	bool changed = false;

	const char *app_id = or_empty(view->impl->get_app_id(view));
	if (refresh_string(&window->app_id, app_id)) {
		wl_resource_for_each (resource, &window->resources) {
			panoptes_window_v1_send_app_id(resource, app_id);
		}
		changed = true;
	}

	const char *title = or_empty(view->impl->get_title(view));
	if (refresh_string(&window->title, title)) {
		wl_resource_for_each (resource, &window->resources) {
			panoptes_window_v1_send_title(resource, title);
		}
		changed = true;
	}

	struct wlr_box box = view_box(view);
	if (!wlr_box_equal(&box, &window->box)) {
		window->box = box;
		wl_resource_for_each (resource, &window->resources) {
			panoptes_window_v1_send_geometry(resource, box.x, box.y, box.width, box.height);
		}
		changed = true;
	}

	struct cg_panoptes_window *parent = listed_parent(view);
	if (parent != window->parent) {
		window->parent = parent;
		wl_resource_for_each (resource, &window->resources) {
			send_parent(window, resource);
		}
		changed = true;
	}

	bool focused = view_focused(view);
	if (focused != window->focused) {
		window->focused = focused;
		wl_resource_for_each (resource, &window->resources) {
			panoptes_window_v1_send_focused(resource, focused);
		}
		changed = true;
	}

	if (changed) {
		wl_resource_for_each (resource, &window->resources) {
			panoptes_window_v1_send_done(resource);
		}
	}
}

void
panoptes_window_unmap(struct cg_view *view)
{
	struct cg_panoptes_window *window = view->panoptes_window;
	if (!window) {
		return;
	}
	view->panoptes_window = NULL;

	/* With this view unlisted, its dialogs' parents read as NULL. */
	struct cg_view *other;
	wl_list_for_each (other, &view->server->views, link) {
		if (other->panoptes_window && other->panoptes_window->parent == window) {
			panoptes_window_update(other);
		}
	}

	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe (resource, tmp, &window->resources) {
		panoptes_window_v1_send_closed(resource);
		wl_resource_set_user_data(resource, NULL);
		wl_list_remove(wl_resource_get_link(resource));
		wl_list_init(wl_resource_get_link(resource));
	}
	wl_list_remove(&window->commit.link);
	free(window->app_id);
	free(window->title);
	free(window);
}

static void
manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct panoptes_window_manager_v1_interface manager_impl = {
	.destroy = manager_handle_destroy,
};

static void
manager_resource_destroy(struct wl_resource *resource)
{
	wl_list_remove(wl_resource_get_link(resource));
}

static void
manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct cg_panoptes_window_manager *manager = data;
	struct wl_resource *manager_resource =
		wl_resource_create(client, &panoptes_window_manager_v1_interface, version, id);
	if (!manager_resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(manager_resource, &manager_impl, manager, manager_resource_destroy);
	wl_list_insert(&manager->resources, wl_resource_get_link(manager_resource));

	if (manager->xwayland_display) {
		panoptes_window_manager_v1_send_xwayland(manager_resource, manager->xwayland_display);
	}

	/* Announce every window before any parent event names one. The objects
	 * just created are each window's newest for this client. */
	struct cg_view *view;
	wl_list_for_each (view, &manager->server->views, link) {
		if (view->panoptes_window) {
			announce(view->panoptes_window, manager_resource);
		}
	}
	wl_list_for_each (view, &manager->server->views, link) {
		struct wl_resource *resource = resource_for_client(view->panoptes_window, client);
		if (resource) {
			send_parent(view->panoptes_window, resource);
			panoptes_window_v1_send_done(resource);
		}
	}
}

static void
handle_display_destroy(struct wl_listener *listener, void *data)
{
	struct cg_panoptes_window_manager *manager = wl_container_of(listener, manager, display_destroy);

	/* Clients may outlive the manager while the display tears down. */
	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe (resource, tmp, &manager->resources) {
		wl_list_remove(wl_resource_get_link(resource));
		wl_list_init(wl_resource_get_link(resource));
	}

	manager->server->panoptes_window_manager = NULL;
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager->xwayland_display);
	free(manager);
}

struct cg_panoptes_window_manager *
panoptes_window_manager_create(struct cg_server *server, const char *xwayland_display)
{
	struct cg_panoptes_window_manager *manager = calloc(1, sizeof(*manager));
	if (!manager) {
		return NULL;
	}
	manager->global = wl_global_create(server->wl_display, &panoptes_window_manager_v1_interface,
					   PANOPTES_WINDOW_MANAGER_V1_VERSION, manager, manager_bind);
	if (!manager->global) {
		free(manager);
		return NULL;
	}
	manager->server = server;
	wl_list_init(&manager->resources);
	if (xwayland_display) {
		manager->xwayland_display = strdup(xwayland_display);
	}
	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(server->wl_display, &manager->display_destroy);
	return manager;
}
