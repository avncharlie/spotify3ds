#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "spotify/tracks_search.h"

static track_item item(int source, const char *name, const char *artist,
	                   const char *album)
{
	track_item result;
	memset(&result, 0, sizeof result);
	result.source_index = source;
	result.kind = TRACK_ITEM_TRACK;
	result.playable = true;
	snprintf(result.name, sizeof result.name, "%s", name);
	snprintf(result.artist, sizeof result.artist, "%s", artist);
	snprintf(result.album, sizeof result.album, "%s", album);
	snprintf(result.uri, sizeof result.uri, "spotify:track:%08d", source);
	return result;
}

static void test_ranking(void)
{
	track_page page;
	memset(&page, 0, sizeof page);
	page.total = 8;
	page.count = 8;
	page.items[0] = item(0, "Love", "Other", "Other");
	page.items[1] = item(1, "Love Story", "Other", "Other");
	page.items[2] = item(2, "A Love Song", "Other", "Other");
	page.items[3] = item(3, "Other", "Love", "Other");
	page.items[4] = item(4, "Other", "Love Band", "Other");
	page.items[5] = item(5, "Other", "A Love Band", "Other");
	page.items[6] = item(6, "Other", "Other", "Love");
	page.items[7] = item(7, "Other", "Other", "A Love Album");

	track_search_results results;
	track_search_results_init(&results);
	assert(track_search_consider_page(&results, &page, "love", true));
	track_search_finalize(&results);
	assert(results.count == 8 && results.matched_total == 8);
	for (int i = 0; i < results.count; i++)
		assert(results.hits[i].item.source_index == i);
	track_search_results_free(&results);

	track_search_results_init(&results);
	assert(track_search_consider_page(&results, &page, "love", false));
	track_search_finalize(&results);
	assert(results.count == 6 && results.matched_total == 6);
	track_search_results_free(&results);
}

static void test_cap_move_and_page(void)
{
	track_search_results results;
	track_search_results_init(&results);
	for (int offset = 0; offset < 550; offset += TRACK_PAGE_MAX) {
		track_page page;
		memset(&page, 0, sizeof page);
		page.offset = offset;
		page.total = 550;
		page.count = TRACK_PAGE_MAX;
		for (int i = 0; i < page.count; i++)
			page.items[i] = item(offset + i, "Match", "Artist", "Album");
		assert(track_search_consider_page(&results, &page, "match", true));
	}
	track_search_finalize(&results);
	assert(results.count == TRACK_SEARCH_RESULTS_MAX);
	assert(results.matched_total == 550 && results.truncated);
	assert(results.hits[0].item.source_index == 0);
	assert(results.hits[499].item.source_index == 499);

	track_search_results moved;
	track_search_results_init(&moved);
	track_search_results_move(&moved, &results);
	assert(results.hits == NULL && results.count == 0);
	assert(moved.count == 500);

	collection_item collection;
	memset(&collection, 0, sizeof collection);
	track_page visible;
	track_search_build_page(&moved, &collection, 450, &visible);
	assert(visible.offset == 450 && visible.count == 50 && visible.total == 500);
	assert(visible.items[0].source_index == 450);
	track_search_results_free(&moved);
}

/* A partial snapshot is taken while the heap is still unsorted and still
 * growing, so copying must not disturb the scan and must not freeze ordering:
 * a better match on a later page still has to reach the top. */
static void test_partial_snapshot(void)
{
	track_page first;
	memset(&first, 0, sizeof first);
	first.offset = 0;
	first.total = 4;
	first.count = 2;
	first.items[0] = item(0, "Other", "Other", "Love Album");
	first.items[1] = item(1, "A Love Song", "Other", "Other");

	track_search_results results;
	track_search_results_init(&results);
	assert(track_search_consider_page(&results, &first, "love", true));

	track_search_results snapshot;
	track_search_results_init(&snapshot);
	assert(track_search_results_copy(&snapshot, &results));
	track_search_finalize(&snapshot);
	assert(snapshot.count == 2);
	assert(snapshot.hits[0].item.source_index == 1);
	assert(snapshot.hits[1].item.source_index == 0);

	/* Sorting the copy must leave the source heap usable for the next page. */
	track_page second;
	memset(&second, 0, sizeof second);
	second.offset = 2;
	second.total = 4;
	second.count = 2;
	second.items[0] = item(2, "Love", "Other", "Other");
	second.items[1] = item(3, "Nothing", "Nobody", "Nowhere");
	assert(track_search_consider_page(&results, &second, "love", true));
	assert(results.matched_total == 3);

	track_search_finalize(&results);
	assert(results.count == 3);
	assert(results.hits[0].item.source_index == 2);
	assert(results.hits[1].item.source_index == 1);
	assert(results.hits[2].item.source_index == 0);

	/* The earlier snapshot keeps its own storage after the source is freed. */
	track_search_results_free(&results);
	assert(snapshot.count == 2);
	assert(snapshot.hits[0].item.source_index == 1);
	track_search_results_free(&snapshot);

	/* Copying an empty result set is valid and allocates nothing. */
	track_search_results empty, empty_copy;
	track_search_results_init(&empty);
	track_search_results_init(&empty_copy);
	assert(track_search_results_copy(&empty_copy, &empty));
	assert(empty_copy.count == 0 && empty_copy.hits == NULL);
	track_search_results_free(&empty_copy);
}

/* The cached index stores text packed without terminators, so the matcher must
 * honour the length it is given rather than scanning to a NUL. Getting this
 * wrong would let a warm cache rank differently from a live scan. */
static void test_length_bounded_matching(void)
{
	const char buf[] = "loveXXXX";
	/* Bounded to "love", so it is an exact match despite the trailing bytes. */
	assert(track_search_match_quality_n(buf, 4, "love", 4) == 3);
	/* Must not read past the bound to find the 'X'. */
	assert(track_search_match_quality_n(buf, 4, "lovex", 5) == 0);
	/* Prefix and interior still grade as they do for whole strings. */
	assert(track_search_match_quality_n(buf, 8, "love", 4) == 2);
	assert(track_search_match_quality_n("a love song", 11, "love", 4) == 1);
	/* Degenerate inputs. */
	assert(track_search_match_quality_n("ab", 2, "abc", 3) == 0);
	assert(track_search_match_quality_n("", 0, "a", 1) == 0);
	assert(track_search_match_quality_n("ab", 2, "", 0) == 0);
	assert(track_search_match_quality_n(NULL, 0, "a", 1) == 0);

	/* Field ranking: name beats artist beats album, and album is skipped
	 * for album collections. */
	assert(track_search_rank_fields("Love", 4, "X", 1, "Y", 1, "love", 4,
	                                true) == 0);
	assert(track_search_rank_fields("X", 1, "Love", 4, "Y", 1, "love", 4,
	                                true) == 3);
	assert(track_search_rank_fields("X", 1, "Y", 1, "Love", 4, "love", 4,
	                                true) == 6);
	assert(track_search_rank_fields("X", 1, "Y", 1, "Love", 4, "love", 4,
	                                false) == -1);
}

int main(void)
{
	test_ranking();
	test_cap_move_and_page();
	test_partial_snapshot();
	test_length_bounded_matching();
	puts("track search: ranking, cap, move, snapshot, bounds, and pagination "
	     "passed");
	return 0;
}
