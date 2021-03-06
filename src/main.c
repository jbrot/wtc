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
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlc/wlc.h>

static struct wtc_tmux *tmux;

struct wtc_output {
	struct wlc_event_source *create_timer;

	pid_t term_pid;
	wlc_handle term_view;
	struct wlc_event_source *term_out;
	struct shl_ring term_buf;

	// Position and size of top left gridsquare of the terminal
	int term_x, term_y, term_w, term_h;

	const struct wtc_tmux_client *client;
	char *table_name;
	int tnlen;
};

struct wtc_view {
	pid_t pane_pid;
	const struct wtc_tmux_pane *pane;
};

static void reposition_view(wlc_handle view);

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

static int set_table_name(struct wtc_output *op, const char *name)
{
	int len;

	if (!op || !name)
		return -EINVAL;

	len = strlen(name);
	if (len > op->tnlen) {
		char *tmp = realloc(op->table_name, (len + 1) * sizeof(char));
		if (!tmp) {
			crit("set_table_name: Could not resize table_name!");
			return -ENOMEM;
		}
		op->table_name = tmp;
		op->tnlen = len + 1;
	}

	strcpy(op->table_name, name);
	return 0;
}

static const struct wtc_tmux_client *get_client(wlc_handle output)
{
	struct wtc_output *ud = wlc_handle_get_user_data(output);
	if (!ud)
		return NULL;
	if (ud->client)
		return ud->client;

	// Now search through the available clients for ours.
	const struct wtc_tmux_session *sess = wtc_tmux_root_session(tmux);
	const struct wtc_tmux_client *client;
	pid_t pid = 0;
	for ( ; sess; sess = sess->hh.next) {
		for (client = sess->clients; client; client = client->next) {
			pid = client->pid;
			do {
				if (pid == ud->term_pid) {
					info("get_client: Identified client: %s!",
					     client->name);
					ud->client = client;
					return client;
				}
				if (get_parent_pid(pid, &pid))
					return NULL;
			} while (pid > 0);
		}
	}

	warn("get_client: Couldn't find client!");

	return NULL;
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
	pid_t pid = wlc_view_get_pid(view);
	wlc_handle vop = wlc_view_get_output(view);

	if (!vop)
		return false;

	struct wtc_output *ud = wlc_handle_get_user_data(vop);
	if (ud && ud->term_pid == pid) {
		ud->term_view = view;

		const struct wlc_geometry g = {
			.origin = {
				.x = 0,
				.y = 0,
			},
			.size = *wlc_output_get_virtual_resolution(vop),
		};

		wlc_view_set_geometry(view, 0, &g);
		wlc_view_set_mask(view, wlc_output_get_mask(vop));
		wlc_view_focus(view);
	} else if (wlc_view_get_parent(view) && 
	           wlc_view_positioner_get_anchor_rect(view)) {
		reposition_view(view);
	} else {
		const char *cmd[] = { "split-window", "-t", NULL, "-PF",
		                      "#{pane_pid}", NULL, NULL };
		const struct wtc_tmux_client *client;
		char *dpane = NULL, *dcmd = NULL;
		char *out = NULL;

		client = get_client(vop);
		if (!client)
			return false;
		if (bprintf(&dpane, "%%%d",
		            client->session->active_window->active_pane->id))
			return false;
		if (bprintf(&dcmd, "echo \"PID: %u\"; sleep infinity", pid)) {
			free(dpane);
			return false;
		}

		cmd[2] = dpane;
		cmd[5] = dcmd;

		wtc_tmux_exec(tmux, cmd, &out, NULL);
		if (!out) {
			free(dcmd);
			free(dpane);
			return false;
		}

		struct wtc_view *vud = calloc(1, sizeof(struct wtc_view));
		if (!vud) {
			crit("wlc_view_cr: Couldn't allocate view user data!");
			free(out);
			free(dcmd);
			free(dpane);
			return false;
		}
		vud->pane_pid = atoi(out);
		wlc_handle_set_user_data(view, vud);

		wlc_view_set_mask(view, 0);

		free(out);
		free(dcmd);
		free(dpane);
	}

	debug("New view: %p -- %p -- %d -- %d\n", view, vop, wlc_output_get_mask(vop), wlc_view_get_state(view));

	return true;
}

void wlc_view_dr(wlc_handle view)
{
	wlc_handle vop = wlc_view_get_output(view);
	debug("View destroyed: %p -- %p\n", view, vop);

	if (!vop)
		return;

	struct wtc_output *oud = wlc_handle_get_user_data(vop);
	if (oud->term_view == view) {
		oud->term_pid = 0;
		oud->term_view = 0;
		// TODO we should probably have a better method of determining when
		// the client no longer exists.
		oud->client = NULL;
		wlc_event_source_remove(oud->term_out); oud->term_out = NULL;
		shl_ring_pop(&(oud->term_buf), oud->term_buf.size);
		launch_term(oud);
	} else {
		const char *cmd[] = { "kill-pane", "-t", NULL, NULL };
		char *dyn = NULL;
		struct wtc_view *vud = wlc_handle_get_user_data(view);
		if (!vud)
			return;

		if (!vud->pane)
			return;

		if (bprintf(&dyn, "%%%u", vud->pane->id))
			return;

		cmd[2] = dyn;
		wtc_tmux_exec(tmux, cmd, NULL, NULL);
		free(dyn);
	}
}

void wlc_view_rg(wlc_handle view, const struct wlc_geometry *geom)
{
	// Ignore request
}

static int create_cb(void *dt)
{
	struct wtc_output *ud = dt;

	if (getenv("DISPLAY")) {
		launch_term(ud);
		return 0;
	}

	if (wlc_event_source_timer_update(ud->create_timer, 10))
		return 0;

	warn("create_cb: Error continuing start timer!");
	return 1;
}

struct wlc_event_source *ev;
bool wlc_out_cr(wlc_handle output)
{
	struct wtc_output *ud;

	debug("New output: %p -- %s -- %p\n", output, wlc_output_get_name(output), wlc_handle_get_user_data(output));

	if (wlc_handle_get_user_data(output) == NULL) {
		ud = calloc(1, sizeof(struct wtc_output));
		if (!ud) {
			crit("wlc_out_cr: Could not allocate output data!");
			return false;
		}
		ud->tnlen = 10;
		ud->table_name = calloc(10, sizeof(char));
		if (!ud->table_name) {
			crit("wlc_out_cr: Could not allocate table name!");
			free(ud);
			return false;
		}
		strcpy(ud->table_name, "root");
		wlc_handle_set_user_data(output, ud);
	} else {
		ud = wlc_handle_get_user_data(output);
	}

	if (!ud->term_pid) {
		if (getenv("DISPLAY")) {
			launch_term(ud);
		} else {
			if (!ud->create_timer) {
				ud->create_timer = wlc_event_loop_add_timer(create_cb, ud);
				if (!ud->create_timer) {
					warn("wlc_out_cr: Couldn't create timer!");
					return false;
				}
			}
			wlc_event_source_timer_update(ud->create_timer, 10);
		}
	}

	const struct wlc_size *sz = wlc_output_get_resolution(output);
	debug("Res: %d x %d\n", sz->w, sz->h);

	// TODO This needs to be more extensible
	wlc_output_set_resolution(output, wlc_output_get_resolution(output), 1);

	return true;
}

void wlc_out_dr(wlc_handle output)
{
	debug("Output destroyed: %s\n", wlc_output_get_name(output));
	struct wtc_output *ud = wlc_handle_get_user_data(output);
	if (!ud)
		return;

	if (ud->create_timer)
		wlc_event_source_remove(ud->create_timer);
	if (ud->term_out)
		wlc_event_source_remove(ud->term_out);

	free(ud->table_name);
	free(ud);
}

bool wlc_kbd(wlc_handle view, uint32_t time,
             const struct wlc_modifiers *mods, uint32_t key,
             enum wlc_key_state state)
{
	const struct wtc_tmux_key_table *table;
	const struct wtc_tmux_key_bind *bind;
	const struct wtc_tmux_client *client;
	struct wtc_output *ud;
	wlc_handle output;
	uint32_t sym, chr;
	key_code code;

	// TODO Handle repeated keys
	if (state != WLC_KEY_STATE_PRESSED)
		return false;

	sym = wlc_keyboard_get_keysym_for_key(key, NULL);
	if (sym == XKB_KEY_q && state == WLC_KEY_STATE_PRESSED
	                     && mods->mods == WLC_BIT_MOD_CTRL) {
		wlc_terminate();
		return true;
	}

	if (!view)
		return false;

	output = wlc_view_get_output(view);
	if (!output)
		return false;

	ud = wlc_handle_get_user_data(output);
	if (!ud)
		return false;

	client = get_client(output);
	if (!client)
		return false;

	if (ud->term_view == view)
		goto ignore;

	table = wtc_tmux_lookup_key_table(tmux, ud->table_name);
	if (!table)
		goto ignore;

	chr = wlc_keyboard_get_utf32_for_key(key, mods);
	code = key_code_from_xkb_key_char(sym, chr);
	if (code < 128)
		debug("Pressed: %c", code);

	if (code == KEYC_NONE || code == KEYC_UNKNOWN)
		return false;

	if (mods->mods & WLC_BIT_MOD_ALT)
		code |= KEYC_ESCAPE;

	// 104 - 8
	info("KEY: %c - %u - %u", chr, sym, code);

	HASH_FIND(hh, table->binds, &code, sizeof(code), bind);
	if (bind) {
		if (set_table_name(ud, bind->next_table->name))
			return false;
		// If we didn't switch back to root then this is a switch command
		// so we don't execute anything
		if (strcmp(bind->next_table->name, "root") != 0)
			return true;

		wtc_tmux_session_exec(tmux, client->session, bind->cmd, NULL, NULL);
		return true;
	}

	if (strcmp(ud->table_name, "root") == 0) {
		if (code == client->session->prefix ||
		    code == client->session->prefix2) {
			strcpy(ud->table_name, "prefix");
			return true;
		}
	}

ignore:
	strcpy(ud->table_name, "root");
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

/*
 * Based on the current tmux state, should the given view be displayed?
 * Returns 1 if yes, 0 if no. If there is missing information, returns -1.
 */
static int is_visible(wlc_handle view)
{
	const struct wtc_tmux_client *client;
	struct wtc_view *vud;
	wlc_handle output;

	// If we have a positioner, defer to our parent (if it exists)
	if (wlc_view_positioner_get_anchor_rect(view)) {
		wlc_handle parent = wlc_view_get_parent(view);
		if (parent)
			return is_visible(parent);
	}

	output = wlc_view_get_output(view);
	if (!output)
		return -1;

	client = get_client(output);
	if (!client)
		return -1;

	vud = wlc_handle_get_user_data(view);
	if (!vud || !vud->pane)
		return -1;

	return vud->pane->window == client->session->active_window &&
	       !(vud->pane->in_mode) && vud->pane->w > 0 && vud->pane->h > 0;
}

static void reposition_view(wlc_handle view)
{
	const struct wlc_geometry *anchor_rect;
	const struct wtc_tmux_client *client;
	struct wtc_output *pud;
	struct wtc_view *vud;
	wlc_handle output, parent;

	switch (is_visible(view)) {
	case -1:
		return;
	case 0:
		wlc_view_set_mask(view, 0);
		return;
	default:
		break;
	}

	/* Adapted from wlc example. */
	anchor_rect = wlc_view_positioner_get_anchor_rect(view);
	parent = wlc_view_get_parent(view);
	if (anchor_rect && parent) {
		struct wlc_size size_req = *wlc_view_positioner_get_size(view);
		if ((size_req.w <= 0) || (size_req.h <= 0))
			size_req = wlc_view_get_geometry(view)->size;

		const struct wlc_geometry *pg = wlc_view_get_geometry(parent);
		const struct wlc_geometry g = {
			.origin = {
				.x = pg->origin.x + anchor_rect->origin.x,
				.y = pg->origin.y + anchor_rect->origin.y,
			},
			.size = size_req,
		};
		wlc_view_set_geometry(view, 0, &g);
		wlc_view_set_mask(view, wlc_output_get_mask(output));
		return;
	}

	output = wlc_view_get_output(view);
	if (!output)
		return;

	pud = wlc_handle_get_user_data(output);
	if (!pud)
		return;

	client = get_client(output);
	if (!client)
		return;

	vud = wlc_handle_get_user_data(view);
	if (!vud || !vud->pane)
		return;

	int offset = client->session->statusbar == WTC_TMUX_SESSION_TOP
	              ? 1 : 0;
	const struct wlc_geometry g = {
		.origin = {
			.x = pud->term_x + pud->term_w * vud->pane->x,
			.y = pud->term_y + pud->term_h * (vud->pane->y + offset),
		},
		.size = {
			.w = pud->term_w * vud->pane->w,
			.h = pud->term_h * vud->pane->h,
		},
	};
	wlc_view_set_geometry(view, 0, &g);
	wlc_view_set_mask(view, wlc_output_get_mask(output));

	if (vud->pane->active)
		wlc_view_focus(view);
}

static void reposition_output(wlc_handle output)
{
	const wlc_handle *views;
	const struct wtc_tmux_client *client;
	struct wtc_output *oud;
	struct wtc_view *vud;
	size_t vc;
	bool found;

	client = get_client(output);
	if (!client)
		return;

	// Since we have a client, we know this exists.
	oud = wlc_handle_get_user_data(output);
	if (!oud->term_view)
		return;

	found = false;
	views = wlc_output_get_views(output, &vc);
	for (int i = 0; i < vc; ++i) {
		reposition_view(views[i]);
		vud = wlc_handle_get_user_data(views[i]);
		if (!vud || !vud->pane)
			continue;
		if (vud->pane->window != client->session->active_window ||
		    !vud->pane->active)
			continue;

		// N.B. This counts an error as visible.
		found = is_visible(views[i]);
	}
	if (!found)
		wlc_view_focus(oud->term_view);
}

static int tmux_new_pane(struct wtc_tmux *tmux, 
                         const struct wtc_tmux_pane *pane)
{
	const wlc_handle *outputs, *views;
	struct wtc_view *ud;
	size_t opc, vc;

	debug("Pane created: %p %u", pane, pane->id);

	outputs = wlc_get_outputs(&opc);
	for (int i = 0; i < opc; ++i) {
		views = wlc_output_get_views(outputs[i], &vc);
		for (int j = 0; j < vc; ++j) {
			ud = wlc_handle_get_user_data(views[j]);
			if (!ud)
				continue;

			if (ud->pane_pid != pane->pid)
				continue;

			ud->pane = pane;
			reposition_view(views[j]);
			if (pane->active)
				wlc_view_focus(views[j]);
			return 0;
		}
	}

	return 0;
}

static int tmux_pane_closed(struct wtc_tmux *tmux, 
                            const struct wtc_tmux_pane *pane)
{
	const wlc_handle *outputs, *views;
	struct wtc_view *ud;
	size_t opc, vc;

	debug("Pane closed: %p %u", pane, pane->id);

	outputs = wlc_get_outputs(&opc);
	for (int i = 0; i < opc; ++i) {
		views = wlc_output_get_views(outputs[i], &vc);
		for (int j = 0; j < vc; ++j) {
			ud = wlc_handle_get_user_data(views[j]);
			if (!ud)
				continue;

			if (ud->pane != pane)
				continue;

			ud->pane = NULL;
			wlc_view_close(views[j]);
			return 0;
		}
	}

	return 0;
}

static int tmux_pane_resized(struct wtc_tmux *tmux, 
                             const struct wtc_tmux_pane *pane)
{
	const wlc_handle *outputs;
	size_t opc;

	debug("Pane resized: %p %u", pane, pane->id);

	outputs = wlc_get_outputs(&opc);
	for (int i = 0; i < opc; ++i)
		reposition_output(outputs[i]);

	return 0;
}

static int tmux_pane_mode_changed(struct wtc_tmux *tmux, 
                                  const struct wtc_tmux_pane *pane)
{
	const wlc_handle *outputs;
	size_t opc;

	debug("Pane changed mode: %p %u", pane, pane->id);

	outputs = wlc_get_outputs(&opc);
	for (int i = 0; i < opc; ++i)
		reposition_output(outputs[i]);

	return 0;
}

static int tmux_window_pane_changed(struct wtc_tmux *tmux,
                                       const struct wtc_tmux_window *wind)
{
	const wlc_handle *outputs;
	size_t opc;

	debug("Pane changed: %p %u", wind, wind->id);

	outputs = wlc_get_outputs(&opc);
	for (int i = 0; i < opc; ++i)
		reposition_output(outputs[i]);

	return 0;
}

static int tmux_session_window_changed(struct wtc_tmux *tmux,
                                       const struct wtc_tmux_session *sess)
{
	const wlc_handle *outputs;
	size_t opc;

	debug("Window changed: %p %u", sess, sess->id);

	outputs = wlc_get_outputs(&opc);
	for (int i = 0; i < opc; ++i)
		reposition_output(outputs[i]);

	return 0;
}

static int tmux_client_session_changed(struct wtc_tmux *tmux,
                                       const struct wtc_tmux_client *client)
{
	const wlc_handle *outputs;
	size_t opc;

	debug("Client moved: %p %s", client, client->name);

	outputs = wlc_get_outputs(&opc);
	for (int i = 0; i < opc; ++i)
		reposition_output(outputs[i]);

	return 0;
}

static int setup_tmux_handlers(struct wtc_tmux *tmux)
{
	int r = 0;

	r =         wtc_tmux_set_new_pane_cb(tmux, tmux_new_pane);
	r = r ? r : wtc_tmux_set_pane_closed_cb(tmux, tmux_pane_closed);
	r = r ? r : wtc_tmux_set_pane_resized_cb(tmux, tmux_pane_resized);
	r = r ? r : wtc_tmux_set_pane_mode_changed_cb(tmux,
	                                             tmux_pane_mode_changed);
	r = r ? r : wtc_tmux_set_window_pane_changed_cb(tmux,
	                                            tmux_window_pane_changed);
	r = r ? r : wtc_tmux_set_session_window_changed_cb(tmux,
	                                           tmux_session_window_changed);
	r = r ? r : wtc_tmux_set_client_session_changed_cb(tmux,
	                                           tmux_client_session_changed);
	return r;
}

int main(int argc, char **argv)
{
	struct sigaction act;
	memset(&act, 0, sizeof(act));
	act.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &act, NULL);

	setup_wlc_handlers();
	if (!wlc_init()) {
		return EXIT_FAILURE;
	}

	int r = wtc_tmux_new(&tmux);
	if (r)
		return -r;
	r = setup_tmux_handlers(tmux);
	if (r)
		return -r;
	r = wtc_tmux_set_bin_file(tmux, "/usr/local/bin/tmux");
	if (r)
		return -r;
	r = wtc_tmux_set_size(tmux, 170, 50); //164, 50);
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
