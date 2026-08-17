#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LYRICS_MAX_LINES 2000
#define LYRICS_TEXT_MAX 1000
#define LYRICS_METADATA_MAX 255

typedef struct {
	uint32_t time_ms;
	char text[LYRICS_TEXT_MAX + 1];
} lyric_line;

typedef struct {
	lyric_line *lines;
	size_t count;
	size_t capacity;
	bool synced;
	char track[LYRICS_METADATA_MAX + 1];
	char artist[LYRICS_METADATA_MAX + 1];
} lyrics_doc;

typedef enum {
	LYRICS_OK = 0,
	LYRICS_INSTRUMENTAL,
	LYRICS_NONE,
	LYRICS_ERR,
	LYRICS_CANCELLED,
} lyrics_result;

typedef bool (*lyrics_cancel_fn)(void *context);

typedef enum {
	LYRICS_FETCH_EXACT = 0,
	LYRICS_FETCH_SEARCH,
	LYRICS_FETCH_PROCESSING,
} lyrics_fetch_phase;

typedef void (*lyrics_progress_fn)(lyrics_fetch_phase phase, size_t received,
                                   size_t total, bool total_known,
                                   void *context);

void lyrics_doc_init(lyrics_doc *doc);
void lyrics_doc_free(lyrics_doc *doc);
void lyrics_doc_move(lyrics_doc *dst, lyrics_doc *src);

/* Parsing replaces an initialized output document only on success. */
lyrics_result lyrics_parse_lrc(const char *text, lyrics_doc *out);
lyrics_result lyrics_parse_plain(const char *text, lyrics_doc *out);

/* Return the last synced line at or before position_ms, or -1. */
int lyrics_index_at(const lyrics_doc *doc, uint32_t position_ms);

/* Blocking LRCLIB lookup. `out` must have been initialized. duration_ms may be
 * zero when unknown; album may be NULL. No authentication is required. */
lyrics_result lyrics_fetch_lrclib(const char *track, const char *artist,
                                  const char *album, long duration_ms,
                                  lyrics_doc *out, lyrics_cancel_fn cancelled,
                                  void *cancel_context, char *err, int errlen);

lyrics_result lyrics_fetch_lrclib_progress(
    const char *track, const char *artist, const char *album, long duration_ms,
    lyrics_doc *out, lyrics_cancel_fn cancelled, void *cancel_context,
    lyrics_progress_fn progress, void *progress_context, char *err, int errlen);
