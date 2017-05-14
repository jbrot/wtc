/*
 * wtc - tmux_parse.c
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

/*
 * wtc_tmux - Process Output Parsing
 *
 * This file contains the functions of the wtc_tmux interface principally
 * dedicated towards the parsing of output from a tmux process.
 */

#include "tmux_internal.h"

#include "log.h"
#include "util.h"

#include <assert.h>
#include <errno.h>
#include <unistd.h>

int wtc_tmux_version_check(struct wtc_tmux *tmux)
{
	int r = 0;
	const char *const cmd[] = { "-V", NULL };
	char *out = NULL;
	r = wtc_tmux_exec(tmux, cmd, &out, NULL);
	if (r != 0)
		goto done;

	// We have a guarantee from the source that the version
	// string contains a space separating the program name
	// from the version. We assume here the version has no
	// space in it.
	char *vst = strrchr(out, ' ');
	if (!vst) {
		warn("wtc_tmux_version_check: No space in version string!");
		goto done;
	}
	++vst; // To get the start of the version string

	// The master branch should be good.
	if (strncmp(vst, "master", 6) == 0) {
		r = 1;
		goto done;
	}

	// Process the version as a double. This may not process the entire
	// version but it should give us a good enough guess to ensure we're
	// past version 2.4. As future versions come out, we may need more cases
	// here.
	double version = atof(vst);
	if (version > 2.4)
		r = 1;

done:
	free(out);
	return r;
}

/*
 * Update the information on the sessions' status bar.
 */
static int update_session_status(struct wtc_tmux *tmux,
                                 struct wtc_tmux_session *sess,
                                 bool gstatus, bool gstop)
{
	int r = 0;
	char *out = NULL;
	bool status, top;

	r = wtc_tmux_get_option(tmux, "status", sess->id,
	                        WTC_TMUX_OPTION_SESSION, &out);
	if (r)
		goto err_out;
	if (strncmp(out, "on", 2) == 0) {
		status = true;
	} else if (strncmp(out, "off", 3) == 0) {
		status = false;
	} else if (strcmp(out, "") == 0) {
		status = gstatus;
	} else {
		warn("update_session_status: Invalid status value: %s", out);
		r = -EINVAL;
		goto err_out;
	}

	free(out); out = NULL;
	r = wtc_tmux_get_option(tmux, "status-position", sess->id,
	                        WTC_TMUX_OPTION_SESSION, &out);
	if (r)
		goto err_out;
	if (strncmp(out, "top", 3) == 0) {
		top = true;
	} else if (strncmp(out, "bottom", 6) == 0) {
		top = false;
	} else if (strcmp(out, "") == 0) {
		top = gstop;
	} else {
		warn("update_session_status: Invalid status-position value: %s",
		     out);
		r = -EINVAL;
		goto err_out;
	}

	sess->statusbar = !status ? WTC_TMUX_SESSION_OFF :
	                   top ? WTC_TMUX_SESSION_TOP : WTC_TMUX_SESSION_BOTTOM;

err_out:
	free(out);
	return r;
}

/*
 * Basic positive integer parsing function, in the vain of atoi. If the
 * string contains a character other than 0-9, returns -1. If the string
 * is empty, returns -1. If the string will overflow an int, returns -1.
 */
static int parseint(const char *str)
{
	int ret = 0;
	if (*str == '\0')
		return -1;

	for ( ; *str != '\0'; str++) {
		if (*str < '0' || *str > '9')
			return -1;
		ret = 10 * ret + (*str - '0');
		if (ret < 0) // Overflow
			return -1;
	}

	return ret;
}

/*
 * Process the given layout string. Whenever the full information regarding
 * a pane is determined, the provided call back will be invoked with the
 * given information. Note that layout will be processed via strtokd and
 * so will be changed after a call to this function. If layout cannot be
 * changed, make a copy before calling.
 *
 * Returns 0 on success and -EINVAL on a parse error. If the callback
 * returns non-zero, parsing will be aborted this function will return
 * the callback's return value.
 */
static int process_layout(char *layout, void *userdata,
	int (*cb)(int pid, int x, int y, int w, int h, void *userdata))
{
	char delim;
	char *saveptr;
	char *token = strtokd(layout, ",x[]{}", &saveptr, &delim);
	if (!token || delim != ',') // Skip the checksum
		return -EINVAL;

	int w, h, x, y, id;
	int state = 0;
	int r = 0;
	while (token = strtokd(NULL, ",x[]{}", &saveptr, &delim)) {
		switch(state) {
		case 0: // width
			if (delim != 'x')
				return -EINVAL;

			w = parseint(token);
			if (w < 0)
				return -EINVAL;

			state = 1;
			break;
		case 1: // height
			if (delim != ',')
				return -EINVAL;

			h = parseint(token);
			if (h < 0)
				return -EINVAL;

			state = 2;
			break;
		case 2: // x
			if (delim != ',')
				return -EINVAL;

			x = parseint(token);
			if (x < 0)
				return -EINVAL;

			state = 3;
			break;
		case 3: // y
			if (delim != ',') { // Larger structure, not actual pane
				state = 0;
				break;
			}

			y = parseint(token);
			if (y < 0)
				return -EINVAL;

			state = 4;
			break;
		case 4: // pid
			if (delim == 'x' || delim == '[' || delim == '{')
				return -EINVAL;

			id = parseint(token);
			if (id < 0)
				return -EINVAL;

			r = cb(id, x, y, w, h, userdata);
			if (r)
				return r;

			state = 0;
			break;
		}
	}

	return 0;
}

static int reload_panes_cb(int pid, int x, int y, int w, int h, void *ud)
{
	struct wtc_tmux *tmux = ud;
	struct wtc_tmux_pane *pane;
	struct wtc_tmux_cb_closure cb;
	bool unchanged;
	HASH_FIND_INT(tmux->panes, &pid, pane);

	if (!pane) {
		warn("reload_panes_cb: Couldn't find pane %d!", pid);
		return -EINVAL;
	}

	// Mark the pane as specified
	if (pane->pid > 0)
		pane->pid *= -1;

	unchanged = (pane->x == x) && (pane->y == y) && 
	            (pane->w == w) && (pane->h == h);

	if (unchanged)
		return 0;

	pane->x = x;
	pane->y = y;
	pane->w = w;
	pane->h = h;

	cb.fid = WTC_TMUX_CB_PANE_RESIZED;
	cb.tmux = tmux;
	cb.value.pane = pane;
	cb.free_after_use = false;
	return wtc_tmux_add_closure(tmux, cb);
}

/*
 * Reload the panes on the server. Note that, when calling this, it is
 * imperative that the windows are already up to date. Depending on where
 * this fails, the server representation may be left in a corrupted state
 * and the only viable method of recovery is to retry this function.
 *
 * WARNING: There is a race condition because there is no synchronization
 * between multiple calls to tmux and this function requires several calls.
 */
int wtc_tmux_reload_panes(struct wtc_tmux *tmux)
{
	int r = 0;
	struct wtc_tmux_cb_closure cb;
	const char *cmd[] = { "list-panes", "-aF",
	                      "#{pane_id} #{window_id} #{pane_active} "
	                      "#{pane_pid}", NULL };
	char *out = NULL;
	r = wtc_tmux_exec(tmux, cmd, &out, NULL);
	if (r < 0) // We swallow non-zero exit to handle no server being up
		goto err_out;

	int count;
	int *pids;
	int *wids;
	int *active;
	int *ppids;
	r = parselniiii("%%%u @%u %u %u%n", out, &count, &pids, &wids, &active,
	                &ppids);
	if (r < 0)
		goto err_out;

	// We now need to synchronize the panes list in the tmux object
	// with the actual panes list. We also clear the linked list while
	// we're at it.
	struct wtc_tmux_pane *pane, *tmp;
	bool found;
	HASH_ITER(hh, tmux->panes, pane, tmp) {
		pane->previous = NULL;
		pane->next = NULL;
		pane->window = NULL;

		found = false;
		for (int i = 0; i < count; ++i) {
			// We don't have a uniqueness guarantee so we need to keep going
			if (pane->id == pids[i]) {
				pids[i] = -pids[i];
				found = true;
			}
		}
		if (found)
			continue;

		HASH_DEL(tmux->panes, pane);

		cb.fid = WTC_TMUX_CB_PANE_CLOSED;
		cb.tmux = tmux;
		cb.value.pane = pane;
		cb.free_after_use = true;
		r = wtc_tmux_add_closure(tmux, cb);
		if (r < 0)
			goto err_pids;
	}

	for (int i = 0; i < count; ++i) {
		if (pids[i] < 0) {
			pids[i] = -pids[i];
			continue;
		}

		// Because of the windows mess, we don't have a uniqueness guarantee
		pane = NULL;
		HASH_FIND_INT(tmux->panes, &pids[i], pane);
		if (pane)
			continue;

		pane = calloc(1, sizeof(struct wtc_tmux_pane));
		if (!pane) {
			crit("wtc_tmux_reload_panes: Couldn't create pane!");
			r = -ENOMEM;
			goto err_pids;
		}
		pane->id = pids[i];
		pane->pid = ppids[i];
		HASH_ADD_INT(tmux->panes, id, pane);

		cb.fid = WTC_TMUX_CB_NEW_PANE;
		cb.tmux = tmux;
		cb.value.pane = pane;
		cb.free_after_use = false;
		r = wtc_tmux_add_closure(tmux, cb);
		if (r < 0)
			goto err_pids;
	}

	// Now to update the linked lists. Although the same window may be
	// listed multiple times, unlike with windows/sessions, a pane will
	// only be associated with one window and so the repeated entries will
	// be identical to the first time.
	// -1 -- determine, 0 -- fill in list, 2 -- skip
	int state;
	struct wtc_tmux_pane *prev;
	struct wtc_tmux_window *wind;
	for (int i = 0; i < count; ++i) {
		if (i == 0 || wids[i] != wids[i - 1]) {
			HASH_FIND_INT(tmux->windows, &wids[i], wind);
			if (!wind) {
				warn("wtc_tmux_reload_panes: Couldn't find window %d!",
				     wids[i]);
				r = -EINVAL;
				goto err_pids;
			}

			state = -1;
			prev = NULL;
		}

		if (state == 2)
			continue;

		HASH_FIND_INT(tmux->panes, &pids[i], pane);
		if (!pane) {
			warn("wtc_tmux_reload_panes: Couldn't find pane %d!", pids[i]);
			r = -EINVAL;
			goto err_pids;
		}

		// There exists a diabolical case here, that forces us to have to
		// check this every time: due to window linking, the same window
		// can actually appear twice in a row meaning our earlier check
		// of wids won't detect the transition so we need to check here to
		// prevent creating a loop. Worse yet, if this window has only one
		// pane pane->next and pane->previous won't be set so we need to
		// add a tertiary check for pane == prev to not miss such a repeat.
		if (pane->next || pane->previous || pane == prev) {
			state = 2;
			continue;
		}

		if (state == -1) {
			state = 0;
			wind->panes = pane;
			wind->pane_count = 0;
		}

		wind->pane_count++;
		pane->window = wind;

		if (active[i] && wind->active_pane != pane) {
			wind->active_pane = pane;

			cb.fid = WTC_TMUX_CB_WINDOW_PANE_CHANGED;
			cb.tmux = tmux;
			cb.value.window = wind;
			cb.free_after_use = false;
			r = wtc_tmux_add_closure(tmux, cb);
			if (r < 0)
				goto err_pids;
		}

		if (prev) {
			prev->next = pane;
			pane->previous = prev;
		}

		prev = pane;
	}

	cmd[0] = "list-windows";
	cmd[1] = "-aF";
	cmd[2] = "#{window_visible_layout}";
	cmd[3] = NULL;
	free(out); out = NULL;
	r = wtc_tmux_exec(tmux, cmd, &out, NULL);
	if (r < 0) // We swallow non-zero exit to handle no server being up
		goto err_pids;
	r = 0; // Actually swallow as we don't necessarily have a next call

	char *saveptr;
	char *token = strtok_r(out, "\n", &saveptr);
	while (token != NULL) {
		r = process_layout(token, tmux, reload_panes_cb);
		if (r < 0) {
			warn("wtc_tmux_reload_panes: Layout processing error: %d", r);
			goto err_layout;
		}

		token = strtok_r(NULL, "\n", &saveptr);
	}

	for (pane = tmux->panes; pane; pane = pane->hh.next) {
		if (pane->pid < 0)
			continue;

		r = reload_panes_cb(pane->id, 0, 0, 0, 0, tmux);
		if (r < 0)
			goto err_layout;
	}

err_layout:
	for (pane = tmux->panes; pane; pane = pane->hh.next)
		if (pane->pid < 0)
			pane->pid *= -1;
err_pids:
	free(pids);
	free(wids);
	free(active);
	free(ppids);
err_out:
	free(out);
	return r;
}

int wtc_tmux_reload_windows(struct wtc_tmux *tmux)
{
	int r = 0;
	struct wtc_tmux_cb_closure cb;
	const char *cmd[] = { "list-windows", "-aF",
	                      "#{window_id} #{session_id} #{window_active}",
	                      NULL };
	char *out = NULL;
	r = wtc_tmux_exec(tmux, cmd, &out, NULL);
	if (r < 0) // We swallow non-zero exit to handle no server being up
		goto err_out;

	int count;
	int *wids;
	int *sids;
	int *active;
	r = parselniii("@%u $%u %u%n", out, &count, &wids, &sids, &active);
	if (r < 0)
		goto err_out;

	// We now need to synchronize the windows list in the tmux object
	// with the actual windows list. We also clear the linked list while
	// we're at it.
	struct wtc_tmux_window *wind, *tmp;
	bool found;
	HASH_ITER(hh, tmux->windows, wind, tmp) {
		found = false;
		for (int i = 0; i < count; ++i) {
			// We don't have a uniqueness guarantee so we need to keep going
			if (wind->id == wids[i]) {
				wids[i] = -wids[i];
				found = true;
			}
		}
		if (found)
			continue;

		HASH_DEL(tmux->windows, wind);

		cb.fid = WTC_TMUX_CB_WINDOW_CLOSED;
		cb.tmux = tmux;
		cb.value.window = wind;
		cb.free_after_use = true;
		r = wtc_tmux_add_closure(tmux, cb);
		if (r < 0)
			goto err_wids;
	}

	for (int i = 0; i < count; ++i) {
		if (wids[i] < 0) {
			wids[i] = -wids[i];
			continue;
		}

		// Because of the windows mess, uniqueness isn't guaranteed
		wind = NULL;
		HASH_FIND_INT(tmux->windows, &wids[i], wind);
		if (wind)
			continue;

		wind = calloc(1, sizeof(struct wtc_tmux_window));
		if (!wind) {
			crit("wtc_tmux_reload_windows: Couldn't allocate window!");
			r = -ENOMEM;
			goto err_wids;
		}
		wind->id = wids[i];
		HASH_ADD_INT(tmux->windows, id, wind);

		cb.fid = WTC_TMUX_CB_NEW_WINDOW;
		cb.tmux = tmux;
		cb.value.window = wind;
		cb.free_after_use = false;
		r = wtc_tmux_add_closure(tmux, cb);
		if (r < 0)
			goto err_wids;
	}

	// Now to update the windows lists
	int wsize = 0;
	struct wtc_tmux_window **windows = NULL;
	struct wtc_tmux_window **windows_tmp = NULL;
	struct wtc_tmux_session *sess = NULL;
	for (int i = 0; i < count; ++i) {
		if (i == 0 || sids[i] != sids[i - 1]) {
			if (sess) {
				free(sess->windows);
				sess->windows = windows;
				windows = NULL;
			}

			HASH_FIND_INT(tmux->sessions, &sids[i], sess);
			if (!sess) {
				warn("wtc_tmux_reload_windows: Couldn't find session %d!",
				     sids[i]);
				r = -EINVAL;
				goto err_wids;
			}

			sess->window_count = 0;
			windows = calloc(4, sizeof(*windows));
			if (!windows) {
				crit("wtc_tmux_reload_windows: Couldn't allocate windows "
				     "list!");
				r = -ENOMEM;
				goto err_wids;
			}
			wsize = 4;
		}

		HASH_FIND_INT(tmux->windows, &wids[i], wind);
		if (!wind) {
			warn("wtc_tmux_reload_windows: Couldn't find window %d!",
			     wids[i]);
			r = -EINVAL;
			goto err_windows;
		}
		windows[sess->window_count] = wind;
		sess->window_count++;
		if (active[i] && sess->active_window != wind) {
			sess->active_window = wind;

			cb.fid = WTC_TMUX_CB_SESSION_WINDOW_CHANGED;
			cb.tmux = tmux;
			cb.value.session = sess;
			cb.free_after_use = false;
			r = wtc_tmux_add_closure(tmux, cb);
			if (r < 0)
				goto err_windows;
		}

		if (sess->window_count == wsize) {
			wsize *= 2;
			windows_tmp = realloc(windows, wsize * sizeof(*windows));
			if (!windows_tmp) {
				crit("wtc_tmux_reload_windows: Couldn't resize windows "
				     "list!");
				r = -ENOMEM;
				goto err_windows;
			}
			windows = windows_tmp;
		}
	}
	if (sess) {
		free(sess->windows);
		sess->windows = windows;
		windows = NULL;
	}

	r = wtc_tmux_reload_panes(tmux);

err_windows:
	free(windows);
err_wids:
	free(wids);
	free(sids);
	free(active);
err_out:
	free(out);
	return r;
}

int wtc_tmux_reload_clients(struct wtc_tmux *tmux)
{
	int r = 0;
	struct wtc_tmux_cb_closure cb;
	const char *cmd[] = { "list-clients", "-F",
	                      "#{session_id} #{client_pid} |#{client_name}",
	                      NULL };
	char *out = NULL;
	r = wtc_tmux_exec(tmux, cmd, &out, NULL);
	if (r < 0) // We swallow non-zero exit to handle no server being up
		goto err_out;

	int count;
	int *sids;
	int *cpids;
	char **names;
	r = parselniis("$%u %u |%n", out, &count, &sids, &cpids, &names);
	if (r < 0)
		goto err_out;

	// We now need to synchronize the clients list in the tmux object
	// with the actual clients list. We also clear the linked list while
	// we're at it.
	struct wtc_tmux_client *client, *tmp;
	HASH_ITER(hh, tmux->clients, client, tmp) {
		client->previous = NULL;
		client->next = NULL;

		for (int i = 0; i < count; ++i) {
			if (client->pid == cpids[i]) {
				cpids[i] = -1;
				goto icont;
			}
		}

		HASH_DEL(tmux->clients, client);
		wtc_tmux_client_free(client);

		icont: ;
	}

	for (int i = 0; i < count; ++i) {
		if (cpids[i] < 0)
			continue;

		client = calloc(1, sizeof(struct wtc_tmux_client));
		if (!client) {
			crit("wtc_tmux_reload_clients: Couldn't create client!");
			r = -ENOMEM;
			goto err_ids;
		}
		client->pid = cpids[i];
		client->name = strdup(names[i]);
		if (!client->name) {
			crit("wtc_tmux_reload_clients: Couldn't create client name!");
			r = -ENOMEM;
			goto err_ids;
		}
		HASH_ADD_KEYPTR(hh, tmux->clients, client->name,
		                strlen(client->name), client);
	}

	// Now to update the linked lists.
	struct wtc_tmux_client *prev;
	struct wtc_tmux_session *sess;
	// First clear the current associations.
	for (sess = tmux->sessions; sess; sess = sess->hh.next)
		sess->clients = NULL;
	// Now fill in the new ones (unlike with the other types, clients aren't
	// sorted by session)
	for (int i = 0; i < count; ++i) {
		HASH_FIND(hh, tmux->clients, names[i], strlen(names[i]), client);
		if (!client) {
			warn("wtc_tmux_reload_clients: Couldn't find client \"%s\"!",
			     names[i]);
			r = -EINVAL;
			goto err_ids;
		}

		if (i == 0 || sids[i] != sids[i - 1]) {
			HASH_FIND_INT(tmux->sessions, &sids[i], sess);
			if (!sess) {
				warn("wtc_tmux_reload_clients: Couldn't find session %d!",
				     sids[i]);
				r = -EINVAL;
				goto err_ids;
			}

			if (sess->clients) {
				for (prev = sess->clients; prev->next; prev = prev->next) ;
			} else {
				prev = NULL;
				sess->clients = client;
			}
		}

		if (prev) {
			prev->next = client;
			client->previous = prev;
		}

		if (client->session != sess) {
			client->session = sess;

			cb.fid = WTC_TMUX_CB_CLIENT_SESSION_CHANGED;
			cb.tmux = tmux;
			cb.value.client = client;
			cb.free_after_use = false;
			r = wtc_tmux_add_closure(tmux, cb);
			if (r < 0)
				goto err_ids;
		}

		prev = client;
	}

err_ids:
	if (names) {
		for (int i = 0; i < count; ++i)
			free(names[i]);
	}
	free(names);
	free(sids);
	free(cpids);
err_out:
	free(out);
	return r;
}

int wtc_tmux_reload_sessions(struct wtc_tmux *tmux)
{
	int r = 0;
	struct wtc_tmux_cb_closure cb;
	const char *cmd[] = { "list-sessions", "-F",
	                      "#{session_id} |#{session_name}", NULL };
	char *out = NULL;
	r = wtc_tmux_exec(tmux, cmd, &out, NULL);
	if (r < 0) // We swallow non-zero exit to handle no server being up
		goto err_out;

	int count;
	int *sids;
	char **names;
	r = parselnis("$%u |%n", out, &count, &sids, &names);
	if (r < 0)
		goto err_out;

	// We now need to synchronize the sessions list in the tmux object
	// with the actual sessions list.
	struct wtc_tmux_session *sess, *tmp;
	HASH_ITER(hh, tmux->sessions, sess, tmp) {
		for (int i = 0; i < count; ++i) {
			if (sess->id == sids[i]) {
				sids[i] = -1;
				goto icont;
			}
		}

		HASH_DEL(tmux->sessions, sess);

		cb.fid = WTC_TMUX_CB_SESSION_CLOSED;
		cb.tmux = tmux;
		cb.value.session = sess;
		cb.free_after_use = true;
		r = wtc_tmux_add_closure(tmux, cb);
		if (r < 0)
			goto err_sids;

		icont: ;
	}

	for (int i = 0; i < count; ++i) {
		if (sids[i] == -1)
			continue;

		sess = calloc(1, sizeof(struct wtc_tmux_session));
		if (!sess) {
			crit("wtc_tmux_reload_sessions: Couldn't allocate session!");
			r = -ENOMEM;
			goto err_sids;
		}
		sess->id = sids[i];
		HASH_ADD_INT(tmux->sessions, id, sess);

		if (strcmp(names[i], WTC_TMUX_TEMP_SESSION_NAME) == 0)
			continue;

		cb.fid = WTC_TMUX_CB_NEW_SESSION;
		cb.tmux = tmux;
		cb.value.session = sess;
		cb.free_after_use = false;
		r = wtc_tmux_add_closure(tmux, cb);
		if (r < 0)
			goto err_sids;
	}

	bool gstatus = true;
	bool gstop = true;
	if (count) {
		free(out); out = NULL;
		r = wtc_tmux_get_option(tmux, "status", 0, WTC_TMUX_OPTION_GLOBAL |
		                        WTC_TMUX_OPTION_SESSION, &out);
		if (r)
			goto err_sids;
		if (strncmp(out, "on", 2) == 0) {
			gstatus = true;
		} else if (strncmp(out, "off", 3) == 0) {
			gstatus = false;
		} else {
			warn("wtc_tmux_reload_sessions: Invalid global status "
			     "value: %s", out);
			r = -EINVAL;
			goto err_sids;
		}

		free(out); out = NULL;
		r = wtc_tmux_get_option(tmux, "status-position", 0,
		                        WTC_TMUX_OPTION_GLOBAL |
		                        WTC_TMUX_OPTION_SESSION, &out);
		if (r)
			goto err_sids;
		if (strncmp(out, "top", 3) == 0) {
			gstop = true;
		} else if (strncmp(out, "bottom", 6) == 0) {
			gstop = false;
		} else {
			warn("wtc_tmux_reload_sessions: Invalid global status-position "
			     "value: %s", out);
			r = -EINVAL;
			goto err_sids;
		}
	}

	for (sess = tmux->sessions; sess; sess = sess->hh.next) {
		r = update_session_status(tmux, sess, gstatus, gstop);
		if (r)
			goto err_sids;
	}

	r = wtc_tmux_reload_windows(tmux);
	if (r)
		goto err_sids;

	r = wtc_tmux_reload_clients(tmux);
	if (r)
		goto err_sids;

	// If we have no sessions, start a temporary session. Otherwise, delete
	// said temporary session if it exists.
	if (!tmux->sessions)
		r = wtc_tmux_cc_launch(tmux, NULL);
err_sids:
	if (names) {
		for (int i = 0; i < count; ++i)
			free(names[i]);
	}
	free(names);
	free(sids);
err_out:
	free(out);
	return r;
}

static void print_status(struct wtc_tmux *tmux)
{
	const struct wtc_tmux_session *sess = wtc_tmux_root_session(tmux);
	const struct wtc_tmux_window *wind;
	const struct wtc_tmux_pane *pane;
	const struct wtc_tmux_client *client;
	for (; sess; sess = sess->hh.next) {
		debug("$%u -- %u -- %u", sess->id, sess->statusbar, sess->window_count);
		for (int i = 0; i < sess->window_count; ++i) {
			wind = sess->windows[i];
			debug("  @%u -- %u", wind->id, wind == sess->active_window);
			for (pane = wind->panes; pane; pane = pane->next)
				debug("    %%%u -- %u -- %u -- %ux%u,%u,%u", pane->id, pane == wind->active_pane, pane->pid, pane->w, pane->h, pane->x, pane->y);
		}
		for (client = sess->clients; client; client = client->next)
			debug("  %s -- %u", client->name, client->pid);
	}
}

int wtc_tmux_refresh_cb(int fd, uint32_t mask, void *userdata)
{
	struct wtc_tmux *tmux = userdata;

	int r = read_available(fd, WTC_RDAVL_DISCARD, NULL, NULL);
	if (r < 0) {
		warn("wtc_tmux_refresh_cb: Error clearing pipe: %d", r);
		return r;
	}

	// We make a copy so that when executing commands that might 
	// inadvertantly change the value, we don't contaminate our list of
	// things to refresh.
	int refresh = tmux->refresh;
	tmux->refresh = 0;

	if (refresh & WTC_TMUX_REFRESH_SESSIONS) {
		r = wtc_tmux_reload_sessions(tmux);
		if (r < 0)
			goto exit;

		refresh = 0;
	}

	if (refresh & WTC_TMUX_REFRESH_WINDOWS) {
		r = wtc_tmux_reload_windows(tmux);
		if (r < 0)
			goto exit;

		refresh &= ~(WTC_TMUX_REFRESH_WINDOWS | WTC_TMUX_REFRESH_PANES);
	}

	if (refresh & WTC_TMUX_REFRESH_PANES) {
		r = wtc_tmux_reload_panes(tmux);
		if (r < 0)
			goto exit;

		refresh &= ~WTC_TMUX_REFRESH_PANES;
	}

	if (refresh & WTC_TMUX_REFRESH_CLIENTS) {
		r = wtc_tmux_reload_clients(tmux);
		if (r < 0)
			goto exit;

		refresh &= ~WTC_TMUX_REFRESH_CLIENTS;
	}

	assert(refresh == 0);

	for (size_t i = 0; i < tmux->closure_size; ++i) {
		r = wtc_tmux_closure_invoke(&(tmux->closures[i]));
		if (r)
			break;
	}

	print_status(tmux);

exit:
	// If there's an error, ensure what we missed gets taken care of next
	// time.
	if (refresh)
		tmux->refresh |= refresh;
	wtc_tmux_clear_closures(tmux);
	return r;
}

int wtc_tmux_queue_refresh(struct wtc_tmux *tmux, int flags)
{
	int r;

	tmux->refresh |= flags;
	r = write(tmux->refreshfd, "", 1);
	if (r < 0) {
		warn("wtc_tmux_queue_refresh: Error writing to pipe: %d", errno);
		return -errno;
	}

	return 0;
}


#define TMUX_CC_BEGIN                    1
#define TMUX_CC_END                      2
#define TMUX_CC_CLIENT_SESSION_CHANGED   3
#define TMUX_CC_EXIT                     4
#define TMUX_CC_LAYOUT_CHANGE            5
#define TMUX_CC_OUTPUT                   6
#define TMUX_CC_PANE_MODE_CHANGED        7
#define TMUX_CC_SESSION_CHANGED          8
#define TMUX_CC_SESSION_RENAMED          9
#define TMUX_CC_SESSION_WINDOW_CHANGED  10
#define TMUX_CC_SESSIONS_CHANGED        11
#define TMUX_CC_UNLINKED_WINDOW_ADD     12
#define TMUX_CC_UNLINKED_WINDOW_CLOSE   13
#define TMUX_CC_UNLINKED_WINDOW_RENAMED 14
#define TMUX_CC_WINDOW_ADD              15
#define TMUX_CC_WINDOW_CLOSE            16
#define TMUX_CC_WINDOW_PANE_CHANGED     17
#define TMUX_CC_WINDOW_RENAMED          18

/*
 * The names of all of the commands, omitting the starting %.
 * tmux_cc_names[i] corresponds with command number i + 1.
 */
static const char * const CC_NAMES[] = { "begin", "end",
	"client-session-changed", "exit", "layout-change", "output",
	"pane-mode-changed", "session-changed", "session-renamed",
	"session-window-changed", "sessions-changed", "unlinked-window-add",
	"unlinked-window-close", "unlinked-window-renamed", "window-add", 
	"window-close", "window-pane-changed", "window-renamed" };
static const int CC_NAMES_LEN = sizeof(CC_NAMES) / sizeof(*CC_NAMES);

/*
 * Scan the start of the start of the ring buffer for the command name. If
 * the command is successfully identified, returns a positive command id.
 * If there is not enough data to fully identifiy the command, returns 0.
 * If there is an invalid/unrecognized command, returns -EINVAL.
 */
static int identify_command(struct wtc_tmux_cc *cc)
{
	struct shl_ring *ring = &(cc->buf);

	struct iovec vecs[2];
	size_t size, pos, len;
	char val;
	int match; // 0 - false, 1 - true, 2 - incomplete

	// Ensure the ring is non-empty
	if (shl_ring_empty(ring))
		return 0;

	for (int i = 0; i < CC_NAMES_LEN; ++i) {
		match = 2;
		len = strlen(CC_NAMES[i]);
		int j = -1;
		SHL_RING_ITERATE(ring, val, vecs, size, pos) {
			if (val == '\0')
				continue;

			if (j == -1) {
				if (val != '%')
					return -EINVAL;

				++j;
				continue;
			}

			if (j == len) {
				match = val == ' ' || val == '\n';
				break;
			}

			if (val != CC_NAMES[i][j]) {
				match = 0;
				break;
			}

			++j;
		}

		if (match == 2)
			return 0;
		else if (match == 1)
			return i + 1;
	}

	return -EINVAL;
}

/*
 * Remove the line at the start of the input of the buffer. Returns 0
 * if there is not a complete line, otherwise returns the number of
 * characters removed.
 */
static int consume_line(struct wtc_tmux_cc *cc)
{
	struct shl_ring *ring = &(cc->buf);

	struct iovec vecs[2];
	size_t size, pos;
	char val;

	SHL_RING_ITERATE(ring, val, vecs, size, pos) {
		if (val != '\n')
			continue;

		shl_ring_pop(ring, pos + 1);
		return pos + 1;
	}

	return 0;
}

static int process_cmd_begin(struct wtc_tmux_cc *cc)
{
	struct shl_ring *ring = &(cc->buf);
	char begin[] = "%begin ";
	char end[] = "%end ";
	char error[] = "%error ";

	struct iovec vecs[2];
	size_t size, pos;
	char val;

	size_t start = 0;
	size_t len = 0;

	int guard = 0;
	long time[2] = {0};
	int cmd[2]   = {0};
	int flags[2] = {0};

	// 0 - text, 1 - time, 2 - cmd, 3 - flags, 4 - unknown, 5 - middle
	int state = 0;
	int index = 0;
	char *match = begin;
	SHL_RING_ITERATE(ring, val, vecs, size, pos) {
		if (val == '\0')
			continue;

		switch (state) {
		case 0:
			if (match[index] != val) {
				if (start)
					state = 5;
				else
					return -EINVAL;
			}

			if (++index == strlen(match))
				state = 1;
			break;
		case 1:
			if (val == ' ') {
				if (start && time[1] != time[0])
					state = 5;
				else
					state = 2;
				break;
			}
			if (val < '0' || val > '9') {
				if (start)
					state = 5;
				else
					return -EINVAL;
			}
			time[guard] *= 10;
			time[guard] += val - '0';
			break;
		case 2:
			if (val == ' ') {
				if (start && cmd[1] != cmd[0])
					state = 5;
				else
					state = 3;
				break;
			}
			if (val < '0' || val > '9') {
				if (start)
					state = 5;
				else
					return -EINVAL;
			}
			cmd[guard] *= 10;
			cmd[guard] += val - '0';
			break;
		case 3:
			if (val == '\n') {
				if (!start) {
					guard = 1;
					index = 0;
					state = 4;
					start = pos + 1;
				} else if (flags[1] != flags[0]) {
					index = 0;
					state = 4;
				} else {
					wlogs(DEBUG, "process_cmd_begin: Processed "
					             "command: \"");
					for (int i = start; i < start + len; ++i) {
						if (i < vecs[0].iov_len)
							wlogm(DEBUG, "%c",
							      ((char *)vecs[0].iov_base)[i]);
						else
							wlogm(DEBUG, "%c", ((char *)vecs[1].iov_base)
							                   [i - vecs[0].iov_len]);
					}
					wlogm(DEBUG, "\"");
					wloge(DEBUG);

					int r = 0;
					if (cc->cmd_cb)
						r = cc->cmd_cb(cc, start, len, match == error);
					shl_ring_pop(ring, pos + 1);
					return r < 0 ? r : pos + 1;
				}
				break;
			}
			if (val < '0' || val > '9') {
				if (start)
					state = 5;
				else
					return -EINVAL;
			}
			flags[guard] *= 10;
			flags[guard] += val - '0';
			break;
		case 4:
			if (val == end[index] && val == error[index]) {
				++index;
				break;
			} else if (val == end[index]) {
				match = end;
				state = 0;
				++index;
				break;
			} else if (val == error[index]) {
				match = error;
				state = 0;
				++index;
				break;
			} else {
				state = 5;
			}
			// Intentionally no "break" here.
		case 5:
			if (val == '\n') {
				len = pos + 1 - start;
				state = 4;
				index = 0;
			}
			break;
		}
	}

	return 0;
}

int wtc_tmux_cc_process_output(struct wtc_tmux_cc *cc)
{
	if (!cc)
		return -EINVAL;

	struct wtc_tmux *tmux = cc->tmux;

	int cmd = 0;
	int r = 0;
	while ((cmd = identify_command(cc)) > 0) {
		debug("wtc_tmux_cc_process_output: Identified command: %d",
		      cmd);
		switch (cmd) {
		case TMUX_CC_BEGIN:
			r = process_cmd_begin(cc);
			if (r <= 0)
				return r;
			break;
		case TMUX_CC_CLIENT_SESSION_CHANGED:
			r = consume_line(cc);
			if (r <= 0)
				return r;
			r = wtc_tmux_queue_refresh(tmux, WTC_TMUX_REFRESH_CLIENTS);
			if (r < 0)
				return r;
			break;
		case TMUX_CC_LAYOUT_CHANGE:
		case TMUX_CC_PANE_MODE_CHANGED:
		case TMUX_CC_WINDOW_PANE_CHANGED:
			r = consume_line(cc);
			if (r <= 0)
				return r;
			r = wtc_tmux_queue_refresh(tmux, WTC_TMUX_REFRESH_PANES);
			if (r < 0)
				return r;
			break;
		case TMUX_CC_SESSIONS_CHANGED:
			r = consume_line(cc);
			if (r <= 0)
				return r;
			r = wtc_tmux_queue_refresh(tmux, WTC_TMUX_REFRESH_SESSIONS);
			if (r < 0)
				return r;
			break;
		case TMUX_CC_SESSION_WINDOW_CHANGED:
		case TMUX_CC_WINDOW_ADD:
		case TMUX_CC_WINDOW_CLOSE:
		case TMUX_CC_UNLINKED_WINDOW_ADD:
		case TMUX_CC_UNLINKED_WINDOW_CLOSE:
			r = consume_line(cc);
			if (r <= 0)
				return r;
			r = wtc_tmux_queue_refresh(tmux, WTC_TMUX_REFRESH_WINDOWS);
			if (r < 0)
				return r;
			break;
		case TMUX_CC_END: // This should be consumed when processing begin
		case TMUX_CC_OUTPUT:
		case TMUX_CC_SESSION_CHANGED:
		case TMUX_CC_SESSION_RENAMED:
		case TMUX_CC_UNLINKED_WINDOW_RENAMED:
		case TMUX_CC_WINDOW_RENAMED:
		default:
			if (!consume_line(cc))
				return 0;
		}
	}

	if (cmd < 0) {
		warn("wtc_tmux_cc_process_output: Couldn't identify command!");
		consume_line(cc); // So we can attempt to move on
		return cmd;
	}

	return 0;
}
