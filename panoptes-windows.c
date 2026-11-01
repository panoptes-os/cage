/*
 * panoptes-windows: what panoptes_window_v1 lists, for people and tests.
 *
 *   panoptes-windows                   Xwayland's DISPLAY, then one line
 *                                      per window
 *   panoptes-windows watch             the same, then a line per change
 *                                      until killed
 *   panoptes-windows activate APP_ID   focus the first window with APP_ID
 *   panoptes-windows close APP_ID      ask that window to close
 *
 * A window line reads:
 *
 *   window id=2 pid=41 app_id="foot" title="~" box=0,0,1024x768 focused=1 parent=0
 *
 * ids number the windows in the order announced, and parent is the id of
 * the window this one is a dialog of, or 0. watch prints a window line at
 * each done, and "closed id=N" when a window goes.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include "panoptes-window-v1-client-protocol.h"

struct window {
	int id;
	struct panoptes_window_v1 *handle;
	int32_t pid;
	char *app_id;
	char *title;
	int32_t x, y, width, height;
	uint32_t focused;
	struct window *parent;
	bool closed;
	struct window *next;
};

static struct panoptes_window_manager_v1 *manager;
static char *xwayland_display;
static struct window *windows;
static struct window **windows_tail = &windows;
static int window_count;
static bool watching;

static const char *
or_empty(const char *s)
{
	return s ? s : "";
}

static void
replace(char **field, const char *value)
{
	free(*field);
	*field = strdup(value);
}

static void
print_window(const struct window *w)
{
	printf("window id=%d pid=%d app_id=\"%s\" title=\"%s\" box=%d,%d,%dx%d focused=%u parent=%d\n", w->id, w->pid,
	       or_empty(w->app_id), or_empty(w->title), w->x, w->y, w->width, w->height, w->focused,
	       w->parent ? w->parent->id : 0);
}

static void
handle_pid(void *data, struct panoptes_window_v1 *handle, int32_t pid)
{
	struct window *window = data;
	window->pid = pid;
}

static void
handle_app_id(void *data, struct panoptes_window_v1 *handle, const char *app_id)
{
	struct window *window = data;
	replace(&window->app_id, app_id);
}

static void
handle_title(void *data, struct panoptes_window_v1 *handle, const char *title)
{
	struct window *window = data;
	replace(&window->title, title);
}

static void
handle_geometry(void *data, struct panoptes_window_v1 *handle, int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct window *window = data;
	window->x = x;
	window->y = y;
	window->width = width;
	window->height = height;
}

static void
handle_parent(void *data, struct panoptes_window_v1 *handle, struct panoptes_window_v1 *parent)
{
	struct window *window = data;
	window->parent = parent ? panoptes_window_v1_get_user_data(parent) : NULL;
}

static void
handle_focused(void *data, struct panoptes_window_v1 *handle, uint32_t focused)
{
	struct window *window = data;
	window->focused = focused;
}

static void
handle_done(void *data, struct panoptes_window_v1 *handle)
{
	if (watching) {
		print_window(data);
	}
}

static void
handle_closed(void *data, struct panoptes_window_v1 *handle)
{
	struct window *window = data;
	window->closed = true;
	if (watching) {
		printf("closed id=%d\n", window->id);
	}
}

static const struct panoptes_window_v1_listener window_listener = {
	.pid = handle_pid,
	.app_id = handle_app_id,
	.title = handle_title,
	.geometry = handle_geometry,
	.parent = handle_parent,
	.focused = handle_focused,
	.done = handle_done,
	.closed = handle_closed,
};

static void
handle_xwayland(void *data, struct panoptes_window_manager_v1 *wm, const char *display)
{
	replace(&xwayland_display, display);
	if (watching) {
		printf("xwayland display=%s\n", display);
	}
}

static void
handle_window(void *data, struct panoptes_window_manager_v1 *wm, struct panoptes_window_v1 *handle)
{
	struct window *window = calloc(1, sizeof(*window));
	if (!window) {
		perror("panoptes-windows");
		exit(1);
	}
	window->id = ++window_count;
	window->handle = handle;
	*windows_tail = window;
	windows_tail = &window->next;
	panoptes_window_v1_add_listener(handle, &window_listener, window);
}

static const struct panoptes_window_manager_v1_listener manager_listener = {
	.xwayland = handle_xwayland,
	.window = handle_window,
};

static void
handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
	if (strcmp(interface, panoptes_window_manager_v1_interface.name) == 0) {
		manager = wl_registry_bind(registry, name, &panoptes_window_manager_v1_interface, 1);
		panoptes_window_manager_v1_add_listener(manager, &manager_listener, NULL);
	}
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

/* Destroys every object and closes the connection, so a leak check finds
 * nothing. A closed window's object stays valid until destroyed. */
static void
disconnect(struct wl_display *display, struct wl_registry *registry)
{
	struct window *next;
	for (struct window *w = windows; w; w = next) {
		next = w->next;
		panoptes_window_v1_destroy(w->handle);
		free(w->app_id);
		free(w->title);
		free(w);
	}
	if (manager) {
		panoptes_window_manager_v1_destroy(manager);
	}
	wl_registry_destroy(registry);
	free(xwayland_display);
	wl_display_disconnect(display);
}

static int
usage(void)
{
	fprintf(stderr, "usage: panoptes-windows [watch | activate APP_ID | close APP_ID]\n");
	return 2;
}

int
main(int argc, char *argv[])
{
	const char *verb = argc > 1 ? argv[1] : "list";
	bool acts = strcmp(verb, "activate") == 0 || strcmp(verb, "close") == 0;
	if (acts ? argc != 3 : argc > 2) {
		return usage();
	}
	watching = strcmp(verb, "watch") == 0;
	if (!acts && !watching && strcmp(verb, "list") != 0) {
		return usage();
	}
	if (watching) {
		setvbuf(stdout, NULL, _IOLBF, 0);
	}

	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "panoptes-windows: cannot connect to the Wayland display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	/* The first roundtrip binds the manager, the second brings every
	 * window's initial state. */
	wl_display_roundtrip(display);
	if (!manager) {
		fprintf(stderr, "panoptes-windows: the compositor has no panoptes_window_manager_v1\n");
		disconnect(display, registry);
		return 1;
	}
	wl_display_roundtrip(display);

	if (watching) {
		while (wl_display_dispatch(display) != -1) {
		}
		disconnect(display, registry);
		return 0;
	}

	if (!acts) {
		if (xwayland_display) {
			printf("xwayland display=%s\n", xwayland_display);
		}
		for (struct window *w = windows; w; w = w->next) {
			if (!w->closed) {
				print_window(w);
			}
		}
		disconnect(display, registry);
		return 0;
	}

	struct window *target = NULL;
	for (struct window *w = windows; w && !target; w = w->next) {
		if (!w->closed && strcmp(or_empty(w->app_id), argv[2]) == 0) {
			target = w;
		}
	}
	if (!target) {
		fprintf(stderr, "panoptes-windows: no window with app_id %s\n", argv[2]);
		disconnect(display, registry);
		return 1;
	}
	if (strcmp(verb, "activate") == 0) {
		panoptes_window_v1_activate(target->handle);
	} else {
		panoptes_window_v1_close(target->handle);
	}
	wl_display_roundtrip(display);
	disconnect(display, registry);
	return 0;
}
