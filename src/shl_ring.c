/*
 * Adapted from shl_pty.c
 *
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@gmail.com>
 * Dedicated to the Public Domain
 */

#include "shl_ring.h"

#include "log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * SHL_RING_MASK(ring, position) ensures position is the correct index in
 * the array. It will compensate for the ring edge.
 */
#define SHL_RING_MASK(_r, _v) ((_v) & ((_r)->size - 1))

bool shl_ring_empty(struct shl_ring *r)
{
	return r->start == r->end;
}

/*
 * Resize ring-buffer to size @nsize. @nsize must be a power-of-2, otherwise
 * ring operations will behave incorrectly.
 */
static int ring_resize(struct shl_ring *r, size_t nsize)
{
	char *buf;

	buf = malloc(nsize);
	if (!buf) {
		crit("ring_resize: Couldn't allocate buf!");
		return -ENOMEM;
	}

	if (r->end == r->start) {
		r->end = 0;
		r->start = 0;
	} else if (r->end > r->start) {
		memcpy(buf, &r->buf[r->start], r->end - r->start);

		r->end -= r->start;
		r->start = 0;
	} else {
		memcpy(buf, &r->buf[r->start], r->size - r->start);
		memcpy(&buf[r->size - r->start], r->buf, r->end);

		r->end += r->size - r->start;
		r->start = 0;
	}

	free(r->buf);
	r->buf = buf;
	r->size = nsize;

	return 0;
}

/* Compute next higher power-of-2 of @v. Returns 4096 in case v is 0. */
static size_t ring_pow2(size_t v)
{
	size_t i;

	if (!v)
		return 4096;

	--v;

	for (i = 1; i < 8 * sizeof(size_t); i *= 2)
		v |= v >> i;

	return ++v;
}

int shl_ring_grow(struct shl_ring *r, size_t add)
{
	size_t len;

	/*
	 * Note that "end == start" means "empty buffer". Hence, we can never
	 * fill the last byte of a buffer. That means, we must account for an
	 * additional byte here ("end == start"-byte).
	 */

	if (r->end < r->start)
		len = r->start - r->end;
	else
		len = r->start + r->size - r->end;

	/* don't use ">=" as "end == start" would be ambigious */
	if (len > add)
		return 0;

	/* +1 for additional "end == start" byte */
	len = r->size + add - len + 1;
	len = ring_pow2(len);

	if (len <= r->size) {
		warn("shl_ring_grow: Can't add %d bytes to ring, as that overflows "
		     "size_t!", add);
		return -ENOMEM;
	}

	return ring_resize(r, len);
}

int shl_ring_push(struct shl_ring *r, const char *u8, size_t len)
{
	int err;
	size_t l;

	err = shl_ring_grow(r, len);
	if (err < 0)
		return err;

	if (r->start <= r->end) {
		l = r->size - r->end;
		if (l > len)
			l = len;

		memcpy(&r->buf[r->end], u8, l);
		r->end = SHL_RING_MASK(r, r->end + l);

		len -= l;
		u8 += l;
	}

	if (!len)
		return 0;

	memcpy(&r->buf[r->end], u8, len);
	r->end = SHL_RING_MASK(r, r->end + len);

	return 0;
}

size_t shl_ring_peek(struct shl_ring *r, struct iovec *vec)
{
	if (r->end > r->start) {
		vec[0].iov_base = &r->buf[r->start];
		vec[0].iov_len = r->end - r->start;
		return 1;
	} else if (r->end < r->start) {
		vec[0].iov_base = &r->buf[r->start];
		vec[0].iov_len = r->size - r->start;
		vec[1].iov_base = r->buf;
		vec[1].iov_len = r->end;
		return 2;
	} else {
		return 0;
	}
}

void shl_ring_pop(struct shl_ring *r, size_t len)
{
	size_t l;

	if (r->start > r->end) {
		l = r->size - r->start;
		if (l > len)
			l = len;

		r->start = SHL_RING_MASK(r, r->start + l);
		len -= l;
	}

	if (!len)
		return;

	l = r->end - r->start;
	if (l > len)
		l = len;

	r->start = SHL_RING_MASK(r, r->start + l);
}
