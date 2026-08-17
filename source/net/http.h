#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
	int    status;    /* HTTP status code, e.g. 200 / 204 / 401 */
	char  *body;      /* NUL-terminated; NULL when there is no body */
	size_t body_len;
} http_response;

/* Decoded response-body progress. `total_known` is false for chunked and
 * close-delimited responses; framing bytes are never included in received. */
typedef void (*http_progress_fn)(size_t received, size_t total,
                                 bool total_known, void *context);

/* Perform one HTTPS request and read the full response.
 *
 * method   "GET" / "PUT" / "POST"
 * path     e.g. "/v1/me/player/currently-playing"
 * bearer   access token, or NULL for no Authorization header
 * ctype    Content-Type for the body, or NULL
 * body     request body, or NULL
 *
 * Returns true if a well-formed response was read (any status code, including
 * 4xx/5xx). Returns false only on transport failure, with err filled in.
 *
 * Handles both Content-Length and chunked transfer-encoding: Spotify uses
 * chunked for JSON responses.
 *
 * Caller must http_free() the response.
 */
bool http_request(const char *host, const char *method, const char *path,
                  const char *bearer, const char *ctype, const char *body,
                  http_response *out, char *err, int errlen);

/* Progress-reporting variant. The callback runs synchronously on the calling
 * thread and may be NULL. Automatic retries reset received to zero. */
bool http_request_progress(const char *host, const char *method,
                           const char *path, const char *bearer,
                           const char *ctype, const char *body,
                           http_progress_fn progress, void *progress_context,
                           http_response *out, char *err, int errlen);

void http_free(http_response *r);
