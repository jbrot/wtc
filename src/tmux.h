/*
 * wtc - tmux.h
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
 * wtc - Tmux Handling
 *
 * The functions in this file describe the tmux interface. The interface
 * uses tmux's control mode to retrieve information about a tmux session
 * such that the window manager can properly overlay windows.
 *
 * Every function which changes the state of the wtc_tmux object has an int
 * return value. 0 means the operation was successful whereas a negative
 * value indicates an error occurred. See individual functions for the
 * specific error codes.
 */

#ifndef WTC_TMUX_H
#define WTC_TMUX_H

#include <stdbool.h>

#include "uthash.h"

struct wtc_tmux;
struct wtc_tmux_client;
struct wtc_tmux_window;

/*
 * Describes a tmux pane. A pane represents one pseudo terminal which may be
 * displayed to a client. A pane is associated with a unique window.
 * wtc_tmux_panes form a doubly linked list which contain the panes
 * associated with a given window.
 */
struct wtc_tmux_pane
{
	/* The pane's tmux id. */
	int id;
	/*
	 * The pid of the root process of the pane. This is the process tmux
	 * launches when it creates the pane, and tmux will destroy the pane
	 * if this process terminates. Typically, this will be a shell like
	 * bash.
	 */
	int pid;
	/*
	 * Whether or not this is the active pane in its window. Only one pane
	 * will be active in a window.
	 */
	bool active;

	/* The window which includes this pane. */
	struct wtc_tmux_window *window;
	/*
	 * The adjacent panes in the linked list. These panes are guaranteed to
	 * be in the same window, but there is no guarantee on the specifics of
	 * the order (i.e., they are not necessarily sorted by id or by display
	 * location). These will be NULL if this is the first or last pane in
	 * the list respectively.
	 */
	struct wtc_tmux_pane *previous;
	struct wtc_tmux_pane *next;

	/*
	 * The pane's extents within the window. The origin is the top-left
	 * corner of the display. Positive x is to the right and positive y is
	 * down.  Note that the status bar is not accounted for by these
	 * coordinates.
	 */
	int x;
	int y;
	int w;
	int h;

	/* So we can be in a hash map. */
	UT_hash_handle hh;
};

/*
 * Describes a tmux window. A window represents the full screen being shown
 * to a session and is comprised of multiple panes which handle the actual
 * content. A window is associated with a unique session group but not
 * necessarily a unique session.
 */
struct wtc_tmux_window
{
	/* The window's tmux id. */
	int id;

	/* The active pane in the window. */
	struct wtc_tmux_pane *active_pane;
	/* The number of panes in this window. */
	int pane_count;
	/*
	 * The first pane associated with this window. Using
	 * wtc_tmux_pane->next, you can use this to iterate through all of the
	 * linked panes.
	 */
	struct wtc_tmux_pane *panes;

	/* So we can be in a hash map. */
	UT_hash_handle hh;
};

/*
 * Describes a tmux session. A session consists of an arbitrary amount of
 * windows, one of which is marked active. Clients connect to a session. A
 * session can have an arbitrary amount of connected clients. Each client
 * will view the same content---the active window. Although each session has
 * a unique active window, several sessions can share a window (and in fact
 * one session can have the same window multiple times), so whether or not
 * a window is active depends on which session it is with respect to. In
 * fact this ambiguity with sessions and windows means that we can't even
 * sort the windows into linked lists with respect to sessions (like how
 * clients and panes are a linked list). Instead each session maintains an
 * array of its windows which is independent from the other windows.
 */
struct wtc_tmux_session
{
	/* The session's tmux id. */
	int id;

	/*
	 * The position of the staus bar for this session. Either
	 * WTC_TMUX_SESSION_TOP, WTC_TMUX_SESSION_BOTTOM, or
	 * WTC_TMUX_SESSION_OFF.
	 */
	int statusbar;
#define WTC_TMUX_SESSION_OFF 0
#define WTC_TMUX_SESSION_TOP 1
#define WTC_TMUX_SESSION_BOTTOM 2

	/* The window this session is currently viewing. */
	struct wtc_tmux_window *active_window;
	/* The number of windows linked to this session. */
	int window_count;
	/*
	 * The first window associated with this session. Using
	 * wtc_tmux_window->next you can iterate through all of the windows
	 * associated with this session.
	 */
	struct wtc_tmux_window **windows;

	/*
	 * The first client linked with this session. Using
	 * wtc_tmux_client->next you can iterate through all of the clients
	 * associated with this session.
	 */
	struct wtc_tmux_client *clients;

	/* So we can be in a hash map. */
	UT_hash_handle hh;
};

/*
 * Describes a tmux client. A client represents an instance of the tmux
 * process which has connected to the server. A client is associated with a
 * unique session and is shown the active window on that session. Every
 * client linked to a given session receives the same content.
 * wtc_tmux_clients constitute a doubly linked list which compruises the
 * clients attached to a specific session.
 */
struct wtc_tmux_client
{
	/* The client's pid. */
	pid_t pid;
	/* The client's name. */
	const char *name;

	/* The client's attached session. */
	struct wtc_tmux_session *session;
	/*
	 * The adjacent clients in the linked list. These clients are guaranteed
	 * to be attached to the same session, but their order is arbitrary.
	 * These will be NULL if this is the first or last client in the list
	 * respectively.
	 */
	struct wtc_tmux_client *previous;
	struct wtc_tmux_client *next;

	/* So we can be in a hash map. */
	UT_hash_handle hh;
};

/*
 * Create a new wtc_tmux object. This can fail with -ENOMEM.
 */
int wtc_tmux_new(struct wtc_tmux **out);
/*
 * Adjust the reference count. If the reference count falls to zero, the
 * object will be freed and the underlying processes shut down. The
 * underlying process will be shut down synchronously. If the tmux does not
 * terminate within the timeout period, it will killed via SIGKILL (see
 * wtc_tmux_set_timeout).
 */
void wtc_tmux_ref(struct wtc_tmux *tmux);
void wtc_tmux_unref(struct wtc_tmux *tmux);

/*
 * The following functions describe how the tmux binary will be launched
 * to properly connect to the server. If the wtc_tmux object is currently
 * connected, the write functions will fail with -EBUSY and have no effect.
 */

/*
 * bin_file is the tmux binary file. It defaults to "/usr/bin/tmux". This
 * file will be launched with exec. This binary is not verified, so it is
 * very important that the correct binary is specified as this can be used
 * to perform arbitrary code execution.
 *
 * bin_file will initially be set to NULL at object creation and may be NULL
 * when the object is not connected. A NULL value indicates that the default
 * value is to be used, and it will be filled in during connection. If the
 * wtc_tmux object is connected, bin_file is guaranteed to be not NULL.
 *
 * The string provided will be copied into the internal structure. -EINVAL
 * will be returned if the tmux object is NULL, and -ENOMEM will be returned
 * if the string could not be duplicated.
 *
 * Passing NULL to wtc_tmux_get_bin_file is an error and will result in NULL
 * being dereferenced.
 */
int wtc_tmux_set_bin_file(struct wtc_tmux *tmux, const char *path);
const char *wtc_tmux_get_bin_file(const struct wtc_tmux *tmux);

/*
 * socket_name corresponds to the "-L" option while socket_path corresponds
 * to the "-S" option. See tmux documentation for details. Setting one of
 * these values to a non-NULL value will cause the other to be set to NULL.
 * wtc_tmux_is_socket_set will report if either value is non-NULL.
 *
 * The string provided will be copied into the internal structure. -EINVAL
 * will be returned if the tmux object is NULL, and -ENOMEM will be returned
 * if the string could not be duplicated.
 *
 * Passing NULL to wtc_tmux_get_socket_name, wtc_tmux_get_socket_path, or
 * wtc_tmux_is_socket_set is an error and will result in NULL being
 * dereferenced.
 */
int wtc_tmux_set_socket_name(struct wtc_tmux *tmux, const char *name);
int wtc_tmux_set_socket_path(struct wtc_tmux *tmux, const char *path);
const char *wtc_tmux_get_socket_name(const struct wtc_tmux *tmux);
const char *wtc_tmux_get_socket_path(const struct wtc_tmux *tmux);
bool wtc_tmux_is_socket_set(const struct wtc_tmux *tmux);

/*
 * This corresponds to the "-f" option. See tmux documentation for details.
 *
 * The string provided will be copied into the internal structure. -EINVAL
 * will be returned if the tmux object is NULL, and -ENOMEM will be returned
 * if the string could not be duplicated.
 *
 * Passing NULL to wtc_tmux_get_config_file is an error and will result in
 * NULL being dereferenced.
 */
int wtc_tmux_set_config_file(struct wtc_tmux *tmux, const char *file);
const char *wtc_tmux_get_config_file(const struct wtc_tmux *tmux);

/*
 * These functions start and stop the interactions with tmux. The details
 * for how the tmux connection will be configured can be set using the
 * previous functions. Once wtc_tmux_connect has been called successfully,
 * they will have no effect until after wtc_tmux_disconnect has been called
 * successfully. Both of these functions are synchronous and respect the
 * timeout value.
 *
 * When wtc_tmux_connect is called, a list of all of the currently running
 * sessions is retrieved from the server. Then a control mode client is
 * attached to each one in order to properly track the entire server.
 * In the event that no sessions are running, a temporary session called
 * "wtc_tmux" will be created. When tmux reports that another session has
 * been created, this session will be terminated (provided that no other
 * client has attached to it).
 *
 * wtc_tmux_disconnect terminates all of the connections to the server. In
 * the event that the "wtc_tmux" session is open and no one else is
 * connected to it, the "wtc_tmux" session will be terminated before
 * disconnection.
 */
int wtc_tmux_connect(struct wtc_tmux *tmux);
int wtc_tmux_disconnect(struct wtc_tmux *tmux);
bool wtc_tmux_is_connected(const struct wtc_tmux *tmux);

/*
 * The following functions may be called at any time, regardless of the
 * connection state. However, it is recommended that they are not changed
 * after connection.
 */

/*
 * The timeout value, set in milliseconds, defaults to 10,000. This is the
 * maximum amount of time that will be waited for any synchronous operation
 * before failing due to lack of response. A value of 0 indicates there is
 * to be no timeout. -EINVAL will be returned if the tmux object is NULL.
 *
 * Passing NULL to wtc_tmux_get_timeout is an error and will result in
 * NULL being dereferenced.
 */
int wtc_tmux_set_timeout(struct wtc_tmux *tmux, unsigned int timeout);
unsigned int wtc_tmux_get_timeout(const struct wtc_tmux *tmux);

/*
 * The size of the control session defaults to 80x24. Note that each
 * window's dimensions are capped to that of the smallest connected client,
 * so this value should be large enough that the clients' windows are
 * unrestricted. However, very large values can have significant memory
 * ramifications, so don't make these values too ridiculous.
 *
 * -EINVAL will be returned if tmux is NULL or either w or h are less than
 *  10. If the tmux object is currently connected then calling
 *  wtc_tmux_set_size will synchronously change the size of all of the
 *  attached clients, respecting the timeout setting (provided a new size
 *  has been set).
 *
 * Passing NULL to wtc_tmux_get_width or wtc_tmux_get_height is an error and
 * will result in NULL being dereferenced.
 */
int wtc_tmux_set_size(struct wtc_tmux *tmux, unsigned int w, unsigned int h);
unsigned int wtc_tmux_get_width(const struct wtc_tmux *tmux);
unsigned int wtc_tmux_get_height(const struct wtc_tmux *tmux);

/*
 * The following callbacks will be invoked in response to various changes
 * in the tmux server. They are the primary way of reacting to the server
 * state. These should all probably be configured before connenection,
 * although they may be configured afterwards.
 *
 * All callbacks are reactive, that is they happen after the event they are
 * in response to has fully completed. The destruction callbacks are called
 * after all references to the passed object have been cleaned up. The
 * passed object will be freed after the callback completes.
 */
int wtc_tmux_set_new_client_cb(struct wtc_tmux *tmux,
	void (*cb)(struct wtc_tmux *, const struct wtc_tmux_client *));
int wtc_tmux_set_client_session_changed_cb(struct wtc_tmux *tmux,
	void (*cb)(struct wtc_tmux *, const struct wtc_tmux_client *));

int wtc_tmux_set_new_session_cb(struct wtc_tmux *tmux,
	void (*cb)(struct wtc_tmux *, const struct wtc_tmux_session *));
int wtc_tmux_set_session_closed_cb(struct wtc_tmux *tmux,
	void (*cb)(struct wtc_tmux *, const struct wtc_tmux_session *));
int wtc_tmux_set_session_window_changed_cb(struct wtc_tmux *tmux,
	void (*cb)(struct wtc_tmux *, const struct wtc_tmux_session *));

int wtc_tmux_set_new_window_cb(struct wtc_tmux *tmux,
	void (*cb)(struct wtc_tmux *, const struct wtc_tmux_window *));
int wtc_tmux_set_window_closed_cb(struct wtc_tmux *tmux,
	void (*cb)(struct wtc_tmux *, const struct wtc_tmux_window *));
int wtc_tmux_set_window_layout_changed_cb(struct wtc_tmux *tmux,
	void (*cb)(struct wtc_tmux *, const struct wtc_tmux_window *));
int wtc_tmux_set_window_pane_changed_cb(struct wtc_tmux *tmux,
	void (*cb)(struct wtc_tmux *, const struct wtc_tmux_window *));
/*
 * Pane creation and destruction should be tracked via the
 * layout_changed_cb.
 */
int wtc_tmux_set_pane_mode_changed_cb(struct wtc_tmux *tmux,
	void (*cb)(struct wtc_tmux *, const struct wtc_tmux_pane *));

/*
 * The following lookup functions can be used after a connection has been
 * established to gain information about the current tmux state.
 */

const struct wtc_tmux_client *
wtc_tmux_lookup_client(const struct wtc_tmux *tmux, const char *name);

const struct wtc_tmux_session *
wtc_tmux_lookup_session(const struct wtc_tmux *tmux, int id);

const struct wtc_tmux_window *
wtc_tmux_lookup_window(const struct wtc_tmux *tmux, int id);

const struct wtc_tmux_pane *
wtc_tmux_lookup_pane(const struct wtc_tmux *tmux, int id);

/*
 * Get the first session in the linked list associated with this tmux
 * object.
 *
 * Passing NULL to wtc_tmux_root_session is an error and will result in NULL
 * being dereferenced.
 */
const struct wtc_tmux_session *
wtc_tmux_root_session(const struct wtc_tmux *tmux);

#endif // !WTC_TMUX_H
