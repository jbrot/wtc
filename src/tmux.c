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
#include "rdavail.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
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
				// TODO possible no session handler
			}

			// TODO possible client disconnect cb
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

	output->timeout = 10000;
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

	struct wtc_tmux_pane *pane, *tmpp;
	HASH_ITER(hh, tmux->panes, pane, tmpp) {
		HASH_DEL(tmux->panes, pane);
		wtc_tmux_pane_free(pane);
	}

	struct wtc_tmux_window *window, *tmpw;
	HASH_ITER(hh, tmux->windows, window, tmpw) {
		HASH_DEL(tmux->windows, window);
		wtc_tmux_window_free(window);
	}

	struct wtc_tmux_session *sess, *tmps;
	HASH_ITER(hh, tmux->sessions, sess, tmps) {
		HASH_DEL(tmux->sessions, sess);
		wtc_tmux_session_free(sess);
	}

	struct wtc_tmux_client *client, *tmpc;
	HASH_ITER(hh, tmux->clients, client, tmpc) {
		HASH_DEL(tmux->clients, client);
		wtc_tmux_client_free(client);
	}

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

int sigcpipe[2];

static void sigchld_handler(int signal)
{
	int save_errno = errno;
	write(sigcpipe[1], "", 1);
	errno = save_errno;
}

int wtc_tmux_connect(struct wtc_tmux *tmux)
{
	struct sigaction act;
	int r = 0;

	if (!tmux)
		return -EINVAL;
	if (tmux->connected)
		return 0;

	r = pipe2(sigcpipe, O_CLOEXEC);
	if (r < 0) {
		crit("wtc_tmux_connect: Couldn't open sigcpipe: %d", errno);
		return -errno;
	}
	for (int i = 0; i < 2; ++i) {
		r = fcntl(sigcpipe[i], F_GETFL);
		if (r < 0) {
			crit("wtc_tmux_connect: Can't get sigcpipe[%d] flags: %d",
			     i, errno);
			r = -errno;
			goto err_pipe;
		}
		r = fcntl(sigcpipe[i], F_SETFL, r | O_NONBLOCK);
		if (r < 0) {
			crit("wtc_tmux_connect: Can't set sigcpipe[i] O_NONBLOCK: %d",
			     i, errno);
			r = -errno;
			goto err_pipe;
		}
	}

	tmux->sigc = wlc_event_loop_add_fd(sigcpipe[0], WL_EVENT_READABLE |
	                                   WL_EVENT_HANGUP, sigc_cb, tmux);
	if (!tmux->sigc) {
		crit("wtc_tmux_connect: Could not register SIGCHLD callback!");
		r = -1;
		goto err_pipe;
	}

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

	r = wtc_tmux_reload_sessions(tmux);
	if (r < 0)
		goto err_sig;

	if (tmux->sessions) {
		struct wtc_tmux_session *sess;
		for (sess = tmux->sessions; sess; sess = sess->hh.next) {
			r = wtc_tmux_cc_launch(tmux, sess);
			if (r < 0)
				fatal("Not sure what to do here yet.");
		}
	}
	return r;

err_sig:
	sigaction(SIGCHLD, &tmux->restore, NULL);
	memset(&tmux->restore, 0, sizeof(struct sigaction));
err_evl:
	wlc_event_source_remove(tmux->sigc);
	tmux->sigc = NULL;
	if (0) {
err_pipe:
		if (close(sigcpipe[0]))
			warn("wtc_tmux_connect: Error closing sigcpipe[0]: %d", errno);
	}
	if (close(sigcpipe[1]))
		warn("wtc_tmux_connect: Error closing sigcpipe[1]: %d", errno);
	return r;
}

int wtc_tmux_disconnect(struct wtc_tmux *tmux)
{
	if (!tmux)
		return -EINVAL;
	if (!tmux->connected)
		return 0;

	sigaction(SIGCHLD, &tmux->restore, NULL);
	memset(&tmux->restore, 0, sizeof(struct sigaction));
	wlc_event_source_remove(tmux->sigc);
	tmux->sigc = NULL;
	if (close(sigcpipe[1]))
		warn("wtc_tmux_disconnect: Error closing sigcpipe[1]: %d", errno);
	// TODO
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
	if (!tmux || w < 10 || h < 10)
		return -EINVAL;

	// Don't make changes if we don't have to.
	if (tmux->w == w && tmux->h == h)
		return 0;

	tmux->w = w;
	tmux->h = h;

	if (!tmux->connected)
		return 0;

	// TODO change active clients' size.
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

int wtc_tmux_set_new_client_cb(struct wtc_tmux *tmux,
	void (*cb)(struct wtc_tmux *tmux, const struct wtc_tmux_client *client))
{
	if (!tmux)
		return -EINVAL;

	tmux->cbs.new_client = cb;
	return 0;
}

int wtc_tmux_set_client_session_changed_cb(struct wtc_tmux *tmux,
	void (*cb)(struct wtc_tmux *tmux, const struct wtc_tmux_client *client))
{
	if (!tmux)
		return -EINVAL;

	tmux->cbs.client_session_changed = cb;
	return 0;
}

int wtc_tmux_set_new_session_cb(struct wtc_tmux *tmux,
	void (*cb)(struct wtc_tmux *tmux, const struct wtc_tmux_session *sess))
{
	if (!tmux)
		return -EINVAL;

	tmux->cbs.new_session = cb;
	return 0;
}

int wtc_tmux_set_session_closed_cb(struct wtc_tmux *tmux,
	void (*cb)(struct wtc_tmux *tmux, const struct wtc_tmux_session *sess))
{
	if (!tmux)
		return -EINVAL;

	tmux->cbs.session_closed = cb;
}

int wtc_tmux_set_session_window_changed_cb(struct wtc_tmux *tmux,
	void (*cb)(struct wtc_tmux *tmux, const struct wtc_tmux_session *sess))
{
	if (!tmux)
		return -EINVAL;

	tmux->cbs.session_window_changed = cb;
}

int wtc_tmux_set_new_window_cb(struct wtc_tmux *tmux,
	void (*cb)(struct wtc_tmux *tmux, const struct wtc_tmux_window *window))
{
	if (!tmux)
		return -EINVAL;

	tmux->cbs.new_window = cb;
}

int wtc_tmux_set_window_closed_cb(struct wtc_tmux *tmux,
	void (*cb)(struct wtc_tmux *tmux, const struct wtc_tmux_window *window))
{
	if (!tmux)
		return -EINVAL;

	tmux->cbs.window_closed = cb;
}
int wtc_tmux_set_window_layout_changed_cb(struct wtc_tmux *tmux,
	void (*cb)(struct wtc_tmux *tmux, const struct wtc_tmux_window *window))
{
	if (!tmux)
		return -EINVAL;

	tmux->cbs.window_layout_changed = cb;
}

int wtc_tmux_set_window_pane_changed_cb(struct wtc_tmux *tmux,
	void (*cb)(struct wtc_tmux *tmux, const struct wtc_tmux_window *window))
{
	if (!tmux)
		return -EINVAL;

	tmux->cbs.window_pane_changed = cb;
}

int wtc_tmux_set_pane_mode_changed_cb(struct wtc_tmux *tmux,
	void (*cb)(struct wtc_tmux *tmux, const struct wtc_tmux_pane *pane))
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
