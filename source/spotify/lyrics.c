#include "lyrics.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "../net/http.h"

#define LRCLIB_HOST "lrclib.net"
#define LYRICS_GROW_LINES 64
#define QUERY_INPUT_MAX 512
#define QUERY_ENCODED_MAX (QUERY_INPUT_MAX * 3)
#define LYRICS_FIELD_MAX (2304u * 1024u)
#define SEARCH_MAX 50

typedef struct {
	int index;
	int metadata_score;
	bool synced;
	bool plain;
	bool instrumental;
	long id;
} search_choice;

typedef struct {
	lyrics_fetch_phase phase;
	lyrics_progress_fn progress;
	void              *context;
} lyrics_http_progress;

static void report_progress(lyrics_progress_fn progress, void *context,
	                        lyrics_fetch_phase phase, size_t received,
	                        size_t total, bool total_known)
{
	if (progress)
		progress(phase, received, total, total_known, context);
}

static void forward_http_progress(size_t received, size_t total,
	                              bool total_known, void *context)
{
	lyrics_http_progress *forward = context;
	report_progress(forward->progress, forward->context, forward->phase,
	                received, total, total_known);
}

static void set_error(char *err, int errlen, const char *fmt, ...)
{
	if (!err || errlen <= 0)
		return;
	va_list ap;
	va_start(ap, fmt);
	(void)vsnprintf(err, (size_t)errlen, fmt, ap);
	va_end(ap);
}

void lyrics_doc_init(lyrics_doc *doc)
{
	if (doc)
		memset(doc, 0, sizeof *doc);
}

void lyrics_doc_free(lyrics_doc *doc)
{
	if (!doc)
		return;
	free(doc->lines);
	lyrics_doc_init(doc);
}

void lyrics_doc_move(lyrics_doc *dst, lyrics_doc *src)
{
	if (!dst || !src || dst == src)
		return;
	lyrics_doc_free(dst);
	*dst = *src;
	lyrics_doc_init(src);
}

static size_t utf8_prefix(const char *text, size_t len, size_t limit)
{
	size_t n = len < limit ? len : limit;
	if (n == len)
		return n;
	while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80)
		n--;
	return n;
}

static void copy_utf8(char *dst, size_t dst_max, const char *src, size_t len)
{
	const size_t n = utf8_prefix(src, len, dst_max);
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static bool reserve_line(lyrics_doc *doc)
{
	if (doc->count >= LYRICS_MAX_LINES)
		return false;
	if (doc->count < doc->capacity)
		return true;

	size_t capacity = doc->capacity + LYRICS_GROW_LINES;
	if (capacity > LYRICS_MAX_LINES)
		capacity = LYRICS_MAX_LINES;
	lyric_line *lines = realloc(doc->lines, capacity * sizeof *lines);
	if (!lines)
		return false;
	doc->lines = lines;
	doc->capacity = capacity;
	return true;
}

static bool add_line(lyrics_doc *doc, uint32_t time_ms, const char *text,
	                 size_t len)
{
	if (doc->count == LYRICS_MAX_LINES)
		return true;
	if (!reserve_line(doc))
		return false;
	lyric_line *line = &doc->lines[doc->count++];
	line->time_ms = time_ms;
	copy_utf8(line->text, LYRICS_TEXT_MAX, text, len);
	return true;
}

static void trim(const char **start, const char **end)
{
	while (*start < *end && (**start == ' ' || **start == '\t'))
		(*start)++;
	while (*end > *start && ((*end)[-1] == ' ' || (*end)[-1] == '\t' ||
	                         (*end)[-1] == '\r'))
		(*end)--;
}

static bool parse_timestamp(const char *start, const char *end, uint32_t *out)
{
	const char *p = start;
	unsigned long minutes = 0;
	unsigned seconds = 0;
	unsigned fraction = 0;
	int fraction_digits = 0;

	if (p == end || !isdigit((unsigned char)*p))
		return false;
	while (p < end && isdigit((unsigned char)*p)) {
		if (minutes > UINT32_MAX / 60000u)
			return false;
		minutes = minutes * 10 + (unsigned)(*p++ - '0');
	}
	if (p == end || *p++ != ':' || end - p < 2 ||
	    !isdigit((unsigned char)p[0]) || !isdigit((unsigned char)p[1]))
		return false;
	seconds = (unsigned)(p[0] - '0') * 10u + (unsigned)(p[1] - '0');
	p += 2;
	if (seconds >= 60)
		return false;
	if (p < end) {
		if (*p++ != '.')
			return false;
		while (p < end && isdigit((unsigned char)*p) && fraction_digits < 3) {
			fraction = fraction * 10u + (unsigned)(*p++ - '0');
			fraction_digits++;
		}
		if (p != end || (fraction_digits != 2 && fraction_digits != 3))
			return false;
	}

	const unsigned fraction_ms = fraction_digits == 2 ? fraction * 10u : fraction;
	const unsigned long long total =
	    (unsigned long long)minutes * 60000ull + seconds * 1000u + fraction_ms;
	if (total > UINT32_MAX)
		return false;
	*out = (uint32_t)total;
	return true;
}

static bool stable_sort_lines(lyrics_doc *doc)
{
	bool sorted = true;
	for (size_t i = 1; i < doc->count; i++) {
		if (doc->lines[i - 1].time_ms > doc->lines[i].time_ms) {
			sorted = false;
			break;
		}
	}
	if (sorted)
		return true;

	lyric_line *scratch = malloc(doc->count * sizeof *scratch);
	if (!scratch)
		return false;
	lyric_line *src = doc->lines;
	lyric_line *dst = scratch;
	for (size_t width = 1; width < doc->count; width *= 2) {
		for (size_t left = 0; left < doc->count; left += width * 2) {
			size_t mid = left + width;
			size_t right = left + width * 2;
			if (mid > doc->count)
				mid = doc->count;
			if (right > doc->count)
				right = doc->count;
			size_t a = left;
			size_t b = mid;
			for (size_t at = left; at < right; at++) {
				if (b == right ||
				    (a < mid && src[a].time_ms <= src[b].time_ms))
					dst[at] = src[a++];
				else
					dst[at] = src[b++];
			}
		}
		lyric_line *swap = src;
		src = dst;
		dst = swap;
	}
	if (src != doc->lines)
		memcpy(doc->lines, src, doc->count * sizeof *src);
	free(scratch);
	return true;
}

lyrics_result lyrics_parse_lrc(const char *text, lyrics_doc *out)
{
	if (!text || !out)
		return LYRICS_NONE;
	lyrics_doc parsed;
	lyrics_doc_init(&parsed);
	parsed.synced = true;

	const char *line = text;
	while (*line && parsed.count < LYRICS_MAX_LINES) {
		const char *end = strchr(line, '\n');
		if (!end)
			end = line + strlen(line);
		const char *start = line;
		const char *line_end = end;
		trim(&start, &line_end);

		const char *p = start;
		while (p < line_end && *p == '[') {
			const char *close = memchr(p + 1, ']', (size_t)(line_end - p - 1));
			uint32_t ignored;
			if (!close || !parse_timestamp(p + 1, close, &ignored))
				break;
			p = close + 1;
		}
		const char *lyric_start = p;
		const char *lyric_end = line_end;
		trim(&lyric_start, &lyric_end);

		p = start;
		while (p < line_end && *p == '[' && parsed.count < LYRICS_MAX_LINES) {
			const char *close = memchr(p + 1, ']', (size_t)(line_end - p - 1));
			uint32_t timestamp;
			if (!close || !parse_timestamp(p + 1, close, &timestamp))
				break;
			if (!add_line(&parsed, timestamp, lyric_start,
			              (size_t)(lyric_end - lyric_start))) {
				lyrics_doc_free(&parsed);
				return LYRICS_ERR;
			}
			p = close + 1;
		}
		line = *end ? end + 1 : end;
	}

	if (!parsed.count) {
		lyrics_doc_free(&parsed);
		return LYRICS_NONE;
	}
	if (!stable_sort_lines(&parsed)) {
		lyrics_doc_free(&parsed);
		return LYRICS_ERR;
	}
	lyrics_doc_move(out, &parsed);
	return LYRICS_OK;
}

lyrics_result lyrics_parse_plain(const char *text, lyrics_doc *out)
{
	if (!text || !out || !*text)
		return LYRICS_NONE;
	lyrics_doc parsed;
	lyrics_doc_init(&parsed);

	const char *line = text;
	while (*line && parsed.count < LYRICS_MAX_LINES) {
		const char *end = strchr(line, '\n');
		if (!end)
			end = line + strlen(line);
		const char *start = line;
		const char *line_end = end;
		trim(&start, &line_end);
		if (!add_line(&parsed, 0, start, (size_t)(line_end - start))) {
			lyrics_doc_free(&parsed);
			return LYRICS_ERR;
		}
		line = *end ? end + 1 : end;
	}
	if (!parsed.count) {
		lyrics_doc_free(&parsed);
		return LYRICS_NONE;
	}
	lyrics_doc_move(out, &parsed);
	return LYRICS_OK;
}

int lyrics_index_at(const lyrics_doc *doc, uint32_t position_ms)
{
	if (!doc || !doc->synced || !doc->count)
		return -1;
	size_t low = 0;
	size_t high = doc->count;
	while (low < high) {
		const size_t mid = low + (high - low) / 2;
		if (doc->lines[mid].time_ms <= position_ms)
			low = mid + 1;
		else
			high = mid;
	}
	return low ? (int)(low - 1) : -1;
}

static void encode_query(const char *input, char out[QUERY_ENCODED_MAX + 1])
{
	static const char hex[] = "0123456789ABCDEF";
	if (!input)
		input = "";
	const size_t input_len = utf8_prefix(input, strlen(input), QUERY_INPUT_MAX);
	size_t at = 0;
	for (size_t i = 0; i < input_len; i++) {
		const unsigned char c = (unsigned char)input[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
		    c == '~') {
			out[at++] = (char)c;
		} else {
			out[at++] = '%';
			out[at++] = hex[c >> 4];
			out[at++] = hex[c & 15];
		}
	}
	out[at] = '\0';
}

static bool was_cancelled(lyrics_cancel_fn cancelled, void *context)
{
	return cancelled && cancelled(context);
}

static void set_metadata(lyrics_doc *doc, const char *track, const char *artist)
{
	copy_utf8(doc->track, LYRICS_METADATA_MAX, track, strlen(track));
	copy_utf8(doc->artist, LYRICS_METADATA_MAX, artist, strlen(artist));
}

static lyrics_result finish_text(char *text, bool synced, const char *track,
	                             const char *artist, lyrics_doc *out)
{
	lyrics_doc parsed;
	lyrics_doc_init(&parsed);
	const lyrics_result result = synced ? lyrics_parse_lrc(text, &parsed)
	                                    : lyrics_parse_plain(text, &parsed);
	free(text);
	if (result != LYRICS_OK) {
		lyrics_doc_free(&parsed);
		return result == LYRICS_NONE ? LYRICS_ERR : result;
	}
	set_metadata(&parsed, track, artist);
	lyrics_doc_move(out, &parsed);
	return LYRICS_OK;
}

static lyrics_result finish_plain_or(char *plain, bool instrumental,
	                                 const char *track, const char *artist,
	                                 lyrics_doc *out)
{
	if (plain)
		return finish_text(plain, false, track, artist, out);
	lyrics_doc empty;
	lyrics_doc_init(&empty);
	set_metadata(&empty, track, artist);
	lyrics_doc_move(out, &empty);
	return instrumental ? LYRICS_INSTRUMENTAL : LYRICS_NONE;
}

static char *take_text(char **text)
{
	char *value = *text;
	*text = NULL;
	return value;
}

static bool string_present(const json_doc *doc, const char *path)
{
	return json_doc_is_nonempty_string(doc, path);
}

static bool strict_long(const json_doc *doc, const char *path, long *out)
{
	char value[64];
	if (!json_doc_str(doc, path, value, sizeof value))
		return false;
	char *end = NULL;
	const long parsed = strtol(value, &end, 10);
	if (end == value || *end != '\0')
		return false;
	*out = parsed;
	return true;
}

static bool duration_seconds(const json_doc *doc, const char *path, double *out)
{
	char value[64];
	if (!json_doc_str(doc, path, value, sizeof value))
		return false;
	char *end = NULL;
	const double parsed = strtod(value, &end);
	if (end == value || *end != '\0' || !isfinite(parsed) || parsed <= 0.0)
		return false;
	*out = parsed;
	return true;
}

static void normalize(const char *input, char *out, size_t outlen)
{
	size_t at = 0;
	bool space = true;
	for (const unsigned char *p = (const unsigned char *)input; *p; p++) {
		unsigned char c = *p;
		if (c >= 'A' && c <= 'Z')
			c = (unsigned char)(c - 'A' + 'a');
		const bool punctuation = c < 0x80 &&
		    (isspace(c) || strchr("'\".,!?:;-_()/\\[]{}+&", c) != NULL);
		if (punctuation) {
			if (!space && at + 1 < outlen)
				out[at++] = ' ';
			space = true;
		} else if (at + 1 < outlen) {
			out[at++] = (char)c;
			space = false;
		}
	}
	if (at && out[at - 1] == ' ')
		at--;
	out[at] = '\0';
}

static bool whole_word_contains(const char *haystack, const char *needle)
{
	if (!*needle)
		return false;
	const size_t len = strlen(needle);
	for (const char *p = strstr(haystack, needle); p; p = strstr(p + 1, needle)) {
		if ((p == haystack || p[-1] == ' ') &&
		    (p[len] == '\0' || p[len] == ' '))
			return true;
	}
	return false;
}

static unsigned qualifier_mask(const char *normalized)
{
	static const struct {
		const char *word;
		unsigned mask;
	} qualifiers[] = {
		{"live", 1u << 0},          {"remix", 1u << 1},
		{"acoustic", 1u << 2},      {"instrumental", 1u << 3},
		{"karaoke", 1u << 4},       {"demo", 1u << 5},
		{"edit", 1u << 6},          {"remaster", 1u << 7},
		{"remastered", 1u << 7},    {"sped", 1u << 8},
		{"slowed", 1u << 9},        {"radio", 1u << 10},
		{"mix", 1u << 11},
	};
	unsigned mask = 0;
	for (size_t i = 0; i < sizeof qualifiers / sizeof qualifiers[0]; i++) {
		if (whole_word_contains(normalized, qualifiers[i].word))
			mask |= qualifiers[i].mask;
	}
	return mask;
}

static void without_feature(const char *normalized, char *out, size_t outlen)
{
	const char *cut = strstr(normalized, " feat ");
	const char *featuring = strstr(normalized, " featuring ");
	const char *ft = strstr(normalized, " ft ");
	if (!cut || (featuring && featuring < cut))
		cut = featuring;
	if (!cut || (ft && ft < cut))
		cut = ft;
	const size_t len = cut ? (size_t)(cut - normalized) : strlen(normalized);
	copy_utf8(out, outlen - 1, normalized, len);
}

static int title_quality(const char *wanted, const char *candidate)
{
	char a[QUERY_INPUT_MAX + 1];
	char b[QUERY_INPUT_MAX + 1];
	normalize(wanted, a, sizeof a);
	normalize(candidate, b, sizeof b);
	if (!a[0] || !b[0] || qualifier_mask(a) != qualifier_mask(b))
		return 0;
	if (strcmp(a, b) == 0)
		return 4;
	char af[QUERY_INPUT_MAX + 1];
	char bf[QUERY_INPUT_MAX + 1];
	without_feature(a, af, sizeof af);
	without_feature(b, bf, sizeof bf);
	if (strcmp(af, bf) == 0)
		return 3;
	if (whole_word_contains(af, bf) || whole_word_contains(bf, af))
		return 1;
	return 0;
}

static int artist_quality(const char *wanted, const char *candidate)
{
	char a[QUERY_INPUT_MAX + 1];
	char b[QUERY_INPUT_MAX + 1];
	normalize(wanted, a, sizeof a);
	normalize(candidate, b, sizeof b);
	if (!a[0] || !b[0])
		return 0;
	if (strcmp(a, b) == 0)
		return 3;
	if (whole_word_contains(a, b) || whole_word_contains(b, a))
		return 1;
	return 0;
}

static bool better_choice(const search_choice *candidate,
	                      const search_choice *best)
{
	if (best->index < 0)
		return true;
	if (candidate->synced != best->synced)
		return candidate->synced;
	if (candidate->metadata_score != best->metadata_score)
		return candidate->metadata_score > best->metadata_score;
	return candidate->id < best->id;
}

static void set_http_error(char *err, int errlen, const char *operation,
	                       int status)
{
	if (status == 429)
		set_error(err, errlen, "LRCLIB rate limited; try again later");
	else if (status >= 500)
		set_error(err, errlen, "LRCLIB unavailable (http %d)", status);
	else
		set_error(err, errlen, "LRCLIB %s http %d", operation, status);
}

static lyrics_result allocated_field(const json_doc *doc, const char *path,
	                                 char **out, char *err, int errlen)
{
	size_t len = 0;
	const json_alloc_result result =
	    json_doc_str_alloc(doc, path, LYRICS_FIELD_MAX, out, &len);
	if (result == JSON_ALLOC_OK && len)
		return LYRICS_OK;
	free(*out);
	*out = NULL;
	if (result == JSON_ALLOC_TOO_LARGE)
		set_error(err, errlen, "LRCLIB lyric field exceeds %u bytes",
		          (unsigned)LYRICS_FIELD_MAX);
	else if (result == JSON_ALLOC_OOM)
		set_error(err, errlen, "out of memory reading LRCLIB lyrics");
	else
		set_error(err, errlen, "missing or invalid LRCLIB lyric field %s (%d)",
		          path, (int)result);
	return LYRICS_ERR;
}

static lyrics_result search_lrclib(const char *path, const char *track,
	                               const char *artist, const char *album,
	                               long wanted_duration_ms, char **exact_plain,
	                               bool exact_instrumental, lyrics_doc *out,
	                               lyrics_cancel_fn cancelled,
	                               void *cancel_context,
	                               lyrics_progress_fn progress,
	                               void *progress_context, char *err, int errlen)
{
	report_progress(progress, progress_context, LYRICS_FETCH_SEARCH, 0, 0,
	                false);
	lyrics_http_progress transfer = {
		.phase = LYRICS_FETCH_SEARCH,
		.progress = progress,
		.context = progress_context,
	};
	http_response response;
	if (!http_request_progress(LRCLIB_HOST, "GET", path, NULL, NULL, NULL,
	                           forward_http_progress, &transfer, &response, err,
	                           errlen)) {
		if (*exact_plain)
			return finish_plain_or(take_text(exact_plain), exact_instrumental,
			                       track, artist, out);
		return LYRICS_ERR;
	}
	if (response.status != 200 || !response.body || !response.body_len) {
		set_http_error(err, errlen, "search", response.status);
		http_free(&response);
		if (*exact_plain)
			return finish_plain_or(take_text(exact_plain), exact_instrumental,
			                       track, artist, out);
		return LYRICS_ERR;
	}
	report_progress(progress, progress_context, LYRICS_FETCH_PROCESSING,
	                response.body_len, 0, false);

	int needed = 0;
	json_doc *doc = json_doc_parse(response.body, response.body_len, &needed);
	const int available = doc ? json_doc_array_size(doc, "") : -1;
	if (!doc || available < 0) {
		set_error(err, errlen, "malformed LRCLIB search response");
		json_doc_free(doc);
		http_free(&response);
		if (*exact_plain)
			return finish_plain_or(take_text(exact_plain), exact_instrumental,
			                       track, artist, out);
		return LYRICS_ERR;
	}

	search_choice choices[SEARCH_MAX];
	int choice_count = 0;
	const int count = available < SEARCH_MAX ? available : SEARCH_MAX;
	for (int i = 0; i < count; i++) {
		if (was_cancelled(cancelled, cancel_context)) {
			json_doc_free(doc);
			http_free(&response);
			return LYRICS_CANCELLED;
		}
		char field[64];
		char candidate_track[QUERY_INPUT_MAX + 1];
		char candidate_artist[QUERY_INPUT_MAX + 1];
		char candidate_album[QUERY_INPUT_MAX + 1] = "";
		(void)snprintf(field, sizeof field, "[%d].trackName", i);
		if (!json_doc_str(doc, field, candidate_track, sizeof candidate_track))
			continue;
		(void)snprintf(field, sizeof field, "[%d].artistName", i);
		if (!json_doc_str(doc, field, candidate_artist, sizeof candidate_artist))
			continue;
		const int tq = title_quality(track, candidate_track);
		const int aq = artist_quality(artist, candidate_artist);
		if (!tq || !aq)
			continue;

		double candidate_duration = 0.0;
		(void)snprintf(field, sizeof field, "[%d].duration", i);
		const bool duration_known = duration_seconds(doc, field, &candidate_duration);
		if (wanted_duration_ms > 0 && duration_known) {
			const double difference =
			    candidate_duration - (double)wanted_duration_ms / 1000.0;
			if (difference < -3.0 || difference > 3.0)
				continue;
		}

		(void)snprintf(field, sizeof field, "[%d].syncedLyrics", i);
		const bool synced = string_present(doc, field);
		(void)snprintf(field, sizeof field, "[%d].plainLyrics", i);
		const bool plain = string_present(doc, field);
		bool instrumental = false;
		(void)snprintf(field, sizeof field, "[%d].instrumental", i);
		(void)json_doc_bool(doc, field, &instrumental);
		if (!synced && !plain && !instrumental)
			continue;

		(void)snprintf(field, sizeof field, "[%d].albumName", i);
		(void)json_doc_str(doc, field, candidate_album, sizeof candidate_album);
		char wanted_album[QUERY_INPUT_MAX + 1];
		char normalized_album[QUERY_INPUT_MAX + 1];
		normalize(album, wanted_album, sizeof wanted_album);
		normalize(candidate_album, normalized_album, sizeof normalized_album);
		const bool album_exact = wanted_album[0] &&
		                         strcmp(wanted_album, normalized_album) == 0;

		long id = LONG_MAX;
		(void)snprintf(field, sizeof field, "[%d].id", i);
		(void)strict_long(doc, field, &id);
		search_choice candidate = {
			.index = i,
			.metadata_score = tq * 100 + aq * 30 + (album_exact ? 8 : 0) +
			                  (duration_known && wanted_duration_ms > 0 ? 2 : 0),
			.synced = synced,
			.plain = plain,
			.instrumental = instrumental,
			.id = id,
		};
		choices[choice_count++] = candidate;
	}

	if (!choice_count) {
		json_doc_free(doc);
		http_free(&response);
		char *plain = *exact_plain;
		*exact_plain = NULL;
		return finish_plain_or(plain, exact_instrumental, track, artist, out);
	}

	/* Try accepted synced matches in deterministic rank order. A non-empty
	 * field is not necessarily valid LRC, so a malformed high-ranked result
	 * must not hide a valid lower-ranked one. */
	bool used[SEARCH_MAX] = {false};
	bool invalid_synced = false;
	for (;;) {
		int best_at = -1;
		search_choice best = {.index = -1, .id = LONG_MAX};
		for (int i = 0; i < choice_count; i++) {
			if (!used[i] && choices[i].synced &&
			    better_choice(&choices[i], &best)) {
				best = choices[i];
				best_at = i;
			}
		}
		if (best_at < 0)
			break;
		used[best_at] = true;

		char field[64];
		(void)snprintf(field, sizeof field, "[%d].syncedLyrics", best.index);
		char *selected = NULL;
		if (allocated_field(doc, field, &selected, err, errlen) != LYRICS_OK) {
			json_doc_free(doc);
			http_free(&response);
			if (*exact_plain)
				return finish_plain_or(take_text(exact_plain), exact_instrumental,
				                       track, artist, out);
			return LYRICS_ERR;
		}
		lyrics_doc parsed;
		lyrics_doc_init(&parsed);
		const lyrics_result parsed_result = lyrics_parse_lrc(selected, &parsed);
		free(selected);
		if (parsed_result == LYRICS_OK) {
			set_metadata(&parsed, track, artist);
			lyrics_doc_move(out, &parsed);
			json_doc_free(doc);
			http_free(&response);
			free(*exact_plain);
			*exact_plain = NULL;
			return LYRICS_OK;
		}
		lyrics_doc_free(&parsed);
		if (parsed_result == LYRICS_ERR) {
			set_error(err, errlen, "out of memory parsing LRCLIB lyrics");
			json_doc_free(doc);
			http_free(&response);
			if (*exact_plain)
				return finish_plain_or(take_text(exact_plain), exact_instrumental,
				                       track, artist, out);
			return LYRICS_ERR;
		}
		invalid_synced = true;
	}

	if (*exact_plain) {
		json_doc_free(doc);
		http_free(&response);
		return finish_plain_or(take_text(exact_plain), exact_instrumental,
		                       track, artist, out);
	}

	search_choice best = {.index = -1, .id = LONG_MAX};
	for (int i = 0; i < choice_count; i++)
		if (choices[i].plain && better_choice(&choices[i], &best))
			best = choices[i];
	if (best.index < 0) {
		json_doc_free(doc);
		http_free(&response);
		if (invalid_synced) {
			set_error(err, errlen, "LRCLIB returned malformed synced lyrics");
			return LYRICS_ERR;
		}
		return finish_plain_or(NULL, exact_instrumental, track, artist, out);
	}
	if (best.instrumental && !best.plain) {
		json_doc_free(doc);
		http_free(&response);
		return finish_plain_or(NULL, true, track, artist, out);
	}

	char field[64];
	(void)snprintf(field, sizeof field, "[%d].plainLyrics", best.index);
	char *selected = NULL;
	const lyrics_result extracted = allocated_field(doc, field, &selected, err,
	                                               errlen);
	json_doc_free(doc);
	http_free(&response);
	if (extracted != LYRICS_OK)
		return LYRICS_ERR;
	return finish_text(selected, false, track, artist, out);
}

lyrics_result lyrics_fetch_lrclib_progress(
	const char *track, const char *artist, const char *album, long duration_ms,
	lyrics_doc *out, lyrics_cancel_fn cancelled, void *cancel_context,
	lyrics_progress_fn progress, void *progress_context, char *err, int errlen)
{
	if (err && errlen > 0)
		err[0] = '\0';
	if (!track || !*track || !artist || !*artist || !out) {
		set_error(err, errlen, "track and artist are required");
		return LYRICS_ERR;
	}
	if (!album)
		album = "";
	if (was_cancelled(cancelled, cancel_context))
		return LYRICS_CANCELLED;

	char encoded_track[QUERY_ENCODED_MAX + 1];
	char encoded_artist[QUERY_ENCODED_MAX + 1];
	char encoded_album[QUERY_ENCODED_MAX + 1];
	encode_query(track, encoded_track);
	encode_query(artist, encoded_artist);
	encode_query(album, encoded_album);
	const long duration = duration_ms > 0
	                          ? duration_ms / 1000 +
	                                (duration_ms % 1000 >= 500 ? 1 : 0)
	                          : 0;
	char path[QUERY_ENCODED_MAX * 3 + 192];
	int path_len = snprintf(path, sizeof path,
	                        "/api/get?track_name=%s&artist_name=%s",
	                        encoded_track, encoded_artist);
	if (album[0] && path_len > 0 && path_len < (int)sizeof path)
		path_len += snprintf(path + path_len, sizeof path - (size_t)path_len,
		                     "&album_name=%s", encoded_album);
	if (duration > 0 && path_len > 0 && path_len < (int)sizeof path)
		(void)snprintf(path + path_len, sizeof path - (size_t)path_len,
		               "&duration=%ld", duration);

	http_response response;
	report_progress(progress, progress_context, LYRICS_FETCH_EXACT, 0, 0, false);
	lyrics_http_progress transfer = {
		.phase = LYRICS_FETCH_EXACT,
		.progress = progress,
		.context = progress_context,
	};
	if (!http_request_progress(LRCLIB_HOST, "GET", path, NULL, NULL, NULL,
	                           forward_http_progress, &transfer, &response, err,
	                           errlen))
		return LYRICS_ERR;

	char *exact_plain = NULL;
	bool exact_instrumental = false;
	if (response.status == 200) {
		if (!response.body || !response.body_len) {
			set_error(err, errlen, "empty LRCLIB exact response");
			http_free(&response);
			return LYRICS_ERR;
		}
		report_progress(progress, progress_context, LYRICS_FETCH_PROCESSING,
		                response.body_len, 0, false);
		int needed = 0;
		json_doc *doc = json_doc_parse(response.body, response.body_len, &needed);
		if (!doc || !json_doc_is_object(doc, "")) {
			set_error(err, errlen, "malformed LRCLIB exact response");
			json_doc_free(doc);
			http_free(&response);
			return LYRICS_ERR;
		}

		(void)json_doc_bool(doc, "instrumental", &exact_instrumental);
		if (string_present(doc, "plainLyrics") &&
		    allocated_field(doc, "plainLyrics", &exact_plain, err, errlen) !=
		        LYRICS_OK) {
			json_doc_free(doc);
			http_free(&response);
			return LYRICS_ERR;
		}
		if (string_present(doc, "syncedLyrics")) {
			char *synced = NULL;
			const lyrics_result extracted =
			    allocated_field(doc, "syncedLyrics", &synced, err, errlen);
			if (extracted != LYRICS_OK) {
				json_doc_free(doc);
				http_free(&response);
				free(exact_plain);
				return LYRICS_ERR;
			}
			lyrics_doc parsed;
			lyrics_doc_init(&parsed);
			const lyrics_result parsed_result = lyrics_parse_lrc(synced, &parsed);
			free(synced);
			if (parsed_result == LYRICS_OK) {
				set_metadata(&parsed, track, artist);
				lyrics_doc_move(out, &parsed);
				json_doc_free(doc);
				http_free(&response);
				free(exact_plain);
				return LYRICS_OK;
			}
			lyrics_doc_free(&parsed);
			if (parsed_result == LYRICS_ERR) {
				set_error(err, errlen, "out of memory parsing LRCLIB lyrics");
				json_doc_free(doc);
				http_free(&response);
				free(exact_plain);
				return LYRICS_ERR;
			}
		}
		json_doc_free(doc);
		http_free(&response);
	} else if (response.status == 404) {
		http_free(&response);
	} else {
		set_http_error(err, errlen, "exact", response.status);
		http_free(&response);
		return LYRICS_ERR;
	}

	if (was_cancelled(cancelled, cancel_context)) {
		free(exact_plain);
		return LYRICS_CANCELLED;
	}
	// Try two - seearch lrclib if our exact query didn't get a result
	path_len = snprintf(path, sizeof path,
	                    "/api/search?track_name=%s&artist_name=%s",
	                    encoded_track, encoded_artist);
	if (album[0] && path_len > 0 && path_len < (int)sizeof path)
		(void)snprintf(path + path_len, sizeof path - (size_t)path_len,
		               "&album_name=%s", encoded_album);
	const lyrics_result result =
	    search_lrclib(path, track, artist, album, duration_ms, &exact_plain,
	                  exact_instrumental, out, cancelled, cancel_context,
	                  progress, progress_context, err, errlen);
	free(exact_plain);
	return result;
}

lyrics_result lyrics_fetch_lrclib(const char *track, const char *artist,
	                              const char *album, long duration_ms,
	                              lyrics_doc *out, lyrics_cancel_fn cancelled,
	                              void *cancel_context, char *err, int errlen)
{
	return lyrics_fetch_lrclib_progress(
	    track, artist, album, duration_ms, out, cancelled, cancel_context, NULL,
	    NULL, err, errlen);
}
