#define _POSIX_C_SOURCE 199309L
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <xkbcommon/xkbcommon.h>
#include <wayland-server-protocol.h>
#include <wlr/backend.h>
#include <wlr/session.h>
#include <wlr/types.h>
#include "shared.h"

static void keyboard_key_notify(struct wl_listener *listener, void *data) {
	struct wlr_keyboard_key *event = data;
	struct keyboard_state *kbstate = wl_container_of(listener, kbstate, key);
	uint32_t keycode = event->keycode + 8;
	enum wlr_key_state key_state = event->state;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(kbstate->xkb_state, keycode, &syms);
	for (int i = 0; i < nsyms; ++i) {
		xkb_keysym_t sym = syms[i];
		char name[64];
		int l = xkb_keysym_get_name(sym, name, sizeof(name));
		if (l != -1 && l != sizeof(name)) {
			fprintf(stderr, "Key event: %s %s\n", name,
					key_state == WLR_KEY_PRESSED ? "pressed" : "released");
		}
		if (kbstate->compositor->keyboard_key_cb) {
			kbstate->compositor->keyboard_key_cb(kbstate, sym, key_state);
		}
	}
	xkb_state_update_key(kbstate->xkb_state, keycode,
		event->state == WLR_KEY_PRESSED ?  XKB_KEY_DOWN : XKB_KEY_UP);
}

static void input_add_notify(struct wl_listener *listener, void *data) {
	struct wlr_input_device *device = data;
	struct compositor_state *state = wl_container_of(listener, state, input_add);
	if (device->type != WLR_INPUT_DEVICE_KEYBOARD) {
		return;
	}
	struct keyboard_state *kbstate = calloc(sizeof(struct keyboard_state), 1);
	kbstate->device = device;
	kbstate->compositor = state;
	wl_list_init(&kbstate->key.link);
	kbstate->key.notify = keyboard_key_notify;
	wl_signal_add(&device->keyboard->events.key, &kbstate->key);
	wl_list_insert(&state->keyboards, &kbstate->link);

	struct xkb_rule_names rules;
	memset(&rules, 0, sizeof(rules));
	rules.rules = getenv("XKB_DEFAULT_RULES");
	rules.model = getenv("XKB_DEFAULT_MODEL");
	rules.layout = getenv("XKB_DEFAULT_LAYOUT");
	rules.variant = getenv("XKB_DEFAULT_VARIANT");
	rules.options = getenv("XKB_DEFAULT_OPTIONS");
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!context) {
		fprintf(stderr, "Failed to create XKB context\n");
		exit(1);
	}
	kbstate->keymap = xkb_map_new_from_names(
			context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!kbstate->keymap) {
		fprintf(stderr, "Failed to create XKB keymap\n");
		exit(1);
	}
	xkb_context_unref(context);
	kbstate->xkb_state = xkb_state_new(kbstate->keymap);
	if (!kbstate->xkb_state) {
		fprintf(stderr, "Failed to create XKB state\n");
		exit(1);
	}
}

static void input_remove_notify(struct wl_listener *listener, void *data) {
	struct wlr_input_device *device = data;
	struct compositor_state *state = wl_container_of(listener, state, input_add);
	if (device->type != WLR_INPUT_DEVICE_KEYBOARD) {
		return;
	}
	struct keyboard_state *kbstate = NULL, *_kbstate;
	wl_list_for_each(_kbstate, &state->keyboards, link) {
		if (_kbstate->device == device) {
			kbstate = kbstate;
			break;
		}
	}
	if (!kbstate) {
		return; // We are unfamiliar with this keyboard
	}
	wl_list_remove(&kbstate->link);
	wl_list_remove(&kbstate->key.link);
}

static void output_frame_notify(struct wl_listener *listener, void *data) {
	struct output_state *output = wl_container_of(listener, output, frame);
	struct compositor_state *compositor = output->compositor;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (compositor->output_frame_cb) {
		compositor->output_frame_cb(output, &now);
	}

	output->last_frame = now;
	compositor->last_frame = now;
}

static void output_add_notify(struct wl_listener *listener, void *data) {
	struct wlr_output *output = data;
	struct compositor_state *state = wl_container_of(listener, state, output_add);
	fprintf(stderr, "Output '%s' added\n", output->name);
	fprintf(stderr, "%s %s %"PRId32"mm x %"PRId32"mm\n", output->make, output->model,
		output->phys_width, output->phys_height);
	wlr_output_set_mode(output, output->modes->items[0]);
	struct output_state *ostate = calloc(1, sizeof(struct output_state));
	clock_gettime(CLOCK_MONOTONIC, &ostate->last_frame);
	ostate->output = output;
	ostate->compositor = state;
	ostate->frame.notify = output_frame_notify;
	wl_list_init(&ostate->frame.link);
	wl_signal_add(&output->events.frame, &ostate->frame);
	wl_list_insert(&state->outputs, &ostate->link);
	if (state->output_add_cb) {
		state->output_add_cb(ostate);
	}
}

static void output_remove_notify(struct wl_listener *listener, void *data) {
	struct wlr_output *output = data;
	struct compositor_state *state = wl_container_of(listener, state, output_remove);
	struct output_state *ostate = NULL, *_ostate;
	wl_list_for_each(_ostate, &state->outputs, link) {
		if (_ostate->output == output) {
			ostate = _ostate;
			break;
		}
	}
	if (!ostate) {
		return; // We are unfamiliar with this output
	}
	if (state->output_remove_cb) {
		state->output_remove_cb(ostate);
	}
	wl_list_remove(&ostate->link);
	wl_list_remove(&ostate->frame.link);
}

void compositor_init(struct compositor_state *state) {
	memset(state, 0, sizeof(struct compositor_state));

	state->display = wl_display_create();
	state->event_loop = wl_display_get_event_loop(state->display);
	state->session = wlr_session_start(state->display);
	if (!state->session
			|| !state->display
			|| !state->event_loop) {
		exit(1);
	}

	wl_list_init(&state->keyboards);
	wl_list_init(&state->input_add.link);
	state->input_add.notify = input_add_notify;
	wl_list_init(&state->input_remove.link);
	state->input_remove.notify = input_remove_notify;

	wl_list_init(&state->outputs);
	wl_list_init(&state->output_add.link);
	state->output_add.notify = output_add_notify;
	wl_list_init(&state->output_remove.link);
	state->output_remove.notify = output_remove_notify;

	struct wlr_backend *wlr = wlr_backend_autocreate(
			state->display, state->session);
	if (!wlr) {
		exit(1);
	}
	wl_signal_add(&wlr->events.input_add, &state->input_add);
	wl_signal_add(&wlr->events.input_remove, &state->input_remove);
	wl_signal_add(&wlr->events.output_add, &state->output_add);
	wl_signal_add(&wlr->events.output_remove, &state->output_remove);
	state->backend = wlr;

	clock_gettime(CLOCK_MONOTONIC, &state->last_frame);
}

void compositor_run(struct compositor_state *state) {
	if (!wlr_backend_init(state->backend)) {
		fprintf(stderr, "Failed to initialize backend\n");
		exit(1);
	}

	while (!state->exit) {
		wl_event_loop_dispatch(state->event_loop, 0);
	}

	wlr_backend_destroy(state->backend);
	wlr_session_finish(state->session);
	wl_display_destroy(state->display);
}
