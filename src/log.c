/*
 * wtc - log.c
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

#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

// TODO Figure out how to put this into the config file.
#define WTC_COLORS_256

#ifdef WTC_COLORS_256

#define BOLD      "\033[1m"
#define ORANGE    "\033[1;38;5;208m"
#define RED       "\033[1;38;5;160m"
#define BRIGHTRED "\033[1;38;5;196m"
#define RESET     "\033[0m"

#else
#ifdef WTC_COLORS

#define BOLD      "\033[1m"
#define ORANGE    "\033[0;33m"
#define RED       "\033[0;31m"
#define BRIGHTRED "\033[1;31m"
#define RESET     "\033[0m"

#else

#define BOLD      ""
#define ORANGE    ""
#define RED       ""
#define BRIGHTRED ""
#define RESET     ""

#endif // WTC_COLORS
#endif // WTC_COLORS_256

static void vlogs(enum wtc_log_level l)
{
	switch (l) {
	case DEBUG:
		printf("[DBG] ");
		break;
	case INFO:
		printf(BOLD "[INF] ");
		break;
	case WARNING:
		fprintf(stderr, ORANGE "[WRN] ");
		break;
	case CRITICAL:
		fprintf(stderr, RED "[CRT] ");
		break;
	case FATAL:
		fprintf(stderr, BRIGHTRED "[FTL] ");
	}
} 

static void vlogm(enum wtc_log_level l, const char *f, va_list v)
{
	switch (l) {
	case DEBUG:
		vprintf(f, v);
		break;
	case INFO:
		vprintf(f, v);
		break;
	case WARNING:
		vfprintf(stderr, f, v);
		break;
	case CRITICAL:
		vfprintf(stderr, f, v);
		break;
	case FATAL:
		vfprintf(stderr, f, v);
	}
} 

static void vloge(enum wtc_log_level l)
{
	switch (l) {
	case DEBUG:
		printf("\n");
		break;
	case INFO:
		printf(RESET "\n");
		break;
	case WARNING:
		fprintf(stderr, RESET "\n");
		break;
	case CRITICAL:
		fprintf(stderr, RESET "\n");
		break;
	case FATAL:
		fprintf(stderr, RESET "\n");
		abort();
	}
} 

static void vlog(enum wtc_log_level l, const char *f, va_list v)
{
	vlogs(l);
	vlogm(l, f, v);
	vloge(l);
}

void wlog(enum wtc_log_level level, const char *format, ...)
{
	va_list v;
	va_start(v, format);
	vlog(level, format, v);
	va_end(v);
}

void wlogs(enum wtc_log_level level, const char *format, ...)
{
	vlogs(level);

	va_list v;
	va_start(v, format);
	vlogm(level, format, v);
	va_end(v);
}

void wlogm(enum wtc_log_level level, const char *format, ...)
{
	va_list v;
	va_start(v, format);
	vlogm(level, format, v);
	va_end(v);
}

void wloge(enum wtc_log_level level)
{
	vloge(level);
}

void debug(const char *format, ...)
{
	va_list v;
	va_start(v, format);
	vlog(DEBUG, format, v);
	va_end(v);
}

void info(const char *format, ...)
{
	va_list v;
	va_start(v, format);
	vlog(INFO, format, v);
	va_end(v);
}
void warn(const char *format, ...)
{
	va_list v;
	va_start(v, format);
	vlog(WARNING, format, v);
	va_end(v);
}

void crit(const char *format, ...)
{
	va_list v;
	va_start(v, format);
	vlog(CRITICAL, format, v);
	va_end(v);
}

void fatal(const char *format, ...)
{
	va_list v;
	va_start(v, format);
	vlog(FATAL, format, v);
	va_end(v);
}
