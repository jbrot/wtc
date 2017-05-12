/*
 * wtc - tmux_process.c
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
 * wtc_tmux - Process Handling
 *
 * This file contains the functions of the wtc_tmux interface principally
 * dedicated towards the low level process handling.
 */

#define _GNU_SOURCE

#include "tmux_internal.h"

#include "log.h"
#include "rdavail.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wait.h>
#include <wlc/wlc.h>

void wtc_tmux_cc_ref(struct wtc_tmux_cc *cc)
{
	if (!cc|| !cc->ref)
		return;

	cc->ref++;
}

void wtc_tmux_cc_unref(struct wtc_tmux_cc *cc)
{
	int s = 0;

	if (!cc || !cc->ref || cc->ref--)
		return;

	if (cc->outs)
		wlc_event_source_remove(cc->outs);

	if (cc->fin != -1 && close(cc->fin))
			warn("wtc_tmux_cc_unref: Error when closing fin: %d", errno);

	free(cc->buf.buf);
	free(cc);
}

// Hacky debug function.
static void print_ring(struct shl_ring *ring) {
	int size = 0;
	struct iovec pts[2];
	size_t ivc = shl_ring_peek(ring, pts);
	wlogs(DEBUG, "Ring: ");
	for (int i = 0; i < ivc; ++i)
		for (int j = 0; j < pts[i].iov_len; ++j)
			wlogm(DEBUG, "%c", ((char *)pts[i].iov_base)[j]);
	wloge(DEBUG);
}

static int cc_cb(int fd, uint32_t mask, void *userdata)
{
	struct wtc_tmux_cc *cc = userdata;
	debug("cc_cb: %d", fd);
	if (mask & WL_EVENT_READABLE) {
		debug("cc_cb: Readable : %d", fd);
		int r = read_available(fd, WTC_RDAVL_CSTRING | WTC_RDAVL_RING,
		                       NULL, &cc->buf);
		if (r) {
			warn("cc_cb: Read error: %d", r);
			return r;
		}

		print_ring(&(cc->buf));
		r = wtc_tmux_cc_process_output(cc);
		if (r)
			return r;
	}
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		if (mask & WL_EVENT_HANGUP)
			debug("cc_cb: HUP: %d", fd);
		if (mask & WL_EVENT_ERROR)
			debug("cc_cb: Error: %d", fd);

		wlc_event_source_remove(cc->outs);
		cc->outs = NULL;
		wtc_tmux_cc_unref(cc);
	}

	return 0;
}

/*
 * Launch a control client on the specified session.
 */
int wtc_tmux_cc_launch(struct wtc_tmux *tmux, struct wtc_tmux_session *sess)
{
	const char *cmd[] = { "-C", "attach-session", "-t", NULL, NULL };
	char *dyn = NULL;
	struct wtc_tmux_cc *cc, *tmp;
	int fin, fout;
	pid_t pid = 0;
	int r = 0, s = 0;

	if (!tmux)
		return -EINVAL;

	if (!sess) {
		cmd[1] = "new-session";
		cmd[2] = "-s";
		cmd[3] = WTC_TMUX_TEMP_SESSION_NAME;
		cmd[4] = NULL;
	} else {
		r = bprintf(&dyn, "$%u", sess->id);
		if (r < 0)
			return r;
		cmd[3] = dyn;
	}

	cc = calloc(1, sizeof(struct wtc_tmux_cc));
	if (!cc) {
		crit("wtc_tmux_cc_launch: Couldn't allocate cc!");
		r = -ENOMEM;
		goto err_cmd;
	}

	r = wtc_tmux_fork(tmux, cmd, &pid, &fin, &fout, NULL);
	if (!pid)
		goto err_cc;

	cc->ref = 2; // outs and tmux.
	cc->tmux = tmux;
	cc->session = sess;

	cc->pid = pid;
	cc->temp = !sess;
	cc->fin = fin;
	cc->fout = fout;

	cc->compensate = true;
	r = wtc_tmux_cc_update_size(cc);
	if (r < 0) {
		warn("wtc_tmux_cc_launch: Couldn't set size: %d", r);
		goto err_pid;
	}

	cc->outs = wlc_event_loop_add_fd(fout, WL_EVENT_READABLE
	                                     | WL_EVENT_HANGUP, cc_cb, cc);
	if (!cc->outs) {
		warn("wtc_tmux_cc_launch: Couldn't add fout to event loop!");
		goto err_pid;
	}

	if (tmux->ccs) {
		tmp = tmux->ccs;
		while (true) {
			if (tmp->temp) {
				cmd[0] = "kill-session";
				cmd[1] = NULL;
				int s = wtc_tmux_cc_exec(tmp, cmd, NULL, NULL);
				if (s)
					warn("wtc_tmux_cc_launch: Error closing temp "
					     "session: %d", s);
			}

			if (!tmp->next)
				break;
		}
		tmp->next = cc;
		cc->previous = tmp;
	} else {
		tmux->ccs = cc;
	}

	free(dyn);
	return r;

err_pid:
	// If we're here, the process is started and we need to stop it...
	kill(pid, SIGKILL);
	while ((s = waitpid(pid, NULL, 0)) == -1 && errno == EINTR) ;
	if (s == -1)
		warn("wtc_tmux_cc_launch: waitpid on client failed with %d", errno);
	if (close(fin))
		warn("wtc_tmux_cc_launch: error closing fin: %d", errno);
	if (close(fout))
		warn("wtc_tmux_cc_launch: error closing fout: %d", errno);
err_cc:
	free(cc);
err_cmd:
	free(dyn);
	return r;
}

int wtc_tmux_cc_update_size(struct wtc_tmux_cc *cc)
{
	const char *cmd[] = { "refresh-client", "-C", NULL, NULL };
	char *dyn = NULL;
	int r;

	if (!cc || !cc->tmux)
		return -EINVAL;

	r = bprintf(&dyn, "%d,%d", cc->tmux->w, cc->tmux->h);
	if (r < 0)
		return r;
	cmd[2] = dyn;

	r = wtc_tmux_cc_exec(cc, cmd, NULL, NULL);

	free(dyn);
	return r;
}

int wtc_tmux_fork(struct wtc_tmux *tmux, const char *const *cmds,
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
	if (!exc) {
		crit("wtc_tmux_fork: Couldn't create exc!");
		return -ENOMEM;
	}

	for (i = 0; i < tmux->cmdlen; i++) {
		exc[i] = strdup(tmux->cmd[i]);
		if (!exc[i]) {
			crit("wtc_tmux_fork: Couldn't fill exc!");
			r = -ENOMEM;
			goto err_exc;
		}
	}
	for ( ; *cmds; ++cmds, ++i) {
		exc[i] = strdup(*cmds);
		if (!exc[i]) {
			crit("wtc_tmux_fork: Couldn't fill exc!");
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
			warn("wtc_tmux_fork: Couldn't open fin: %d", errno);
			r = -errno;
			goto err_exc;
		}
	}
	if (fout) {
		r = pipe2(pout, O_CLOEXEC);
		if (r < 0) {
			warn("wtc_tmux_fork: Couldn't open fout: %d", errno);
			r = -errno;
			goto err_pin;
		}
		r = fcntl(pout[0], F_GETFL);
		if (r < 0) {
			warn("wtc_tmux_fork: Can't get fout flags: %d", errno);
			r = -errno;
			goto err_pout;
		}
		r = fcntl(pout[0], F_SETFL, r | O_NONBLOCK);
		if (r < 0) {
			warn("wtc_tmux_fork: Can't set fout O_NONBLOCK: %d", errno);
			r = -errno;
			goto err_pout;
		}
	}
	if (ferr) {
		r = pipe2(perr, O_CLOEXEC);
		if (r < 0) {
			warn("wtc_tmux_fork: Couldn't open ferr: %d", errno);
			r = -errno;
			goto err_pout;
		}
		r = fcntl(perr[0], F_GETFL);
		if (r < 0) {
			warn("wtc_tmux_fork: Can't get ferr flags: %d", errno);
			r = -errno;
			goto err_perr;
		}
		r = fcntl(perr[0], F_SETFL, r | O_NONBLOCK);
		if (r < 0) {
			warn("wtc_tmux_fork: Can't set ferr O_NONBLOCK: %d", errno);
			r = -errno;
			goto err_perr;
		}
	}

	wlogs(DEBUG, "wtc_tmux_fork: Forking: ");
	for (int i = 0; exc[i]; ++i) wlogm(DEBUG, "%s ", exc[i]);
	wloge(DEBUG);

	pid_t cpid = fork();
	if (cpid == -1) {
		warn("wtc_tmux_fork: Couldn't fork: %d", errno);
		r = -errno;
		goto err_perr;
	} else if (cpid == 0) { // Child
		r = 0;

		if (fin)
			while ((r = dup2(pin[0],  STDIN_FILENO)) == -1 &&
			       errno == EINTR) ;
		if (fout && r != -1)
			while ((r = dup2(pout[1], STDOUT_FILENO)) == -1 &&
			       errno == EINTR) ;
		if (ferr && r != -1)
			while ((r = dup2(perr[1], STDERR_FILENO)) == -1 &&
			       errno == EINTR) ;
		if (r == -1) {
			crit("Could not change stdio file descriptors: %d", errno);
			_exit(errno);
		}

		execv(exc[0], exc);
		crit("Exec failed: %d", errno);
		_exit(errno); // Exec failed
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

	if(fin && close(pin[0]) == -1 && errno != EINTR) {
		warn("wtc_tmux_fork: Couldn't close fin: %d", errno);
		r = -errno;
	}
	if(fout && close(pout[1]) == -1 && errno != EINTR) {
		warn("wtc_tmux_fork: Couldn't close fout: %d", errno);
		r = r ? r : -errno;
	}
	if(ferr && close(perr[1]) == -1 && errno != EINTR) {
		warn("wtc_tmux_fork: Couldn't close ferr: %d", errno);
		r = r ? r : -errno;
	}

	return r;

err_perr:
	if (ferr) {
		if (close(perr[0]))
			warn("wtc_tmux_fork: Couldn't close perr0: %d", errno);
		if (close(perr[1]))
			warn("wtc_tmux_fork: Couldn't close perr1: %d", errno);
	}
err_pout:
	if (fout) {
		if (close(pout[0]))
			warn("wtc_tmux_fork: Couldn't close pout0: %d", errno);
		if (close(pout[1]))
			warn("wtc_tmux_fork: Couldn't close pout1: %d", errno);
	}
err_pin:
	if (fin) {
		if (close(pin[0]))
			warn("wtc_tmux_fork: Couldn't close pin0: %d", errno);
		if (close(pin[1]))
			warn("wtc_tmux_fork: Couldn't close pin1: %d", errno);
	}
err_exc:
	for (int i = 0; i < len; ++i)
		free(exc[i]);
	free(exc);
	return r;
}

int wtc_tmux_exec(struct wtc_tmux *tmux, const char *const *cmds,
                  char **out, char **err)
{
	pid_t pid = 0;
	int fout, ferr;
	int *pout, *perr;
	int r = 0;

	if (!tmux || !cmds)
		return -EINVAL;

	if (tmux->ccs) {
		struct wtc_tmux_cc *cc;
		for (cc = tmux->ccs; cc && cc->temp; cc = cc->next) ;
		if (cc)
			return wtc_tmux_cc_exec(cc, cmds, out, err);
	}

	pout = out ? &fout : NULL;
	perr = err ? &ferr : NULL;

	r = wtc_tmux_fork(tmux, cmds, &pid, NULL, pout, perr);
	if (!pid)
		return r;

	int status;
	r = wtc_tmux_waitpid(tmux, pid, &status, 0);
	if (r < 0)
		goto err_fds;
	if (!WIFEXITED(status)) {
		warn("wtc_tmux_exec: Child didn't exit!");
		r = -EINVAL;
		goto err_fds;
	}
	status = WEXITSTATUS(status);
	if (status)
		warn("wtc_tmux_exec: Child exit status: %d", status);

	if (out) {
		r = read_available(fout, WTC_RDAVL_CSTRING | WTC_RDAVL_BUF,
		                   NULL, out);
		if (r)
			goto err_fds;
	}

	if (err)
		r = read_available(ferr, WTC_RDAVL_CSTRING | WTC_RDAVL_BUF,
		                   NULL, err);

err_fds:
	if (out) {
		if (close(fout) == -1 && errno != EINTR) {
			warn("wtc_tmux_exec: Error closing fout: %d", errno);
			r = r ? r : -errno;
		}
	}
	if (err) {
		if (close(ferr) == -1 && errno != EINTR) {
			warn("wtc_tmux_exec: Error closing ferr: %d", errno);
			r = r ? r : -errno;
		}
	}
	return r ? r : status;
}

struct cb_dat {
	bool handled;
	char **out;
	char **err;
};

static int exec_cc_cb(struct wtc_tmux_cc *cc, size_t start,
                      size_t len, bool err)
{
	struct shl_ring *ring = &(cc->buf);
	struct cb_dat *dat = cc->userdata;
	char **out = err ? dat->err : dat->out;

	// When starting a control client, there is a blank response at the
	// beginning which we need to ignore.
	if (cc->compensate) {
		cc->compensate = false;
		return 0;
	}

	if (!out) {
		dat->handled = true;
		return 0;
	}

	size_t i = 0;
	if (*out) {
		i = strlen(*out);
		len += i;
	}

	struct iovec vecs[2];
	size_t size, pos;
	char val;

	char *buf = calloc(len + 1, sizeof(char)); // + 1 for end '\0'
	if (!buf) {
		crit("exec_cc_cb: Couldn't allocate buffer!");
		return -ENOMEM;
	}

	if (*out)
		memcpy(buf, *out, i);

	SHL_RING_ITERATE(ring, val, vecs, size, pos) {
		if (pos < start) {
			pos = start - 1;
			continue;
		}

		if (pos == start + len)
			break;

		if (val == '\0')
			continue;

		buf[i++] = val;
	}
	buf[i] = '\0';

	if (*out)
		free(*out);
	*out = buf;
	dat->handled = true;

	return 0;
}

int wtc_tmux_cc_exec(struct wtc_tmux_cc *cc, const char *const *cmds,
                     char **out, char **err)
{
	int r = 0;

	if (!cc)
		return -EINVAL;

	// First, encode the command into a string.
	int len = 0;
	char *buf;
	for (int i = 0; cmds[i]; ++i) {
		len += 3; // space quote ... quote
		for (int j = 0; cmds[i][j] != '\0'; ++j) {
			if (cmds[i][j] == '"')
				len += 2; // \"
			else if (cmds[i][j] == '\n')
				len += 2; // \n
			else
				++len;
		}
	}

	buf = calloc(len + 1, sizeof(char)); // For terminating \0
	if (!buf) {
		crit("tmux_cc_exec: Couldn't allocate buf!");
		return -ENOMEM;
	}

	int pos = 0;
	for (int i = 0; cmds[i]; ++i) {
		if (i != 0)
			buf[pos++] = ' ';
		buf[pos++] = '"';
		for (int j = 0; cmds[i][j] != '\0'; ++j) {
			if (cmds[i][j] == '"') {
				buf[pos++] = '\\';
				buf[pos++] = '"';
			} else if (cmds[i][j] == '\n') {
				buf[pos++] = '\\';
				buf[pos++] = 'n';
			} else {
				buf[pos++] = cmds[i][j];
			}
		}
		buf[pos++] = '"';
	}
	buf[pos++] = '\n';
	buf[pos++] = '\0';

	debug("wtc_tmux_cc_exec: Command: %s", buf);

	pos = 0;
	while (r = write(cc->fin, buf + pos, len - pos)) {
		if (r == -1) {
			if (errno == EINTR)
				continue;
			warn("wtc_tmux_cc_exec: Error while writing: %d", errno);
			r = -errno;
			goto err_buf;
		}

		pos += r;
		if (pos == len)
			break;
	}

	void *ud_bak = cc->userdata;
	int (*cmd_bak)(struct wtc_tmux_cc *, size_t, size_t, bool) = cc->cmd_cb;
	struct cb_dat dat = { false, out, err};

	cc->userdata = &dat;
	cc->cmd_cb = exec_cc_cb;

	struct pollfd pol = { .fd = cc->fout, .events = POLLIN, .revents = 0 };
	uint32_t mask = 0;
	while (r = poll(&pol, 1, cc->tmux->timeout)) {
		if (r == -1) {
			if (errno == EINTR)
				continue;
			warn("wtc_tmux_cc_exec: Error waiting for results: %d", errno);
			r = -errno;
			goto err_poll;
		}
		mask = 0;
		if (pol.revents & POLLIN)
			mask |= WL_EVENT_READABLE;
		if (pol.revents & (POLLHUP | POLLERR))
			goto err_poll;
		r = cc_cb(pol.fd, mask, cc);
		if (r < 0)
			goto err_poll;
		if (dat.handled)
			break;
	}

err_poll:
	cc->userdata = ud_bak;
	cc->cmd_cb = cmd_bak;
err_buf:
	free(buf);
	return r;
}

int wtc_tmux_get_option(struct wtc_tmux *tmux, const char *name,
                        int target, int mode, char **out)
{
	int r = 0;
	if (!tmux || !out || *out)
		return -EINVAL;

	int i = 0;
	const char *cmd[] = { "show-options", NULL, NULL, NULL, NULL };
	char *dyn = NULL;
	if (mode & WTC_TMUX_OPTION_SERVER) {
		cmd[++i] = "-vs";
	} else if (mode & WTC_TMUX_OPTION_SESSION) {
		if (mode & WTC_TMUX_OPTION_GLOBAL) {
			cmd[++i] = "-vg";
		} else {
			cmd[++i] = "-vt";
			r = bprintf(&dyn, "$%u", target);
			if (r < 0)
				return r;
			cmd[++i] = dyn;
		}
	} else { // WTC_TMUX_OPTION_WINDOW
		if (mode & WTC_TMUX_OPTION_GLOBAL) {
			cmd[++i] = "-vwg";
		} else {
			cmd[++i] = "-vwgt";
			r = bprintf(&dyn, "@%u", mode);
			if (r < 0)
				return r;
			cmd[++i] = dyn;
		}
	}
	cmd[++i] = name;

	r = wtc_tmux_exec(tmux, cmd, out, NULL);
	if (*out) {
		int l = strlen(*out);
		if ((*out)[l - 1] == '\n')
			(*out)[l - 1] = '\0';
	}

	free(dyn);
	return r;
}
