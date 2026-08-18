#include "searchhistory.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define HISTORY_PATH "sdmc:/spotify/searches.txt"

/* One query per line, newest first. Simpler than the name cache's format on
 * purpose: that one carries a timestamp because its entries expire, and tabs
 * because it holds three fields. A query expires only by being pushed out, and
 * is a single value, so the line is the query. Spotify allows spaces in a
 * search but a newline cannot survive the round trip anyway, which is what
 * makes one-per-line safe. */

static searchhistory_store s_store;
static bool                s_loaded;
static bool                s_dirty;

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

void searchhistory_store_clear(searchhistory_store *s)
{
	if (s)
		memset(s, 0, sizeof *s);
}

/* --- file-backed singleton --------------------------------------------- */

static void load(void)
{
	if (s_loaded)
		return;
	s_loaded = true;

	FILE *f = fopen(HISTORY_PATH, "r");
	if (!f)
		return;
	char line[SEARCHHISTORY_QUERY_MAX + 8];
	while (s_store.count < SEARCHHISTORY_MAX && fgets(line, sizeof line, f)) {
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
		snprintf(s_store.q[s_store.count], sizeof s_store.q[0], "%.*s",
		         SEARCHHISTORY_QUERY_MAX - 1, line);
		s_store.count++;
	}
	fclose(f);
}

int searchhistory_count(void)
{
	load();
	return s_store.count;
}

const char *searchhistory_at(int index)
{
	load();
	if (index < 0 || index >= s_store.count)
		return NULL;
	return s_store.q[index];
}

void searchhistory_push(const char *query)
{
	load();
	searchhistory_store_push(&s_store, query);
	/* Dirty whenever the push was accepted. A promotion leaves the count
	 * unchanged but reorders the file, so the count is not the test - the
	 * query reaching the front is. */
	if (s_store.count > 0 && same_query(s_store.q[0], query))
		s_dirty = true;
}

void searchhistory_flush(void)
{
	load();
	if (!s_dirty)
		return;
	FILE *f = fopen(HISTORY_PATH, "w");
	if (!f)
		return; /* best effort: the next search simply offers less */
	for (int i = 0; i < s_store.count; i++)
		fprintf(f, "%s\n", s_store.q[i]);
	fclose(f);
	s_dirty = false;
}

void searchhistory_clear(void)
{
	load();
	searchhistory_store_clear(&s_store);
	remove(HISTORY_PATH);
	s_dirty = false;
}
