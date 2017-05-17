/*
 * wtc - tmux.c
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

#define _GNU_SOURCE

#include "tmux_internal.h"

#include "log.h"
#include "util.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <wait.h>
#include <wayland-server-core.h>
#include <wlc/wlc.h>

void wtc_tmux_pane_free(struct wtc_tmux_pane *pane)
{
	free(pane);
}

void wtc_tmux_window_free(struct wtc_tmux_window *window)
{
	free(window);
}

void wtc_tmux_session_free(struct wtc_tmux_session *sess)
{
	if (!sess)
		return;

	free(sess->windows);
	free(sess);
}

void wtc_tmux_client_free(struct wtc_tmux_client *client)
{
	if (!client)
		return;

	free((void *) client->name);
	free(client);
}

static int sigc_cb(int fd, uint32_t mask, void *userdata)
{
	struct wtc_tmux *tmux = userdata;
	pid_t pid;
	int r = 0;

	r = read_available(fd, WTC_RDAVL_DISCARD, NULL, NULL);
	if (r < 0) {
		warn("sigc_cb: Error clearing SIGCHLD pipe: %d", r);
		return r;
	}

	// Because we're handling this synchronously, we don't have to worry
	// about stealing wtc_tmux_exec's waitpid. However, wtc_exec_tmux can 
	// steal our waitpid, so instead of having a guarantee of at least 1 
	// child we don't even have that.
	struct wtc_tmux_cc *prev, *cc;
	while (pid = waitpid(-1, NULL, WNOHANG)) {
		if (r < 0) {
			if (errno == EINTR)
				continue;

			warn("sigc_cb: waitpid error: %d", errno);
			return r;
		}

		prev = NULL;
		for (cc = tmux->ccs; cc; prev = cc, cc = cc->next) {
			if (cc->pid != pid)
				continue;

			debug("sigc_cb: Removing child %u", pid);

			if (prev) {
				prev->next = cc->next;
				if (cc->next)
					cc->next->previous = prev;
			} else {
				tmux->ccs = cc->next;
				if (cc->next)
					cc->next->previous = NULL;
				else
					wtc_tmux_queue_refresh(tmux, WTC_TMUX_REFRESH_SESSIONS);
			}

			wtc_tmux_cc_unref(cc);
			break;
		}
	}

	return 0;
}

int wtc_tmux_new(struct wtc_tmux **out)
{
	struct wtc_tmux *output = calloc(1, sizeof(*output));
	if (!output) {
		crit("wtc_tmux_new: Couldn't allocate tmux object!");
		return -ENOMEM;
	}

	output->ref = 1;

	output->timeout = 5000;
	output->w = 80;
	output->h = 24;

	*out = output;
	return 0;
}

void wtc_tmux_ref(struct wtc_tmux *tmux)
{
	if (!tmux || !tmux->ref)
		return;

	tmux->ref++;
}

static bool cmd_freeable(struct wtc_tmux *tmux, char *ptr)
{
	return ptr != tmux->bin &&
	       ptr != tmux->socket &&
	       ptr != tmux->socket_path &&
	       ptr != tmux->config;
}

void wtc_tmux_unref(struct wtc_tmux *tmux)
{
	if (!tmux || !tmux->ref || tmux->ref--)
		return;

	if (tmux->connected)
		wtc_tmux_disconnect(tmux);

	for (int i = 0; i < tmux->cmdlen; ++i)
		if (cmd_freeable(tmux, tmux->cmd[i]))
			free(tmux->cmd[i]);
	free(tmux->cmd);

	free(tmux->bin);
	free(tmux->socket);
	free(tmux->socket_path);
	free(tmux->config);

	free(tmux);
}

int wtc_tmux_set_bin_file(struct wtc_tmux *tmux, const char *path)
{
	if (!tmux)
		return -EINVAL;

	if (tmux->connected)
		return -EBUSY;

	char *dup = NULL;
	if (path) {
		dup = strdup(path);
		if (!dup) {
			crit("wtc_tmux_set_bin_file: Couldn't duplicate path!");
			return -ENOMEM;
		}
	}

	free(tmux->bin);
	tmux->bin = dup;

	return 0;
}

const char *wtc_tmux_get_bin_file(const struct wtc_tmux *tmux)
{
	return tmux->bin;
}

int wtc_tmux_set_socket_name(struct wtc_tmux *tmux, const char *name)
{
	if (!tmux)
		return -EINVAL;

	if (tmux->connected)
		return -EBUSY;

	if (!name) {
		free(tmux->socket);
		tmux->socket = NULL;
		return 0;
	}

	char *dup = strdup(name);
	if (!dup) {
		crit("wtc_tmux_set_socket_name: Couldn't duplicate name!");
		return -ENOMEM;
	}

	free(tmux->socket);
	tmux->socket = dup;

	free(tmux->socket_path);
	tmux->socket_path = NULL;

	return 0;
}

int wtc_tmux_set_socket_path(struct wtc_tmux *tmux, const char *path)
{
	if (!tmux)
		return -EINVAL;

	if (tmux->connected)
		return -EBUSY;

	if (!path) {
		free(tmux->socket_path);
		tmux->socket_path = NULL;
		return 0;
	}

	char *dup = strdup(path);
	if (!dup) {
		crit("wtc_tmux_set_socket_path: Couldn't duplicate path!");
		return -ENOMEM;
	}

	free(tmux->socket_path);
	tmux->socket_path = dup;

	free(tmux->socket);
	tmux->socket = NULL;

	return 0;
}

const char *wtc_tmux_get_socket_name(const struct wtc_tmux *tmux)
{
	return tmux->socket;
}

const char *wtc_tmux_get_socket_path(const struct wtc_tmux *tmux)
{
	return tmux->socket_path;
}

bool wtc_tmux_is_socket_set(const struct wtc_tmux *tmux)
{
	return tmux->socket || tmux->socket_path;
}

int wtc_tmux_set_config_file(struct wtc_tmux *tmux, const char *file)
{
	if (!tmux)
		return -EINVAL;

	if (tmux->connected)
		return -EBUSY;

	char *dup = NULL;
	if (file) {
		dup = strdup(file);
		if (!dup) {
			crit("wtc_tmux_set_config_file: Couldn't duplicate file name!");
			return -ENOMEM;
		}
	}

	free(tmux->config);
	tmux->config = dup;
}

const char *wtc_tmux_get_config_file(const struct wtc_tmux *tmux)
{
	return tmux->config;
}

static int update_cmd(struct wtc_tmux *tmux)
{
	int i = 0;
	int r = 0;

	int cmdlen = 1 + (wtc_tmux_is_socket_set(tmux) ? 2 : 0)
	               + (tmux->config ? 2 : 0);

	char **cmd = calloc(cmdlen, sizeof(char *));
	if (!cmd) {
		crit("update_cmd: Couldn't allocate cmd!");
		return -ENOMEM;
	}

	if (!tmux->bin) {
		tmux->bin = strdup("/usr/bin/tmux");
		if (!tmux->bin) {
			crit("update_cmd: Couldn't duplicate default bin!");
			r = -ENOMEM;
			goto err_cmd;
		}
	}
	cmd[i] = tmux->bin;

	if (wtc_tmux_is_socket_set(tmux)) {
		if (tmux->socket) {
			cmd[++i] = strdup("-L");
			if (!cmd[i]) {
				crit("update_cmd: Couldn't duplicate -L");
				r = -ENOMEM;
				goto err_cmd;
			}
			cmd[++i] = tmux->socket;
		} else {
			cmd[++i] = strdup("-S");
			if (!cmd[i]) {
				crit("update_cmd: Couldn't duplicate -S");
				r = -ENOMEM;
				goto err_cmd;
			}
			cmd[++i] = tmux->socket_path;
		}
	}

	if (tmux->config) {
		cmd[++i] = strdup("-f");
		if (!cmd[i]) {
			crit("update_cmd: Couldn't duplicate -f");
			r = -ENOMEM;
			goto err_cmd;
		}
		cmd[++i] = tmux->config;
	}

	assert(i + 1 == cmdlen);

	for (i = 0; i < tmux->cmdlen; ++i)
		if (cmd_freeable(tmux, tmux->cmd[i]))
			free(tmux->cmd[i]);
	free(tmux->cmd);

	tmux->cmd = cmd;
	tmux->cmdlen = cmdlen;

	return r;

err_cmd:
	for (i = 0; i < cmdlen; ++i)
		if (cmd_freeable(tmux, cmd[i]))
			free(cmd[i]);
	free(cmd);
	return r;
}

static int setup_pipe(int *fds, struct wlc_event_source **ev,
                      int (*cb)(int, uint32_t, void *), void *userdata)
{
	int r;

	if (!fds || !ev)
		return -EINVAL;

	r = pipe2(fds, O_CLOEXEC);
	if (r < 0) {
		crit("setup_pipe: Couldn't open pipe: %d", errno);
		return -errno;
	}

	for (int i = 0; i < 2; ++i) {
		r = fcntl(fds[i], F_GETFL);
		if (r < 0) {
			crit("setup_pipe: Can't get fds[%d] flags: %d",
			     i, errno);
			r = -errno;
			goto err_pipe;
		}
		r = fcntl(fds[i], F_SETFL, r | O_NONBLOCK);
		if (r < 0) {
			crit("setup_pipe: Can't set fds[i] O_NONBLOCK: %d",
			     i, errno);
			r = -errno;
			goto err_pipe;
		}
	}

	*ev = wlc_event_loop_add_fd(fds[0], WL_EVENT_READABLE | WL_EVENT_HANGUP,
	                            cb, userdata);
	if (!*ev) {
		crit("setup_pipe: Couldn't register callback!");
		r = -1;
		goto err_pipe;
	}

	return 0;

err_pipe:
	if (close(fds[0]))
		warn("setup_pipe: Error closing fds[0]: %d", errno);
	if (close(fds[1]))
		warn("setup_pipe: Error closing fds[1]: %d", errno);
	return r;
}

// TODO Find a way to make multiple tmux instances share this. Albeit this
// may just be a stupid idea (and I doubt it's a feature I'll ever use), but
// it might be fun to work out.
int sigcpipe[2];

static void sigchld_handler(int signal)
{
	int save_errno = errno;
	write(sigcpipe[1], "", 1);
	errno = save_errno;
}

int wtc_tmux_waitpid(struct wtc_tmux *tmux, pid_t pid, int *stat, int opt)
{
	int r = 0;

	if (!tmux || pid <= 0)
		return -EINVAL;

	struct pollfd pol = { .fd = sigcpipe[0], .events = POLLIN,
	                      .revents = 0 };
	bool reflag = false, success = false;
	while (r = poll(&pol, 1, tmux->timeout)) {
		if (r == -1) {
			if (errno == EINTR)
				continue;
			warn("wtc_tmux_waitpid: Error waiting for sigc: %d", errno);
			r = -errno;
			goto out;
		}

		r = read_available(pol.fd, WTC_RDAVL_DISCARD, NULL, NULL);
		if (r < 0) {
			warn("wtc_tmux_waitpid: Error clearing SIGCHLD pipe: %d", r);
			return r;
		}

		r = waitpid(pid, stat, opt | WNOHANG);
		if (r < 0) {
			warn("wtc_tmux_waitpid: waitpid error: %d", errno);
			r = -errno;
			goto out;
		} else if (r == 0) {
			reflag = 1;
		} else {
			success = true;
			break;
		}
	}

	if (!success) {
		warn("wtc_tmux_waitpid: Wait for %d timed out. Killing...", pid);
		kill(pid, SIGKILL);
		while ((r = waitpid(pid, stat, opt & ~WNOHANG)) == -1 &&
		       errno == EINTR) ;
		if (r == -1)
			warn("wtc_tmux_waitpid: waitpid error: %d", errno);
	}

out:
	if (reflag) {
		int s;
		while ((s = write(sigcpipe[1], "", 1)) == -1 && errno == EINTR) ;
		if (s == -1)
			warn("wtc_tmux_waitpid: Error reflagging SIGCLD: %d", errno);
	}
	return r;
}

int wtc_tmux_connect(struct wtc_tmux *tmux)
{
	struct sigaction act;
	int refreshfds[2];
	int r = 0;

	if (!tmux)
		return -EINVAL;
	if (tmux->connected)
		return 0;

	r = setup_pipe(refreshfds, &(tmux->rfev), wtc_tmux_refresh_cb, tmux);
	if (r < 0)
		return r;
	tmux->refresh = 0;
	tmux->refreshfd = refreshfds[1];

	r = setup_pipe(sigcpipe, &(tmux->sigc), sigc_cb, tmux);
	if (r < 0)
		goto err_rf;

	memset(&act, 0, sizeof(struct sigaction));
	act.sa_handler = sigchld_handler;
	act.sa_flags = SA_NOCLDSTOP;
	r = sigaction(SIGCHLD, &act, &tmux->restore);
	if (r < 0) {
		crit("wtc_tmux_connect: Could not set SIGCHLD handler: %d", errno);
		r = -errno;
		goto err_evl;
	}

	r = update_cmd(tmux);
	if (r < 0)
		goto err_sig;

	r = wtc_tmux_version_check(tmux);
	switch (r) {
	case 0:
		crit("Invalid tmux version! tmux must either be version 'master' "
		     "or newer than version '2.4'");
		r = -1;
		goto err_sig;
	case 1:
		r = 0;
		break;
	default:
		goto err_sig;
	}

	r = wtc_tmux_queue_refresh(tmux, WTC_TMUX_REFRESH_SESSIONS);
	if (r < 0)
		goto err_sig;

	tmux->connected = true;
	return r;

err_sig:
	sigaction(SIGCHLD, &tmux->restore, NULL);
	memset(&tmux->restore, 0, sizeof(struct sigaction));
err_evl:
	wlc_event_source_remove(tmux->sigc);
	tmux->sigc = NULL;
	if (close(sigcpipe[1]))
		warn("wtc_tmux_connect: Error closing sigcpipe[1]: %d", errno);
err_rf:
	wlc_event_source_remove(tmux->rfev);
	tmux->rfev = NULL;
	if (close(tmux->refreshfd))
		warn("wtc_tmux_connect: Error closing refreshfd: %d", errno);
	return r;
}

void wtc_tmux_disconnect(struct wtc_tmux *tmux)
{
	if (!tmux || !tmux->connected)
		return;

	struct wtc_tmux_cc *cc;
	const char *cmd[] = { "detach-client", NULL };
	for (cc = tmux->ccs; cc; cc = cc->next) {
		wtc_tmux_cc_exec(cc, cmd, NULL, NULL);
		wtc_tmux_waitpid(tmux, cc->pid, NULL, 0);
		wtc_tmux_cc_unref(cc);
	}
	tmux->ccs = NULL;

	sigaction(SIGCHLD, &tmux->restore, NULL);
	memset(&tmux->restore, 0, sizeof(struct sigaction));

	wlc_event_source_remove(tmux->sigc);
	tmux->sigc = NULL;
	if (close(sigcpipe[1]))
		warn("wtc_tmux_disconnect: Error closing sigcpipe[1]: %d", errno);

	wlc_event_source_remove(tmux->rfev);
	tmux->rfev = NULL;
	if (close(tmux->refreshfd))
		warn("wtc_tmux_disconnect: Error closing refreshfd: %d", errno);

	struct wtc_tmux_pane *pane, *tmpp;
	struct wtc_tmux_window *window, *tmpw;
	struct wtc_tmux_client *client, *tmpc;
	struct wtc_tmux_session *sess, *tmps;
	struct wtc_tmux_key_table *table, *tmpt;
	struct wtc_tmux_key_bind *bind, *tmpb;

	HASH_ITER(hh, tmux->panes, pane, tmpp) {
		HASH_DEL(tmux->panes, pane);
		wtc_tmux_pane_free(pane);
	}

	HASH_ITER(hh, tmux->windows, window, tmpw) {
		HASH_DEL(tmux->windows, window);
		wtc_tmux_window_free(window);
	}

	HASH_ITER(hh, tmux->clients, client, tmpc) {
		HASH_DEL(tmux->clients, client);
		wtc_tmux_client_free(client);
	}

	HASH_ITER(hh, tmux->sessions, sess, tmps) {
		HASH_DEL(tmux->sessions, sess);
		wtc_tmux_session_free(sess);
	}

	HASH_ITER(hh, tmux->tables, table, tmpt) {
		HASH_ITER(hh, table->binds, bind, tmpb) {
			HASH_DEL(table->binds, bind);
			free(bind);
		}

		HASH_DEL(tmux->tables, table);
		free((void *)table->name);
		free(table);
	}

	tmux->connected = false;
}

bool wtc_tmux_is_connected(const struct wtc_tmux *tmux)
{
	return tmux->connected;
}

int wtc_tmux_set_timeout(struct wtc_tmux *tmux, unsigned int timeout)
{
	if (!tmux)
		return -EINVAL;

	tmux->timeout = timeout;
	return 0;
}

unsigned int wtc_tmux_get_timeout(const struct wtc_tmux *tmux)
{
	return tmux->timeout;
}

int wtc_tmux_set_size(struct wtc_tmux *tmux, unsigned int w, unsigned int h)
{
	struct wtc_tmux_cc *cc;
	int r = 0;

	if (!tmux || w < 10 || h < 10)
		return -EINVAL;

	// Don't make changes if we don't have to.
	if (tmux->w == w && tmux->h == h)
		return 0;

	tmux->w = w;
	tmux->h = h;

	if (!tmux->connected)
		return 0;

	for (cc = tmux->ccs; cc; cc = cc->next) {
		r = wtc_tmux_cc_update_size(cc);
		if (r < 0)
			return r;
	}

	return 0;
}

unsigned int wtc_tmux_get_width(const struct wtc_tmux *tmux)
{
	return tmux->w;
}

unsigned int wtc_tmux_get_height(const struct wtc_tmux *tmux)
{
	return tmux->h;
}

int wtc_tmux_set_client_session_changed_cb(struct wtc_tmux *tmux,
	int (*cb)(struct wtc_tmux *tmux, const struct wtc_tmux_client *client))
{
	if (!tmux)
		return -EINVAL;

	tmux->cbs.client_session_changed = cb;
	return 0;
}

int wtc_tmux_set_new_session_cb(struct wtc_tmux *tmux,
	int (*cb)(struct wtc_tmux *tmux, const struct wtc_tmux_session *sess))
{
	if (!tmux)
		return -EINVAL;

	tmux->cbs.new_session = cb;
	return 0;
}

int wtc_tmux_set_session_closed_cb(struct wtc_tmux *tmux,
	int (*cb)(struct wtc_tmux *tmux, const struct wtc_tmux_session *sess))
{
	if (!tmux)
		return -EINVAL;

	tmux->cbs.session_closed = cb;
}

int wtc_tmux_set_session_window_changed_cb(struct wtc_tmux *tmux,
	int (*cb)(struct wtc_tmux *tmux, const struct wtc_tmux_session *sess))
{
	if (!tmux)
		return -EINVAL;

	tmux->cbs.session_window_changed = cb;
}

int wtc_tmux_set_new_window_cb(struct wtc_tmux *tmux,
	int (*cb)(struct wtc_tmux *tmux, const struct wtc_tmux_window *window))
{
	if (!tmux)
		return -EINVAL;

	tmux->cbs.new_window = cb;
}

int wtc_tmux_set_window_closed_cb(struct wtc_tmux *tmux,
	int (*cb)(struct wtc_tmux *tmux, const struct wtc_tmux_window *window))
{
	if (!tmux)
		return -EINVAL;

	tmux->cbs.window_closed = cb;
}

int wtc_tmux_set_window_pane_changed_cb(struct wtc_tmux *tmux,
	int (*cb)(struct wtc_tmux *tmux, const struct wtc_tmux_window *window))
{
	if (!tmux)
		return -EINVAL;

	tmux->cbs.window_pane_changed = cb;
}

int wtc_tmux_set_new_pane_cb(struct wtc_tmux *tmux,
	int (*cb)(struct wtc_tmux *tmux, const struct wtc_tmux_pane *pane))
{
	if (!tmux)
		return -EINVAL;

	tmux->cbs.new_pane= cb;
}

int wtc_tmux_set_pane_closed_cb(struct wtc_tmux *tmux,
	int (*cb)(struct wtc_tmux *tmux, const struct wtc_tmux_pane *pane))
{
	if (!tmux)
		return -EINVAL;

	tmux->cbs.pane_closed = cb;
}

int wtc_tmux_set_pane_resized_cb(struct wtc_tmux *tmux,
	int (*cb)(struct wtc_tmux *tmux, const struct wtc_tmux_pane *pane))
{
	if (!tmux)
		return -EINVAL;

	tmux->cbs.pane_resized= cb;
}

int wtc_tmux_set_pane_mode_changed_cb(struct wtc_tmux *tmux,
	int (*cb)(struct wtc_tmux *tmux, const struct wtc_tmux_pane *pane))
{
	if (!tmux)
		return -EINVAL;

	tmux->cbs.pane_mode_changed = cb;
}

const struct wtc_tmux_session *
wtc_tmux_root_session(const struct wtc_tmux *tmux)
{
	return tmux->sessions;
}

int wtc_tmux_add_closure(struct wtc_tmux *tmux,
                         struct wtc_tmux_cb_closure cl)
{
	struct wtc_tmux_cb_closure *tmp;

	if (!tmux)
		return -EINVAL;

	if (++(tmux->closure_size) <= tmux->closure_len) {
		tmux->closures[tmux->closure_size - 1] = cl;
		return 0;
	}

	if (tmux->closure_len == 0)
		tmux->closure_len = 5;
	else
		tmux->closure_len *= 2;

	tmp = realloc(tmux->closures, tmux->closure_len * 
	                              sizeof(struct wtc_tmux_cb_closure));
	if (!tmp) {
		crit("wtc_tmux_add_closure: Couldn't allocate tmux->closures!");
		return -ENOMEM;
	}

	tmux->closures = tmp;
	tmux->closures[tmux->closure_size - 1] = cl;

	return 0;
}

int wtc_tmux_closure_invoke(struct wtc_tmux_cb_closure *cl)
{
	int r = 0;
	int p = -1; // 0 - pane; 1 - window; 2 - session; 3 - client

	if (!cl)
		return -EINVAL;

	struct wtc_tmux *tmux = cl->tmux;
	switch (cl->fid) {
	case WTC_TMUX_CB_CLIENT_SESSION_CHANGED:
		p = 3;
		if (tmux->cbs.client_session_changed)
			r = tmux->cbs.client_session_changed(tmux, cl->value.client);
		break;

	case WTC_TMUX_CB_NEW_SESSION:
		p = 2;
		r = wtc_tmux_cc_launch(tmux, cl->value.session);
		if (r < 0)
			break;
		if (tmux->cbs.new_session)
			r = tmux->cbs.new_session(tmux, cl->value.session);
		break;
	case WTC_TMUX_CB_SESSION_CLOSED:
		p = 2;
		if (tmux->cbs.session_closed)
			r = tmux->cbs.session_closed(tmux, cl->value.session);
		break;
	case WTC_TMUX_CB_SESSION_WINDOW_CHANGED:
		p = 2;
		if (tmux->cbs.session_window_changed)
			r = tmux->cbs.session_window_changed(tmux, cl->value.session);
		break;

	case WTC_TMUX_CB_NEW_WINDOW:
		p = 1;
		if (tmux->cbs.new_window)
			r = tmux->cbs.new_window(tmux, cl->value.window);
		break;
	case WTC_TMUX_CB_WINDOW_CLOSED:
		p = 1;
		if (tmux->cbs.window_closed)
			r = tmux->cbs.window_closed(tmux, cl->value.window);
		break;
	case WTC_TMUX_CB_WINDOW_PANE_CHANGED:
		p = 1;
		if (tmux->cbs.window_pane_changed)
			r = tmux->cbs.window_pane_changed(tmux, cl->value.window);
		break;

	case WTC_TMUX_CB_NEW_PANE:
		p = 0;
		if (tmux->cbs.new_pane)
			r = tmux->cbs.new_pane(tmux, cl->value.pane);
		break;
	case WTC_TMUX_CB_PANE_CLOSED:
		p = 0;
		if (tmux->cbs.pane_closed)
			r = tmux->cbs.pane_closed(tmux, cl->value.pane);
		break;
	case WTC_TMUX_CB_PANE_RESIZED:
		p = 0;
		if (tmux->cbs.pane_resized)
			r = tmux->cbs.pane_resized(tmux, cl->value.pane);
		break;
	case WTC_TMUX_CB_PANE_MODE_CHANGED:
		p = 0;
		if (tmux->cbs.pane_mode_changed)
			r = tmux->cbs.pane_mode_changed(tmux, cl->value.pane);
		break;

	case WTC_TMUX_CB_EMPTY:
	default:
		break;
	}

	if (r != 0)
		return r;

	cl->fid = WTC_TMUX_CB_EMPTY;
	if (!cl->free_after_use)
		return r;

	cl->free_after_use = false;
	switch (p) {
	case 0:
		wtc_tmux_pane_free(cl->value.pane);
		cl->value.pane = NULL;
		break;
	case 1:
		wtc_tmux_window_free(cl->value.window);
		cl->value.window = NULL;
		break;
	case 2:
		wtc_tmux_session_free(cl->value.session);
		cl->value.session = NULL;
		break;
	case 3:
		wtc_tmux_client_free(cl->value.client);
		cl->value.client = NULL;
		break;
	default:
		break;
	}

	return r;
}

void wtc_tmux_clear_closures(struct wtc_tmux *tmux)
{
	if (!tmux)
		return;

	struct wtc_tmux_cb_closure cb;
	for (size_t i = 0; i < tmux->closure_size; ++i) {
		cb = tmux->closures[i];
		if (cb.fid == WTC_TMUX_CB_EMPTY || !cb.free_after_use)
			continue;

		switch (cb.fid) {
		case WTC_TMUX_CB_CLIENT_SESSION_CHANGED:
			wtc_tmux_client_free(cb.value.client);
			break;
		case WTC_TMUX_CB_NEW_SESSION:
		case WTC_TMUX_CB_SESSION_CLOSED:
		case WTC_TMUX_CB_SESSION_WINDOW_CHANGED:
			wtc_tmux_session_free(cb.value.session);
			break;
		case WTC_TMUX_CB_NEW_WINDOW:
		case WTC_TMUX_CB_WINDOW_CLOSED:
		case WTC_TMUX_CB_WINDOW_PANE_CHANGED:
			wtc_tmux_window_free(cb.value.window);
			break;
		case WTC_TMUX_CB_NEW_PANE:
		case WTC_TMUX_CB_PANE_CLOSED:
		case WTC_TMUX_CB_PANE_RESIZED:
		case WTC_TMUX_CB_PANE_MODE_CHANGED:
			wtc_tmux_pane_free(cb.value.pane);
			break;
		}
	}

	tmux->closure_size = 0;
}
