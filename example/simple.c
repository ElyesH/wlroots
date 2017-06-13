#define _POSIX_C_SOURCE 199309L
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <inttypes.h>
#include <wayland-server.h>
#include <GLES3/gl3.h>
#include <wlr/backend.h>
#include <wlr/session.h>
#include <wlr/types.h>
#include <xkbcommon/xkbcommon.h>
#include "shared.h"

struct sample_state {
	float color[3];
	int dec;
};

void handle_output_frame(struct output_state *output, struct timespec *ts) {
	struct compositor_state *state = output->compositor;
	struct sample_state *sample = state->data;

	long ms = (ts->tv_sec - state->last_frame.tv_sec) * 1000 +
		(ts->tv_nsec - state->last_frame.tv_nsec) / 1000000;
	int inc = (sample->dec + 1) % 3;

	sample->color[inc] += ms / 2000.0f;
	sample->color[sample->dec] -= ms / 2000.0f;

	if (sample->color[sample->dec] < 0.0f) {
		sample->color[inc] = 1.0f;
		sample->color[sample->dec] = 0.0f;
		sample->dec = inc;
	}

	glClearColor(sample->color[0], sample->color[1], sample->color[2], 1.0);
	glClear(GL_COLOR_BUFFER_BIT);
}

static void handle_keyboard_key(struct keyboard_state *kbstate,
		xkb_keysym_t sym, enum wlr_key_state key_state) {
	if (sym == XKB_KEY_Escape) {
		kbstate->compositor->exit = true;
	}
}

int main() {
	struct sample_state state = {
		.color = { 1.0, 0.0, 0.0 },
		.dec = 0,
	};
	struct compositor_state compositor;

	compositor_init(&compositor);
	compositor.output_frame_cb = handle_output_frame;
	compositor.keyboard_key_cb = handle_keyboard_key;
	compositor.data = &state;
	compositor_run(&compositor);
}
