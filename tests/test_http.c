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
	(void)buf;
	(void)len;
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

	assert(!request("GET",
	                "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhel",
	                &out, err, sizeof err));
	assert(strstr(err, "truncated body") != NULL);
	assert(s_takes == 2); /* idempotent GET retries once on a fresh stream */

	assert(request("GET",
	               "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
	               "5\r\nhello\r\n0\r\n\r\n",
	               &out, err, sizeof err));
	assert(out.body_len == 5 && memcmp(out.body, "hello", 5) == 0);
	http_free(&out);

	assert(!request("GET",
	                "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
	                "5\r\nhel",
	                &out, err, sizeof err));
	assert(strcmp(err, "bad chunked body") == 0);
	assert(s_takes == 2);

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

	puts("http framing: complete 6MiB bodies accepted; truncation rejected/retried");
	return 0;
}
