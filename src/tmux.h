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
 */

#include "uthash.h"

struct wtc_tmux;
struct wtc_tmux_window;

/*
 * Describes a tmux pane. A pane represents one pseudo terminal which may be
 * displayed to a client. A pane is associated with a unique window.
 * wtc_tmux_panes form a doubly linked list which contain the panes associated
 * with a given window.
 */
struct wtc_tmux_pane
{
	/* The pane's tmux id. */
	int id;
	/*
	 * The pid of the root process of the pane. This is the process tmux
	 * launches when it creates the pane, and tmux will destroy the pane
	 * if this process terminates. Typically, this will be a shell like bash.
	 */
	int pid;
	/* 
	 * Whether or not this is the active pane in its window. Only one pane will
	 * be active in a window.
	 */
	bool active;

	/* The window which includes this pane. */
	struct wtc_tmux_window *window;
	/* 
	 * The adjacent panes in the linked list. These panes are guaranteed to be
	 * in the same window, but there is no guarantee on the specifics of the
	 * order (i.e., they are not necessarily sorted by id or by display
	 * location). These will be NULL if this is the first or last pane in the
	 * list respectively.
	 */
	struct wtc_tmux_pane *previous;
	struct wtc_tmux_pane *next;

	/*
	 * The pane's extents within the window. The origin is the top-left
	 * corner of the display. Positive x is to the right and positive y is down.
	 * Note that the status bar is not accounted for by these coordinates.
	 */
	int x;
	int y;
	int w;
	int h;
};

/*
 * Describes a tmux window. A window represents the full screen being shown to a
 * session and is comprised of multiple panes which handle the actual content. 
 * A window is associated with a unique session group but not necessarily a 
 * unique session. wtc_tmux_windows constitute a doubly linked list which 
 * contain the windows associated with a given session group.
 */
struct wtc_tmux_window
{
	/* The window's tmux id. */
	int id;

	/* 
	 * The adjacent windows in the linked list. These windows are guarnteed to
	 * be in the same session gropu, but there is no guarantee on the specifics
	 * of the order. These willbe NULL if this is the first or last pane in the
	 * list respectively.
	 */
	struct wtc_tmux_window *previous;
	struct wtc_tmux_window *next;

	/* The active pane in the window. */
	struct wtc_tmux_pane *active_pane;
	/* The number of panes in this window. */
	int pane_count;
	/* 
	 * The first pane associated with this window. Using wtc_tmux_pane->next,
	 * you can use this to iterate through all of the linked panes.
	 */
	struct wtc_tmux_pane *panes;
};

/*
 * Describes a tmux session. A session consists of an arbitrary amount of
 * windows, one of which is marked active. Clients connect to a session. A
 * session can have an arbitrary amount of connected clients. Each client
 * will view the same content---the active window. Although each session has
 * a unique active window, several sessions can constitute a session group in
 * which case every session in the same group will share the same windows. So,
 * whether or not a window is active depends not on the window but on which
 * session it is associated with as well. Thus, unlike with panes, 
 * wtc_tmux_window does not have an active property. To determine if a window
 * is active with respect to a session, check if it is the window pointed to
 * by active_window.
 */
struct wtc_tmux_session
{
	/* The session's tmux id. */
	int id;

	/* The window this session is currently viewing. */
	struct wtc_tmux_window *active_window;
	/* The number of windows linked to this session. */
	int window_count;
	/*
	 * The first window associated with this session. Using 
	 * wtc_tmux_window->next you can iterate through all of the windows
	 * associated with this session.
	 */
	struct wtc_tmux_window *windows;
};

/*
 * Descrives a tmux client. A client represents an instance of the tmux process
 * which has connenected to the server. A client is associated with a unique
 * session and is shown the active window on that session. Every client linked
 * to a given session receives the same content.
 */
struct wtc_tmux_client
{
	/* The client's pid. */
	int pid;
	/* The client's name. */
	const char *name;

	/* The client's attached session. */
	struct wtc_tmux_session *session;
};

/*
 * Creates a new tmux handler. This principally involves forking and
 * execing a new tmux process in command mode. The newly created object
 * wil have a reference count of 1.
 *
 * out:  The newly created wtc_tmux object will be stored here on success. This
 *       may not be NULL.
 * w, h: The width and height of the control session. These will be set 
 *       automatically using "refresh-client". These can later be adjusted
 *       using wtc_tmux_resize. Note that if the size here is smaller than the
 *       connecting client, the client's view will be restricted. Don't go too
 *       large though as the buffer does cost memory.
 * args: A NULL terminated array of options to be passed to tmux during exec
 *       these will be appended to "/bin/tmux", "-C". Note that passing
 *       a second "-C" option will most likely break the interface.
 *       The options here are resposible for connecting to the proper
 *       session and so should include either "attach-session -t ..."
 *       or "new-session" if appropriate. This may be NULL to specify
 *       no additional arguments.
 *
 * Return: 0 on success. -EINVAL if out is NULL or w or h are not positive.
 */
int wtc_tmux_new(struct wtc_tmux **out, int w, int h, const char **args);

/*
 * Increment the reference count.
 */
void wtc_tmux_ref(struct wtc_tmux *tmux);

/*
 * Decrement the reference count. If the reference count falls to zero, the
 * object will be freed and the underlying process shut down. The underlying
 * process will be shut down synchronously. If the tmux does not terminate
 * within 1 second, it will killed via SIGKILL. To change this time out, use
 * the overloaded version of wtc_tmux_unref.
 */
void wtc_tmux_unref(struct wtc_tmux *tmux);

/*
 * Decrement the reference count. If the reference count falls to zero, the
 * object will be freed and the underlying process shut down.
 *
 * timeout: How long, in milliseconds, to wait for tmux to quit before issuing
 *          SIGKILL.
 */
void wtc_tmux_unref(struct wtc_tmux *tmux, long timeout);

/*
 * Resize the control session.
 */
int wtc_tmux_resize(int w, int h);
