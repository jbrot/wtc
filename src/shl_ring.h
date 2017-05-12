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

#include <stdbool.h>
#include <sys/uio.h>

struct shl_ring {
	char *buf;
	size_t size;
	size_t start;
	size_t end;
};

bool shl_ring_empty(struct shl_ring *r);

/*
 * Resize ring-buffer to provide enough room for @add bytes of new data.
 * This resizes the buffer if it is too small. It returns -ENOMEM on OOM
 * and 0 on success.
 */
int shl_ring_grow(struct shl_ring *r, size_t add);

/*
 * Push @len bytes from @u8 into the ring buffer. The buffer is resized if
 * it is too small. -ENOMEM is returned on OOM, 0 on success.
 */
int shl_ring_push(struct shl_ring *r, const char *u8, size_t len);

/*
 * Get data pointers for current ring-buffer data. @vec must be an array of
 * 2 iovec objects. They are filled according to the data available in the
 * ring-buffer. 0, 1 or 2 is returned according to the number of iovec
 * objects that were filled (0 meaning buffer is empty).
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

/*
 * r is a (struct shl_ring *), vl is a (char), ivc is a (struct iovec[2]),
 * sz is a (size_t), and i is a (size_t). Expands to a for loop which will
 * have vl iterate through the contents of the ring. ivc is used to store 
 * the pointers to the rings' contents, sz is used to keep track of the
 * total size of the ring and i is used to keep track of the iteration
 * progress. i starts with a value of zero and increases by one each
 * iteration.
 */
#define SHL_RING_ITERATE(r, vl, ivc, sz, i)                        \
for( (sz) = shl_ring_peek((r), (ivc)),                             \
     (sz) = (sz) == 2 ? ((ivc)[0].iov_len + (ivc)[1].iov_len)      \
                        : (sz) == 1 ? (ivc)[0].iov_len : 0,        \
     (i) = 0,                                                      \
     (vl) = (sz) ? ((char *) (ivc)[0].iov_base)[0] : (vl);         \
     (i) < (sz);                                                   \
     ++(i),                                                        \
     (vl) = (i) >= (sz) ? (vl) : (i) < (ivc)[0].iov_len            \
            ? ((char *) (ivc)[0].iov_base)[(i)]                    \
            : ((char *) (ivc)[1].iov_base)[(i) - (ivc)[0].iov_len] )
#endif // !SHL_RING_H
