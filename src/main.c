/*
 * wtc - main.c
 *
 * Copyright (c) 2017 Joshua Brot <jbrot@umich.edu>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wlc/wlc.h>

struct wtc_output {
	pid_t term_pid;
	wlc_handle term_view;
};

void launch_term(struct wtc_output *output)
{
	char *const cl[] = {"/home/jbrot/wlterm/wlterm", NULL};
	pid_t p;

	if (!output)
		return;

	if (output->term_pid || output->term_view)
		return;

	if ((p = fork()) == 0) {
		setsid();
		freopen("/dev/null", "w", stdout);
		freopen("/dev/null", "w", stderr);
		execvp(cl[0], cl);
		_exit(EXIT_FAILURE);
	} else if (p < 0) {
		// TODO Logging
		fprintf(stderr, "Failed to fork for '%s'", cl[0]);
	} else {
		output->term_pid = p;
	}
}

void wlc_log(enum wlc_log_type type, const char *str)
{
	// Swallow for now
	printf("[wlc] %s\n", str);
}

bool wlc_view_cr(wlc_handle view)
{
	// First check if this is a terminal and handle appropriately.
	pid_t pid = wlc_view_get_pid(view);
	wlc_handle vop = 0;

	size_t count;
	const wlc_handle *outputs = wlc_get_outputs(&count);
	bool found = false;
	for (int i = 0; i < count; ++i)
	{
		struct wtc_output *ud = wlc_handle_get_user_data(outputs[i]);
		if (!ud || ud->term_pid != pid)
			continue;

		// Only one output should be waiting for this process. If multiple are
		// then, we assign ourselves to the first one and launch a new
		// terminal for the others.
		if (found)
		{
			ud->term_pid = 0;
			if (ud->term_view)
				wlc_view_close(ud->term_view);
			launch_term(ud);
			continue;
		}

		ud->term_view = view;
		vop = outputs[i];
		found = true;

		const struct wlc_geometry g = {
			.origin = {
				.x = 0,
				.y = 0,
			},
			.size = *wlc_output_get_virtual_resolution(vop),
		};

		wlc_view_set_geometry(view, 0, &g);
	}

	if (!found)
		vop = wlc_view_get_output(view);

	if (!vop)
		return false;

	wlc_view_set_output(view, vop);
	wlc_view_set_mask(view, wlc_output_get_mask(vop));
	wlc_view_focus(view);
	printf("New view: %p -- %p -- %d -- %d\n", view, vop, wlc_output_get_mask(vop), wlc_view_get_state(view));

	return true;
}

void wlc_view_dr(wlc_handle view)
{
	wlc_handle vop = wlc_view_get_output(view);
	printf("View destroyed: %p -- %p\n", view, vop);

	// If the view doesn't have an output, we're shutting down so we can't get
	// a list of outputs.
	if (!vop)
		return;
	size_t count;
	const wlc_handle *outputs = wlc_get_outputs(&count);
	for (int i = 0; i < count; ++i)
	{
		struct wtc_output *ud = wlc_handle_get_user_data(outputs[i]);
		if (!ud || ud->term_view != view)
			continue;

		ud->term_pid = 0;
		ud->term_view = 0;
		launch_term(ud);
		return;
	}
}

void wlc_view_rg(wlc_handle view, const struct wlc_geometry *geom)
{
	// Ignore request
}

bool wlc_out_cr(wlc_handle output)
{
	struct wtc_output *ud;

	printf("New output: %p -- %s -- %p\n", output, wlc_output_get_name(output), wlc_handle_get_user_data(output));

	if (wlc_handle_get_user_data(output) == NULL) {
		ud = malloc(sizeof(struct wtc_output));
		if (!ud) {
			// TODO Logging mechanism
			fprintf(stderr, "ERROR: Could not allocate output data!");
			return false;
		}
		memset(ud, 0, sizeof(struct wtc_output));
		wlc_handle_set_user_data(output, ud);

	} else {
		ud = wlc_handle_get_user_data(output);
	}

	if (!ud->term_pid)
		launch_term(ud);

	const struct wlc_size *sz = wlc_output_get_resolution(output);
	printf("Res: %d x %d\n", sz->w, sz->h);

	// TODO This needs to be more extensible
	wlc_output_set_resolution(output, wlc_output_get_resolution(output), 2);

	return true;
}

void wlc_out_dr(wlc_handle output)
{
	printf("Output destroyed: %s\n", wlc_output_get_name(output));
	if (wlc_handle_get_user_data(output) != NULL)
		free(wlc_handle_get_user_data(output));
}

bool wlc_kbd(wlc_handle view, uint32_t time, const struct wlc_modifiers *mods,
             uint32_t key, enum wlc_key_state state)
{
	uint32_t sym = wlc_keyboard_get_keysym_for_key(key, NULL);

	switch(sym)
	{
	case XKB_KEY_q:
		if (state != WLC_KEY_STATE_PRESSED || mods->mods != WLC_BIT_MOD_LOGO)
			break;

		wlc_terminate();
		return true;
	}

	return false;
}

bool wlc_ptr(wlc_handle view, uint32_t time, const struct wlc_point *pos)
{
	wlc_pointer_set_position(pos);
	return false;
}

void setup_wlc_handlers(void)
{
	wlc_log_set_handler(wlc_log);

	wlc_set_keyboard_key_cb(wlc_kbd);
	wlc_set_pointer_motion_cb(wlc_ptr);

	wlc_set_output_created_cb(wlc_out_cr);
	wlc_set_output_destroyed_cb(wlc_out_dr);

	wlc_set_view_created_cb(wlc_view_cr);
	wlc_set_view_destroyed_cb(wlc_view_dr);
	wlc_set_view_request_geometry_cb(wlc_view_rg);
}

int main(int argc, char **argv)
{
	setup_wlc_handlers();
	if (!wlc_init()) {
		return EXIT_FAILURE;
	}

	wlc_run();
	return EXIT_SUCCESS;
}
