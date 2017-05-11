/*
 * wtc - rdavail.c
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

#include "rdavail.h"

#include "log.h"
#include "shl_ring.h"

#include <errno.h>
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

int parselni(const char *fmt, char *str, int *olen, int **out)
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
