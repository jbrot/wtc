/*
 * wtc - util.c
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

#include "util.h"

#include "log.h"
#include "shl_ring.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

int read_available(int fd, int mode, int *size, void *out)
{
	struct shl_ring *ring;
	char *buf, *tmp;
	int pos, len, rd;
	bool disc;
	int r;

	if ((mode & WTC_RDAVL_CSTRING) && (mode & WTC_RDAVL_STANDARD))
		return -EINVAL;

	// WTC_RDAVL_DISCARD
	disc = !(mode & (WTC_RDAVL_CSTRING | WTC_RDAVL_STANDARD));

	if (disc) {
		len = 128;
		pos = 0;
	} else {
		if (!out)
			return -EINVAL;

		if (mode & WTC_RDAVL_RING) {
			ring = out;
			len = 128;
			pos = 0;
		} else { // WTC_RDAVL_BUF
			tmp = *((char **) out);
			if (tmp && (mode & WTC_RDAVL_CSTRING)) {
				pos = strlen(tmp);
			} else if (tmp && (mode & WTC_RDAVL_STANDARD)) {
				if (!size || *size < 0)
					return -EINVAL;
				pos = *size;
			} else { // !tmp
				pos = 0;
			}

			len = npo2(pos);
			len = len < 128 ? 128 : len;
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

		if (mode & WTC_RDAVL_CSTRING)
			for (int i = pos - r; i < pos; ++i)
				if (buf[i] == '\0')
					buf[i] = 1;

		if (!disc && (mode & WTC_RDAVL_RING)) {
			r = shl_ring_push(ring, buf, r);
			if (r < 0)
				goto err_buf;
		}

		if (pos != len)
			break;

		if (disc || (mode & WTC_RDAVL_RING)) {
			pos = 0;
			continue;
		}

		// WTC_RDAVL_BUF
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

	// WTC_RDAVL_BUF
	if (!disc && !(mode & WTC_RDAVL_RING)) {
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
	if (!disc && (mode & WTC_RDAVL_RING) && r != -ENOMEM) {
		int s = 0;
		if (mode & WTC_RDAVL_CSTRING)
			s = shl_ring_push(ring, "\0", 1);

		if (s)
			r = s;
		else if (size)
			*size = rd + 1;
	}
	free(buf);
	return r;
}

__attribute__((format (printf, 2, 3)))
int bprintf(char **out, const char *format, ...)
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

int parselnis(const char *fmt, char *str, int *olen,
              int **out, char ***out2)
{
	int r = 0;

	if (!fmt || !str || !olen || !out || !out2)
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
		crit("parselnis: Couldn't allocate is!");
		return -ENOMEM;
	}

	char **ss = calloc(count, sizeof(char *));
	if (!ss) {
		crit("parselnis: Couldn't allocate ss!");
		r = -ENOMEM;
		goto err_is;
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
		r = sscanf(pos, fmt, &is[ncount], &linec);
		if (r != 1) {
			warn("parselnis: Parse error!");
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
	*out2 = ss;
	return 0;

err_ss:
	if (ss) {
		for (int i = 0; i < count; ++i)
			free(ss[i]);
	}
	free(ss);
err_is:
	free(is);
	return r;
}

int parselniii(const char *fmt, char *str, int *olen, int **out,
               int **out2, int **out3)
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
int parselniiiii(const char *fmt, char *str, int *olen, int **out,
                 int **out2, int **out3, int **out4, int **out5)
{
	int r = 0;

	if (!fmt || !str || !olen || !out || !out2 || !out3 || !out4 || !out5)
		return -EINVAL;

	int count = 0;
	for (int i = 0; str[i]; ++i)
		count += str[i] == '\n';

	int *is = calloc(count, sizeof(int));
	if (!is) {
		crit("parselniiiii: couldn't allocate is!");
		return -ENOMEM;
	}

	int *is2 = calloc(count, sizeof(int));
	if (!is2) {
		crit("parselniiiii: couldn't allocate is2!");
		r = -ENOMEM;
		goto err_is;
	}

	int *is3 = calloc(count, sizeof(int));
	if (!is3) {
		crit("parselniiiii: couldn't allocate is3!");
		r = -ENOMEM;
		goto err_is2;
	}

	int *is4 = calloc(count, sizeof(int));
	if (!is4) {
		crit("parselniiiii: couldn't allocate is4!");
		r = -ENOMEM;
		goto err_is3;
	}

	int *is5 = calloc(count, sizeof(int));
	if (!is5) {
		crit("parselniiiii: couldn't allocate is5!");
		r = -ENOMEM;
		goto err_is4;
	}

	count = 0;
	char *svptr = NULL;
	char *pos = strtok_r(str, "\n", &svptr);
	int linec = 0;
	while (pos != NULL) {
		r = sscanf(pos, fmt, &is[count], &is2[count], &is3[count],
		           &is4[count], &is5[count], &linec);
		if (r != 5 || linec != strlen(pos)) {
			warn("parselniiiii: Parse error!");
			r = -EINVAL;
			goto err_is5;
		}

		count++;
		pos = strtok_r(NULL, "\n", &svptr);
	}

	*olen = count;
	*out = is;
	*out2 = is2;
	*out3 = is3;
	*out4 = is4;
	*out5 = is5;
	return 0;

err_is5:
	free(is5);
err_is4:
	free(is4);
err_is3:
	free(is3);
err_is2:
	free(is2);
err_is:
	free(is);
	return r;
}

int parselniis(const char *fmt, char *str, int *olen, int **out,
               int **out2, char ***out3)
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

char *strtokd(char *str, const char *delim, char **saveptr, char *fdelim)
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


int fork_exec(char *const *cmd, pid_t *pid, int *fin, 
              int *fout, int *ferr)
{
	int r = 0;

	if (!cmd)
		return -EINVAL;

	int pin[2];
	int pout[2];
	int perr[2];
	if (fin) {
		r = pipe2(pin, O_CLOEXEC);
		if (r < 0) {
			warn("fork_exec: Couldn't open fin: %d", errno);
			return -errno;
		}
	}
	if (fout) {
		r = pipe2(pout, O_CLOEXEC);
		if (r < 0) {
			warn("fork_exec: Couldn't open fout: %d", errno);
			r = -errno;
			goto err_pin;
		}
		r = fcntl(pout[0], F_GETFL);
		if (r < 0) {
			warn("fork_exec: Can't get fout flags: %d", errno);
			r = -errno;
			goto err_pout;
		}
		r = fcntl(pout[0], F_SETFL, r | O_NONBLOCK);
		if (r < 0) {
			warn("fork_exec: Can't set fout O_NONBLOCK: %d", errno);
			r = -errno;
			goto err_pout;
		}
	}
	if (ferr) {
		r = pipe2(perr, O_CLOEXEC);
		if (r < 0) {
			warn("fork_exec: Couldn't open ferr: %d", errno);
			r = -errno;
			goto err_pout;
		}
		r = fcntl(perr[0], F_GETFL);
		if (r < 0) {
			warn("fork_exec: Can't get ferr flags: %d", errno);
			r = -errno;
			goto err_perr;
		}
		r = fcntl(perr[0], F_SETFL, r | O_NONBLOCK);
		if (r < 0) {
			warn("fork_exec: Can't set ferr O_NONBLOCK: %d", errno);
			r = -errno;
			goto err_perr;
		}
	}

	wlogs(DEBUG, "fork_exec: Forking: ");
	for (int i = 0; cmd[i]; ++i) wlogm(DEBUG, "%s ", cmd[i]);
	wloge(DEBUG);

	pid_t cpid = fork();
	if (cpid == -1) {
		warn("fork_exec: Couldn't fork: %d", errno);
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
		else if (r != -1 && !freopen("/dev/null", "w", stdout))
			r = -1;
		if (ferr && r != -1)
			while ((r = dup2(perr[1], STDERR_FILENO)) == -1 &&
			       errno == EINTR) ;
		else if (r != -1 && !freopen("/dev/null", "w", stderr))
			r = -1;
		if (r == -1) {
			crit("Could not change stdio file descriptors: %d", errno);
			_exit(errno);
		}

		execv(cmd[0], cmd);
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
		warn("fork_exec: Couldn't close fin: %d", errno);
		r = -errno;
	}
	if(fout && close(pout[1]) == -1 && errno != EINTR) {
		warn("fork_exec: Couldn't close fout: %d", errno);
		r = r ? r : -errno;
	}
	if(ferr && close(perr[1]) == -1 && errno != EINTR) {
		warn("fork_exec: Couldn't close ferr: %d", errno);
		r = r ? r : -errno;
	}

	return r;

err_perr:
	if (ferr) {
		if (close(perr[0]))
			warn("fork_exec: Couldn't close perr0: %d", errno);
		if (close(perr[1]))
			warn("fork_exec: Couldn't close perr1: %d", errno);
	}
err_pout:
	if (fout) {
		if (close(pout[0]))
			warn("fork_exec: Couldn't close pout0: %d", errno);
		if (close(pout[1]))
			warn("fork_exec: Couldn't close pout1: %d", errno);
	}
err_pin:
	if (fin) {
		if (close(pin[0]))
			warn("fork_exec: Couldn't close pin0: %d", errno);
		if (close(pin[1]))
			warn("fork_exec: Couldn't close pin1: %d", errno);
	}
	return r;
}

int get_parent_pid(pid_t pid, pid_t *out)
{
	char *path = NULL;
	char *stat = NULL, *offset;
	int fd;
	int r = 0;

	if (!out)
		return -EINVAL;

	r = bprintf(&path, "/proc/%u/stat", pid);
	if (r)
		return r;

	while ((fd = open(path, O_RDONLY)) == -1 && errno == -EINTR) ;;
	if (fd == -1) {
		warn("get_parent: Error opening stat file: %d", errno);
		r = -errno;
		goto err_path;
	}

	r = read_available(fd, WTC_RDAVL_CSTRING, NULL, &stat); 
	if (r < 0)
		goto err_fd;

	offset = strrchr(stat, ')');
	if (!offset) {
		crit("get_parent: stat file of pid %d has invalid format!", pid);
		r = -EINVAL;
		goto err_fd;
	}

	r = sscanf(offset, ") %*c %d", out);
	if (--r != 0) {
		crit("get_parent: stat file of pid %d has invalid format!", pid);
		r = -EINVAL;
	}

err_fd:
	if (close(fd)) {
		warn("get_parent: Error closing stat file: %d", errno);
		r = r ? r : -errno;
	}
err_path:
	free(path);
	return r;
}
