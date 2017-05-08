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
#include <stdbool.h>
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
			} else if (tmp && (mode & WTC_RDAVL_BUF)) {
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

		if (disc || (mode & WTC_RDAVL_CSTRING)) {
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
