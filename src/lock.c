/* lock.c — ext-session-lock-v1 implementation
 *
 * SECURITY: This is the lock screen.  When locked:
 * - All input MUST be inhibited from reaching normal clients
 * - Only the lock surface receives keyboard/pointer events
 * - No compositor shortcut can bypass the lock (no emergency unlock)
 * - Recovery from a broken lock is via TTY switch or hardware reboot
 *
 * swaylock uses this protocol.  The YubiKey PAM integration (Phase 2)
 * layers on top of swaylock's PAM conversation — it does not change
 * the Wayland protocol requirement here.
 */

#include <stdlib.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_session_lock_v1.h>

#include "evilway.h"

static void lock_surface_map(struct wl_listener *listener, void *data) {
	(void)listener;
	(void)data;
}

static void lock_surface_destroy(struct wl_listener *listener, void *data) {
	(void)listener;
	(void)data;
}

static void lock_handle_new_surface(struct wl_listener *listener, void *data) {
	struct ew_session_lock *lock = wl_container_of(listener, lock,
		new_surface);
	struct wlr_session_lock_surface_v1 *surface = data;

	/* Create scene surface in the lock layer (topmost) */
	struct wlr_scene_tree *tree = wlr_scene_subsurface_tree_create(
		lock->scene_tree, surface->surface);
	(void)tree;

	/* Configure the lock surface to cover the full output */
	struct wlr_output *output = surface->output;
	wlr_session_lock_surface_v1_configure(surface,
		output->width, output->height);
}

static void lock_handle_unlock(struct wl_listener *listener, void *data) {
	struct ew_session_lock *lock = wl_container_of(listener, lock, unlock);
	(void)data;

	wlr_log(WLR_INFO, "session unlocked");

	/* Destroy the lock layer scene tree */
	wlr_scene_node_destroy(&lock->scene_tree->node);

	/* Recreate the lock layer tree for future locks */
	lock->server->layers[EW_LAYER_LOCK] = wlr_scene_tree_create(
		&lock->server->scene->tree);

	lock->server->active_lock = NULL;

	wl_list_remove(&lock->new_surface.link);
	wl_list_remove(&lock->unlock.link);
	wl_list_remove(&lock->destroy.link);
	free(lock);
}

static void lock_handle_destroy(struct wl_listener *listener, void *data) {
	struct ew_session_lock *lock = wl_container_of(listener, lock, destroy);
	(void)data;

	/* SECURITY: if the lock is destroyed without unlock, we're in
	 * an ambiguous state.  Keep the screen locked (don't clear
	 * active_lock) — better to require a TTY login than to
	 * accidentally expose the session. */
	if (lock->server->active_lock == lock) {
		wlr_log(WLR_ERROR, "lock destroyed without unlock — "
			"session remains locked");
		/* Don't clear active_lock — input remains inhibited */
	}

	wl_list_remove(&lock->new_surface.link);
	wl_list_remove(&lock->unlock.link);
	wl_list_remove(&lock->destroy.link);
	free(lock);
}

static void handle_new_session_lock(struct wl_listener *listener, void *data) {
	struct ew_server *server = wl_container_of(listener, server,
		new_session_lock);
	struct wlr_session_lock_v1 *wlr_lock = data;

	/* Only one lock at a time */
	if (server->active_lock) {
		wlr_session_lock_v1_destroy(wlr_lock);
		return;
	}

	struct ew_session_lock *lock = calloc(1, sizeof(*lock));
	if (!lock) {
		wlr_session_lock_v1_destroy(wlr_lock);
		return;
	}
	lock->lock = wlr_lock;
	lock->server = server;

	/* Use the lock layer of the scene tree */
	lock->scene_tree = server->layers[EW_LAYER_LOCK];

	lock->new_surface.notify = lock_handle_new_surface;
	wl_signal_add(&wlr_lock->events.new_surface, &lock->new_surface);
	lock->unlock.notify = lock_handle_unlock;
	wl_signal_add(&wlr_lock->events.unlock, &lock->unlock);
	lock->destroy.notify = lock_handle_destroy;
	wl_signal_add(&wlr_lock->events.destroy, &lock->destroy);

	server->active_lock = lock;
	wlr_session_lock_v1_send_locked(wlr_lock);

	wlr_log(WLR_INFO, "session locked");
}

void lock_init(struct ew_server *server) {
	server->session_lock_mgr = wlr_session_lock_manager_v1_create(
		server->wl_display);

	server->new_session_lock.notify = handle_new_session_lock;
	wl_signal_add(&server->session_lock_mgr->events.new_lock,
		&server->new_session_lock);
}
