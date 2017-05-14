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

#define WTC_TMUX_TEMP_SESSION_NAME "__wtc_tmux_tmp"

struct wtc_tmux_cc;

/*
 * wtc_tmux_cbs is a wrapper for the callback functions to keep the actual
 * wtc_tmux definition simpler.
 */
struct wtc_tmux_cbs {
	int (*client_session_changed)(struct wtc_tmux *tmux,
	                              const struct wtc_tmux_client *client);

	int (*new_session)(struct wtc_tmux *tmux,
	                   const struct wtc_tmux_session *session);
	int (*session_closed)(struct wtc_tmux *tmux,
	                      const struct wtc_tmux_session *session);
	int (*session_window_changed)(struct wtc_tmux *tmux,
	                              const struct wtc_tmux_session *session);

	int (*new_window)(struct wtc_tmux *tmux,
	                  const struct wtc_tmux_window *window);
	int (*window_closed)(struct wtc_tmux *tmux,
	                     const struct wtc_tmux_window *window);
	int (*window_pane_changed)(struct wtc_tmux *tmux,
	                           const struct wtc_tmux_window *window);

	int (*new_pane)(struct wtc_tmux *tmux,
	                const struct wtc_tmux_pane *pane);
	int (*pane_closed)(struct wtc_tmux *tmux,
	                   const struct wtc_tmux_pane *pane);
	int (*pane_resized)(struct wtc_tmux *tmux,
	                    const struct wtc_tmux_pane *pane);
	int (*pane_mode_changed)(struct wtc_tmux *tmux,
	                         const struct wtc_tmux_pane *pane);
};

/*
 * Contains all the necessary information to invoke a callback.
 */
struct wtc_tmux_cb_closure {
	int fid;
#define WTC_TMUX_CB_EMPTY                   0
#define WTC_TMUX_CB_CLIENT_SESSION_CHANGED  1
#define WTC_TMUX_CB_NEW_SESSION             2
#define WTC_TMUX_CB_SESSION_CLOSED          3
#define WTC_TMUX_CB_SESSION_WINDOW_CHANGED  4
#define WTC_TMUX_CB_NEW_WINDOW              5
#define WTC_TMUX_CB_WINDOW_CLOSED           6
#define WTC_TMUX_CB_WINDOW_PANE_CHANGED     7
#define WTC_TMUX_CB_NEW_PANE                8
#define WTC_TMUX_CB_PANE_CLOSED             9
#define WTC_TMUX_CB_PANE_RESIZED           10
#define WTC_TMUX_CB_PANE_MODE_CHANGED      11
	struct wtc_tmux *tmux;
	union {
		struct wtc_tmux_pane *pane;
		struct wtc_tmux_window *window;
		struct wtc_tmux_session *session;
		struct wtc_tmux_client *client;
	} value;
	bool free_after_use;
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

	int refresh;
#define WTC_TMUX_REFRESH_PANES    (1<<0)
#define WTC_TMUX_REFRESH_WINDOWS  (1<<1)
#define WTC_TMUX_REFRESH_SESSIONS (1<<2)
#define WTC_TMUX_REFRESH_CLIENTS  (1<<3)
	int refreshfd;
	struct wlc_event_source *rfev;

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

	struct wtc_tmux_cb_closure *closures;
	size_t closure_size; /* Amount used by closures */
	size_t closure_len; /* Amount allocated for closures */
};

/*
 * wtc_tmux_cc represents a long running control mode tmux process.
 */
struct wtc_tmux_cc {
	unsigned long ref;

	struct wtc_tmux *tmux;
	struct wtc_tmux_session *session; // This may be NULL if temp is true

	pid_t pid;
	bool temp;
	int fin;
	int fout; // This will be closed automatically when removing outs
	struct wlc_event_source *outs;
	struct shl_ring buf;

	struct wtc_tmux_cc *previous;
	struct wtc_tmux_cc *next;

	/* 
	 * This callback is invoked when the control process responds to
	 * a command. This should probably not be used directly. Instead,
	 * user wtc_tmux_cc_exec.
	 */
	bool compensate;
	void *userdata;
	int (*cmd_cb)(struct wtc_tmux_cc *cc, size_t st, size_t l, bool err);
};

/*
 * The following functions are implemented in tmux.c
 */

void wtc_tmux_pane_free(struct wtc_tmux_pane *pane);
void wtc_tmux_window_free(struct wtc_tmux_window *window);
void wtc_tmux_session_free(struct wtc_tmux_session *sess);
void wtc_tmux_client_free(struct wtc_tmux_client *client);

/*
 * A specialized version of waitpid that will, after the timeout value in
 * the object object elapses, kill the specified child to force termination.
 *
 * NOTE: tmux must not be NULL and pid must be positive (i.e., this will
 * only work for a specific process and not an arbitrary process or process 
 * group.
 */
int wtc_tmux_waitpid(struct wtc_tmux *tmux, pid_t pid, int *stat, int opt);

int wtc_tmux_add_closure(struct wtc_tmux *, struct wtc_tmux_cb_closure);
/*
 * Run the specified closure. Returns 0 on success and 1 on failure.
 * On success, fid will be set to WTC_TMUX_CB_EMPTY. Furthermore, if
 * free_after_use is set, the resource in value will be freed and
 * free_after_use will be reset. Thus, calling this function a second time
 * on the same closure will have no effect and, furthermore, it is safe
 * to call wtc_tmux_clear_closures even if this function has been called
 * on some of the closures already.
 */
int wtc_tmux_closure_invoke(struct wtc_tmux_cb_closure *closure);
/*
 * Clear all of the closures currently queued without executing. If a
 * closure has free_after_use set, its resource will be freed.
 */
void wtc_tmux_clear_closures(struct wtc_tmux *tmux);

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
 * Adjust the size of the control client per the linked tmux's setting.
 */
int wtc_tmux_cc_update_size(struct wtc_tmux_cc *cc);

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
 * Functionally equivalent to wtc_tmux_exec. However, instead of forking
 * a new tmux instance, the command is run on the specified wtc_tmux_cc.
 */
int wtc_tmux_cc_exec(struct wtc_tmux_cc *cc, const char *const *cmds,
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

/*
 * This function gets called whenever a wtc_tmux_cc's process outputs data.
 * It will process as much of the available data as possible. Returns 0 on
 * success or a negative error code.
 */
int wtc_tmux_cc_process_output(struct wtc_tmux_cc *cc);

/*
 * Called to refresh part or all of the server state. userdata is
 * a (struct wtc_tmux *).
 */
int wtc_tmux_refresh_cb(int fd, uint32_t mask, void *userdata);

/*
 * Schedule the specified flags to be refreshed.
 */
int wtc_tmux_queue_refresh(struct wtc_tmux *tmux, int flags);

#endif // !WTC_TMUX_INTERNAL_H
