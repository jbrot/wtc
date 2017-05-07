/*
 * wtc - log.h
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
 * wtc - Logging
 *
 * This file provides some simple logging functions.
 */

#ifndef WTC_LOGGING_H
#define WTC_LOGGING_H

/*
 * Five logging levels ranging from least severe to most sever.
 *
 * WARNING: Logging a fatal message will result in a core dump.
 */
enum wtc_log_level {
	DEBUG,
	INFO,
	WARNING,
	CRITICAL,
	FATAL,
};

void wlog(enum wtc_log_level level, const char *format, ...);

void debug(const char *format, ...);
void info(const char *format, ...);
void warning(const char *format, ...);
void critical(const char *format, ...);
void fatal(const char *format, ...);

#endif // !WTC_LOGGING_H
