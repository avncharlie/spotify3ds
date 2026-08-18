#include "tracks_search.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int track_search_match_quality_n(const char *text, size_t textlen,
	                             const char *query, size_t querylen)
{
	if (!text || !query || !querylen || querylen > textlen)
		return 0;
	/* `textlen - querylen` cannot underflow after that guard, so the loop
	 * stops before the tail that is too short to hold a match. */
	for (size_t at = 0; at <= textlen - querylen; at++) {
		size_t i = 0;
		while (i < querylen &&
		       tolower((unsigned char)text[at + i]) ==
		           tolower((unsigned char)query[i]))
			i++;
		if (i != querylen)
			continue;
		if (at == 0)
			return at + querylen == textlen ? 3 : 2;
		return 1;
	}
	return 0;
}

int track_search_rank_fields(const char *name, size_t namelen,
	                         const char *artist, size_t artistlen,
	                         const char *album, size_t albumlen,
	                         const char *query, size_t querylen,
	                         bool match_album)
{
	int quality = track_search_match_quality_n(name, namelen, query, querylen);
	if (quality)
		return 3 - quality;
	quality = track_search_match_quality_n(artist, artistlen, query, querylen);
	if (quality)
		return 6 - quality;
	quality = match_album ? track_search_match_quality_n(album, albumlen, query,
	                                                     querylen)
	                      : 0;
	return quality ? 9 - quality : -1;
}

static int hit_rank(const track_item *item, const char *query,
	                bool match_album)
{
	return track_search_rank_fields(item->name, strlen(item->name),
	                                item->artist, strlen(item->artist),
	                                item->album, strlen(item->album), query,
	                                strlen(query), match_album);
}

static bool better(const track_search_hit *a, const track_search_hit *b)
{
	if (a->rank != b->rank)
		return a->rank < b->rank;
	return a->item.source_index < b->item.source_index;
}

static bool worse(const track_search_hit *a, const track_search_hit *b)
{
	return better(b, a);
}

static void heap_up(track_search_hit *hits, int index)
{
	while (index > 0) {
		const int parent = (index - 1) / 2;
		if (!worse(&hits[index], &hits[parent]))
			break;
		const track_search_hit swap = hits[index];
		hits[index] = hits[parent];
		hits[parent] = swap;
		index = parent;
	}
}

static void heap_down(track_search_hit *hits, int count, int index)
{
	for (;;) {
		const int left = index * 2 + 1;
		const int right = left + 1;
		int worst = index;
		if (left < count && worse(&hits[left], &hits[worst]))
			worst = left;
		if (right < count && worse(&hits[right], &hits[worst]))
			worst = right;
		if (worst == index)
			return;
		const track_search_hit swap = hits[index];
		hits[index] = hits[worst];
		hits[worst] = swap;
		index = worst;
	}
}

void track_search_results_init(track_search_results *results)
{
	if (results)
		memset(results, 0, sizeof *results);
}

void track_search_results_free(track_search_results *results)
{
	if (!results)
		return;
	free(results->hits);
	track_search_results_init(results);
}

void track_search_results_move(track_search_results *dst,
	                           track_search_results *src)
{
	if (!dst || !src || dst == src)
		return;
	track_search_results_free(dst);
	*dst = *src;
	track_search_results_init(src);
}

bool track_search_results_copy(track_search_results *dst,
	                           const track_search_results *src)
{
	if (!dst || !src)
		return false;
	track_search_results_free(dst);
	*dst = *src;
	dst->hits = NULL;
	if (src->count > 0) {
		dst->hits = malloc(TRACK_SEARCH_RESULTS_MAX * sizeof *dst->hits);
		if (!dst->hits) {
			track_search_results_init(dst);
			return false;
		}
		memcpy(dst->hits, src->hits, (size_t)src->count * sizeof *dst->hits);
	} else {
		dst->count = 0;
	}
	return true;
}

bool track_search_consider_hit(track_search_results *results,
	                           const track_item *item, int rank)
{
	if (!results || !item || rank < 0)
		return false;
	results->matched_total++;
	track_search_hit hit = {.item = *item, .rank = (unsigned char)rank};
	if (results->count < TRACK_SEARCH_RESULTS_MAX) {
		if (!results->hits) {
			results->hits =
			    malloc(TRACK_SEARCH_RESULTS_MAX * sizeof *results->hits);
			if (!results->hits) {
				results->matched_total--;
				return false;
			}
		}
		results->hits[results->count] = hit;
		heap_up(results->hits, results->count);
		results->count++;
	} else if (better(&hit, &results->hits[0])) {
		results->hits[0] = hit;
		heap_down(results->hits, results->count, 0);
	}
	results->truncated = results->matched_total > results->count;
	return true;
}

bool track_search_consider_page(track_search_results *results,
	                            const track_page *page, const char *query,
	                            bool match_album)
{
	if (!results || !page || !query || !query[0])
		return false;
	if (page->total > results->source_total)
		results->source_total = page->total;
	for (int i = 0; i < page->count; i++) {
		const track_item *item = &page->items[i];
		if (item->kind != TRACK_ITEM_TRACK)
			continue;
		const int rank = hit_rank(item, query, match_album);
		if (rank < 0)
			continue;
		if (!track_search_consider_hit(results, item, rank))
			return false;
	}
	results->truncated = results->matched_total > results->count;
	return true;
}

static int compare_hits(const void *left, const void *right)
{
	const track_search_hit *a = left;
	const track_search_hit *b = right;
	if (better(a, b))
		return -1;
	if (better(b, a))
		return 1;
	return 0;
}

void track_search_finalize(track_search_results *results)
{
	if (!results || results->count <= 1)
		return;
	qsort(results->hits, (size_t)results->count, sizeof *results->hits,
	      compare_hits);
}

void track_search_build_page(const track_search_results *results,
	                         const collection_item *collection, int offset,
	                         track_page *out)
{
	memset(out, 0, sizeof *out);
	if (!results || !collection)
		return;
	if (offset < 0)
		offset = 0;
	if (offset >= results->count && results->count > 0)
		offset = ((results->count - 1) / TRACK_PAGE_MAX) * TRACK_PAGE_MAX;
	out->collection = *collection;
	out->offset = offset;
	out->total = results->count;
	out->count = results->count - offset;
	if (out->count > TRACK_PAGE_MAX)
		out->count = TRACK_PAGE_MAX;
	for (int i = 0; i < out->count; i++)
		out->items[i] = results->hits[offset + i].item;
}
