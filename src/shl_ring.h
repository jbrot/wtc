/*
 * Adapted from shl_pty.c
 *
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@gmail.com>
 * Dedicated to the Public Domain
 */

/*
 * Ring Buffer
 */

#ifndef SHL_RING_H
#define SHL_RING_H

#include <sys/uio.h>

// TODO perhaps this should be opaque and have ref counting.
struct shl_ring {
	char *buf;
	size_t size;
	size_t start;
	size_t end;
};

/*
 * Resize ring-buffer to provide enough room for @add bytes of new data. This
 * resizes the buffer if it is too small. It returns -ENOMEM on OOM and 0 on
 * success.
 */
int shl_ring_grow(struct shl_ring *r, size_t add);

/*
 * Push @len bytes from @u8 into the ring buffer. The buffer is resized if it
 * is too small. -ENOMEM is returned on OOM, 0 on success.
 */
int shl_ring_push(struct shl_ring *r, const char *u8, size_t len);

/*
 * Get data pointers for current ring-buffer data. @vec must be an array of 2
 * iovec objects. They are filled according to the data available in the
 * ring-buffer. 0, 1 or 2 is returned according to the number of iovec objects
 * that were filled (0 meaning buffer is empty).
 *
 * Hint: "struct iovec" is defined in <sys/uio.h> and looks like this:
 *     struct iovec {
 *         void *iov_base;
 *         size_t iov_len;
 *     };
 */
size_t shl_ring_peek(struct shl_ring *r, struct iovec *vec);

/*
 * Remove @len bytes from the start of the ring-buffer. Note that we protect
 * against overflows so removing more bytes than available is safe.
 */
void shl_ring_pop(struct shl_ring *r, size_t len);

#endif // !SHL_RING_H
