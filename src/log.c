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

static void vlog(enum wtc_log_level l, const char *f, va_list v)
{
	switch (l) {
	case DEBUG:
		printf("[DEBUG] ");
		vprintf(f, v);
		break;
	case INFO:
		printf("[INFO] ");
		vprintf(f, v);
		break;
	case WARNING:
		fprintf(stderr, "[WARNING] ");
		vfprintf(stderr, f, v);
		break;
	case CRITICAL:
		fprintf(stderr, "[CRITICAL] ");
		vfprintf(stderr, f, v);
		break;
	case FATAL:
		fprintf(stderr, "[FATAL] ");
		vfprintf(stderr, f, v);
		abort();
	}
} 

void wlog(enum wtc_log_level level, const char *format, ...)
{
	va_list v;
	va_start(v, format);
	vlog(level, format, v);
	va_end(v);
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
void warning(const char *format, ...)
{
	va_list v;
	va_start(v, format);
	vlog(WARNING, format, v);
	va_end(v);
}

void critical(const char *format, ...)
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
