/* layer.c — wlr-layer-shell-v1 support
 *
 * Required for waybar (status bar), swaylock (lock screen surface),
 * and any other overlay/panel applications.
 */

#include <stdlib.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>

#include "evilway.h"

/* Map layer shell layer enum to our scene tree layer */
static enum ew_layer shell_layer_to_ew(enum zwlr_layer_shell_v1_layer layer) {
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
		return EW_LAYER_BACKGROUND;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		return EW_LAYER_BOTTOM;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		return EW_LAYER_TOP;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
		return EW_LAYER_OVERLAY;
	default:
		return EW_LAYER_TOP;
	}
}

static void layer_surface_commit(struct wl_listener *listener, void *data) {
	struct ew_layer_surface *ls = wl_container_of(listener, ls, commit);
	(void)data;

	if (!ls->layer_surface->initialized) return;

	/* If the layer changed, re-parent the scene node */
	if (ls->layer_surface->current.committed &
	    WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		enum ew_layer target = shell_layer_to_ew(
			ls->layer_surface->current.layer);
		wlr_scene_node_reparent(&ls->scene->tree->node,
			ls->server->layers[target]);
	}

	/* Auto-configure: give the surface the space it asked for */
	struct wlr_output *output = ls->layer_surface->output;
	if (!output) {
		/* Pick the first output */
		struct ew_output *eo;
		wl_list_for_each(eo, &ls->server->outputs, link) {
			output = eo->wlr_output;
			break;
		}
	}
	if (!output) return;

	struct wlr_box full_area = { 0, 0, output->width, output->height };
	struct wlr_box usable_area = full_area;
	wlr_scene_layer_surface_v1_configure(ls->scene, &full_area,
		&usable_area);
}

static void layer_surface_map(struct wl_listener *listener, void *data) {
	(void)listener;
	(void)data;
}

static void layer_surface_unmap(struct wl_listener *listener, void *data) {
	(void)listener;
	(void)data;
}

static void layer_surface_destroy(struct wl_listener *listener, void *data) {
	struct ew_layer_surface *ls = wl_container_of(listener, ls, destroy);
	(void)data;

	wl_list_remove(&ls->map.link);
	wl_list_remove(&ls->unmap.link);
	wl_list_remove(&ls->commit.link);
	wl_list_remove(&ls->destroy.link);
	wl_list_remove(&ls->link);
	free(ls);
}

static void handle_new_layer_surface(struct wl_listener *listener,
                                     void *data) {
	struct ew_server *server = wl_container_of(listener, server,
		new_layer_surface);
	struct wlr_layer_surface_v1 *surface = data;

	/* If no output assigned, pick the first one */
	if (!surface->output) {
		struct ew_output *eo;
		wl_list_for_each(eo, &server->outputs, link) {
			surface->output = eo->wlr_output;
			break;
		}
	}
	if (!surface->output) {
		wlr_layer_surface_v1_destroy(surface);
		return;
	}

	struct ew_layer_surface *ls = calloc(1, sizeof(*ls));
	if (!ls) return;
	ls->server = server;
	ls->layer_surface = surface;

	/* Create scene node in the appropriate layer */
	enum ew_layer target = shell_layer_to_ew(surface->pending.layer);
	ls->scene = wlr_scene_layer_surface_v1_create(
		server->layers[target], surface);

	ls->map.notify = layer_surface_map;
	wl_signal_add(&surface->surface->events.map, &ls->map);
	ls->unmap.notify = layer_surface_unmap;
	wl_signal_add(&surface->surface->events.unmap, &ls->unmap);
	ls->commit.notify = layer_surface_commit;
	wl_signal_add(&surface->surface->events.commit, &ls->commit);
	ls->destroy.notify = layer_surface_destroy;
	wl_signal_add(&surface->events.destroy, &ls->destroy);
}

void layer_init(struct ew_server *server) {
	server->layer_shell = wlr_layer_shell_v1_create(server->wl_display, 4);

	server->new_layer_surface.notify = handle_new_layer_surface;
	wl_signal_add(&server->layer_shell->events.new_surface,
		&server->new_layer_surface);
}
