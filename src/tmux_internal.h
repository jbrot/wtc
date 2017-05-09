/*
 * wtc - tmux_internal.h
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
 * wtc - Tmux Internal Interface
 *
 * This file contains the internal definitions used by the tmux handler.
 * You probably shouldn't have to use anything here.
 */

#ifndef WTC_TMUX_INTERNAL_H
#define WTC_TMUX_INTERNAL_H

#include "tmux.h"

#include "shl_ring.h"

#include <signal.h>

struct wtc_tmux_cc;

/*
 * wtc_tmux_cbs is a wrapper for the callback functions to keep the actual
 * wtc_tmux definition simpler.
 */
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

/*
 * The actual wtc_tmux definition.
 */
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

/*
 * wtc_tmux_cc represents a long running control mode tmux process.
 */
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

/*
 * The following functions are implemented in tmux.c
 */

void wtc_tmux_pane_free(struct wtc_tmux_pane *pane);
void wtc_tmux_window_free(struct wtc_tmux_window *window);
void wtc_tmux_session_free(struct wtc_tmux_session *sess);
void wtc_tmux_client_free(struct wtc_tmux_client *client);

/*
 * The following functions are implemented in tmux_process.c
 */

void wtc_tmux_cc_ref(struct wtc_tmux_cc *cc);
void wtc_tmux_cc_unref(struct wtc_tmux_cc *cc);

/*
 * Launch a control client on the specified session.
 */
int wtc_tmux_cc_launch(struct wtc_tmux *tmux, struct wtc_tmux_session *s);

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
int wtc_tmux_fork(struct wtc_tmux *tmux, const char *const *cmds,
                  pid_t *pid, int *fin, int *fout, int *ferr);

/*
 * Run the command specified in cmds, wait for it to finish, and store its
 * stdout in out and stderr in err. Out and err may be NULL to indicate the
 * respective streams should be ignored. Out and err will be populated using
 * read_available with mode WTC_RDAVL_CSTRING | WTC_RDAVL_BUF. Thus, if *out
 * or *err are not NULL, then they are expected to point to an existing 
 * c string. On success the newly read data will be appended to the existing
 * data.
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
int wtc_tmux_exec(struct wtc_tmux *tmux, const char *const *cmds,
                  char **out, char **err);

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
int wtc_tmux_get_option(struct wtc_tmux *tmux, const char *name,
                        int target, int mode, char **out);

/*
 * The following functions are implemented in tmux_parse.c
 */

/*
 * Ensures the version of tmux is new enough to support the needed messages.
 * Returns 1 if the versions is new enough and 0 if it is not. Will return
 * a negative error code if something goes wrong checking the version.
 */
int wtc_tmux_version_check(struct wtc_tmux *tmux);

/*
 * Reload the panes on the server. Note that, when calling this, it is
 * imperative that the windows are already up to date. Depending on where
 * this fails, the server representation may be left in a corrupted state
 * and the only viable method of recovery is to retry this function.
 *
 * WARNING: There is a race condition because there is no synchronization
 * between multiple calls to tmux and this function requires several calls.
 */
int wtc_tmux_reload_panes(struct wtc_tmux *tmux);

/*
 * Reload the windows on the server. Note that, when calling this, it is
 * imperative that the sessions are already up to date. Depending on where
 * this fails, the server representation may be left in a corrupted state
 * and the only viable method of recovery is to retry this function. As a
 * result of calling this function, the panes will be reloaded as well.
 *
 * WARNING: There is a race condition because there is no synchronization
 * between multiple calls to tmux. This function itself only needs one
 * call, but it also calls wtc_tmux_reload_panes which requires several.
 */
int wtc_tmux_reload_windows(struct wtc_tmux *tmux);

/*
 * Reload the clients on the server. Note that, when calling this, it is
 * imperative that the sessions are already up to date. Depending on where
 * this fails, the server representation may be left in a corrupted state
 * and the only viable method of recovery is to retry this function.
 *
 * Note: Unlike the other reload functions, this one only requires one call
 * to tmux and so does not have a race condition.
 */
int wtc_tmux_reload_clients(struct wtc_tmux *tmux);

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
int wtc_tmux_reload_sessions(struct wtc_tmux *tmux);

#endif // !WTC_TMUX_INTERNAL_H
