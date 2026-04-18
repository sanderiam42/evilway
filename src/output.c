/* output.c — output (display) management
 *
 * Handles new outputs, frame rendering (delegated to wlr_scene),
 * and output destruction.
 */

#include <stdlib.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>

#include "evilway.h"

static void output_handle_frame(struct wl_listener *listener, void *data) {
	struct ew_output *output = wl_container_of(listener, output, frame);
	(void)data;

	struct wlr_scene *scene = output->server->scene;
	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
		scene, output->wlr_output);
	if (!scene_output) return;

	wlr_scene_output_commit(scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_handle_request_state(struct wl_listener *listener,
                                        void *data) {
	struct ew_output *output = wl_container_of(listener, output,
		request_state);
	struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_handle_destroy(struct wl_listener *listener, void *data) {
	struct ew_output *output = wl_container_of(listener, output, destroy);
	(void)data;

	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);
}

static void handle_new_output(struct wl_listener *listener, void *data) {
	struct ew_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	/* Initialize output with preferred mode */
	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode) {
		wlr_output_state_set_mode(&state, mode);
	}

	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	struct ew_output *output = calloc(1, sizeof(*output));
	if (!output) return;
	output->server = server;
	output->wlr_output = wlr_output;

	output->frame.notify = output_handle_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = output_handle_request_state;
	wl_signal_add(&wlr_output->events.request_state,
		&output->request_state);
	output->destroy.notify = output_handle_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wl_list_insert(&server->outputs, &output->link);

	/* Add to output layout — auto-arrange */
	struct wlr_output_layout_output *l_output =
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	output->scene_output = wlr_scene_output_create(server->scene,
		wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout,
		l_output, output->scene_output);

	wlr_log(WLR_INFO, "new output: %s (%dx%d)",
		wlr_output->name,
		wlr_output->width, wlr_output->height);
}

void output_init(struct ew_server *server) {
	server->output_layout = wlr_output_layout_create(server->wl_display);
	wl_list_init(&server->outputs);

	server->scene_layout = wlr_scene_attach_output_layout(
		server->scene, server->output_layout);

	server->new_output.notify = handle_new_output;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);
}
