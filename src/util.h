/*
 * wtc - util.h
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
 * This file contains some general purpose utility functions I wrote for
 * wtc.
 */

#ifndef WTC_RDAVL_H
#define WTC_RDAVL_H

#include <sys/types.h>

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
 *   WTC_RDAVL_DISCARD, WTC_RDAVL_CSTRING, WTC_RDAVL_STANDARD
 *
 *   WTC_RDAVL_BUF, WTC_RDAVL_RING
 *
 * If no flag from the first list is included, WTC_RDAVL_DISCARD will
 * be chosen by default and, likewise, WTC_RDAVL_BUF will be chosen if
 * no flag is included from the second list. Depending on which flags are
 * provided, the interpretation of size and out changes.
 *
 * WTC_RDAVL_DISCARD:
 *   The contents of the file descriptor are read and immediately discarded.
 *   This an be useful for clearing out a buffer. If size is not NULL, the
 *   total amount of bytes read will be stored in *size. If this flag is set
 *   all other flags are ignored (in particular, the value of out is ignored
 *   and left unchanged).
 *
 * WTC_RDAVL_CSTRING:
 *   The contents of the file descriptor are read into the appropriate
 *   structure (see WTC_RDAVL_BUF and WTC_RDAVL_RING) followed by
 *   a '\0' terminator. If '\0' occurs within the content read, it will
 *   be replace with 0x01 such that for each call of read_available, exactly
 *   one '\0' will be output, and it is guaranteed to be the last character
 *   output. If size is not NULL and WTC_RDAVL_BUF is set, the total
 *   amount of bytes read (that is, the '\0' terminator is not included) 
 *   will be stored in *size. If size is not NULL and WTC_RDAVL_RING is
 *   set, then the total amount of bytes written to the ring will be stored
 *   in *size (that is, the '\0' terminator IS included).
 *
 * WTC_RDAVL_STANDARD:
 *   The contents of the file descriptor are read into the appropriate
 *   structure (see WTC_RDAVL_BUF and WTC_RDAVL_RING). The data read
 *   will be unprocessed. If size is not NULL, the total amount of bytes
 *   read will be stored in *size.
 *
 * WTC_RDAVL_BUF:
 *   The contents read from the file descriptor are be to stored in a newly
 *   allocated buffer of type char *. out will be interpreted as type
 *   (char **). After a successful read, the new data will be stored in
 *   *out. If *out is not NULL, the contents of *out will be copied to the
 *   start of the newly allocated buffer and any read data will be appended
 *   to them. Upon a successful read, the current contents of *out will be
 *   freed and *out will be set to point to the new buffer. If
 *   WTC_RDAVL_CSTRING is set, the size of *out will be determined via
 *   strlen (thus, the '\0' terminator will not be copied over and the
 *   buffer at *out upon a successful call will contain exactly one '\0').
 *   If WTC_RDAVL_STANDARD is set, the size of *out will be determined
 *   via the value stored in *size.
 *
 * WTC_RDAVL_RING:
 *   The contents read from the file descriptor are to be stored in an
 *   existing shl_ring (see shl_ring.h). out will be interpeted as type
 *   (struct shl_ring *) and the data read will be appended via
 *   shl_ring_push. Note that unlike with WTC_RDAVL_BUF, data is always
 *   appended to an existing structure and, furthermore, no existing data
 *   is removed. In particular, this means that multiple calls to
 *   read_available with WTC_RDAVL_CSTRING set will result in multiple
 *   '\0' characters being added to the ring (one after the contents read
 *   by each invocation). This is in direct contrast with WTC_RDAVL_BUF
 *   where multiple calls with WTC_RDAVL_CSTRING set will result in a
 *   single '\0' character at the end of the total amount read. To emulate
 *   this behavior with WTC_RDAVL_RING set, simply decrement
 *   shl_ring->end before calling read_available a second time to "unset"
 *   the '\0' terminator. Be careful, though, that if shl_ring->end == 0,
 *   then shl_ring->end needs to be set to  (shl_ring->size - 1) not -1.
 *
 * read_available returns 0 on success, and a negative error code on
 * failure. The errors that can occur are:
 *
 * -ENOMEM:
 *   An issue occurred allocating memory for the read buffer or, if
 *   WTC_RDAVL_RING is set, increasing the size of the ring buffer.
 *
 * -EINVAL:
 *   One of the following situations occured: WTC_RDAVL_CSTRING and
 *   WTC_RDAVL_STANDARD were set, out was NULL and WTC_RDAVL_DISCARD
 *   was not set, or *out was not NULL, WTC_RDAVL_BUF was set and either
 *   size was NULL or *size was negative.
 *
 * In addition, if a read call fails, then the following behavior occurs:
 * if the error is EINTR, the error is ignored and the read is tried again;
 * if the error is EAGAIN or EWOULDBLOCK, the error is ignored and the read
 * is considered finished; otherwise, the read is aborted and -errno is
 * returned.
 *
 * NOTE: If the read fails and either WTC_RDAVL_DISCARD or 
 *       WTC_RDAVL_BUF is set, then size and out are unchanged
 *       (although the file descriptor state will most likely be different
 *       as content may have been read before the error). However, if
 *       instead WTC_RDAVL_RING is set, then content is added to the 
 *       ring immediately after being read. If some content is written to 
 *       the ring and the function fails, then size will be updated to 
 *       reflect the amount of data added to the ring. Furthermore, if
 *       WTC_RDAVL_CSTRING is set, the null terminator will still be
 *       added. However, if the error occurs while adding data to the ring,
 *       size will not be updated. Note that the only error that can occur
 *       when adding to the ring is ENOMEM in which case there isn't really
 *       much hope of recovering, anyway.
 */
#define WTC_RDAVL_DISCARD  (0 << 0)
#define WTC_RDAVL_CSTRING  (1 << 0)
#define WTC_RDAVL_STANDARD (2 << 0)

#define WTC_RDAVL_BUF      (0 << 2)
#define WTC_RDAVL_RING     (1 << 2)
int read_available(int fd, int mode, int *size, void *out);

/*
 * Dynamically allocate memory for the specified printf operation and
 * then store the result in it. Returns 0 on success and negative error
 * value. out must be non-NULL while *out must be NULL.
 */
__attribute__((format (printf, 2, 3)))
int bprintf(char **out, const char *format, ...);

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
 * a copy before calling one of these function.
 *
 * Parse an integer and a string. Note that the format should parse 
 * everything up until the string and then the rest of the line will be 
 * parsed as the string.
 *
 * Example parselnis fmt: "$%u %n"
 */
int parselnis(const char *fmt, char *str, int *olen, int **out,
              char ***out2);

/*
 * Parse three integers per line.
 *
 * Example parselniii format: "$%u @%u %u%n"
 */
int parselniii(const char *fmt, char *str, int *olen, int **out, 
               int **out2, int **out3);

/*
 * Parse two integers and a string per line. Note that the format should 
 * parse everything up until the string and then the rest of the line will 
 * be parsed as the string.
 *
 * Example parselniis format: "$%u @%u %n"
 */
int parselniis(const char *fmt, char *str, int *olen, int **out,
               int **out2, char ***out3);

/*
 * strtokd functions identically to strtok_r except that the character
 * which is overwritten to makr the end of the token is stored in fdelim.
 *
 * delim and saveptr must not be NULL. fdelim may be NULL (in which case
 * the changed token will not be stored)
 */
char *strtokd(char *str, const char *delim, char **saveptr, char *fdelim);

/*
 * Fork and exec cmds. The resulting process' id will be put in pid.
 * If fin, fout, or ferr are not NULL, then they will be set to a file
 * descriptor which is the end of a pipe to stdin, stdout, and stderr of the
 * child process respectively.
 *
 * Returns 0 on success and a negative value if an error occurs. However, if
 * the error occurs after forking (i.e., when closing the child half of the
 * pipes), then pid, fin, fout, and ferr will be properly populated.
 */
int fork_exec(char *const *cmds, pid_t *pid, int *fin, 
              int *fout, int *ferr);

/*
 * Find the parent pid of the process with the specified pid by parsing the
 * file /proc/<pid>/stat.  The parent pid will be stored in out. Returns 0 
 * on sucess and a negative value if an error occurs. Note that if an error
 * occurs when closing the file descriptor, an error will be reported but
 * the pid will still be stored in out.
 *
 * Note: This function assumes the user has procfs mounted at /proc. This is
 * almost always a good assumption.
 */
int get_parent_pid(pid_t pid, pid_t *out);

#endif // !WTC_RDAVL_H
