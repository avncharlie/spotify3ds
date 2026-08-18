#include "searchhistory.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *const s_path[SEARCHHISTORY_SCOPES] = {
	[SEARCHHISTORY_LIBRARY] = "sdmc:/spotify/searches-library.txt",
	[SEARCHHISTORY_TRACKS] = "sdmc:/spotify/searches-tracks.txt",
};

/* One query per line, newest first. Simpler than the name cache's format on
 * purpose: that one carries a timestamp because its entries expire, and tabs
 * because it holds three fields. A query expires only by being pushed out, and
 * is a single value, so the line is the query. Spotify allows spaces in a
 * search but a newline cannot survive the round trip anyway, which is what
 * makes one-per-line safe. */

static searchhistory_store s_store[SEARCHHISTORY_SCOPES];
static bool                s_loaded[SEARCHHISTORY_SCOPES];
static bool                s_dirty[SEARCHHISTORY_SCOPES];

static bool valid(searchhistory_scope scope)
{
	return scope >= 0 && scope < SEARCHHISTORY_SCOPES;
}

static bool blank(const char *s)
{
	for (; *s; s++)
		if (!isspace((unsigned char)*s))
			return false;
	return true;
}

static bool same_query(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return false;
	return *a == *b;
}

void searchhistory_store_push(searchhistory_store *s, const char *query)
{
	if (!s || !query || !query[0] || blank(query) || strchr(query, '\n'))
		return;

	int at = -1;
	for (int i = 0; i < s->count; i++) {
		if (same_query(s->q[i], query)) {
			at = i;
			break;
		}
	}

	if (at < 0) {
		/* New: make room at the front, dropping the oldest if full. */
		at = s->count < SEARCHHISTORY_MAX ? s->count : SEARCHHISTORY_MAX - 1;
		if (s->count < SEARCHHISTORY_MAX)
			s->count++;
	}
	/* Everything above the slot shifts down by one, which both promotes an
	 * existing entry and opens index 0 for a new one. */
	memmove(&s->q[1], &s->q[0], (size_t)at * sizeof s->q[0]);
	snprintf(s->q[0], sizeof s->q[0], "%.*s", SEARCHHISTORY_QUERY_MAX - 1,
	         query);
}

void searchhistory_store_remove(searchhistory_store *s, int index)
{
	if (!s || index < 0 || index >= s->count)
		return;
	memmove(&s->q[index], &s->q[index + 1],
	        (size_t)(s->count - index - 1) * sizeof s->q[0]);
	s->count--;
	memset(s->q[s->count], 0, sizeof s->q[0]);
}

void searchhistory_store_clear(searchhistory_store *s)
{
	if (s)
		memset(s, 0, sizeof *s);
}

/* --- file-backed singleton --------------------------------------------- */

static void load(searchhistory_scope scope)
{
	if (!valid(scope) || s_loaded[scope])
		return;
	s_loaded[scope] = true;

	searchhistory_store *st = &s_store[scope];
	FILE *f = fopen(s_path[scope], "r");
	if (!f)
		return;
	char line[SEARCHHISTORY_QUERY_MAX + 8];
	while (st->count < SEARCHHISTORY_MAX && fgets(line, sizeof line, f)) {
		char *nl = strchr(line, '\n');
		if (nl) {
			*nl = '\0';
		} else if (!feof(f)) {
			/* Longer than the buffer. Swallow the rest of it, or the tail
			 * would come back as a second, bogus entry. */
			int c;
			while ((c = fgetc(f)) != EOF && c != '\n')
				;
			continue;
		}
		if (!line[0] || blank(line))
			continue;
		/* Straight in, preserving file order: push would reverse it. */
		snprintf(st->q[st->count], sizeof st->q[0], "%.*s",
		         SEARCHHISTORY_QUERY_MAX - 1, line);
		st->count++;
	}
	fclose(f);
}

int searchhistory_count(searchhistory_scope scope)
{
	if (!valid(scope))
		return 0;
	load(scope);
	return s_store[scope].count;
}

const char *searchhistory_at(searchhistory_scope scope, int index)
{
	if (!valid(scope))
		return NULL;
	load(scope);
	if (index < 0 || index >= s_store[scope].count)
		return NULL;
	return s_store[scope].q[index];
}

void searchhistory_push(searchhistory_scope scope, const char *query)
{
	if (!valid(scope))
		return;
	load(scope);
	searchhistory_store_push(&s_store[scope], query);
	/* Dirty whenever the push was accepted. A promotion leaves the count
	 * unchanged but reorders the file, so the count is not the test - the
	 * query reaching the front is. */
	if (s_store[scope].count > 0 && same_query(s_store[scope].q[0], query))
		s_dirty[scope] = true;
}

void searchhistory_flush(searchhistory_scope scope)
{
	if (!valid(scope))
		return;
	load(scope);
	if (!s_dirty[scope])
		return;
	FILE *f = fopen(s_path[scope], "w");
	if (!f)
		return; /* best effort: the next search simply offers less */
	for (int i = 0; i < s_store[scope].count; i++)
		fprintf(f, "%s\n", s_store[scope].q[i]);
	fclose(f);
	s_dirty[scope] = false;
}

void searchhistory_remove(searchhistory_scope scope, int index)
{
	if (!valid(scope))
		return;
	load(scope);
	const int before = s_store[scope].count;
	searchhistory_store_remove(&s_store[scope], index);
	if (s_store[scope].count != before)
		s_dirty[scope] = true;
}

void searchhistory_clear(searchhistory_scope scope)
{
	if (!valid(scope))
		return;
	load(scope);
	searchhistory_store_clear(&s_store[scope]);
	remove(s_path[scope]);
	s_dirty[scope] = false;
}
