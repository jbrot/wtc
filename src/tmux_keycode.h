/*
 * wtc - tmux_keycode.h
 *
 * This file is adopted from tmux.
 *
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * wtc - Key Codes
 *
 * This file contains the keycode definitions from tmux used to represent
 * key bindings.
 */

#include <stdint.h>

/* Special key codes. */
#define KEYC_NONE 0xffff00000000ULL
#define KEYC_UNKNOWN 0xfffe00000000ULL
#define KEYC_BASE 0x000010000000ULL

/* Key modifier bits. */
#define KEYC_ESCAPE 0x200000000000ULL
#define KEYC_CTRL   0x400000000000ULL
#define KEYC_SHIFT  0x800000000000ULL
#define KEYC_XTERM 0x1000000000000ULL

/* Mask to obtain key w/o modifiers. */
#define KEYC_MASK_MOD (KEYC_ESCAPE|KEYC_CTRL|KEYC_SHIFT|KEYC_XTERM)
#define KEYC_MASK_KEY (~KEYC_MASK_MOD)

/* Is this a mouse key? */
#define KEYC_IS_MOUSE(key) (((key) & KEYC_MASK_KEY) >= KEYC_MOUSE &&	\
    ((key) & KEYC_MASK_KEY) < KEYC_BSPACE)

/* Multiple click timeout. */
#define KEYC_CLICK_TIMEOUT 300

/* Mouse key codes. */
#define KEYC_MOUSE_KEY(name)				\
	KEYC_ ## name ## _PANE,				\
	KEYC_ ## name ## _STATUS,			\
	KEYC_ ## name ## _BORDER
#define KEYC_MOUSE_STRING(name, s)			\
	{ #s "Pane", KEYC_ ## name ## _PANE },		\
	{ #s "Status", KEYC_ ## name ## _STATUS },	\
	{ #s "Border", KEYC_ ## name ## _BORDER }

/*
 * A single key. This can be ASCII or Unicode or one of the keys starting at
 * KEYC_BASE.
 */
typedef unsigned long long key_code;

/* Special key codes. */
enum {
	/* Focus events. */
	KEYC_FOCUS_IN = KEYC_BASE,
	KEYC_FOCUS_OUT,

	/* Mouse keys. */
	KEYC_MOUSE, /* unclassified mouse event */
	KEYC_DRAGGING, /* dragging in progress */
	KEYC_MOUSE_KEY(MOUSEMOVE),
	KEYC_MOUSE_KEY(MOUSEDOWN1),
	KEYC_MOUSE_KEY(MOUSEDOWN2),
	KEYC_MOUSE_KEY(MOUSEDOWN3),
	KEYC_MOUSE_KEY(MOUSEUP1),
	KEYC_MOUSE_KEY(MOUSEUP2),
	KEYC_MOUSE_KEY(MOUSEUP3),
	KEYC_MOUSE_KEY(MOUSEDRAG1),
	KEYC_MOUSE_KEY(MOUSEDRAG2),
	KEYC_MOUSE_KEY(MOUSEDRAG3),
	KEYC_MOUSE_KEY(MOUSEDRAGEND1),
	KEYC_MOUSE_KEY(MOUSEDRAGEND2),
	KEYC_MOUSE_KEY(MOUSEDRAGEND3),
	KEYC_MOUSE_KEY(WHEELUP),
	KEYC_MOUSE_KEY(WHEELDOWN),
	KEYC_MOUSE_KEY(DOUBLECLICK1),
	KEYC_MOUSE_KEY(DOUBLECLICK2),
	KEYC_MOUSE_KEY(DOUBLECLICK3),
	KEYC_MOUSE_KEY(TRIPLECLICK1),
	KEYC_MOUSE_KEY(TRIPLECLICK2),
	KEYC_MOUSE_KEY(TRIPLECLICK3),

	/* Backspace key. */
	KEYC_BSPACE,

	/* Function keys. */
	KEYC_F1,
	KEYC_F2,
	KEYC_F3,
	KEYC_F4,
	KEYC_F5,
	KEYC_F6,
	KEYC_F7,
	KEYC_F8,
	KEYC_F9,
	KEYC_F10,
	KEYC_F11,
	KEYC_F12,
	KEYC_IC,
	KEYC_DC,
	KEYC_HOME,
	KEYC_END,
	KEYC_NPAGE,
	KEYC_PPAGE,
	KEYC_BTAB,

	/* Arrow keys. */
	KEYC_UP,
	KEYC_DOWN,
	KEYC_LEFT,
	KEYC_RIGHT,

	/* Numeric keypad. */
	KEYC_KP_SLASH,
	KEYC_KP_STAR,
	KEYC_KP_MINUS,
	KEYC_KP_SEVEN,
	KEYC_KP_EIGHT,
	KEYC_KP_NINE,
	KEYC_KP_PLUS,
	KEYC_KP_FOUR,
	KEYC_KP_FIVE,
	KEYC_KP_SIX,
	KEYC_KP_ONE,
	KEYC_KP_TWO,
	KEYC_KP_THREE,
	KEYC_KP_ENTER,
	KEYC_KP_ZERO,
	KEYC_KP_PERIOD,
};

/* key_string.c */
key_code key_string_lookup_string(const char *string);
key_code key_code_from_xkb_key_char(uint32_t key, uint32_t chr);
