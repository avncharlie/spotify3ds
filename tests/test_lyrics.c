#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/http.h"
#include "spotify/json.h"
#include "spotify/lyrics.h"

typedef struct {
	bool transport_ok;
	int status;
	const char *body;
} mock_reply;

static mock_reply replies[8];
static int reply_count;
static int request_count;
static char paths[8][5000];

static void mock_reset(void)
{
	memset(replies, 0, sizeof replies);
	memset(paths, 0, sizeof paths);
	reply_count = 0;
	request_count = 0;
}

static void mock_add(bool transport_ok, int status, const char *body)
{
	assert(reply_count < (int)(sizeof replies / sizeof replies[0]));
	replies[reply_count++] = (mock_reply){transport_ok, status, body};
}

bool http_request(const char *host, const char *method, const char *path,
                  const char *bearer, const char *ctype, const char *body,
                  http_response *out, char *err, int errlen)
{
	(void)bearer;
	(void)ctype;
	(void)body;
	assert(strcmp(host, "lrclib.net") == 0);
	assert(strcmp(method, "GET") == 0);
	assert(request_count < reply_count);
	(void)snprintf(paths[request_count], sizeof paths[request_count], "%s", path);
	const mock_reply reply = replies[request_count++];
	memset(out, 0, sizeof *out);
	if (!reply.transport_ok) {
		if (err && errlen > 0)
			(void)snprintf(err, (size_t)errlen, "mock transport failure");
		return false;
	}
	out->status = reply.status;
	if (reply.body) {
		out->body_len = strlen(reply.body);
		out->body = malloc(out->body_len + 1);
		assert(out->body);
		memcpy(out->body, reply.body, out->body_len + 1);
	}
	return true;
}

bool http_request_progress(const char *host, const char *method,
	                       const char *path, const char *bearer,
	                       const char *ctype, const char *body,
	                       http_progress_fn progress, void *progress_context,
	                       http_response *out, char *err, int errlen)
{
	if (progress)
		progress(0, 0, false, progress_context);
	const bool ok = http_request(host, method, path, bearer, ctype, body, out,
	                             err, errlen);
	if (ok && progress)
		progress(out->body_len, out->body_len, true, progress_context);
	return ok;
}

void http_free(http_response *response)
{
	if (!response)
		return;
	free(response->body);
	memset(response, 0, sizeof *response);
}

static void test_json_alloc(void)
{
	const char json[] = "{\"value\":\"A\\uD83D\\uDE00B\"}";
	json_doc *doc = json_doc_parse(json, strlen(json), NULL);
	assert(doc);
	char *value = NULL;
	size_t len = 0;
	assert(json_doc_str_alloc(doc, "value", 6, &value, &len) == JSON_ALLOC_OK);
	assert(len == 6 && strcmp(value, "A\xF0\x9F\x98\x80" "B") == 0);
	free(value);
	assert(json_doc_str_alloc(doc, "value", 5, &value, &len) ==
	       JSON_ALLOC_TOO_LARGE);
	assert(value == NULL);
	assert(json_doc_str_alloc(doc, "missing", 10, &value, &len) ==
	       JSON_ALLOC_ABSENT);
	json_doc_free(doc);
}

static void test_parsers(void)
{
	lyrics_doc doc;
	lyrics_doc_init(&doc);
	assert(lyrics_parse_lrc(NULL, &doc) == LYRICS_NONE);
	assert(lyrics_parse_lrc(
	           "[00:02.50][00:01.250]  first  \r\n"
	           "[ar:ignored]\r\n[00:01.250]second\n[00:03.00]  \r\n",
	           &doc) == LYRICS_OK);
	assert(doc.synced && doc.count == 4);
	assert(doc.lines[0].time_ms == 1250 && strcmp(doc.lines[0].text, "first") == 0);
	assert(doc.lines[1].time_ms == 1250 && strcmp(doc.lines[1].text, "second") == 0);
	assert(doc.lines[2].time_ms == 2500 && strcmp(doc.lines[2].text, "first") == 0);
	assert(doc.lines[3].time_ms == 3000 && doc.lines[3].text[0] == '\0');
	assert(lyrics_index_at(&doc, 1249) == -1);
	assert(lyrics_index_at(&doc, 1250) == 1);
	assert(lyrics_index_at(&doc, 2999) == 2);
	assert(lyrics_index_at(&doc, 3000) == 3);

	assert(lyrics_parse_plain(" one \r\n\nthree", &doc) == LYRICS_OK);
	assert(!doc.synced && doc.count == 3);
	assert(strcmp(doc.lines[0].text, "one") == 0);
	assert(doc.lines[1].text[0] == '\0');
	assert(strcmp(doc.lines[2].text, "three") == 0);
	assert(lyrics_index_at(&doc, UINT32_MAX) == -1);
	lyrics_doc_free(&doc);
}

static void test_parser_limits(void)
{
	const size_t line_bytes = 19;
	char *many = malloc((LYRICS_MAX_LINES + 1u) * line_bytes + 1);
	assert(many);
	char *at = many;
	for (int i = 0; i < LYRICS_MAX_LINES + 1; i++) {
		const int written = snprintf(at, line_bytes + 1, "[00:00.00]line%04d\n", i);
		assert(written == (int)line_bytes);
		at += written;
	}
	*at = '\0';

	lyrics_doc doc;
	lyrics_doc_init(&doc);
	assert(lyrics_parse_lrc(many, &doc) == LYRICS_OK);
	assert(doc.count == LYRICS_MAX_LINES);
	free(many);

	char long_line[1100];
	memcpy(long_line, "[00:00.00]", 10);
	memset(long_line + 10, 'a', 998);
	memcpy(long_line + 1008, "\xE2\x82\xAC", 3);
	memset(long_line + 1011, 'z', 20);
	long_line[1031] = '\0';
	assert(lyrics_parse_lrc(long_line, &doc) == LYRICS_OK);
	assert(doc.count == 1);
	assert(strlen(doc.lines[0].text) == 998);
	assert(doc.lines[0].text[997] == 'a');
	lyrics_doc_free(&doc);
}

static lyrics_result fetch(lyrics_doc *doc, char *err, int errlen)
{
	return lyrics_fetch_lrclib("A/B Song", "The Artist", "Album & One", 199500,
	                           doc, NULL, NULL, err, errlen);
}

static void test_exact_synced(void)
{
	mock_reset();
	mock_add(true, 200,
	         "{\"id\":7,\"instrumental\":false,\"plainLyrics\":\"plain\","
	         "\"syncedLyrics\":\"[00:01.20]hello\\n[00:02.345]world\"}");
	lyrics_doc doc;
	lyrics_doc_init(&doc);
	char err[128];
	assert(fetch(&doc, err, sizeof err) == LYRICS_OK);
	assert(request_count == 1 && doc.synced && doc.count == 2);
	assert(strcmp(doc.track, "A/B Song") == 0);
	assert(strcmp(doc.artist, "The Artist") == 0);
	assert(strcmp(doc.lines[1].text, "world") == 0);
	assert(strcmp(paths[0],
	              "/api/get?track_name=A%2FB%20Song&artist_name=The%20Artist&"
	              "album_name=Album%20%26%20One&duration=200") == 0);
	lyrics_doc_free(&doc);
}

static void test_exact_plain_search_synced(void)
{
	mock_reset();
	mock_add(true, 200,
	         "{\"instrumental\":false,\"plainLyrics\":\"exact plain\","
	         "\"syncedLyrics\":null}");
	mock_add(true, 200,
	         "[{\"id\":9,\"trackName\":\"A B Song\","
	         "\"artistName\":\"The Artist\",\"albumName\":\"Album & One\","
	         "\"duration\":199.8,\"instrumental\":false,"
	         "\"plainLyrics\":\"candidate plain\","
	         "\"syncedLyrics\":\"[00:04.00]searched\"}]");
	lyrics_doc doc;
	lyrics_doc_init(&doc);
	char err[128];
	assert(fetch(&doc, err, sizeof err) == LYRICS_OK);
	assert(request_count == 2 && doc.synced && doc.count == 1);
	assert(strcmp(doc.lines[0].text, "searched") == 0);
	assert(strcmp(paths[1],
	              "/api/search?track_name=A%2FB%20Song&artist_name=The%20Artist&"
	              "album_name=Album%20%26%20One") == 0);
	lyrics_doc_free(&doc);
}

static void test_malformed_synced_fallback(void)
{
	mock_reset();
	mock_add(true, 200,
	         "{\"instrumental\":false,\"plainLyrics\":\"exact plain\","
	         "\"syncedLyrics\":\"not lrc\"}");
	mock_add(true, 200,
	         "["
	         "{\"id\":1,\"trackName\":\"A B Song\","
	         "\"artistName\":\"The Artist\",\"albumName\":\"Album & One\","
	         "\"duration\":200,\"plainLyrics\":null,"
	         "\"syncedLyrics\":\"still not lrc\"},"
	         "{\"id\":2,\"trackName\":\"A B Song feat Guest\","
	         "\"artistName\":\"The Artist\",\"albumName\":\"Other\","
	         "\"duration\":201,\"plainLyrics\":null,"
	         "\"syncedLyrics\":\"[00:03.00]valid lower match\"}"
	         "]");
	lyrics_doc doc;
	lyrics_doc_init(&doc);
	char err[128];
	assert(fetch(&doc, err, sizeof err) == LYRICS_OK);
	assert(doc.synced && doc.count == 1);
	assert(strcmp(doc.lines[0].text, "valid lower match") == 0);
	lyrics_doc_free(&doc);

	mock_reset();
	mock_add(true, 200,
	         "{\"instrumental\":false,\"plainLyrics\":\"exact survives\","
	         "\"syncedLyrics\":\"not lrc\"}");
	mock_add(false, 0, NULL);
	lyrics_doc_init(&doc);
	assert(fetch(&doc, err, sizeof err) == LYRICS_OK);
	assert(!doc.synced && doc.count == 1);
	assert(strcmp(doc.lines[0].text, "exact survives") == 0);
	lyrics_doc_free(&doc);

	mock_reset();
	mock_add(true, 404, "{}");
	mock_add(true, 200,
	         "[{\"id\":3,\"trackName\":\"A B Song\","
	         "\"artistName\":\"The Artist\",\"albumName\":\"Album & One\","
	         "\"duration\":200,\"plainLyrics\":\"candidate survives\","
	         "\"syncedLyrics\":\"not lrc\"}]");
	lyrics_doc_init(&doc);
	assert(fetch(&doc, err, sizeof err) == LYRICS_OK);
	assert(!doc.synced && doc.count == 1);
	assert(strcmp(doc.lines[0].text, "candidate survives") == 0);
	lyrics_doc_free(&doc);
}

static void test_unicode_plain_presence(void)
{
	mock_reset();
	mock_add(true, 200,
	         "{\"instrumental\":false,\"plainLyrics\":\"\\u4F60好\\nworld\","
	         "\"syncedLyrics\":null}");
	mock_add(true, 200, "[]");
	lyrics_doc doc;
	lyrics_doc_init(&doc);
	char err[128];
	assert(fetch(&doc, err, sizeof err) == LYRICS_OK);
	assert(!doc.synced && doc.count == 2);
	assert(strcmp(doc.lines[0].text, "\xE4\xBD\xA0\xE5\xA5\xBD") == 0);
	lyrics_doc_free(&doc);
}

static void test_plain_fallback_and_instrumental(void)
{
	mock_reset();
	mock_add(true, 200,
	         "{\"plainLyrics\":\"line one\\nline two\",\"syncedLyrics\":null}");
	mock_add(false, 0, NULL);
	lyrics_doc doc;
	lyrics_doc_init(&doc);
	char err[128];
	assert(fetch(&doc, err, sizeof err) == LYRICS_OK);
	assert(!doc.synced && doc.count == 2);
	assert(strcmp(doc.lines[0].text, "line one") == 0);
	lyrics_doc_free(&doc);

	mock_reset();
	mock_add(true, 200,
	         "{\"plainLyrics\":\"exact wins\",\"syncedLyrics\":null}");
	mock_add(true, 200,
	         "[{\"id\":1,\"trackName\":\"A B Song feat Guest\","
	         "\"artistName\":\"The Artist\",\"albumName\":\"Other\","
	         "\"duration\":200,\"plainLyrics\":\"fuzzy plain\","
	         "\"syncedLyrics\":null}]");
	lyrics_doc_init(&doc);
	assert(fetch(&doc, err, sizeof err) == LYRICS_OK);
	assert(!doc.synced && doc.count == 1);
	assert(strcmp(doc.lines[0].text, "exact wins") == 0);
	lyrics_doc_free(&doc);

	mock_reset();
	mock_add(true, 200,
	         "{\"instrumental\":true,\"plainLyrics\":null,"
	         "\"syncedLyrics\":null}");
	mock_add(true, 200, "[]");
	lyrics_doc_init(&doc);
	assert(fetch(&doc, err, sizeof err) == LYRICS_INSTRUMENTAL);
	assert(doc.count == 0 && strcmp(doc.track, "A/B Song") == 0);
	lyrics_doc_free(&doc);
}

static void test_empty_and_errors(void)
{
	lyrics_doc doc;
	lyrics_doc_init(&doc);
	char err[128];

	mock_reset();
	mock_add(true, 404, "{}");
	mock_add(true, 200, "[]");
	assert(fetch(&doc, err, sizeof err) == LYRICS_NONE);

	mock_reset();
	mock_add(true, 200, "{");
	assert(fetch(&doc, err, sizeof err) == LYRICS_ERR);
	assert(request_count == 1 && strstr(err, "malformed") != NULL);

	const int statuses[] = {429, 500, 503};
	for (size_t i = 0; i < sizeof statuses / sizeof statuses[0]; i++) {
		mock_reset();
		mock_add(true, statuses[i], "{}");
		assert(fetch(&doc, err, sizeof err) == LYRICS_ERR);
		assert(request_count == 1);
	}

	mock_reset();
	mock_add(true, 404, "{}");
	mock_add(true, 500, "{}");
	assert(fetch(&doc, err, sizeof err) == LYRICS_ERR);
	lyrics_doc_free(&doc);
}

static void test_ranking_and_duration(void)
{
	mock_reset();
	mock_add(true, 404, "{}");
	mock_add(true, 200,
	         "["
	         "{\"id\":1,\"trackName\":\"A B Song\",\"artistName\":\"The Artist\","
	         "\"albumName\":\"Album & One\",\"duration\":210,"
	         "\"syncedLyrics\":\"[00:01.00]wrong duration\",\"plainLyrics\":null},"
	         "{\"id\":2,\"trackName\":\"A B Song Live\",\"artistName\":\"The Artist\","
	         "\"albumName\":\"Album & One\",\"duration\":200,"
	         "\"syncedLyrics\":\"[00:01.00]wrong version\",\"plainLyrics\":null},"
	         "{\"id\":8,\"trackName\":\"A B Song\",\"artistName\":\"The Artist\","
	         "\"albumName\":\"Album & One\",\"duration\":201,"
	         "\"syncedLyrics\":\"[00:01.00]higher id\",\"plainLyrics\":null},"
	         "{\"id\":3,\"trackName\":\"a-b song\",\"artistName\":\"the artist\","
	         "\"albumName\":\"album one\",\"duration\":198.6,"
	         "\"syncedLyrics\":\"[00:01.00]winner\",\"plainLyrics\":\"plain\"},"
	         "{\"id\":0,\"trackName\":\"A B Song\",\"artistName\":\"Other\","
	         "\"albumName\":\"Album & One\",\"duration\":200,"
	         "\"syncedLyrics\":\"[00:01.00]wrong artist\",\"plainLyrics\":null}"
	         "]");
	lyrics_doc doc;
	lyrics_doc_init(&doc);
	char err[128];
	assert(fetch(&doc, err, sizeof err) == LYRICS_OK);
	assert(doc.synced && doc.count == 1);
	assert(strcmp(doc.lines[0].text, "winner") == 0);
	lyrics_doc_free(&doc);
}

typedef struct {
	int calls;
	int cancel_at;
} cancel_state;

static bool cancel_after(void *context)
{
	cancel_state *state = context;
	state->calls++;
	return state->calls >= state->cancel_at;
}

static void test_cancellation(void)
{
	mock_reset();
	mock_add(true, 404, "{}");
	cancel_state state = {0, 2};
	lyrics_doc doc;
	lyrics_doc_init(&doc);
	char err[128];
	assert(lyrics_fetch_lrclib("Track", "Artist", "Album", 1000, &doc,
	                           cancel_after, &state, err, sizeof err) ==
	       LYRICS_CANCELLED);
	assert(request_count == 1);
	lyrics_doc_free(&doc);
}

typedef struct {
	lyrics_fetch_phase phases[16];
	size_t received[16];
	bool known[16];
	int count;
} lyrics_progress_state;

static void record_lyrics_progress(lyrics_fetch_phase phase, size_t received,
	                               size_t total, bool total_known,
	                               void *context)
{
	(void)total;
	lyrics_progress_state *state = context;
	if (state->count >= (int)(sizeof state->phases / sizeof state->phases[0]))
		return;
	state->phases[state->count] = phase;
	state->received[state->count] = received;
	state->known[state->count] = total_known;
	state->count++;
}

static void test_progress_phases(void)
{
	mock_reset();
	mock_add(true, 200,
	         "{\"instrumental\":false,\"plainLyrics\":\"exact plain\","
	         "\"syncedLyrics\":null}");
	mock_add(true, 200,
	         "[{\"id\":9,\"trackName\":\"A B Song\","
	         "\"artistName\":\"The Artist\",\"albumName\":\"Album & One\","
	         "\"duration\":200,\"instrumental\":false,"
	         "\"plainLyrics\":null,"
	         "\"syncedLyrics\":\"[00:04.00]searched\"}]");
	lyrics_doc doc;
	lyrics_doc_init(&doc);
	lyrics_progress_state progress = {0};
	char err[128];
	assert(lyrics_fetch_lrclib_progress(
	           "A/B Song", "The Artist", "Album & One", 199500, &doc, NULL,
	           NULL, record_lyrics_progress, &progress, err, sizeof err) ==
	       LYRICS_OK);
	assert(progress.count >= 6);
	assert(progress.phases[0] == LYRICS_FETCH_EXACT);
	bool saw_search = false;
	bool saw_processing = false;
	for (int i = 0; i < progress.count; i++) {
		if (progress.phases[i] == LYRICS_FETCH_SEARCH)
			saw_search = true;
		if (progress.phases[i] == LYRICS_FETCH_PROCESSING &&
		    progress.received[i] > 0)
			saw_processing = true;
	}
	assert(saw_search && saw_processing);
	lyrics_doc_free(&doc);
}

int main(void)
{
	test_json_alloc();
	test_parsers();
	test_parser_limits();
	test_exact_synced();
	test_exact_plain_search_synced();
	test_malformed_synced_fallback();
	test_unicode_plain_presence();
	test_plain_fallback_and_instrumental();
	test_empty_and_errors();
	test_ranking_and_duration();
	test_cancellation();
	test_progress_phases();
	puts("lyrics: parsing limits, LRCLIB fallback, errors, and ranking passed");
	return 0;
}
