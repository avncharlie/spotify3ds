#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/http.h"
#include "net/httppool.h"
#include "net/tls.h"

struct tls_conn {
	const char *data;
	size_t len;
	size_t pos;
};

static const char *s_response;
static int s_takes;
static size_t s_read_limit = 3;
static struct tls_conn s_conn;
static char *s_written;
static size_t s_written_len;

typedef struct {
	int calls;
	size_t received;
	size_t total;
	bool total_known;
	size_t first_nonzero;
	int zero_calls;
} progress_state;

static void record_progress(size_t received, size_t total, bool total_known,
	                        void *context)
{
	progress_state *state = context;
	state->calls++;
	state->received = received;
	state->total = total;
	state->total_known = total_known;
	if (!state->first_nonzero && received)
		state->first_nonzero = received;
	if (!received)
		state->zero_calls++;
}

tls_conn *pool_take(const char *host, int port, bool *reused, char *err,
	                int errlen)
{
	(void)host;
	(void)port;
	(void)err;
	(void)errlen;
	*reused = false;
	s_takes++;
	s_conn.data = s_response;
	s_conn.len = strlen(s_response);
	s_conn.pos = 0;
	return &s_conn;
}

void pool_give(const char *host, int port, tls_conn *conn, bool keep)
{
	(void)host;
	(void)port;
	(void)conn;
	(void)keep;
}

bool tls_write(tls_conn *conn, const void *buf, size_t len)
{
	(void)conn;
	free(s_written);
	s_written = malloc(len + 1);
	assert(s_written);
	memcpy(s_written, buf, len);
	s_written[len] = '\0';
	s_written_len = len;
	return true;
}

int tls_read(tls_conn *conn, void *buf, size_t len)
{
	if (conn->pos == conn->len)
		return 0;
	if (len > s_read_limit)
		len = s_read_limit;
	if (len > conn->len - conn->pos)
		len = conn->len - conn->pos;
	memcpy(buf, conn->data + conn->pos, len);
	conn->pos += len;
	return (int)len;
}

void tl_log(const char *fmt, ...)
{
	(void)fmt;
}

static bool request(const char *method, const char *response,
	                http_response *out, char *err, int errlen)
{
	s_response = response;
	s_takes = 0;
	return http_request("test.invalid", method, "/", NULL, NULL, NULL, out,
	                    err, errlen);
}

static bool request_path(const char *path, const char *response,
	                     http_response *out, char *err, int errlen)
{
	s_response = response;
	s_takes = 0;
	return http_request("test.invalid", "GET", path, NULL, NULL, NULL, out,
	                    err, errlen);
}

static bool request_progress(const char *response, progress_state *progress,
	                         http_response *out, char *err, int errlen)
{
	s_response = response;
	s_takes = 0;
	memset(progress, 0, sizeof *progress);
	return http_request_progress("test.invalid", "GET", "/", NULL, NULL, NULL,
	                             record_progress, progress, out, err, errlen);
}

int main(void)
{
	http_response out;
	char err[128];

	assert(request("GET",
	               "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
	               &out, err, sizeof err));
	assert(out.status == 200 && out.body_len == 5);
	assert(memcmp(out.body, "hello", 5) == 0);
	assert(s_takes == 1);
	http_free(&out);

	progress_state progress;
	assert(request_progress(
	    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", &progress,
	    &out, err, sizeof err));
	assert(progress.calls >= 2);
	assert(progress.received == 5 && progress.total == 5 &&
	       progress.total_known);
	http_free(&out);

	/* LRCLIB metadata can percent-encode to several kilobytes. The request line
	 * must grow with it rather than passing through a fixed staging array. */
	char long_path[3001];
	long_path[0] = '/';
	memset(long_path + 1, 'q', sizeof long_path - 2);
	long_path[sizeof long_path - 1] = '\0';
	assert(request_path(long_path,
	                    "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n",
	                    &out, err, sizeof err));
	assert(s_written_len > strlen(long_path));
	assert(strncmp(s_written, "GET /qqq", 8) == 0);
	assert(strstr(s_written, " HTTP/1.1\r\nHost: test.invalid\r\n") != NULL);
	http_free(&out);

	assert(!request("GET",
	                "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhel",
	                &out, err, sizeof err));
	assert(strstr(err, "truncated body") != NULL);
	assert(s_takes == 2); /* idempotent GET retries once on a fresh stream */
	assert(!request_progress(
	    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhel", &progress,
	    &out, err, sizeof err));
	assert(s_takes == 2 && progress.zero_calls >= 2);

	assert(request("GET",
	               "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
	               "5\r\nhello\r\n0\r\n\r\n",
	               &out, err, sizeof err));
	assert(out.body_len == 5 && memcmp(out.body, "hello", 5) == 0);
	http_free(&out);
	assert(request_progress(
	    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
	    "2\r\nhe\r\n3\r\nllo\r\n0\r\n\r\n",
	    &progress, &out, err, sizeof err));
	assert(progress.calls >= 3);
	assert(progress.received == 5 && progress.total == 0 &&
	       !progress.total_known);
	assert(progress.first_nonzero < 5);
	http_free(&out);

	assert(!request("GET",
	                "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
	                "5\r\nhel",
	                &out, err, sizeof err));
	assert(strcmp(err, "bad chunked body") == 0);
	assert(s_takes == 2);

	assert(!request("GET",
	                "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
	                "FFFFFFFF\r\n",
	                &out, err, sizeof err));
	assert(strcmp(err, "bad chunked body") == 0);

	/* Header names in a close-delimited body are body text, not framing. */
	assert(request("GET",
	               "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n"
	               "Content-Length: 999\r\nbody",
	               &out, err, sizeof err));
	assert(out.body_len == strlen("Content-Length: 999\r\nbody"));
	http_free(&out);

	assert(!request("POST",
	                "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhel",
	                &out, err, sizeof err));
	assert(s_takes == 1); /* do not duplicate a fresh non-idempotent request */

	/* Regression: detailed Spotify covers can be larger than the old 256KB
	 * response cap (Currents is ~333KB; Cherry Bomb is ~411KB). */
	const size_t large_len = 6 * 1024 * 1024;
	char header[96];
	const int header_len = snprintf(header, sizeof header,
	                                "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n",
	                                large_len);
	char *large = malloc((size_t)header_len + large_len + 1);
	assert(large);
	memcpy(large, header, (size_t)header_len);
	memset(large + header_len, 'x', large_len);
	large[header_len + large_len] = '\0';
	s_read_limit = 2048;
	assert(request("GET", large, &out, err, sizeof err));
	assert(out.body_len == large_len);
	assert(out.body[0] == 'x' && out.body[large_len - 1] == 'x');
	http_free(&out);
	free(large);
	free(s_written);

	puts("http framing: long paths and complete 6MiB bodies accepted; truncation rejected/retried");
	return 0;
}
