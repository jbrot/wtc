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

#include "log.h"
#include "shl_ring.h"
#include "tmux.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlc/wlc.h>

static struct wtc_tmux *tmux;

struct wtc_output {
	pid_t term_pid;
	wlc_handle term_view;
	struct wlc_event_source *term_out;
	struct shl_ring term_buf;

	// Position and size of top left gridsquare of the terminal
	int term_x, term_y, term_w, term_h;
};

static void wlc_log(enum wlc_log_type type, const char *str)
{
	switch(type) {
	case WLC_LOG_INFO:
		info("[wlc] %s", str);
		break;
	case WLC_LOG_WARN:
		warn("[wlc] %s", str);
		break;
	case WLC_LOG_ERROR:
		crit("[wlc] %s", str);
		break;
	case  WLC_LOG_WAYLAND:
		info("[wayland] %s", str);
	}
}

// Hacky debug function.
static void print_ring(struct shl_ring *ring)
{
	int size = 0;
	struct iovec pts[2];
	size_t ivc = shl_ring_peek(ring, pts);
	wlogs(DEBUG, "Ring: ");
	for (int i = 0; i < ivc; ++i)
		for (int j = 0; j < pts[i].iov_len; ++j)
			wlogm(DEBUG, "%c", ((char *)pts[i].iov_base)[j]);
	wloge(DEBUG);
}

static void parse_output(struct wtc_output *output)
{
	struct shl_ring *ring = &(output->term_buf);
	struct iovec ivc[2];
	size_t sz, pos;
	char val;

	int state = 0; // 0 - text, 1 - w, 2 - h, 3 - x, 4 - y, 5 - skip
	char start[] = "WTC: ";
	int i = 0, x = 0, y = 0, w = 0, h = 0;
	SHL_RING_ITERATE(ring, val, ivc, sz, pos) {
		if (val == '\0')
			continue;

		if (val == '\n') {
			if (state == 4) {
				output->term_x = x;
				output->term_y = y;
				output->term_w = w;
				output->term_h = h;
				debug("parse_output: %dx%d,%d,%d", w, h, x, y);
			}
			i = 0;
			state = 0;
			shl_ring_pop(ring, pos + 1);
			continue;
		}

		switch(state) {
		case 0:
			if (start[i++] != val) {
				state = 5;
				break;
			}
			if (i == strlen(start))
				state = 1;
			break;
		case 1:
			if (val == 'x') {
				state = 2;
				break;
			}
			if (val < '0' || val > '9') {
				state = 5;
				break;
			}
			w = w * 10 + (val - '0');
			break;
		case 2:
			if (val == ',') {
				state = 3;
				break;
			}
			if (val < '0' || val > '9') {
				state = 5;
				break;
			}
			h = h * 10 + (val - '0');
			break;
		case 3:
			if (val == ',') {
				state = 4;
				break;
			}
			if (val < '0' || val > '9') {
				state = 5;
				break;
			}
			x = x * 10 + (val - '0');
			break;
		case 4:
			if (val < '0' || val > '9') {
				state = 5;
				break;
			}
			y = y * 10 + (val - '0');
			break;
		case 5:
			break;
		}
	}
}

static int term_cb(int fd, uint32_t mask, void *userdata)
{
	struct wtc_output *output = userdata;
	debug("term_cb: %d", fd);
	if (mask & WL_EVENT_READABLE) {
		debug("term_cb: Readable : %d", fd);
		int r = read_available(fd, WTC_RDAVL_CSTRING | WTC_RDAVL_RING,
		                       NULL, &(output->term_buf));
		if (r) {
			warn("term_cb: Read error: %d", r);
			return r;
		}

		print_ring(&(output->term_buf));
		parse_output(output);
	}
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		if (mask & WL_EVENT_HANGUP)
			debug("term_cb: HUP: %d", fd);
		if (mask & WL_EVENT_ERROR)
			debug("term_cb: Error: %d", fd);

		wlc_event_source_remove(output->term_out);
		output->term_out = NULL;
	}

	return 0;
}

static int launch_term(struct wtc_output *output)
{
	char *const cl[] = {"/home/jbrot/wlterm/wlterm", NULL};
	int r = 0, s = 0;

	if (!output || output->term_pid || output->term_view ||
	    output->term_out || !shl_ring_empty(&(output->term_buf)))
		return -EINVAL;

	int fout;
	r = fork_exec(cl, &(output->term_pid), NULL, &fout, NULL);
	if (!output->term_pid)
		return r;

	output->term_out = wlc_event_loop_add_fd(fout, WL_EVENT_READABLE,
	                                         term_cb, output);
	if (!output) {
		r = -1;
		warn("launch_term: Couldn't add out to event loop!");
		goto err_pid;
	}

	return r;

err_pid:
	// If we're here, the process is started and we need to stop it...
	kill(output->term_pid, SIGKILL);
	while ((s = waitpid(output->term_pid, NULL, 0)) == -1 &&
	       errno == EINTR) ;
	if (s == -1)
		warn("launch_term: waitpid on child failed with %d", errno);
	output->term_pid = 0;
	if (close(fout))
		warn("launch_term: error closing fout: %d", errno);
	return r;
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
		break;
	}

	if (!found)
		vop = wlc_view_get_output(view);

	if (!vop)
		return false;

	wlc_view_set_output(view, vop);
	wlc_view_set_mask(view, wlc_output_get_mask(vop));
	wlc_view_focus(view);
	debug("New view: %p -- %p -- %d -- %d\n", view, vop, wlc_output_get_mask(vop), wlc_view_get_state(view));

	return true;
}

void wlc_view_dr(wlc_handle view)
{
	wlc_handle vop = wlc_view_get_output(view);
	debug("View destroyed: %p -- %p\n", view, vop);

	/*
	if (wtc_tmux_is_connected(tmux))
		wtc_tmux_disconnect(tmux);
	else
		wtc_tmux_connect(tmux);
	*/

	// If the view doesn't have an output, we're shutting down so we can't 
	// get a list of outputs.
	if (!vop)
		return;

	struct wtc_output *ud = wlc_handle_get_user_data(vop);
	if (ud->term_view != view)
		return;

	ud->term_pid = 0;
	ud->term_view = 0;
	wlc_event_source_remove(ud->term_out); ud->term_out = NULL;
	shl_ring_pop(&(ud->term_buf), ud->term_buf.size);
	launch_term(ud);
}

void wlc_view_rg(wlc_handle view, const struct wlc_geometry *geom)
{
	// Ignore request
}

bool wlc_out_cr(wlc_handle output)
{
	struct wtc_output *ud;

	debug("New output: %p -- %s -- %p\n", output, wlc_output_get_name(output), wlc_handle_get_user_data(output));

	if (wlc_handle_get_user_data(output) == NULL) {
		ud = malloc(sizeof(struct wtc_output));
		if (!ud) {
			crit("Could not allocate output data!");
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
	debug("Res: %d x %d\n", sz->w, sz->h);

	// TODO This needs to be more extensible
	wlc_output_set_resolution(output, wlc_output_get_resolution(output), 2);

	return true;
}

void wlc_out_dr(wlc_handle output)
{
	debug("Output destroyed: %s\n", wlc_output_get_name(output));
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
	struct sigaction act;
	memset(&act, 0, sizeof(act));
	act.sa_handler = SIG_IGN;
	//sigaction(SIGPIPE, &act, NULL);

	setup_wlc_handlers();
	if (!wlc_init()) {
		return EXIT_FAILURE;
	}

	int r = wtc_tmux_new(&tmux);
	if (r)
		return -r;
	r = wtc_tmux_set_bin_file(tmux, "/usr/local/bin/tmux");
	if (r)
		return -r;
	r = wtc_tmux_set_size(tmux, 150, 25); //164, 50);
	if (r)
		return -r;
	r = wtc_tmux_set_socket_name(tmux, "test"); //164, 50);
	if (r)
		return -r;
	r = wtc_tmux_connect(tmux);
	if (r)
		return -r;

	wlc_run();

	wtc_tmux_disconnect(tmux);
	wtc_tmux_unref(tmux);
	return EXIT_SUCCESS;
}
