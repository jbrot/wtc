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
#include "log.h"
#include "shl_ring.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <wait.h>
#include <wayland-server-core.h>
#include <wlc/wlc.h>

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

struct wtc_tmux_cc {
	unsigned long ref;

	struct wtc_tmux *tmux;
	struct wtc_tmux_session *session;

	pid_t pid;
	bool temp;
	int fin;
	struct wlc_event_source *outs;
	struct shl_ring buf;

	struct wtc_tmux_cc *previous;
	struct wtc_tmux_cc *next;
};

struct wtc_tmux {
	unsigned long ref;

	struct wtc_tmux_pane *panes;
	struct wtc_tmux_window *windows;
	struct wtc_tmux_session *sessions;
	struct wtc_tmux_client *clients;

	struct sigaction restore;
	struct wlc_event_source *sigc;
	struct wtc_tmux_cc *ccs;

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

static void wtc_tmux_pane_free(struct wtc_tmux_pane *pane)
{
	free(pane);
}

static void wtc_tmux_window_free(struct wtc_tmux_window *window)
{
	free(window);
}

static void wtc_tmux_session_free(struct wtc_tmux_session *sess)
{
	if (!sess)
		return;

	free(sess->windows);
	free(sess);
}

static void wtc_tmux_client_free(struct wtc_tmux_client *client)
{
	if (!client)
		return;

	free((void *) client->name);
	free(client);
}

/*
 * Dynamically allocate memory for the specified printf operation and
 * then store the result in it. Returns 0 on success and negative error
 * value. out must be non-NULL while *out must be NULL.
 */
__attribute__((format (printf, 2, 3)))
static int bprintf(char **out, const char *format, ...)
{
	va_list args;
	char *buf;
	int flen;

	if (!out || *out)
		return -EINVAL;

	va_start(args, format);
	flen = vsnprintf(NULL, 0, format, args);
	va_end(args);

	if (flen < 0) {
		warn("bprintf: Couldn't compute buffer size: %d", flen);
		return flen;
	}

	buf = malloc(flen + 1);
	if (!buf) {
		crit("bprintf: Couldn't make buffer!");
		return -ENOMEM;
	}

	va_start(args, format);
	flen = vsnprintf(buf, flen + 1, format, args);
	va_end(args);

	if (flen < 0) {
		warn("bprintf: Couldn't print to buffer: %d", flen);
		return flen;
	}

	*out = buf;
	return 0;
}

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
	if (!exc) {
		crit("fork_tmux: Couldn't create exc!");
		return -ENOMEM;
	}

	for (i = 0; i < tmux->cmdlen; i++) {
		exc[i] = strdup(tmux->cmd[i]);
		if (!exc[i]) {
			crit("fork_tmux: Couldn't fill exc!");
			r = -ENOMEM;
			goto err_exc;
		}
	}
	for ( ; *cmds; ++cmds, ++i) {
		exc[i] = strdup(*cmds);
		if (!exc[i]) {
			crit("fork_tmux: Couldn't fill exc!");
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
			warn("fork_tmux: Couldn't open fin: %d", errno);
			r = -errno;
			goto err_exc;
		}
	}
	if (fout) {
		r = pipe2(pout, O_CLOEXEC);
		if (r < 0) {
			warn("fork_tmux: Couldn't open fout: %d", errno);
			r = -errno;
			goto err_pin;
		}
		r = fcntl(pout[0], F_GETFL);
		if (r < 0) {
			warn("fork_tmux: Can't get fout flags: %d", errno);
			r = -errno;
			goto err_pout;
		}
		r = fcntl(pout[0], F_SETFL, r | O_NONBLOCK);
		if (r < 0) {
			warn("fork_tmux: Can't set fout O_NONBLOCK: %d", errno);
			r = -errno;
			goto err_pout;
		}
	}
	if (ferr) {
		r = pipe2(perr, O_CLOEXEC);
		if (r < 0) {
			warn("fork_tmux: Couldn't open ferr: %d", errno);
			r = -errno;
			goto err_pout;
		}
		r = fcntl(perr[0], F_GETFL);
		if (r < 0) {
			warn("fork_tmux: Can't get ferr flags: %d", errno);
			r = -errno;
			goto err_perr;
		}
		r = fcntl(perr[0], F_SETFL, r | O_NONBLOCK);
		if (r < 0) {
			warn("fork_tmux: Can't set ferr O_NONBLOCK: %d", errno);
			r = -errno;
			goto err_perr;
		}
	}

	wlogs(DEBUG, "fork_tmux: Forking: ");
	for (int i = 0; exc[i]; ++i) wlogm(DEBUG, "%s ", exc[i]);
	wloge(DEBUG);

	pid_t cpid = fork();
	if (cpid == -1) {
		warn("fork_tmux: Couldn't fork: %d", errno);
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
		warn("fork_tmux: Couldn't close fin: %d", errno);
		r = -errno;
	}
	if(fout && close(pout[1]) == -1 && errno != EINTR) {
		warn("fork_tmux: Couldn't close fout: %d", errno);
		r = r ? r : -errno;
	}
	if(ferr && close(perr[1]) == -1 && errno != EINTR) {
		warn("fork_tmux: Couldn't close ferr: %d", errno);
		r = r ? r : -errno;
	}

	return r;

err_perr:
	if (ferr) {
		if (close(perr[0]))
			warn("fork_tmux: Couldn't close perr0: %d", errno);
		if (close(perr[1]))
			warn("fork_tmux: Couldn't close perr1: %d", errno);
	}
err_pout:
	if (fout) {
		if (close(pout[0]))
			warn("fork_tmux: Couldn't close pout0: %d", errno);
		if (close(pout[1]))
			warn("fork_tmux: Couldn't close pout1: %d", errno);
	}
err_pin:
	if (fin) {
		if (close(pin[0]))
			warn("fork_tmux: Couldn't close pin0: %d", errno);
		if (close(pin[1]))
			warn("fork_tmux: Couldn't close pin1: %d", errno);
	}
err_exc:
	for (int i = 0; i < len; ++i)
		free(exc[i]);
	free(exc);
	return r;
}

/*
 * Calculates the next power of 2. From Sean Eron Anderson's Bit Twiddling
 * Hacks.
 */
static inline unsigned int npo2(unsigned int base) {
	--base;

	base |= (base >> 1);
	base |= (base >> 2);
	base |= (base >> 4);
	base |= (base >> 8);
	base |= (base >> 16);

	return ++base;
}

/*
 *   int read_available(int fd, int mode, int *size, void *out)
 *
 * read_available in effect does exactly what it sounds like: it reads the
 * available contents of a file descriptor. However, the specifics of this
 * can be tweaked in several ways.
 *
 * The mode parameter is a bitwise or of up to one flag from the following
 * two lists:
 *
 *   WTC_TMUX_READ_DISCARD, WTC_TMUX_READ_CSTRING, WTC_TMUX_READ_STANDARD
 *
 *   WTC_TMUX_READ_BUF, WTC_TMUX_READ_RING
 *
 * If no flag from the first list is included, WTC_TMUX_READ_DISCARD will
 * be chosen by default and, likewise, WTC_TMUX_READ_BUF will be chosen if
 * no flag is included from the second list. Depending on which flags are
 * provided, the interpretation of size and out changes.
 *
 * WTC_TMUX_READ_DISCARD:
 *   The contents of the file descriptor are read and immediately discarded.
 *   This an be useful for clearing out a buffer. If size is not NULL, the
 *   total amount of bytes read will be stored in *size. If this flag is set
 *   all other flags are ignored (in particular, the value of out is ignored
 *   and left unchanged).
 *
 * WTC_TMUX_READ_CSTRING:
 *   The contents of the file descriptor are read into the appropriate
 *   structure (see WTC_TMUX_READ_BUF and WTC_TMUX_READ_RING) followed by
 *   a '\0' terminator. If '\0' occurs within the content read, it will
 *   be replace with 0x01 such that for each call of read_available, exactly
 *   one '\0' will be output, and it is guaranteed to be the last character
 *   output. If size is not NULL and WTC_TMUX_READ_BUF is set, the total
 *   amount of bytes read (that is, the '\0' terminator is not included) 
 *   will be stored in *size. If size is not NULL and WTC_TMUX_READ_RING is
 *   set, then the total amount of bytes written to the ring will be stored
 *   in *size (that is, the '\0' terminator IS included).
 *
 * WTC_TMUX_READ_STANDARD:
 *   The contents of the file descriptor are read into the appropriate
 *   structure (see WTC_TMUX_READ_BUF and WTC_TMUX_READ_RING). The data read
 *   will be unprocessed. If size is not NULL, the total amount of bytes
 *   read will be stored in *size.
 *
 * WTC_TMUX_READ_BUF:
 *   The contents read from the file descriptor are be to stored in a newly
 *   allocated buffer of type char *. out will be interpreted as type
 *   (char **). After a successful read, the new data will be stored in
 *   *out. If *out is not NULL, the contents of *out will be copied to the
 *   start of the newly allocated buffer and any read data will be appended
 *   to them. Upon a successful read, the current contents of *out will be
 *   freed and *out will be set to point to the new buffer. If
 *   WTC_TMUX_READ_CSTRING is set, the size of *out will be determined via
 *   strlen (thus, the '\0' terminator will not be copied over and the
 *   buffer at *out upon a successful call will contain exactly one '\0').
 *   If WTC_TMUX_READ_STANDARD is set, the size of *out will be determined
 *   via the value stored in *size.
 *
 * WTC_TMUX_READ_RING:
 *   The contents read from the file descriptor are to be stored in an
 *   existing shl_ring (see shl_ring.h). out will be interpeted as type
 *   (struct shl_ring *) and the data read will be appended via
 *   shl_ring_push. Note that unlike with WTC_TMUX_READ_BUF, data is always
 *   appended to an existing structure and, furthermore, no existing data
 *   is removed. In particular, this means that multiple calls to
 *   read_available with WTC_TMUX_READ_CSTRING set will result in multiple
 *   '\0' characters being added to the ring (one after the contents read
 *   by each invocation). This is in direct contrast with WTC_TMUX_READ_BUF
 *   where multiple calls with WTC_TMUX_READ_CSTRING set will result in a
 *   single '\0' character at the end of the total amount read. To emulate
 *   this behavior with WTC_TMUX_READ_RING set, simply decrement
 *   shl_ring->end before calling read_available a second time to "unset"
 *   the '\0' terminator. Be careful, though, that if shl_ring->end == 0,
 *   then shl_ring->end needs to be set to  (shl_ring->size - 1) not -1.
 *
 * read_available returns 0 on success, and a negative error code on
 * failure. The errors that can occur are:
 *
 * -ENOMEM:
 *   An issue occurred allocating memory for the read buffer or, if
 *   WTC_TMUX_READ_RING is set, increasing the size of the ring buffer.
 *
 * -EINVAL:
 *   One of the following situations occured: WTC_TMUX_READ_CSTRING and
 *   WTC_TMUX_READ_STANDARD were set, out was NULL and WTC_TMUX_READ_DISCARD
 *   was not set, or *out was not NULL, WTC_TMUX_READ_BUF was set and either
 *   size was NULL or *size was negative.
 *
 * In addition, if a read call fails, then the following behavior occurs:
 * if the error is EINTR, the error is ignored and the read is tried again;
 * if the error is EAGAIN or EWOULDBLOCK, the error is ignored and the read
 * is considered finished; otherwise, the read is aborted and -errno is
 * returned.
 *
 * NOTE: If the read fails and either WTC_TMUX_READ_DISCARD or 
 *       WTC_TMUX_READ_BUF is set, then size and out are unchanged
 *       (although the file descriptor state will most likely be different
 *       as content may have been read before the error). However, if
 *       instead WTC_TMUX_READ_RING is set, then content is added to the 
 *       ring immediately after being read. If some content is written to 
 *       the ring and the function fails, then size will be updated to 
 *       reflect the amount of data added to the ring. Furthermore, if
 *       WTC_TMUX_READ_CSTRING is set, the null terminator will still be
 *       added. However, if the error occurs while adding data to the ring,
 *       size will not be updated. Note that the only error that can occur
 *       when adding to the ring is ENOMEM in which case there isn't really
 *       much hope of recovering, anyway.
 */
#define WTC_TMUX_READ_DISCARD  (0 << 0)
#define WTC_TMUX_READ_CSTRING  (1 << 0)
#define WTC_TMUX_READ_STANDARD (2 << 0)

#define WTC_TMUX_READ_BUF      (0 << 2)
#define WTC_TMUX_READ_RING     (1 << 2)
static int read_available(int fd, int mode, int *size, void *out)
{
	struct shl_ring *ring;
	char *buf, *tmp;
	int pos, len, rd;
	bool disc;
	int r;

	if ((mode & WTC_TMUX_READ_CSTRING) && (mode & WTC_TMUX_READ_STANDARD))
		return -EINVAL;

	// WTC_TMUX_READ_DISCARD
	disc = !(mode & (WTC_TMUX_READ_CSTRING | WTC_TMUX_READ_STANDARD));

	if (disc) {
		len = 128;
		pos = 0;
	} else {
		if (!out)
			return -EINVAL;

		if (mode & WTC_TMUX_READ_RING) {
			ring = out;
			len = 128;
			pos = 0;
		} else { // WTC_TMUX_READ_BUF
			tmp = *((char **) out);
			if (tmp && (mode & WTC_TMUX_READ_CSTRING)) {
				pos = strlen(tmp);
			} else if (tmp && (mode & WTC_TMUX_READ_BUF)) {
				if (!size || *size < 0)
					return -EINVAL;
				pos = *size;
			} else { // !tmp
				pos = 0;
			}

			len = npo2(pos);
			len = len < 128 ? 128 : pos;
		}
	}

	// The + 1 ensures we'll always have space for a '\0' terminator
	buf = calloc(len + 1, sizeof(char));
	if (!buf) {
		crit("read_available: Couldn't create buf!");
		return -ENOMEM;
	}
	if (pos)
		memcpy(buf, tmp, pos);

	rd = 0;
	while (true) {
		r = read(fd, buf + pos, len - pos);

		if (r == -1) {
			switch (errno) {
			case EINTR:
				continue;
			case EAGAIN:
#if EAGAIN != EWOULDBLOCK
			case EWOULDBLOCK:
#endif
				r = 0;
				break;
			default:
				warn("read_available: read error: %d", errno);
				r = -errno;
				goto err_buf;
			}
		}

		rd += r;
		pos += r;
		if (r != len)
			break;

		if (disc) {
			pos = 0;
			continue;
		}

		if (mode & WTC_TMUX_READ_CSTRING)
			for (int i = pos - r; i < pos; ++i)
				if (buf[i] == '\0')
					buf[i] = 1;

		if (mode & WTC_TMUX_READ_RING) {
			r = shl_ring_push(ring, buf, r);
			if (r < 0)
				goto err_buf;

			pos = 0;
			continue;
		}

		// WTC_TMUX_READ_BUF
		len *= 2;
		tmp = realloc(buf, len + 1);
		if (!tmp) {
			crit("read_available: Couldn't resize buf!");
			r = -ENOMEM;
			goto err_buf;
		}
		buf = tmp;
	}

	r = 0;

	// WTC_TMUX_READ_BUF
	if (!disc && !(mode & WTC_TMUX_READ_RING)) {
		buf[pos] = '\0'; // The len + 1's earlier ensure this is in bounds

		if (size)
			*size = rd;
		free(*((char **) out));
		*((char **) out) = buf;
		return 0;
	}

	if (disc && size)
		*size = rd;

err_buf:
	if (!disc && (mode & WTC_TMUX_READ_RING) && r != -ENOMEM) {
		int s = 0;
		if (mode & WTC_TMUX_READ_CSTRING)
			s = shl_ring_push(ring, "\0", 1);

		if (s)
			r = s;
		else if (size)
			*size = rd + 1;
	}
	free(buf);
	return r;
}

/*
 * Run the command specified in cmds, wait for it to finish, and store its
 * stdout in out and stderr in err. Out and err may be NULL to indicate the
 * respective streams should be ignored. Out and err will be populated by
 * read_avilable in "c string" mode. If *out or *err is not NULL, then they
 * are expected to point to an existing c string. On success the newly read
 * data will be appended to the existing data.
 *
 * Returns the client exit status (non-negative) or a negative value on
 * failure. Note that failures during closing the fds will not be reported
 * if an earlier failure occured (and likewise an error closing the first
 * fd hides any error closing the second). If an error occurs after the
 * client exits, the negative error code will be reported instead of the
 * client exit status. Furthermore, output may be stored in *out, *err, or
 * both even if a negative exit status is returned depending on where the
 * error occurs. However, there is a guarantee that if *out or *err have
 * their values changed, then the new value points to the complete output
 * on that stream and no errors were detected while processing it.
 *
 * TODO handle timeout
 * TODO use control session instead of new process if possible.
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

	// TODO Waitpid deal with EINTR
	int status = 0;
	if (waitpid(pid, &status, 0) != pid || r) {
		if (!r)
			warn("exec_tmux: waitpid error: %d", errno);
		r = r ? r : -errno;
		goto err_fds;
	}
	if (!WIFEXITED(status)) {
		warn("exec_tmux: Child didn't exit!");
		r = -EINVAL;
		goto err_fds;
	}
	status = WEXITSTATUS(status);
	if (status)
		warn("exec_tmux: Child exit status: %d", status);

	if (out) {
		r = read_available(fout, NULL, out);
		if (r)
			goto err_fds;
	}

	if (err)
		r = read_available(ferr, NULL, err);

err_fds:
	if (out) {
		if (close(fout) == -1 && errno != EINTR) {
			warn("exec_tmux: Error closing fout: %d", errno);
			r = r ? r : -errno;
		}
	}
	if (err) {
		if (close(ferr) == -1 && errno != EINTR) {
			warn("exec_tmux: Error closing ferr: %d", errno);
			r = r ? r : -errno;
		}
	}
	return r ? r : status;
}

static void wtc_tmux_cc_ref(struct wtc_tmux_cc *cc)
{
	if (!cc|| !cc->ref)
		return;

	cc->ref++;
}

static void wtc_tmux_cc_unref(struct wtc_tmux_cc *cc)
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

static int cc_cb(int fd, uint32_t mask, void *userdata)
{
	struct wtc_tmux_cc *cc = userdata;
	debug("CB: %d", fd);
	if (mask & WL_EVENT_READABLE) {
		debug("Readable : %d", fd);
		int r = read_available(fd, NULL, &cc->buf);
		if (cc->buf)
			debug("Read (%d): %s", r, cc->buf);
		else
			warn("Read error: %d", r);
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

static int sigc_cb(int fd, uint32_t mask, void *userdata)
{
	struct wtc_tmux *tmux = userdata;
	pid_t pid;
	int r = 0;

	r = read_available(fd, NULL, NULL);
	if (r < 0) {
		warn("sigc_cb: Error clearing SIGCHLD pipe: %d", r);
		return r;
	}

	// Because we're handling this synchronously, we don't have to worry
	// about stealing exec_tmux's waitpid. However, exec_tmux can steal
	// our waitpid, so instead of having a guarantee of at least 1 child
	// we don't even have that.
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

/*
 * Launch a control client on the specified session.
 */
static int launch_cc(struct wtc_tmux *tmux, struct wtc_tmux_session *sess)
{
	const char *cmd[] = { "-C", "attach-session", "-t", NULL, NULL };
	char *dyn = NULL;
	struct wtc_tmux_cc *cc, *tmp;
	int fin, fout;
	pid_t pid = 0;
	int r = 0, s = 0;

	if (!tmux || !sess) // TODO Launch new session here if sess is NULL?
		return -EINVAL;

	r = bprintf(&dyn, "$%u", sess->id);
	if (r < 0)
		return r;
	cmd[3] = dyn;

	cc = calloc(1, sizeof(struct wtc_tmux_cc));
	if (!cc) {
		crit("launch_cc: Couldn't allocate cc!");
		r = -ENOMEM;
		goto err_cmd;
	}

	r = fork_tmux(tmux, cmd, &pid, &fin, &fout, NULL);
	if (!pid)
		goto err_cc;

	cc->ref = 2; // outs and tmux.
	cc->tmux = tmux;
	cc->session = sess;

	cc->pid = pid;
	cc->temp = false;
	cc->fin = fin;

	cc->outs = wlc_event_loop_add_fd(fout, WL_EVENT_READABLE
	                                     | WL_EVENT_HANGUP, cc_cb, cc);
	if (!cc->outs) {
		warn("launch_cc: Couldn't add fout to event loop!");
		goto err_pid;
	}

	if (tmux->ccs) {
		for (tmp = tmux->ccs; tmp->next; tmp = tmp->next) ;
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
		warn("launch_cc: waitpid on client failed with %d", errno);

	if (close(fin))
		warn("launch_cc: error closing fin: %d", errno);
	if (close(fout))
		warn("launch_cc: error closing fout: %d", errno);
err_cc:
	free(cc);
err_cmd:
	free(dyn);
	return r;
}

/*
 * Retrieve the value of the option specified by name. The trailing newline
 * will be omitted in *out. Mode can be a bitwise or of several of the
 * gollowing flags. If the global or server flags are set, then target is
 * ignored. If the session flag is set, then target will be used to
 * determine which session to query and likewise for the window flag.
 *
 * Note that out must be non-NULL and *out must be NULL.
 */
#define WTC_TMUX_OPTION_LOCAL   (0 << 0)
#define WTC_TMUX_OPTION_GLOBAL  (1 << 0)

#define WTC_TMUX_OPTION_WINDOW  (0 << 1)
#define WTC_TMUX_OPTION_SESSION (1 << 1)
#define WTC_TMUX_OPTION_SERVER  (2 << 1)
static int get_option(struct wtc_tmux *tmux, const char *name,
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

	r = exec_tmux(tmux, cmd, out, NULL);
	if (*out) {
		int l = strlen(*out);
		if ((*out)[l - 1] == '\n')
			(*out)[l - 1] = '\0';
	}

	free(dyn);
	return r;
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

/*
 * The following functions all parse the input string str line by line
 * per a format string fmt. What gets parsed in each line varies from
 * function to function. The number of lines parsed is put into *olen
 * while the contents of each line are put into *out (and friends).
 * *out (and friends) will be dynamically allocated. Unless specified
 * elsewhere, the fmt should always end with "%n" to allow full line
 * parsing verification.
 *
 * Theses functions return 0 on success, -EIVAl if passed NULL inputs or if
 * there is a parse error, and -ENOMEM if the output arrays can't be
 * created. If there is a non-zero return, *olen and *out will not be
 * modified. However, lines are parsed from str via strtok_r, which means
 * that str will be modified during parsing. str will not be restored to
 * its original state, so if it is important that str is unchanged, make
 * a copy before calling one of these functions.
 *
 * Example parselni fmt: "$%u%n"
 */
static int parselni(const char *fmt, char *str, int *olen, int **out)
{
	int r = 0;

	if (!fmt || !str || !out || !olen)
		return -EINVAL;

	// First, we estimate with number of lines (this over counts in the
	// empty case).
	int count = 0;
	for (int i = 0; str[i]; ++i)
		count += str[i] == '\n';

	int *is = calloc(count, sizeof(int));
	if (!is) {
		crit("parselni: couldn't allocate is!");
		return -ENOMEM;
	}

	count = 0;
	char *svptr = NULL;
	char *pos = strtok_r(str, "\n", &svptr);
	int linec = 0;
	while (pos != NULL) {
		r = sscanf(pos, fmt, &is[count], &linec);
		if (r != 1 || linec != strlen(pos)) {
			warn("parselni: Parse error!");
			r = -EINVAL;
			goto err_is;
		}

		count++;
		pos = strtok_r(NULL, "\n", &svptr);
	}

	*out = is;
	*olen = count;
	return 0;

err_is:
	free(is);
	return r;
}

/*
 * Parse three integers per line instead of one.
 *
 * Example parselniii format: "$%u @%u %u%n"
 */
static int parselniii(const char *fmt, char *str, int *olen,
                     int **out, int **out2, int **out3)
{
	int r = 0;

	if (!fmt || !str || !olen || !out || !out2 || !out3)
		return -EINVAL;

	int count = 0;
	for (int i = 0; str[i]; ++i)
		count += str[i] == '\n';

	int *is = calloc(count, sizeof(int));
	if (!is) {
		crit("parselniii: couldn't allocate is!");
		return -ENOMEM;
	}

	int *is2 = calloc(count, sizeof(int));
	if (!is2) {
		crit("parselniii: couldn't allocate is2!");
		r = -ENOMEM;
		goto err_is;
	}

	int *is3 = calloc(count, sizeof(int));
	if (!is3) {
		crit("parselniii: couldn't allocate is3!");
		r = -ENOMEM;
		goto err_is2;
	}

	count = 0;
	char *svptr = NULL;
	char *pos = strtok_r(str, "\n", &svptr);
	int linec = 0;
	while (pos != NULL) {
		r = sscanf(pos, fmt, &is[count], &is2[count],
		           &is3[count], &linec);
		if (r != 3 || linec != strlen(pos)) {
			warn("parselniii: Parse error!");
			r = -EINVAL;
			goto err_is3;
		}

		count++;
		pos = strtok_r(NULL, "\n", &svptr);
	}

	*olen = count;
	*out = is;
	*out2 = is2;
	*out3 = is3;
	return 0;

err_is3:
	free(is3);
err_is2:
	free(is2);
err_is:
	free(is);
	return r;
}

/*
 * Parse two integers and a string per line instead of one. Note that the
 * format should parse everything up until the string and then the rest of
 * the line will be parsed as the string.
 *
 * Example parselniis format: "$%u @%u %n"
 */
static int parselniis(const char *fmt, char *str, int *olen,
                     int **out, int **out2, char ***out3)
{
	int r = 0;

	if (!fmt || !str || !olen || !out || !out2 || !out3)
		return -EINVAL;

	int count = 0, lc = 0, mxl = 0;
	for (int i = 0; str[i]; ++i) {
		if (str[i] == '\n') {
			count++;
			lc = 0;
		} else {
			mxl = mxl > ++lc ? mxl : lc;
		}
	}
	mxl++; // For '\0'

	int *is = calloc(count, sizeof(int));
	if (!is) {
		crit("parselniis: Couldn't allocate is!");
		return -ENOMEM;
	}

	int *is2 = calloc(count, sizeof(int));
	if (!is2) {
		crit("parselniis: Couldn't allocate is2!");
		r = -ENOMEM;
		goto err_is;
	}

	char **ss = calloc(count, sizeof(char *));
	if (!ss) {
		crit("parselniis: Couldn't allocate ss!");
		r = -ENOMEM;
		goto err_is2;
	}
	for (int i = 0; i < count; ++i) {
		ss[i] = calloc(mxl, sizeof(char));
		if (!ss[i])
			goto err_ss;
	}

	int ncount = 0;
	char *svptr = NULL;
	char *pos = strtok_r(str, "\n", &svptr);
	int linec = 0;
	while (pos != NULL) {
		r = sscanf(pos, fmt, &is[ncount], &is2[ncount], &linec);
		if (r != 2) {
			warn("parselniis: Parse error!");
			r = -EINVAL;
			goto err_ss;
		}
		strcpy(ss[ncount], pos + linec);

		ncount++;
		pos = strtok_r(NULL, "\n", &svptr);
	}

	for (int i = ncount; i < count; ++i)
		free(ss[i]);

	*olen = ncount;
	*out = is;
	*out2 = is2;
	*out3 = ss;
	return 0;

err_ss:
	if (ss) {
		for (int i = 0; i < count; ++i)
			free(ss[i]);
	}
	free(ss);
err_is2:
	free(is2);
err_is:
	free(is);
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
	if (r != 0)
		goto done;

	// We have a guarantee from the source that the version
	// string contains a space separating the program name
	// from the version. We assume here the version has no
	// space in it.
	char *vst = strrchr(out, ' ');
	if (!vst) {
		warn("version_check: No space in version string!");
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

	r = get_option(tmux, "status", sess->id, WTC_TMUX_OPTION_SESSION, &out);
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
	r = get_option(tmux, "status-position", sess->id,
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
 * strtokd functions identically to strtok_r except that the character
 * which is overwritten to makr the end of the token is stored in fdelim.
 *
 * delim and saveptr must not be NULL. fdelim may be NULL (in which case
 * the changed token will not be stored)
 */
static char *strtokd(char *str, const char *delim,
                     char **saveptr, char *fdelim)
{
	char *startptr = str ? str : *saveptr;
	// Find the first character past startptr which isn't in deliml If there
	// isn't one, return NULL.
	while (true) {
		if (*startptr == '\0')
			return NULL;

		for (int i = 0; delim[i] != '\0'; ++i) {
			if (*startptr == delim[i]) {
				++startptr;
				goto wcont;
			}
		}

		break;

		wcont: ;
	}

	for (*saveptr = startptr + 1; **saveptr != '\0'; (*saveptr)++) {
		for (int i = 0; delim[i] != '\0'; ++i) {
			if (**saveptr != delim[i])
				continue;

			if (fdelim)
				*fdelim = delim[i];

			**saveptr = '\0';
			(*saveptr)++; // So we don't trip up on the '\0' next time

			goto out;
		}
	}

out:
	return startptr;
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
	HASH_FIND_INT(tmux->panes, &pid, pane);

	if (!pane) {
		warn("reload_panes_cb: Couldn't find pane %d!", pid);
		return -EINVAL;
	}

	pane->x = x;
	pane->y = y;
	pane->w = w;
	pane->h = h;

	return 0;
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
static int reload_panes(struct wtc_tmux *tmux)
{
	int r = 0;
	// First, establish the list of panes.
	const char *cmd[] = { "list-panes", "-aF",
	                      "#{pane_id} #{window_id} #{pane_active}",
	                      NULL };
	char *out = NULL;
	r = exec_tmux(tmux, cmd, &out, NULL);
	if (r < 0) // We swallow non-zero exit to handle no server being up
		goto err_out;

	int count;
	int *pids;
	int *wids;
	int *active;
	r = parselniii("%%%u @%u %u%n", out, &count, &pids, &wids, &active);
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
		wtc_tmux_pane_free(pane);
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
			crit("reload_panes: Couldn't create pane!");
			r = -ENOMEM;
			goto err_pids;
		}
		pane->id = pids[i];
		HASH_ADD_INT(tmux->panes, id, pane);
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
				warn("reload_panes: Couldn't find window %d!", wids[i]);
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
			warn("reload_panes: Couldn't find pane %d!", pids[i]);
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

		if (active[i])
			wind->active_pane = pane;

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
	r = exec_tmux(tmux, cmd, &out, NULL);
	if (r < 0) // We swallow non-zero exit to handle no server being up
		goto err_pids;
	r = 0; // Actually swallow as we don't necessarily have a next call

	char *saveptr;
	char *token = strtok_r(out, "\n", &saveptr);
	while (token != NULL) {
		r = process_layout(token, tmux, reload_panes_cb);
		if (r < 0) {
			warn("reload_panes: Layout processing error: %d", r);
			goto err_pids;
		}

		token = strtok_r(NULL, "\n", &saveptr);
	}

err_pids:
	free(pids);
	free(wids);
	free(active);
err_out:
	free(out);
	return r;
}

/*
 * Reload the windows on the server. Note that, when calling this, it is
 * imperative that the sessions are already up to date. Depending on where
 * this fails, the server representation may be left in a corrupted state
 * and the only viable method of recovery is to retry this function. As a
 * result of calling this function, the panes will be reloaded as well.
 *
 * WARNING: There is a race condition because there is no synchronization
 * between multiple calls to tmux. This function itself only needs one
 * call, but it also calls reload_panes which requires several.
 */
static int reload_windows(struct wtc_tmux *tmux)
{
	int r = 0;
	// First, establish the list of windows.
	const char *cmd[] = { "list-windows", "-aF",
	                      "#{window_id} #{session_id} #{window_active}",
	                      NULL };
	char *out = NULL;
	r = exec_tmux(tmux, cmd, &out, NULL);
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
		wtc_tmux_window_free(wind);
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
			crit("reload_windows: Couldn't allocate window!");
			r = -ENOMEM;
			goto err_wids;
		}
		wind->id = wids[i];
		HASH_ADD_INT(tmux->windows, id, wind);
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
				warn("reload_windows: Couldn't find session %d!", sids[i]);
				r = -EINVAL;
				goto err_wids;
			}

			sess->window_count = 0;
			windows = calloc(4, sizeof(*windows));
			if (!windows) {
				crit("reload_windows: Couldn't allocate windows list!");
				r = -ENOMEM;
				goto err_wids;
			}
			wsize = 4;
		}

		HASH_FIND_INT(tmux->windows, &wids[i], wind);
		if (!wind) {
			warn("reload_windows: Couldn't find window %d!", wids[i]);
			r = -EINVAL;
			goto err_windows;
		}
		windows[sess->window_count] = wind;
		sess->window_count++;
		if (active[i])
			sess->active_window = wind;

		if (sess->window_count == wsize) {
			wsize *= 2;
			windows_tmp = realloc(windows, wsize * sizeof(*windows));
			if (!windows_tmp) {
				crit("reload_windows: COuldn't resize windows list!");
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

	r = reload_panes(tmux);

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

/*
 * Reload the clients on the server. Note that, when calling this, it is
 * imperative that the sessions are already up to date. Depending on where
 * this fails, the server representation may be left in a corrupted state
 * and the only viable method of recovery is to retry this function.
 *
 * Note: Unlike the other reload functions, this one only requiers one call
 * to tmux and so does not have a race condition.
 */
static int reload_clients(struct wtc_tmux *tmux)
{
	int r = 0;
	// First, establish the list of clients.
	const char *cmd[] = { "list-clients", "-F",
	                      "#{session_id} #{client_pid} |#{client_name}",
	                      NULL };
	char *out = NULL;
	r = exec_tmux(tmux, cmd, &out, NULL);
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
		client->session = NULL;

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
			crit("reload_clients: Couldn't create client!");
			r = -ENOMEM;
			goto err_ids;
		}
		client->pid = cpids[i];
		client->name = strdup(names[i]);
		if (!client->name) {
			crit("reload_clients: Couldn't create client name!");
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
			warn("reload_clients: Couldn't find client \"%s\"!", names[i]);
			r = -EINVAL;
			goto err_ids;
		}

		if (i == 0 || sids[i] != sids[i - 1]) {
			HASH_FIND_INT(tmux->sessions, &sids[i], sess);
			if (!sess) {
				warn("reload_clients: Couldn't find session %d!", sids[i]);
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

		client->session = sess;
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

/*
 * Reload the sessions on the server. Note that as a result of calling this,
 * the entire representation gets updated as the sessions are the root node
 * of the representation tree. Note that depending on where this fails, the
 * server representation may be left in a corrupted state and the only
 * viable method of recovery is to try the entire process again.
 *
 * WARNING: There is a race condition. This function relies on many
 * sequential calls to the server with no guarantees of atomicity. If
 * the server changes state while this function is running, bad things
 * will happen.
 *
 * TODO Possibly patch tmux to allow atomic queries.
 */
static int reload_sessions(struct wtc_tmux *tmux)
{
	int r = 0;
	// First, establish the list of sessions.
	const char *const cmd[] = { "list-sessions", "-F", "#{session_id}",
	                            NULL };
	char *out = NULL;
	r = exec_tmux(tmux, cmd, &out, NULL);
	if (r < 0) // We swallow non-zero exit to handle no server being up
		goto err_out;

	int count;
	int *sids;
	r = parselni("$%u%n", out, &count, &sids);
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
		wtc_tmux_session_free(sess);

		icont: ;
	}

	for (int i = 0; i < count; ++i) {
		if (sids[i] == -1)
			continue;

		sess = calloc(1, sizeof(struct wtc_tmux_session));
		if (!sess) {
			crit("reload_sessions: Couldn't allocate session!");
			r = -ENOMEM;
			goto err_sids;
		}
		sess->id = sids[i];
		HASH_ADD_INT(tmux->sessions, id, sess);
	}

	bool gstatus = true;
	bool gstop = true;
	if (count) {
		free(out); out = NULL;
		r = get_option(tmux, "status", 0,
		               WTC_TMUX_OPTION_GLOBAL | WTC_TMUX_OPTION_SESSION,
		               &out);
		if (r)
			goto err_sids;
		if (strncmp(out, "on", 2) == 0) {
			gstatus = true;
		} else if (strncmp(out, "off", 3) == 0) {
			gstatus = false;
		} else {
			warn("reload_sessions: Invalid global status value: %s", out);
			r = -EINVAL;
			goto err_sids;
		}

		free(out); out = NULL;
		r = get_option(tmux, "status-position", 0,
		               WTC_TMUX_OPTION_GLOBAL | WTC_TMUX_OPTION_SESSION,
		               &out);
		if (r)
			goto err_sids;
		if (strncmp(out, "top", 3) == 0) {
			gstop = true;
		} else if (strncmp(out, "bottom", 6) == 0) {
			gstop = false;
		} else {
			warn("reload_sessions: Invalid global status-position "
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

	r = reload_windows(tmux);
	if (r)
		goto err_sids;

	r = reload_clients(tmux);

err_sids:
	free(sids);
err_out:
	free(out);
	return r;
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

	r = version_check(tmux);
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

	r = reload_sessions(tmux);
	if (r < 0)
		goto err_sig;

	if (tmux->sessions) {
		struct wtc_tmux_session *sess;
		for (sess = tmux->sessions; sess; sess = sess->hh.next) {
			r = launch_cc(tmux, sess);
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
