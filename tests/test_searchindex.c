#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spotify/searchindex.h"
#include "spotify/tracks_search.h"

#define ART_SCDN "https://i.scdn.co/image/"
#define ART_MOSAIC "https://mosaic.scdn.co/60/"

static const char *hex40(int seed, char *out)
{
	static const char digits[] = "0123456789abcdef";
	for (int i = 0; i < 40; i++)
		out[i] = digits[(seed * 7 + i * 3) & 0x0F];
	out[40] = '\0';
	return out;
}

static collection_item playlist_of(const char *name, int total)
{
	collection_item c;
	memset(&c, 0, sizeof c);
	c.kind = COLLECTION_PLAYLIST;
	c.item_total = total;
	snprintf(c.name, sizeof c.name, "%s", name);
	snprintf(c.context_uri, sizeof c.context_uri, "spotify:playlist:%s", "abc");
	return c;
}

static track_item item(int source, const char *name, const char *artist,
	                   const char *album, const char *art)
{
	track_item t;
	memset(&t, 0, sizeof t);
	t.source_index = source;
	t.kind = TRACK_ITEM_TRACK;
	t.playable = true;
	snprintf(t.name, sizeof t.name, "%s", name);
	snprintf(t.artist, sizeof t.artist, "%s", artist);
	snprintf(t.album, sizeof t.album, "%s", album);
	snprintf(t.uri, sizeof t.uri, "spotify:track:%022d", source);
	snprintf(t.art_url, sizeof t.art_url, "%s", art ? art : "");
	return t;
}

/* Build an index from `pages` and hand back the opened result. */
static searchindex *build(const collection_item *c, const track_page *pages,
	                      int npages, const char *snapshot)
{
	searchindex_builder *b =
	    searchindex_builder_new(c, snapshot, c->item_total);
	assert(b);
	for (int i = 0; i < npages; i++)
		assert(searchindex_builder_add_page(b, &pages[i]));
	unsigned char *blob = NULL;
	size_t         len = 0;
	assert(searchindex_builder_finish(b, &blob, &len));
	searchindex_builder_free(b);
	searchindex *ix = searchindex_open(blob, len);
	assert(ix);
	return ix;
}

static void test_round_trip(void)
{
	char art[41];
	collection_item c = playlist_of("Mixed", 6);
	track_page page;
	memset(&page, 0, sizeof page);
	page.collection = c;
	page.total = 6;
	page.count = 6;

	char scdn[64], mosaic[200];
	snprintf(scdn, sizeof scdn, "%s%s", ART_SCDN, hex40(1, art));
	/* A mosaic tail is four concatenated 40-char ids. */
	char four[161];
	for (int i = 0; i < 4; i++)
		memcpy(four + i * 40, hex40(i + 2, art), 40);
	four[160] = '\0';
	snprintf(mosaic, sizeof mosaic, "%s%s", ART_MOSAIC, four);

	page.items[0] = item(0, "Plain", "Artist", "Album", scdn);
	page.items[1] = item(1, "Mosaic Cover", "Band", "Record", mosaic);
	page.items[2] = item(2, "No Art", "Nobody", "Nowhere", "");
	page.items[3] = item(3, "Odd Host", "X", "Y", "https://example.com/a.png");
	/* Multibyte utf-8 must survive byte-for-byte. */
	page.items[4] = item(4, "Café Möller — ½", "Ártist", "Ålbum", scdn);
	page.items[5] = item(5, "", "", "", scdn); /* empty text fields */
	page.items[5].playable = false;
	page.items[5].explicit_content = true;

	searchindex *ix = build(&c, &page, 1, "snap-1");
	assert(searchindex_count(ix) == 6);
	assert(strcmp(searchindex_snapshot(ix), "snap-1") == 0);

	/* Recover every record by searching for something all rows match on:
	 * use a per-row query instead, then check the materialised fields. */
	struct { const char *q; int idx; const char *art; } probe[] = {
		{"Plain", 0, scdn},
		{"Mosaic", 1, mosaic},
		{"No Art", 2, ""},
		{"Odd Host", 3, "https://example.com/a.png"},
		{"Möller", 4, scdn},
	};
	for (size_t i = 0; i < sizeof probe / sizeof *probe; i++) {
		track_search_results r;
		track_search_results_init(&r);
		assert(searchindex_search(ix, &c, probe[i].q, &r));
		assert(r.count == 1);
		const track_item *got = &r.hits[0].item;
		const track_item *want = &page.items[probe[i].idx];
		assert(got->source_index == want->source_index);
		assert(strcmp(got->name, want->name) == 0);
		assert(strcmp(got->artist, want->artist) == 0);
		assert(strcmp(got->album, want->album) == 0);
		assert(strcmp(got->uri, want->uri) == 0);
		assert(strcmp(got->art_url, probe[i].art) == 0);
		assert(got->playable == want->playable);
		track_search_results_free(&r);
	}
	searchindex_free(ix);
}

/* The whole safety argument for the cache: a cached index must rank exactly as
 * a live scan of the same pages would. If these ever diverge, a warm search
 * silently returns different results from a cold one. */
static void test_equivalence_with_live_scan(void)
{
	char art[41];
	const int total = 1200; /* enough to blow past the 500 retention cap */
	collection_item c = playlist_of("Corpus", total);

	static track_page pages[24];
	const int npages = (total + TRACK_PAGE_MAX - 1) / TRACK_PAGE_MAX;
	assert(npages <= (int)(sizeof pages / sizeof *pages));

	static const char *names[] = {"Love Song", "love", "A Love Story",
	                              "Beloved", "Nothing", "LOVE", "glove"};
	static const char *artists[] = {"Lover", "Band", "The Loveless", "X"};
	static const char *albums[] = {"Love Album", "Other", "lovely"};

	for (int p = 0; p < npages; p++) {
		memset(&pages[p], 0, sizeof pages[p]);
		pages[p].collection = c;
		pages[p].offset = p * TRACK_PAGE_MAX;
		pages[p].total = total;
		pages[p].count = (p == npages - 1) ? total - p * TRACK_PAGE_MAX
		                                   : TRACK_PAGE_MAX;
		for (int i = 0; i < pages[p].count; i++) {
			const int src = pages[p].offset + i;
			char full[64];
			snprintf(full, sizeof full, "%s%s", ART_SCDN, hex40(src, art));
			pages[p].items[i] = item(src, names[src % 7], artists[src % 4],
			                         albums[src % 3], full);
		}
	}

	searchindex *ix = build(&c, pages, npages, "snap-eq");

	static const char *queries[] = {"love", "LOVE", "l",     "glove",
	                                "band", "x",    "zzzz",  "Love Song",
	                                "lovely", "The Loveless"};
	for (size_t q = 0; q < sizeof queries / sizeof *queries; q++) {
		track_search_results live, cached;
		track_search_results_init(&live);
		track_search_results_init(&cached);
		for (int p = 0; p < npages; p++)
			assert(track_search_consider_page(&live, &pages[p], queries[q],
			                                  true));
		assert(searchindex_search(ix, &c, queries[q], &cached));
		track_search_finalize(&live);
		track_search_finalize(&cached);

		assert(live.count == cached.count);
		assert(live.matched_total == cached.matched_total);
		assert(live.truncated == cached.truncated);
		for (int i = 0; i < live.count; i++) {
			assert(live.hits[i].rank == cached.hits[i].rank);
			assert(live.hits[i].item.source_index ==
			       cached.hits[i].item.source_index);
			assert(strcmp(live.hits[i].item.name, cached.hits[i].item.name) == 0);
			assert(strcmp(live.hits[i].item.artist,
			              cached.hits[i].item.artist) == 0);
			assert(strcmp(live.hits[i].item.album,
			              cached.hits[i].item.album) == 0);
			assert(strcmp(live.hits[i].item.uri, cached.hits[i].item.uri) == 0);
			assert(strcmp(live.hits[i].item.art_url,
			              cached.hits[i].item.art_url) == 0);
		}
		track_search_results_free(&live);
		track_search_results_free(&cached);
	}
	searchindex_free(ix);
}

/* An album collection stores no album column and must not match on it. */
static void test_album_collection(void)
{
	collection_item c;
	memset(&c, 0, sizeof c);
	c.kind = COLLECTION_ALBUM;
	c.item_total = 2;
	snprintf(c.name, sizeof c.name, "Rumours");

	track_page page;
	memset(&page, 0, sizeof page);
	page.collection = c;
	page.total = 2;
	page.count = 2;
	page.items[0] = item(0, "Dreams", "Fleetwood Mac", "Rumours", "");
	page.items[1] = item(1, "The Chain", "Fleetwood Mac", "Rumours", "");

	searchindex *ix = build(&c, &page, 1, "");

	/* "rumours" is the album name: a live album scan does not match it. */
	track_search_results live, cached;
	track_search_results_init(&live);
	track_search_results_init(&cached);
	assert(track_search_consider_page(&live, &page, "rumours", false));
	assert(searchindex_search(ix, &c, "rumours", &cached));
	assert(live.count == 0 && cached.count == 0);
	track_search_results_free(&live);
	track_search_results_free(&cached);

	/* But the album name is still restored on a row that matches by title. */
	track_search_results r;
	track_search_results_init(&r);
	assert(searchindex_search(ix, &c, "dreams", &r));
	assert(r.count == 1);
	assert(strcmp(r.hits[0].item.album, "Rumours") == 0);
	track_search_results_free(&r);
	searchindex_free(ix);
}

/* A damaged file must read exactly like a missing one, never like a valid
 * index with wrong contents. */
static void test_corruption_is_a_miss(void)
{
	collection_item c = playlist_of("Small", 2);
	track_page page;
	memset(&page, 0, sizeof page);
	page.collection = c;
	page.total = 2;
	page.count = 2;
	page.items[0] = item(0, "One", "A", "B", "");
	page.items[1] = item(1, "Two", "C", "D", "");

	searchindex_builder *b = searchindex_builder_new(&c, "snap", 2);
	unsigned char       *good = NULL;
	size_t               len = 0;
	assert(b && searchindex_builder_add_page(b, &page));
	assert(searchindex_builder_finish(b, &good, &len));
	searchindex_builder_free(b);

	unsigned char *copy = malloc(len);
	assert(copy);

	/* A flipped payload byte fails the crc. */
	memcpy(copy, good, len);
	copy[len - 1] ^= 0xFF;
	assert(searchindex_open(copy, len) == NULL);

	/* Truncation. */
	memcpy(copy, good, len);
	assert(searchindex_open(copy, len - 4) == NULL);

	/* Wrong magic and wrong version. */
	memcpy(copy, good, len);
	copy[0] ^= 0xFF;
	assert(searchindex_open(copy, len) == NULL);
	memcpy(copy, good, len);
	copy[4] = 0xFE;
	assert(searchindex_open(copy, len) == NULL);

	/* Header only, and shorter than a header. */
	memcpy(copy, good, len);
	assert(searchindex_open(copy, 96) == NULL);
	assert(searchindex_open(copy, 8) == NULL);
	assert(searchindex_open(NULL, len) == NULL);

	free(copy);
	free(good);
}

/* Every record must start 4-byte aligned, whatever the text lengths, or the
 * ARM11 walk would perform unaligned loads. */
static void test_alignment_and_bounds(void)
{
	collection_item c = playlist_of("Adversarial", 40);
	track_page page;
	memset(&page, 0, sizeof page);
	page.collection = c;
	page.total = 40;
	page.count = 40;
	char name[130];
	for (int i = 0; i < 40; i++) {
		/* Sweep lengths so every raw size mod 4 occurs. */
		const int n = i + 1;
		memset(name, 'a', (size_t)n);
		name[n] = '\0';
		page.items[i] = item(i, name, "b", "c", "");
	}
	searchindex *ix = build(&c, &page, 1, "");
	assert(searchindex_count(ix) == 40);

	/* Searching walks every record; a misaligned or mis-sized step would
	 * desynchronise and either miss rows or fail the bounds check. */
	track_search_results r;
	track_search_results_init(&r);
	assert(searchindex_search(ix, &c, "a", &r));
	assert(r.matched_total == 40);
	track_search_results_free(&r);
	searchindex_free(ix);

	/* A collection larger than the u16 source_index cannot be indexed. */
	collection_item huge = playlist_of("Huge", SEARCHINDEX_TRACKS_MAX + 1);
	assert(searchindex_builder_new(&huge, "", SEARCHINDEX_TRACKS_MAX + 1) ==
	       NULL);

	/* Nothing added means nothing to write. */
	searchindex_builder *empty = searchindex_builder_new(&c, "", 0);
	unsigned char       *blob = NULL;
	size_t               len = 0;
	assert(empty && !searchindex_builder_finish(empty, &blob, &len));
	searchindex_builder_free(empty);
}

int main(void)
{
	test_round_trip();
	test_equivalence_with_live_scan();
	test_album_collection();
	test_corruption_is_a_miss();
	test_alignment_and_bounds();
	puts("search index: round-trip, live equivalence, albums, corruption, and "
	     "alignment passed");
	return 0;
}
