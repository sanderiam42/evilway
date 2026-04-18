/* main.c — evilWay compositor entry point
 *
 * Initializes the wlroots compositor, sets up the scene graph with
 * layered trees, loads configuration, and enters the event loop.
 *
 * SIGHUP reloads the config file (evilwm behavior).
 * SIGTERM/SIGINT cleanly shut down.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/log.h>

#include "evilway.h"

/* Global server pointer for signal handlers */
static struct ew_server *g_server = NULL;

/* ── Signal handlers ────────────────────────────────────────────────── */

static int handle_signal(int sig, void *data) {
	struct ew_server *server = data;

	switch (sig) {
	case SIGHUP:
		/* Reload config — evilwm reloads .evilwmrc on SIGHUP */
		wlr_log(WLR_INFO, "SIGHUP: reloading config");
		config_destroy(&server->config);
		config_init(&server->config);
		{
			char path[512];
			const char *home = getenv("HOME");
			if (home) {
				snprintf(path, sizeof(path), "%s/.evilwayrc", home);
				config_load(&server->config, path);
			}
		}
		/* Update all view borders with new colors */
		{
			struct ew_view *v;
			wl_list_for_each(v, &server->views, link) {
				if (v->mapped) view_update_borders(v);
			}
		}
		break;

	case SIGTERM:
	case SIGINT:
		wlr_log(WLR_INFO, "signal %d: shutting down", sig);
		wl_display_terminate(server->wl_display);
		break;
	}

	return 0;
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
	(void)argc;
	(void)argv;

	wlr_log_init(WLR_DEBUG, NULL);
	wlr_log(WLR_INFO, "evilWay starting");

	struct ew_server server = {0};
	g_server = &server;

	/* ── Load configuration ───────────────────────────────────── */

	config_init(&server.config);

	char config_path[512];
	const char *home = getenv("HOME");
	if (home) {
		snprintf(config_path, sizeof(config_path), "%s/.evilwayrc", home);
		config_load(&server.config, config_path);
	}

	/* ── Wayland display ──────────────────────────────────────── */

	server.wl_display = wl_display_create();
	if (!server.wl_display) {
		wlr_log(WLR_ERROR, "failed to create wl_display");
		return 1;
	}

	/* ── Backend ──────────────────────────────────────────────── */

	server.backend = wlr_backend_autocreate(
		wl_display_get_event_loop(server.wl_display), NULL);
	if (!server.backend) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		wl_display_destroy(server.wl_display);
		return 1;
	}

	/* ── Renderer + allocator ─────────────────────────────────── */

	server.renderer = wlr_renderer_autocreate(server.backend);
	if (!server.renderer) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return 1;
	}
	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	server.allocator = wlr_allocator_autocreate(server.backend,
		server.renderer);
	if (!server.allocator) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		return 1;
	}

	/* ── Wayland globals ──────────────────────────────────────── */

	wlr_compositor_create(server.wl_display, 5, server.renderer);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);

	/* ── Scene graph ──────────────────────────────────────────── */

	server.scene = wlr_scene_create();

	/* Create layered scene trees in bottom-to-top order.
	 * This defines the visual stacking:
	 *   background → bottom → views → top → overlay → lock
	 */
	for (int i = 0; i < EW_LAYER_COUNT; i++) {
		server.layers[i] = wlr_scene_tree_create(&server.scene->tree);
	}

	/* ── Subsystems ───────────────────────────────────────────── */

	wl_list_init(&server.views);
	server.current_vdesk = 0;
	server.prev_vdesk = 0;
	server.docks_visible = true;
	server.cursor_mode = EW_CURSOR_PASSTHROUGH;

	output_init(&server);
	input_init(&server);
	window_init(&server);
	layer_init(&server);
	lock_init(&server);

	/* ── Signal handling ──────────────────────────────────────── */

	struct wl_event_loop *loop = wl_display_get_event_loop(server.wl_display);
	wl_event_loop_add_signal(loop, SIGHUP, handle_signal, &server);
	wl_event_loop_add_signal(loop, SIGTERM, handle_signal, &server);
	wl_event_loop_add_signal(loop, SIGINT, handle_signal, &server);

	/* ── Socket ───────────────────────────────────────────────── */

	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_log(WLR_ERROR, "failed to create Wayland socket");
		wlr_backend_destroy(server.backend);
		return 1;
	}

	/* SECURITY: socket permissions are set by wlroots (0600, owned by
	 * session user).  Verify with: ls -la $XDG_RUNTIME_DIR/wayland-* */
	wlr_log(WLR_INFO, "Wayland socket: %s", socket);

	/* ── Start backend ────────────────────────────────────────── */

	if (!wlr_backend_start(server.backend)) {
		wlr_log(WLR_ERROR, "failed to start backend");
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	setenv("WAYLAND_DISPLAY", socket, true);
	wlr_log(WLR_INFO, "evilWay running on %s", socket);

	/* ── Event loop ───────────────────────────────────────────── */

	wl_display_run(server.wl_display);

	/* ── Cleanup ──────────────────────────────────────────────── */

	wlr_log(WLR_INFO, "evilWay shutting down");
	wl_display_destroy_clients(server.wl_display);
	wl_display_destroy(server.wl_display);
	config_destroy(&server.config);

	return 0;
}
