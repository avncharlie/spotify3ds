#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "spotify/searchhistory.h"

static int count_of(const searchhistory_store *s) { return s->count; }

static void test_push_and_order(void)
{
	searchhistory_store s;
	searchhistory_store_clear(&s);

	searchhistory_store_push(&s, "daft punk");
	assert(count_of(&s) == 1);
	assert(strcmp(s.q[0], "daft punk") == 0);

	/* Newest first, so the order is the reverse of the pushes. */
	searchhistory_store_push(&s, "ella mai");
	searchhistory_store_push(&s, "kaiser chiefs");
	assert(count_of(&s) == 3);
	assert(strcmp(s.q[0], "kaiser chiefs") == 0);
	assert(strcmp(s.q[1], "ella mai") == 0);
	assert(strcmp(s.q[2], "daft punk") == 0);
}

/* Re-running an old query is a recall, not a new search: it moves to the front
 * rather than appearing twice. */
static void test_repeat_promotes(void)
{
	searchhistory_store s;
	searchhistory_store_clear(&s);
	searchhistory_store_push(&s, "a");
	searchhistory_store_push(&s, "b");
	searchhistory_store_push(&s, "c");
	searchhistory_store_push(&s, "a");

	assert(count_of(&s) == 3); /* not 4 */
	assert(strcmp(s.q[0], "a") == 0);
	assert(strcmp(s.q[1], "c") == 0);
	assert(strcmp(s.q[2], "b") == 0);
}

/* Case is not part of the recall - somebody retyping "Tame" after "tame" means
 * the same search - but the newest spelling is what gets shown, since that is
 * what they last typed. */
static void test_case_insensitive_dedup(void)
{
	searchhistory_store s;
	searchhistory_store_clear(&s);
	searchhistory_store_push(&s, "tame impala");
	searchhistory_store_push(&s, "Tame Impala");

	assert(count_of(&s) == 1);
	assert(strcmp(s.q[0], "Tame Impala") == 0);
}

static void test_capacity(void)
{
	searchhistory_store s;
	searchhistory_store_clear(&s);
	char q[16];
	for (int i = 0; i < SEARCHHISTORY_MAX + 3; i++) {
		snprintf(q, sizeof q, "q%d", i);
		searchhistory_store_push(&s, q);
	}
	assert(count_of(&s) == SEARCHHISTORY_MAX);

	/* The newest survives and the first three pushed are gone. */
	snprintf(q, sizeof q, "q%d", SEARCHHISTORY_MAX + 2);
	assert(strcmp(s.q[0], q) == 0);
	for (int i = 0; i < s.count; i++) {
		assert(strcmp(s.q[i], "q0") != 0);
		assert(strcmp(s.q[i], "q1") != 0);
		assert(strcmp(s.q[i], "q2") != 0);
	}
	/* Promoting from the tail must not resurrect a dropped entry or grow
	 * past the cap. */
	searchhistory_store_push(&s, s.q[SEARCHHISTORY_MAX - 1]);
	assert(count_of(&s) == SEARCHHISTORY_MAX);
}

/* Nothing that cannot be searched for, and nothing that would break the
 * one-query-per-line file. */
static void test_rejects(void)
{
	searchhistory_store s;
	searchhistory_store_clear(&s);
	searchhistory_store_push(&s, "");
	searchhistory_store_push(&s, "   ");
	searchhistory_store_push(&s, "\t\t");
	searchhistory_store_push(&s, "two\nlines");
	searchhistory_store_push(&s, NULL);
	assert(count_of(&s) == 0);

	/* A valid one still lands after the refusals. */
	searchhistory_store_push(&s, "real");
	assert(count_of(&s) == 1);
}

/* The store's rows are the same width as the query buffers in main.c, so a
 * maximum-length query has to fit exactly, terminator included. */
static void test_long_query(void)
{
	searchhistory_store s;
	searchhistory_store_clear(&s);
	char big[SEARCHHISTORY_QUERY_MAX * 2];
	memset(big, 'x', sizeof big - 1);
	big[sizeof big - 1] = '\0';

	searchhistory_store_push(&s, big);
	assert(count_of(&s) == 1);
	assert(strlen(s.q[0]) == SEARCHHISTORY_QUERY_MAX - 1);

	/* Truncation must not make two different long queries collide into one
	 * entry only by accident - they share a prefix, so they legitimately do. */
	assert(s.q[0][SEARCHHISTORY_QUERY_MAX - 1] == '\0');
}

int main(void)
{
	test_push_and_order();
	test_repeat_promotes();
	test_case_insensitive_dedup();
	test_capacity();
	test_rejects();
	test_long_query();
	puts("search history: order, promotion, case, capacity, and rejects passed");
	return 0;
}
