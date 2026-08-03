#include "testlog.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>

#define RESULT_PATH "sdmc:/testresult.txt"
#define LOG_DIR     "sdmc:/spotify"
#define LOG_PATH    "sdmc:/spotify/log.txt"

static FILE *s_result;
static FILE *s_log;
static int   s_phase;

/* Every write is followed by fflush: a crash mid-run must still leave a
 * readable trail on the SD card. */
static void emit(FILE *f, const char *line)
{
	if (!f)
		return;
	fputs(line, f);
	fputc('\n', f);
	fflush(f);
}

/* Mirror to stdout as well. Under `3dslink -s` this streams over the network to
 * the host terminal, which is the only practical way to see what is happening
 * on real hardware: 3dslink cannot read files back off the SD card. */
static void emit_stdout(const char *line)
{
	fputs(line, stdout);
	fputc('\n', stdout);
	fflush(stdout);
}

void tl_init(int phase)
{
	s_phase = phase;

	mkdir(LOG_DIR, 0777); /* harmless if it already exists */

	s_result = fopen(RESULT_PATH, "w");
	s_log    = fopen(LOG_PATH, "a");

	tl_log("---- phase %d start ----", phase);
}

void tl_log(const char *fmt, ...)
{
	char    buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);

	emit(s_log, buf);
	emit_stdout(buf);
}

static int s_timing;

void tl_set_timing(int on)
{
	s_timing = on;
}

void tl_timing(const char *fmt, ...)
{
	if (!s_timing)
		return;

	char    buf[384];
	char    line[416];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);

	snprintf(line, sizeof line, "TIMING %s", buf);
	emit(s_log, line);
	emit_stdout(line);
}

void tl_banner(const char *fmt, ...)
{
	char    buf[512];
	char    line[544];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);

	snprintf(line, sizeof line, "BANNER %s", buf);
	emit(s_log, line);
	emit_stdout(line);
}

void tl_step(const char *step, int pass, const char *fmt, ...)
{
	char detail[384];
	char line[512];

	if (fmt) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(detail, sizeof detail, fmt, ap);
		va_end(ap);
	} else {
		detail[0] = '\0';
	}

	snprintf(line, sizeof line, "PHASE=%d STEP=%s RESULT=%s%s%s", s_phase, step,
	         pass ? "PASS" : "FAIL", detail[0] ? " detail=" : "", detail);

	emit(s_result, line);
	emit(s_log, line);
	emit_stdout(line);
}

void tl_done(void)
{
	emit(s_result, "DONE");
	tl_log("---- phase %d done ----", s_phase);

	if (s_result) {
		fclose(s_result);
		s_result = NULL;
	}
	if (s_log) {
		fclose(s_log);
		s_log = NULL;
	}
}
