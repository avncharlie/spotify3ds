#include "http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../testlog.h"
#include "httppool.h"
#include "tls.h"

static bool http_exchange(const char *host, const char *method,
                           const char *path, const char *bearer,
                           const char *ctype, const char *body,
                           http_progress_fn progress, void *progress_context,
                           http_response *out, bool *out_reused,
                           bool *retryable, char *err, int errlen);

/* Spotify JSON responses are a few KB, but detailed covers can be much larger.
 * Allocation grows on demand, so this ceiling costs nothing for normal replies;
 * it only prevents a broken or hostile peer from exhausting all application
 * memory. 8 MiB is over six times the raw RGB size of a 640x640 cover. */
#define MAX_RESPONSE (8 * 1024 * 1024)
#define READ_CHUNK   2048

typedef struct {
	char  *buf;
	size_t len;
	size_t cap;
} growbuf;

static bool gb_reserve(growbuf *g, size_t extra)
{
	if (extra >= MAX_RESPONSE || g->len > MAX_RESPONSE - extra - 1)
		return false;
	const size_t required = g->len + extra + 1;
	if (required <= g->cap)
		return true;

	size_t want = g->cap ? g->cap * 2 : 4096;
	while (want < required) {
		if (want >= MAX_RESPONSE / 2) {
			want = MAX_RESPONSE;
			break;
		}
		want *= 2;
	}
	if (want > MAX_RESPONSE)
		want = MAX_RESPONSE;
	if (required > want)
		return false; /* would exceed the cap */

	char *p = realloc(g->buf, want);
	if (!p)
		return false;
	g->buf = p;
	g->cap = want;
	return true;
}

static bool gb_append(growbuf *g, const char *src, size_t n)
{
	if (!gb_reserve(g, n))
		return false;
	memcpy(g->buf + g->len, src, n);
	g->len += n;
	g->buf[g->len] = '\0';
	return true;
}

static bool gb_append_str(growbuf *g, const char *src)
{
	return gb_append(g, src, strlen(src));
}

/* Case-insensitive header lookup within the header block. Returns a pointer to
 * the value (past ": ") or NULL. */
bool http_retry_after_str(long seconds, char *out, int outlen)
{
	if (seconds < 0 || outlen <= 0)
		return false;
	const long mins = seconds / 60;
	if (mins >= 60)
		snprintf(out, (size_t)outlen, "%ldh %ldm", mins / 60, mins % 60);
	else if (mins >= 1)
		snprintf(out, (size_t)outlen, "%ldm", mins);
	else
		snprintf(out, (size_t)outlen, "%lds", seconds);
	return true;
}

static const char *find_header(const char *headers, size_t headers_len,
	                           const char *name)
{
	const size_t nlen = strlen(name);
	const char *const end = headers + headers_len;
	for (const char *p = headers; p < end;) {
		const char *line_end = memchr(p, '\n', (size_t)(end - p));
		if (!line_end)
			line_end = end;
		const size_t line_len = (size_t)(line_end - p);
		if (line_len > nlen && p[nlen] == ':' &&
		    strncasecmp(p, name, nlen) == 0) {
			p += nlen + 1;
			while (p < line_end && (*p == ' ' || *p == '\t'))
				p++;
			return p;
		}
		p = line_end < end ? line_end + 1 : end;
	}
	return NULL;
}

static bool parse_decimal_header(const char *text, const char *headers_end,
	                             size_t *out)
{
	size_t value = 0;
	bool have_digit = false;
	while (text < headers_end && (*text == ' ' || *text == '\t'))
		text++;
	while (text < headers_end && *text >= '0' && *text <= '9') {
		const unsigned digit = (unsigned)(*text++ - '0');
		if (value > (MAX_RESPONSE - digit) / 10)
			return false;
		value = value * 10 + digit;
		have_digit = true;
	}
	while (text < headers_end && (*text == ' ' || *text == '\t'))
		text++;
	if (!have_digit || (text < headers_end && *text != '\r' && *text != '\n'))
		return false;
	*out = value;
	return true;
}

static bool parse_chunk_size(const char *text, const char *end, size_t *out)
{
	size_t value = 0;
	bool have_digit = false;
	for (; text < end && *text != ';'; text++) {
		unsigned digit;
		if (*text >= '0' && *text <= '9')
			digit = (unsigned)(*text - '0');
		else if (*text >= 'a' && *text <= 'f')
			digit = (unsigned)(*text - 'a' + 10);
		else if (*text >= 'A' && *text <= 'F')
			digit = (unsigned)(*text - 'A' + 10);
		else
			return false;
		if (value > (MAX_RESPONSE - digit) / 16)
			return false;
		value = value * 16 + digit;
		have_digit = true;
	}
	if (!have_digit)
		return false;
	*out = value;
	return true;
}

/* Read from the TLS connection until `want` bytes are buffered, or the peer
 * closes. Returns false only on a hard read error. */
static bool fill_to(tls_conn *c, growbuf *g, size_t want, bool *eof)
{
	char tmp[READ_CHUNK];
	while (g->len < want) {
		int n = tls_read(c, tmp, sizeof tmp);
		if (n == 0) {
			*eof = true;
			return true;
		}
		if (n < 0)
			return false;
		if (!gb_append(g, tmp, (size_t)n))
			return false;
	}
	return true;
}

/* Decode chunked transfer-encoding in place from `src` into `out`. */
static bool decode_chunked(tls_conn *c, growbuf *raw, size_t body_start,
                           growbuf *out, http_progress_fn progress,
                           void *progress_context)
{
	size_t pos = body_start;
	bool   eof = false;

	for (;;) {
		/* Need a full chunk-size line. */
		const char *nl;
		while (!(nl = strstr(raw->buf + pos, "\r\n"))) {
			size_t before = raw->len;
			if (!fill_to(c, raw, raw->len + 1, &eof))
				return false;
			if (eof && raw->len == before)
				return false; /* truncated */
		}

		size_t sz;
		if (!parse_chunk_size(raw->buf + pos, nl, &sz))
			return false;
		pos = (size_t)(nl - raw->buf) + 2;

		if (sz == 0) {
			/* Consume trailers through their empty terminating line before this
			 * connection is returned to the keep-alive pool. */
			for (;;) {
				while (!(nl = strstr(raw->buf + pos, "\r\n"))) {
					const size_t before = raw->len;
					if (!fill_to(c, raw, raw->len + 1, &eof) ||
					    (eof && raw->len == before))
						return false;
				}
				if (nl == raw->buf + pos)
					return true;
				pos = (size_t)(nl - raw->buf) + 2;
			}
		}

		/* Ensure the chunk body plus its trailing CRLF is buffered. */
		const size_t chunk_end = pos + sz;
		if (chunk_end < pos || chunk_end > MAX_RESPONSE - 2)
			return false;
		if (progress) {
			size_t available = raw->len > pos ? raw->len - pos : 0;
			if (available > sz)
				available = sz;
			progress(out->len + available, 0, false, progress_context);
		}
		while (raw->len < chunk_end + 2) {
			size_t before = raw->len;
			if (!fill_to(c, raw, raw->len + 1, &eof))
				return false;
			if (progress) {
				size_t available = raw->len > pos ? raw->len - pos : 0;
				if (available > sz)
					available = sz;
				progress(out->len + available, 0, false,
				         progress_context);
			}
			if (eof && raw->len == before)
				return false;
		}

		if (raw->buf[chunk_end] != '\r' || raw->buf[chunk_end + 1] != '\n')
			return false;
		if (!gb_append(out, raw->buf + pos, sz))
			return false;
		if (progress)
			progress(out->len, 0, false, progress_context);
		pos = chunk_end + 2;
	}
}

bool http_request(const char *host, const char *method, const char *path,
                  const char *bearer, const char *ctype, const char *body,
                  http_response *out, char *err, int errlen)
{
	return http_request_progress(host, method, path, bearer, ctype, body, NULL,
	                             NULL, out, err, errlen);
}

bool http_request_progress(const char *host, const char *method,
                           const char *path, const char *bearer,
                           const char *ctype, const char *body,
                           http_progress_fn progress, void *progress_context,
                           http_response *out, char *err, int errlen)
{
	memset(out, 0, sizeof *out);
	/* Zero would read as "retry immediately"; absent is not the same thing. */
	out->retry_after = -1;

	/* One retry: a pooled connection can be closed by the peer between us
	 * taking it and writing to it, which is indistinguishable from a transport
	 * error until we try. Retrying once on a *reused* connection turns that
	 * into a reconnect instead of a spurious failure. */
	for (int attempt = 0; attempt < 2; attempt++) {
		bool reused = false;
		bool retryable;
		if (progress)
			progress(0, 0, false, progress_context);

		if (http_exchange(host, method, path, bearer, ctype, body, progress,
		                  progress_context, out, &reused, &retryable, err, errlen))
			return true;

		/* GET is idempotent, so a truncated fresh response is also safe to retry.
		 * Commands retain the old rule: only retry a reused connection that was
		 * already stale before the request could complete. */
		if (!retryable || (!reused && strcmp(method, "GET") != 0))
			return false;

		tl_log("request to %s failed, redialling (%s)", host, err);
	}

	return false;
}

/* One request/response over a single connection, taken from the pool.
 * *out_reused says whether the connection came from the pool; *retryable says
 * whether the failure is the kind a fresh connection would fix. */
static bool http_exchange(const char *host, const char *method,
                           const char *path, const char *bearer,
                           const char *ctype, const char *body,
                           http_progress_fn progress, void *progress_context,
                           http_response *out, bool *out_reused,
                          bool *retryable, char *err, int errlen)
{
	*retryable = false;

	tls_conn *c = pool_take(host, 443, out_reused, err, errlen);
	if (!c)
		return false;

	/* --- request ---------------------------------------------------- */
	growbuf req = {0};
	char    line[64];

	/* Keep-alive is the whole point: a fresh TLS handshake to Spotify costs
	 * 700-1500ms and used to be paid on every request. */
	if (!gb_append_str(&req, method) || !gb_append_str(&req, " ") ||
	    !gb_append_str(&req, path) ||
	    !gb_append_str(&req, " HTTP/1.1\r\nHost: ") ||
	    !gb_append_str(&req, host) ||
	    !gb_append_str(&req,
	                   "\r\nUser-Agent: Spotify3DS/0.1 "
	                   "(+https://github.com/avncharlie/spotify3ds)\r\n"
	                   "Accept: */*\r\nConnection: keep-alive\r\n")) {
		snprintf(err, errlen, "request headers too large");
		free(req.buf);
		pool_give(host, 443, c, false);
		return false;
	}

	if (bearer) {
		if (!gb_append_str(&req, "Authorization: Bearer ") ||
		    !gb_append_str(&req, bearer) || !gb_append_str(&req, "\r\n")) {
			snprintf(err, errlen, "authorization header too large");
			free(req.buf);
			pool_give(host, 443, c, false);
			return false;
		}
	}
	if (ctype) {
		if (!gb_append_str(&req, "Content-Type: ") ||
		    !gb_append_str(&req, ctype) || !gb_append_str(&req, "\r\n")) {
			snprintf(err, errlen, "content type header too large");
			free(req.buf);
			pool_give(host, 443, c, false);
			return false;
		}
	}

	/* Length must be sent even when empty: Spotify's PUT endpoints reject a
	 * body-less request that omits it. */
	size_t blen = body ? strlen(body) : 0;
	const int line_len = snprintf(line, sizeof line,
	                              "Content-Length: %u\r\n\r\n",
	                              (unsigned)blen);
	if (line_len < 0 || line_len >= (int)sizeof line ||
	    !gb_append(&req, line, (size_t)line_len) ||
	    (body && !gb_append(&req, body, blen))) {
		snprintf(err, errlen, "request body too large");
		free(req.buf);
		pool_give(host, 443, c, false);
		return false;
	}

	bool sent = tls_write(c, req.buf, req.len);
	free(req.buf);

	if (!sent) {
		snprintf(err, errlen, "send failed");
		pool_give(host, 443, c, false);
		*retryable = true; /* a dead pooled connection looks exactly like this */
		return false;
	}

	/* --- response headers -------------------------------------------- */
	growbuf raw = {0};
	bool    eof = false;
	const char *hdr_end = NULL;

	for (;;) {
		hdr_end = raw.buf ? strstr(raw.buf, "\r\n\r\n") : NULL;
		if (hdr_end)
			break;
		size_t before = raw.len;
		if (!fill_to(c, &raw, raw.len + 1, &eof)) {
			snprintf(err, errlen, "read failed");
			*retryable = (raw.len == 0); /* nothing at all came back */
			goto fail;
		}
		if (eof && raw.len == before) {
			snprintf(err, errlen, "no headers (got %u bytes)",
			         (unsigned)raw.len);
			/* A pooled connection the peer had already closed yields a clean
			 * EOF with no data - retry on a fresh one. */
			*retryable = (raw.len == 0);
			goto fail;
		}
	}

	if (strncmp(raw.buf, "HTTP/1.", 7) != 0) {
		snprintf(err, errlen, "malformed status line");
		goto fail;
	}
	out->status = atoi(raw.buf + 9);

	size_t body_start = (size_t)(hdr_end - raw.buf) + 4;

	/* --- body -------------------------------------------------------- */
	const size_t headers_len = (size_t)(hdr_end - raw.buf);
	const char *const headers_end = raw.buf + headers_len;
	const char *te = find_header(raw.buf, headers_len, "Transfer-Encoding");
	const char *cl = find_header(raw.buf, headers_len, "Content-Length");
	const char *conn = find_header(raw.buf, headers_len, "Connection");

	/* Kept because a 429 from Spotify can name a wait measured in hours, and
	 * a caller that cannot see it has nothing useful to tell the user. */
	out->retry_after = -1;
	const char *ra = find_header(raw.buf, headers_len, "Retry-After");
	if (ra) {
		size_t secs = 0;
		if (parse_decimal_header(ra, headers_end, &secs))
			out->retry_after = (long)secs;
	}

	/* Only keep the connection if we can find the end of this body without
	 * relying on the close itself. Anything else and we would have no way to
	 * know where the next response starts. */
	bool keep = !(conn && strncasecmp(conn, "close", 5) == 0);

	growbuf out_body = {0};

	if (te && strncasecmp(te, "chunked", 7) == 0) {
		if (progress)
			progress(0, 0, false, progress_context);
		if (!decode_chunked(c, &raw, body_start, &out_body, progress,
		                    progress_context)) {
			free(out_body.buf);
			snprintf(err, errlen, "bad chunked body");
			*retryable = true;
			goto fail;
		}
	} else if (cl) {
		size_t want;
		if (!parse_decimal_header(cl, headers_end, &want)) {
			snprintf(err, errlen, "bad content-length");
			goto fail;
		}
		size_t received = raw.len > body_start ? raw.len - body_start : 0;
		if (received > want)
			received = want;
		if (progress)
			progress(received, want, true, progress_context);
		while (raw.len < body_start + want) {
			size_t before = raw.len;
			if (!fill_to(c, &raw, raw.len + 1, &eof))
				break;
			received = raw.len > body_start ? raw.len - body_start : 0;
			if (received > want)
				received = want;
			if (progress)
				progress(received, want, true, progress_context);
			if (eof && raw.len == before)
				break;
		}
		size_t have = raw.len > body_start ? raw.len - body_start : 0;
		if (have < want) {
			snprintf(err, errlen, "truncated body (%u/%u bytes)",
			         (unsigned)have, (unsigned)want);
			*retryable = true;
			goto fail;
		}
		if (have > want)
			have = want;
		if (have && !gb_append(&out_body, raw.buf + body_start, have)) {
			snprintf(err, errlen, "response body too large");
			goto fail;
		}
	} else if (out->status == 204 || out->status == 304 ||
	           strcmp(method, "HEAD") == 0) {
		/* Defined to have no body, so the response ends at the headers. This
		 * matters for keep-alive: 204 is the normal reply to every playback
		 * command, and reading to EOF here would block until the server gave
		 * up on an otherwise healthy connection. */
		if (progress)
			progress(0, 0, true, progress_context);
	} else {
		/* No length signalled at all: the body ends when the connection does,
		 * so this one cannot be reused. */
		size_t received = raw.len > body_start ? raw.len - body_start : 0;
		if (progress)
			progress(received, 0, false, progress_context);
		while (!eof) {
			size_t before = raw.len;
			if (!fill_to(c, &raw, raw.len + 1, &eof)) {
				snprintf(err, errlen, "read failed");
				*retryable = true;
				goto fail;
			}
			if (raw.len == before)
				break;
			received = raw.len > body_start ? raw.len - body_start : 0;
			if (progress)
				progress(received, 0, false, progress_context);
		}
		if (raw.len > body_start &&
		    !gb_append(&out_body, raw.buf + body_start, raw.len - body_start)) {
			snprintf(err, errlen, "response body too large");
			goto fail;
		}
		keep = false;
	}

	out->body     = out_body.buf;
	out->body_len = out_body.len;

	free(raw.buf);
	pool_give(host, 443, c, keep);
	return true;

fail:
	free(raw.buf);
	pool_give(host, 443, c, false);
	return false;
}

void http_free(http_response *r)
{
	if (!r)
		return;
	free(r->body);
	r->body     = NULL;
	r->body_len = 0;
}
