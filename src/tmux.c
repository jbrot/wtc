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

#include "tmux.h"

#include <errno.h>

struct wtc_tmux_cbs {
	void (*new_client)(struct wtc_tmux *tmux, 
	                   const struct wtc_tmux_client *client);
	void (*client_session_changed)(struct wtc_tmux *tmux, 
	                               const struct wtc_tmux_client *client);

	void (*new_session)(struct wtc_tmux *tmux, 
	                    const struct wtc_tmux_session *session);
	void (*session_closed)(struct wtc_tmux *tmux, 
	                       const struct wtc_tmux_session *session);
	void (*session_window_changed)(struct wtc_tmux *tmux, 
	                               const struct wtc_tmux_session *session);

	void (*new_window)(struct wtc_tmux *tmux, 
	                   const struct wtc_tmux_window *window);
	void (*window_closed)(struct wtc_tmux *tmux, 
	                      const struct wtc_tmux_window *window);
	void (*window_layout_changed)(struct wtc_tmux *tmux, 
	                              const struct wtc_tmux_window *window);
	void (*window_pane_changed)(struct wtc_tmux *tmux, 
	                            const struct wtc_tmux_window *window);

	void (*pane_mode_changed)(struct wtc_tmux *tmux, 
	                          const struct wtc_tmux_pane *pane);
};

struct wtc_tmux {
	unsigned long ref;

	struct wtc_tmux_pane *panes;
	struct wtc_tmux_window *windows;
	struct wtc_tmux_session *sessions;
	struct wtc_tmux_client *clients;

	char *bin;
	char *socket;
	char *socket_path;
	char *config;
	char **cmd;

	bool connected;
	unsigned int timeout;
	unsigned int w;
	unsigned int h;

	struct wtc_tmux_cbs cbs;
};

int wtc_tmux_new(struct wtc_tmux **out)
{
	struct wtc_tmux *output = calloc(1, sizeof(*output));
	if (!output)
		return -ENOMEM;

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

void wtc_tmux_unref(struct wtc_tmux *tmux)
{
	if (!tmux || !tmux->ref || tmux->ref--)
		return;

	// TODO Cleanup
	if (tmux->cmd)
		for (int i = 0; tmux->cmd[i]; ++i)
			if (tmux->cmd[i] != tmux->bin &&
			    tmux->cmd[i] != tmux->socket &&
			    tmux->cmd[i] != tmux->socket_path &&
			    tmux->cmd[i] != tmux->config)
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
		if (!dup)
			return -ENOMEM;
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
	if (!dup)
		return -ENOMEM;

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
	if (!dup)
		return -ENOMEM;

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
		if (!dup)
			return -ENOMEM;
	}

	free(tmux->config);
	tmux->config = dup;
}

const char *wtc_tmux_get_config_file(const struct wtc_tmux *tmux)
{
	return tmux->config;
}

int wtc_tmux_connect(struct wtc_tmux *tmux)
{
	// TODO
}

int wtc_tmux_disconnect(struct wtc_tmux *tmux)
{
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
