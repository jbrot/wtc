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

#include "tmux.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <wait.h>

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
	int cmdlen;

	bool connected;
	unsigned int timeout;
	unsigned int w;
	unsigned int h;

	struct wtc_tmux_cbs cbs;
};

/*
 * Fork a tmux process. cmds will be appended to tmux->cmds to produce the
 * info passed to exec. The resulting process id will be put in pid.
 * If fin, fout, or ferr are not NULL, then they will be set to a file
 * descriptor which is the end of a pipe to stdin, stdout, and stderr of the
 * child process respectively.
 *
 * Returns 0 on success and a negative value if an error occurs. However, if
 * the error occurs after forking (i.e., when closing the child half of the
 * pipes), then pid, fin, fout, and ferr will be properly populated.
 */
static int fork_tmux(struct wtc_tmux *tmux, const char *const *cmds, 
                     pid_t *pid, int *fin, int *fout, int *ferr)
{
	int i;
	int r = 0;

	if (!tmux || !cmds)
		return -EINVAL;

	int len = tmux->cmdlen;
	for (i = 0; cmds[i]; ++i)
		++len;

	char **exc = calloc(len + 1, sizeof(char *));
	if (!exc)
		return -ENOMEM;

	for (i = 0; i < tmux->cmdlen; i++) {
		exc[i] = strdup(tmux->cmd[i]);
		if (!exc[i]) {
			r = -ENOMEM;
			goto err_exc;
		}
	}
	for ( ; *cmds; ++cmds, ++i) {
		exc[i] = strdup(*cmds);
		if (!exc[i]) {
			r = -ENOMEM;
			goto err_exc;
		}
	}

	int pin[2];
	int pout[2];
	int perr[2];
	if (fin) {
		r = pipe2(pin, O_CLOEXEC);
		if (r < 0) {
			r = -errno;
			goto err_exc;
		}
	}
	if (fout) {
		r = pipe2(pout, O_CLOEXEC);
		if (r < 0) {
			r = -errno;
			goto err_pin;
		}
	}
	if (ferr) {
		r = pipe2(perr, O_CLOEXEC);
		if (r < 0) {
			r = -errno;
			goto err_pout;
		}
	}

	pid_t cpid = fork();
	if (cpid == -1) {
		r = -errno;
		goto err_perr;
	} else if (cpid == 0) { // Child
		if ((!fin  || dup2(pin[0],  STDIN_FILENO)  != STDIN_FILENO) &&
		    (!fout || dup2(pout[1], STDOUT_FILENO) != STDOUT_FILENO) &&
		    (!ferr || dup2(perr[1], STDERR_FILENO) != STDERR_FILENO)) {
			int err = errno;
			fprintf(stderr, "Could not change stdio file descriptors!");
			_exit(err);
		}

		execv(exc[0], exc);
		_exit(-errno); // Exec failed
	}

	// Parent
	if (pid)
		*pid = cpid;

	if (fin)
		*fin = pin[1];
	if (fout)
		*fout = pout[0];
	if (ferr)
		*ferr = perr[0];

	if(fin && close(pin[0]))
		r = -errno;
	if (fout && close(pout[1]) && !r)
		r = -errno;
	if (ferr && close(perr[1]) && !r)
		r = -errno;

	return r;

err_perr:
	if (ferr) {
		close(perr[0]);
		close(perr[1]);
	}
err_pout:
	if (fout) {
		close(pout[0]);
		close(pout[1]);
	}
err_pin:
	if (fin) {
		close(pin[0]);
		close(pin[1]);
	}
err_exc:
	for (int i = 0; i < len; i++)
		free(exc[i]);
	free(exc);
	return r;
}

/*
 * Fully read the current contents of the fd into a newly allocated string.
 * Returns 0 on success, otherwise returns a negative error code. Can
 * return -ENOMEM if there are problems allocating the string and any of the
 * error codes from a call to read().
 */
static int read_full(int fd, char **out)
{
	int r = 0;
	char *buf = calloc(257, sizeof(char));
	char *tmp = NULL;
	int pos = 0;
	int len = 256;
	if (!buf)
		return -ENOMEM;
	while ((r = read(fd, buf + pos, len - pos)) == len - pos) {
		pos += r;
		len *= 2;
		tmp = realloc(buf, len + 1);
		if (!tmp) {
			r = -ENOMEM;
			goto err_buf;
		}
		buf = tmp;
	}

	if (r == -1) {
		r = -errno;
		goto err_buf;
	}

	pos += r;
	memset(buf + pos, 0, len + 1 - pos);

	*out = buf;
	return 0;

err_buf:
	free(buf);
	return r;
}

/*
 * Run the command specified in cmds, wait for it to finish, and
 * store its stdout in out and stderr in err. Out and err may be
 * NULL to indicate the respective streams should be ignored.
 *
 * Returns 0 on success and a negative value on failure. Note that
 * failures during closing the fds will not be reported if an earlier
 * failure occured (and likewise an error closing the first fd hides
 * any error closing the second). Furthermore, output may be stored
 * in *out, *err, or both even if a non-zero exit status is returned
 * depending on where the error occurs. However, there is a guarantee
 * that if *out or *err have there values changed, then the new value
 * points to the complete output on that stream and no errors were
 * detected while processing it.
 *
 * TODO handle timeout
 */
static int exec_tmux(struct wtc_tmux *tmux, const char *const *cmds, 
                     char **out, char **err)
{
	pid_t pid = 0;
	int fout, ferr;
	int *pout, *perr;
	int r = 0;

	if (!tmux || !cmds)
		return -EINVAL;

	pout = out ? &fout : NULL;
	perr = err ? &ferr : NULL;

	r = fork_tmux(tmux, cmds, &pid, NULL, pout, perr);
	if (!pid)
		return r;

	if (waitpid(pid, NULL, 0) != pid || r) {
		r = r ? r : -errno;
		goto err_fds;
	}

	if (out) {
		r = read_full(fout, out);
		if (r)
			goto err_fds;
	}

	if (err)
		r = read_full(ferr, err);

err_fds:
	if (out && close(fout) && !r)
		r = -errno;
	if (err && close(ferr) && !r)
		r = -errno;
	return r;
}

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

	// TODO Cleanup
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

static int update_cmd(struct wtc_tmux *tmux)
{
	int i = 0;
	int r = 0;

	int cmdlen = 1 + (wtc_tmux_is_socket_set(tmux) ? 2 : 0)
	               + (tmux->config ? 2 : 0);

	char **cmd = calloc(cmdlen, sizeof(char *));
	if (!cmd)
		return -ENOMEM;

	if (!tmux->bin) {
		tmux->bin = strdup("/usr/bin/tmux");
		if (!tmux->bin) {
			r = -ENOMEM;
			goto err_cmd;
		}
	}
	cmd[i] = tmux->bin;

	if (wtc_tmux_is_socket_set(tmux)) {
		if (tmux->socket) {
			cmd[++i] = strdup("-L");
			if (!cmd[i]) {
				r = -ENOMEM;
				goto err_cmd;
			}
			cmd[++i] = tmux->socket;
		} else {
			cmd[++i] = strdup("-S");
			if (!cmd[i]) {
				r = -ENOMEM;
				goto err_cmd;
			}
			cmd[++i] = tmux->socket_path;
		}
	}

	if (tmux->config) {
		cmd[++i] = strdup("-f");
		if (!cmd[i]) {
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

/*
 * Ensures the version of tmux is new enough to support the needed messages.
 * Returns 1 if the versions is new enough and 0 if it is not. Will return
 * a negative error code if something goes wrong checking the version.
 */
static int version_check(struct wtc_tmux *tmux)
{
	int r = 0;
	const char *const cmd[] = { "-V", NULL };
	char *out = NULL;
	r = exec_tmux(tmux, cmd, &out, NULL);
	if (r < 0)
		goto done;

	// We have a guarantee from the source that the version
	// string contains a space separating the program name
	// from the version. We assume here the version has no
	// space in it.
	char *vst = strrchr(out, ' ');
	if (!vst) // This really shouldn't happen.
		goto done;
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
	return -1;
}

/*
 * Reload the entire model of the tmux server.
 */
static int reload_structure(struct wtc_tmux *tmux)
{
	int r = 0;
	// First, establish the list of sessions.
	const char *const cmd[] = { "list-sessions", "-F", "#{session_id}",
	                            NULL };
	char *out = NULL;
	r = exec_tmux(tmux, cmd, &out, NULL);
	if (r < 0)
		goto err_out;

	// First, estimate the session count by number of lines
	// (if there aren't any sessions, this will over count)
	int count = 0;
	for (int i = 0; out[i]; ++i)
		count += out[i] == '\n';

	int *sids = calloc(count, sizeof(int));
	if (!sids) {
		r = -ENOMEM;
		goto err_out;
	}

	count = 0;
	char *svptr = NULL;
	char *pos = strtok_r(out, "\n", &svptr);
	int linec = 0;
	while (pos != NULL) {
		r = sscanf(pos, "$%u%n", &sids[count], &linec);
		if (r != 1 || linec != strlen(pos)) { // Parse error
			r = -EINVAL;
			goto err_sids;
		}

		count++;
		pos = strtok_r(NULL, "\n", &svptr);
	}

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
		free(sess); // TODO possible ref count?

		icont: ;
	}

	for (int i = 0; i < count; ++i) {
		if (sids[i] == -1)
			continue;

		sess = calloc(1, sizeof(struct wtc_tmux_session));
		if (!sess) {
			// In an ideal world, we'd make all the changes to a temp copy
			// so that the state is unaffected in the even of failure.
			// However, that's way too much work in this case and since the
			// error is OOM, there's not really much hope for recovery so
			// it's not that big of a deal.
			r = -ENOMEM;
			goto err_sids;
		}
		sess->id = sids[i];
		HASH_ADD_INT(tmux->sessions, id, sess);
	}

	bool gstatus = true;
	bool gstop = true;
	if (count) {
		// TODO update gstatus and gstop
	}

	for (sess = tmux->sessions; sess; sess = sess->hh.next) {
		r = update_session_status(tmux, sess, gstatus, gstop);
		if (r < 0)
			// In this case, the error might be something recoverable and
			// we've just gone and mucked things up pretty badly, but
			// what can you do?
			goto err_sids;
	}

err_sids:
	free(sids);
err_out:
	free(out);
	return r;
}

int wtc_tmux_connect(struct wtc_tmux *tmux)
{
	int r = 0;

	if (!tmux || tmux->connected)
		return -EINVAL;

	r = update_cmd(tmux);
	if (r < 0)
		return r;

	r = version_check(tmux);
	switch (r) {
	case 0:
		// TODO Logging
		fprintf(stderr, "Invalid tmux version! tmux must either be version "
		                "'master' or newer than version '2.4'\n");
		return -1;
	case 1:
		r = 0;
		break;
	default:
		return r;
	}

	r = reload_structure(tmux);
	return r;
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
