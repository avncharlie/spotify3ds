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
                          http_response *out, bool *out_reused,
                          bool *retryable, char *err, int errlen);

/* Spotify JSON responses are a few KB; album art is ~41KB. */
#define MAX_RESPONSE (256 * 1024)
#define READ_CHUNK   2048

typedef struct {
	char  *buf;
	size_t len;
	size_t cap;
} growbuf;

static bool gb_reserve(growbuf *g, size_t extra)
{
	if (g->len + extra + 1 <= g->cap)
		return true;

	size_t want = g->cap ? g->cap * 2 : 4096;
	while (want < g->len + extra + 1)
		want *= 2;
	if (want > MAX_RESPONSE)
		want = MAX_RESPONSE;
	if (g->len + extra + 1 > want)
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

/* Case-insensitive header lookup within the header block. Returns a pointer to
 * the value (past ": ") or NULL. */
static const char *find_header(const char *headers, const char *name)
{
	size_t nlen = strlen(name);
	for (const char *p = headers; p && *p;) {
		if (strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
			p += nlen + 1;
			while (*p == ' ' || *p == '\t')
				p++;
			return p;
		}
		p = strstr(p, "\r\n");
		if (!p)
			break;
		p += 2;
	}
	return NULL;
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
                           growbuf *out)
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

		unsigned long sz = strtoul(raw->buf + pos, NULL, 16);
		pos = (size_t)(nl - raw->buf) + 2;

		if (sz == 0)
			return true; /* terminating chunk */

		/* Ensure the chunk body plus its trailing CRLF is buffered. */
		while (raw->len < pos + sz + 2) {
			size_t before = raw->len;
			if (!fill_to(c, raw, pos + sz + 2, &eof))
				return false;
			if (eof && raw->len == before)
				return false;
		}

		if (!gb_append(out, raw->buf + pos, sz))
			return false;
		pos += sz + 2; /* skip chunk data + CRLF */
	}
}

bool http_request(const char *host, const char *method, const char *path,
                  const char *bearer, const char *ctype, const char *body,
                  http_response *out, char *err, int errlen)
{
	memset(out, 0, sizeof *out);

	/* One retry: a pooled connection can be closed by the peer between us
	 * taking it and writing to it, which is indistinguishable from a transport
	 * error until we try. Retrying once on a *reused* connection turns that
	 * into a reconnect instead of a spurious failure. */
	for (int attempt = 0; attempt < 2; attempt++) {
		bool reused = false;
		bool retryable;

		if (http_exchange(host, method, path, bearer, ctype, body, out, &reused,
		                  &retryable, err, errlen))
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
                          http_response *out, bool *out_reused,
                          bool *retryable, char *err, int errlen)
{
	*retryable = false;

	tls_conn *c = pool_take(host, 443, out_reused, err, errlen);
	if (!c)
		return false;

	/* --- request ---------------------------------------------------- */
	growbuf req = {0};
	char    line[512];

	/* Keep-alive is the whole point: a fresh TLS handshake to Spotify costs
	 * 700-1500ms and used to be paid on every request. */
	snprintf(line, sizeof line,
	         "%s %s HTTP/1.1\r\n"
	         "Host: %s\r\n"
	         "User-Agent: spotify3ds/0.1\r\n"
	         "Accept: */*\r\n"
	         "Connection: keep-alive\r\n",
	         method, path, host);
	gb_append(&req, line, strlen(line));

	if (bearer) {
		snprintf(line, sizeof line, "Authorization: Bearer %s\r\n", bearer);
		gb_append(&req, line, strlen(line));
	}
	if (ctype) {
		snprintf(line, sizeof line, "Content-Type: %s\r\n", ctype);
		gb_append(&req, line, strlen(line));
	}

	/* Length must be sent even when empty: Spotify's PUT endpoints reject a
	 * body-less request that omits it. */
	size_t blen = body ? strlen(body) : 0;
	snprintf(line, sizeof line, "Content-Length: %u\r\n\r\n", (unsigned)blen);
	gb_append(&req, line, strlen(line));

	if (body)
		gb_append(&req, body, blen);

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
	const char *te   = find_header(raw.buf, "Transfer-Encoding");
	const char *cl   = find_header(raw.buf, "Content-Length");
	const char *conn = find_header(raw.buf, "Connection");

	/* Only keep the connection if we can find the end of this body without
	 * relying on the close itself. Anything else and we would have no way to
	 * know where the next response starts. */
	bool keep = !(conn && strncasecmp(conn, "close", 5) == 0);

	growbuf out_body = {0};

	if (te && strncasecmp(te, "chunked", 7) == 0) {
		if (!decode_chunked(c, &raw, body_start, &out_body)) {
			free(out_body.buf);
			snprintf(err, errlen, "bad chunked body");
			*retryable = true;
			goto fail;
		}
	} else if (cl) {
		size_t want = (size_t)strtoul(cl, NULL, 10);
		while (raw.len < body_start + want) {
			size_t before = raw.len;
			if (!fill_to(c, &raw, body_start + want, &eof))
				break;
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
		if (have)
			gb_append(&out_body, raw.buf + body_start, have);
	} else if (out->status == 204 || out->status == 304 ||
	           strcmp(method, "HEAD") == 0) {
		/* Defined to have no body, so the response ends at the headers. This
		 * matters for keep-alive: 204 is the normal reply to every playback
		 * command, and reading to EOF here would block until the server gave
		 * up on an otherwise healthy connection. */
	} else {
		/* No length signalled at all: the body ends when the connection does,
		 * so this one cannot be reused. */
		while (!eof) {
			size_t before = raw.len;
			if (!fill_to(c, &raw, raw.len + 1, &eof))
				break;
			if (raw.len == before)
				break;
		}
		if (raw.len > body_start)
			gb_append(&out_body, raw.buf + body_start, raw.len - body_start);
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
