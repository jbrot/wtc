/*
 * wtc - key_string.c
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
 * wtc - key_string.c
 *
 * This file contains the key code lookup code from tmux. Furthermore, it
 * includes an excerpt of the utf-8 handling code needed for some key code
 * resolution.
 */

#define _XOPEN_SOURCE

#include "tmux_keycode.h"

#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <wchar.h>

typedef unsigned char u_char;
typedef unsigned int u_int;

/* Number of items in array. */
#ifndef nitems
#define nitems(_a) (sizeof((_a)) / sizeof((_a)[0]))
#endif

/*
 * A single UTF-8 character. UTF8_SIZE must be big enough to hold at least 
 * one combining character as well.
 */
#define UTF8_SIZE 9
struct utf8_data {
	u_char	data[UTF8_SIZE];

	u_char	have;
	u_char	size;

	u_char	width;	/* 0xff if invalid */
} __packed;
enum utf8_state {
	UTF8_MORE,
	UTF8_DONE,
	UTF8_ERROR
};

static enum utf8_state utf8_open(struct utf8_data *ud, u_char ch);
static enum utf8_state utf8_append(struct utf8_data *ud, u_char ch);
static int utf8_width(wchar_t wc);
static enum utf8_state utf8_combine(const struct utf8_data *ud, 
                                    wchar_t *wc);

static key_code key_string_search_table(const char *);
static key_code key_string_get_modifiers(const char **);

static const struct {
	const char     *string;
	key_code	key;
} key_string_table[] = {
	/* Function keys. */
	{ "F1",		KEYC_F1 },
	{ "F2",		KEYC_F2 },
	{ "F3",		KEYC_F3 },
	{ "F4",		KEYC_F4 },
	{ "F5",		KEYC_F5 },
	{ "F6",		KEYC_F6 },
	{ "F7",		KEYC_F7 },
	{ "F8",		KEYC_F8 },
	{ "F9",		KEYC_F9 },
	{ "F10",	KEYC_F10 },
	{ "F11",	KEYC_F11 },
	{ "F12",	KEYC_F12 },
	{ "IC",		KEYC_IC },
	{ "DC",		KEYC_DC },
	{ "Home",	KEYC_HOME },
	{ "End",	KEYC_END },
	{ "NPage",	KEYC_NPAGE },
	{ "PageDown",	KEYC_NPAGE },
	{ "PgDn",	KEYC_NPAGE },
	{ "PPage",	KEYC_PPAGE },
	{ "PageUp",	KEYC_PPAGE },
	{ "PgUp",	KEYC_PPAGE },
	{ "Tab",	'\011' },
	{ "BTab",	KEYC_BTAB },
	{ "Space",	' ' },
	{ "BSpace",	KEYC_BSPACE },
	{ "Enter",	'\r' },
	{ "Escape",	'\033' },

	/* Arrow keys. */
	{ "Up",		KEYC_UP },
	{ "Down",	KEYC_DOWN },
	{ "Left",	KEYC_LEFT },
	{ "Right",	KEYC_RIGHT },

	/* Numeric keypad. */
	{ "KP/", 	KEYC_KP_SLASH },
	{ "KP*",	KEYC_KP_STAR },
	{ "KP-",	KEYC_KP_MINUS },
	{ "KP7",	KEYC_KP_SEVEN },
	{ "KP8",	KEYC_KP_EIGHT },
	{ "KP9",	KEYC_KP_NINE },
	{ "KP+",	KEYC_KP_PLUS },
	{ "KP4",	KEYC_KP_FOUR },
	{ "KP5",	KEYC_KP_FIVE },
	{ "KP6",	KEYC_KP_SIX },
	{ "KP1",	KEYC_KP_ONE },
	{ "KP2",	KEYC_KP_TWO },
	{ "KP3",	KEYC_KP_THREE },
	{ "KPEnter",	KEYC_KP_ENTER },
	{ "KP0",	KEYC_KP_ZERO },
	{ "KP.",	KEYC_KP_PERIOD },

	/* Mouse keys. */
	KEYC_MOUSE_STRING(MOUSEDOWN1, MouseDown1),
	KEYC_MOUSE_STRING(MOUSEDOWN2, MouseDown2),
	KEYC_MOUSE_STRING(MOUSEDOWN3, MouseDown3),
	KEYC_MOUSE_STRING(MOUSEUP1, MouseUp1),
	KEYC_MOUSE_STRING(MOUSEUP2, MouseUp2),
	KEYC_MOUSE_STRING(MOUSEUP3, MouseUp3),
	KEYC_MOUSE_STRING(MOUSEDRAG1, MouseDrag1),
	KEYC_MOUSE_STRING(MOUSEDRAG2, MouseDrag2),
	KEYC_MOUSE_STRING(MOUSEDRAG3, MouseDrag3),
	KEYC_MOUSE_STRING(MOUSEDRAGEND1, MouseDragEnd1),
	KEYC_MOUSE_STRING(MOUSEDRAGEND2, MouseDragEnd2),
	KEYC_MOUSE_STRING(MOUSEDRAGEND3, MouseDragEnd3),
	KEYC_MOUSE_STRING(WHEELUP, WheelUp),
	KEYC_MOUSE_STRING(WHEELDOWN, WheelDown),
	KEYC_MOUSE_STRING(DOUBLECLICK1, DoubleClick1),
	KEYC_MOUSE_STRING(DOUBLECLICK2, DoubleClick2),
	KEYC_MOUSE_STRING(DOUBLECLICK3, DoubleClick3),
	KEYC_MOUSE_STRING(TRIPLECLICK1, TripleClick1),
	KEYC_MOUSE_STRING(TRIPLECLICK2, TripleClick2),
	KEYC_MOUSE_STRING(TRIPLECLICK3, TripleClick3),
};

/* Find key string in table. */
static key_code
key_string_search_table(const char *string)
{
	u_int	i;

	for (i = 0; i < nitems(key_string_table); i++) {
		if (strcasecmp(string, key_string_table[i].string) == 0)
			return (key_string_table[i].key);
	}
	return (KEYC_UNKNOWN);
}

/* Find modifiers. */
static key_code
key_string_get_modifiers(const char **string)
{
	key_code	modifiers;

	modifiers = 0;
	while (((*string)[0] != '\0') && (*string)[1] == '-') {
		switch ((*string)[0]) {
		case 'C':
		case 'c':
			modifiers |= KEYC_CTRL;
			break;
		case 'M':
		case 'm':
			modifiers |= KEYC_ESCAPE;
			break;
		case 'S':
		case 's':
			modifiers |= KEYC_SHIFT;
			break;
		default:
			*string = NULL;
			return (0);
		}
		*string += 2;
	}
	return (modifiers);
}

/* Lookup a string and convert to a key value. */
key_code
key_string_lookup_string(const char *string)
{
	static const char	*other = "!#()+,-.0123456789:;<=>?'\r\t";
	key_code		 key;
	u_int			 u;
	key_code		 modifiers;
	struct utf8_data	 ud;
	u_int			 i;
	enum utf8_state		 more;
	wchar_t			 wc;

	/* Is this no key? */
	if (strcasecmp(string, "None") == 0)
		return (KEYC_NONE);

	/* Is this a hexadecimal value? */
	if (string[0] == '0' && string[1] == 'x') {
	        if (sscanf(string + 2, "%x", &u) != 1)
	                return (KEYC_UNKNOWN);
		if (u > 0x1fffff)
	                return (KEYC_UNKNOWN);
	        return (u);
	}

	/* Check for modifiers. */
	modifiers = 0;
	if (string[0] == '^' && string[1] != '\0') {
		modifiers |= KEYC_CTRL;
		string++;
	}
	modifiers |= key_string_get_modifiers(&string);
	if (string == NULL || string[0] == '\0')
		return (KEYC_UNKNOWN);

	/* Is this a standard ASCII key? */
	if (string[1] == '\0' && (u_char)string[0] <= 127) {
		key = (u_char)string[0];
		if (key < 32 || key == 127)
			return (KEYC_UNKNOWN);
	} else {
		/* Try as a UTF-8 key. */
		if ((more = utf8_open(&ud, (u_char)*string)) == UTF8_MORE) {
			if (strlen(string) != ud.size)
				return (KEYC_UNKNOWN);
			for (i = 1; i < ud.size; i++)
				more = utf8_append(&ud, (u_char)string[i]);
			if (more != UTF8_DONE)
				return (KEYC_UNKNOWN);
			if (utf8_combine(&ud, &wc) != UTF8_DONE)
				return (KEYC_UNKNOWN);
			return (wc | modifiers);
		}

		/* Otherwise look the key up in the table. */
		key = key_string_search_table(string);
		if (key == KEYC_UNKNOWN)
			return (KEYC_UNKNOWN);
	}

	/* Convert the standard control keys. */
	if (key < KEYC_BASE && (modifiers & KEYC_CTRL) && !strchr(other, key)) {
		if (key >= 97 && key <= 122)
			key -= 96;
		else if (key >= 64 && key <= 95)
			key -= 64;
		else if (key == 32)
			key = 0;
		else if (key == 63)
			key = KEYC_BSPACE;
		else
			return (KEYC_UNKNOWN);
		modifiers &= ~KEYC_CTRL;
	}

	return (key | modifiers);
}

/*
 * Open UTF-8 sequence.
 *
 * 11000010-11011111 C2-DF start of 2-byte sequence
 * 11100000-11101111 E0-EF start of 3-byte sequence
 * 11110000-11110100 F0-F4 start of 4-byte sequence
 */
static enum utf8_state
utf8_open(struct utf8_data *ud, u_char ch)
{
	memset(ud, 0, sizeof *ud);
	if (ch >= 0xc2 && ch <= 0xdf)
		ud->size = 2;
	else if (ch >= 0xe0 && ch <= 0xef)
		ud->size = 3;
	else if (ch >= 0xf0 && ch <= 0xf4)
		ud->size = 4;
	else
		return (UTF8_ERROR);
	utf8_append(ud, ch);
	return (UTF8_MORE);
}

/* Append character to UTF-8, closing if finished. */
static enum utf8_state
utf8_append(struct utf8_data *ud, u_char ch)
{
	wchar_t	wc;
	int	width;

	if (ud->have >= ud->size)
		fatal("UTF-8 character overflow");
	if (ud->size > sizeof ud->data)
		fatal("UTF-8 character size too large");

	if (ud->have != 0 && (ch & 0xc0) != 0x80)
		ud->width = 0xff;

	ud->data[ud->have++] = ch;
	if (ud->have != ud->size)
		return (UTF8_MORE);

	if (ud->width == 0xff)
		return (UTF8_ERROR);

	if (utf8_combine(ud, &wc) != UTF8_DONE)
		return (UTF8_ERROR);
	if ((width = utf8_width(wc)) < 0)
		return (UTF8_ERROR);
	ud->width = width;

	return (UTF8_DONE);
}

/* Get width of Unicode character. */
static int
utf8_width(wchar_t wc)
{
	int	width;

#ifdef HAVE_UTF8PROC
	width = utf8proc_wcwidth(wc);
#else
	width = wcwidth(wc);
#endif
	if (width < 0 || width > 0xff) {
		debug("Unicode %04lx, wcwidth() %d", (long)wc, width);

#ifndef __OpenBSD__
		/*
		 * Many platforms (particularly and inevitably OS X) have no
		 * width for relatively common characters (wcwidth() returns
		 * -1); assume width 1 in this case. This will be wrong for
		 * genuinely nonprintable characters, but they should be
		 * rare. We may pass through stuff that ideally we would block,
		 * but this is no worse than sending the same to the terminal
		 * without tmux.
		 */
		if (width < 0)
			return (1);
#endif
		return (-1);
	}
	return (width);
}

/* Combine UTF-8 into Unicode. */
static enum utf8_state
utf8_combine(const struct utf8_data *ud, wchar_t *wc)
{
#ifdef HAVE_UTF8PROC
	switch (utf8proc_mbtowc(wc, ud->data, ud->size)) {
#else
	switch (mbtowc(wc, ud->data, ud->size)) {
#endif
	case -1:
		debug("UTF-8 %.*s, mbtowc() %d", (int)ud->size, ud->data, errno);
		mbtowc(NULL, NULL, MB_CUR_MAX);
		return (UTF8_ERROR);
	case 0:
		return (UTF8_ERROR);
	default:
		return (UTF8_DONE);
	}
}
