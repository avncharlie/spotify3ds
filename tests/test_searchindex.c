#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
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

/* Same reflected crc32 the format uses, so a test can repair a tampered
 * blob exactly as someone editing the file would. */
static void repair_crc(unsigned char *blob, size_t len)
{
	static uint32_t tbl[256];
	static bool ready;
	if (!ready) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			tbl[i] = c;
		}
		ready = true;
	}
	const uint32_t zero = 0;
	memcpy(blob + 84, &zero, 4);
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < len; i++)
		c = tbl[(c ^ blob[i]) & 0xFF] ^ (c >> 8);
	c ^= 0xFFFFFFFFu;
	memcpy(blob + 84, &c, 4);
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
	/* What Spotify said the collection held, which validation compares
	 * against a fresh report. Distinct from the record count, since episodes
	 * and local files are skipped when packing. */
	assert(searchindex_item_total(ix) == 6);

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

/* A playlist track whose album name is genuinely empty - local files and some
 * unavailable tracks come back that way - must stay empty when served from the
 * index. Substituting the collection name there is only correct for album
 * collections, which do not store the column at all. */
static void test_empty_album_on_playlist(void)
{
	collection_item c = playlist_of("MyPlaylist", 1);
	track_page page;
	memset(&page, 0, sizeof page);
	page.collection = c;
	page.total = 1;
	page.count = 1;
	page.items[0] = item(0, "Local Song", "Someone", "", "");

	searchindex *ix = build(&c, &page, 1, "snap");
	track_search_results live, cached;
	track_search_results_init(&live);
	track_search_results_init(&cached);
	assert(track_search_consider_page(&live, &page, "local", true));
	assert(searchindex_search(ix, &c, "local", &cached));
	assert(live.count == 1 && cached.count == 1);
	/* The live scan leaves it empty; the cached one must agree. */
	assert(live.hits[0].item.album[0] == '\0');
	assert(strcmp(cached.hits[0].item.album, live.hits[0].item.album) == 0);
	track_search_results_free(&live);
	track_search_results_free(&cached);
	searchindex_free(ix);
}

/* Episodes are not packed, so the record count is lower than what Spotify
 * reported. Validation must compare against the reported total, or a playlist
 * holding one would look changed on every single search. */
static void test_item_total_counts_skipped_rows(void)
{
	collection_item c = playlist_of("Mixed", 3);
	track_page page;
	memset(&page, 0, sizeof page);
	page.collection = c;
	page.total = 3;
	page.count = 3;
	page.items[0] = item(0, "Song", "Artist", "Album", "");
	page.items[1] = item(1, "An Episode", "Host", "Show", "");
	page.items[1].kind = TRACK_ITEM_EPISODE;
	page.items[2] = item(2, "Another", "Artist", "Album", "");

	searchindex *ix = build(&c, &page, 1, "snap");
	assert(searchindex_count(ix) == 2);      /* episode skipped */
	assert(searchindex_item_total(ix) == 3); /* what Spotify reported */
	searchindex_free(ix);
}

/* The count the builder is created with comes from the library listing, which
 * can predate an edit by the time a search runs. The pages carry the real
 * figure, and the index must be stamped with that - otherwise validation
 * compares a fresh count against a stale one and concludes wrongly. */
static void test_item_total_follows_the_pages(void)
{
	collection_item c = playlist_of("Grew", 1); /* listing says 1 */
	track_page page;
	memset(&page, 0, sizeof page);
	page.collection = c;
	page.total = 3; /* the pages say 3 */
	page.count = 3;
	for (int i = 0; i < 3; i++)
		page.items[i] = item(i, "Song", "Artist", "Album", "");

	searchindex_builder *b = searchindex_builder_new(&c, "snap", c.item_total);
	assert(b);
	searchindex_builder_set_item_total(b, page.total);
	assert(searchindex_builder_add_page(b, &page));
	unsigned char *blob = NULL;
	size_t         len = 0;
	assert(searchindex_builder_finish(b, &blob, &len));
	searchindex_builder_free(b);

	searchindex *ix = searchindex_open(blob, len);
	assert(ix);
	assert(searchindex_item_total(ix) == 3); /* not the stale 1 */
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

	/* A corrupted header must be caught too. The snapshot id lives here and
	 * is what the freshness check compares, so leaving the header outside the
	 * crc would let a damaged entry pass as a valid index claiming the wrong
	 * version of the playlist. */
	for (size_t off = 4; off < 92; off += 7) {
		memcpy(copy, good, len);
		copy[off] ^= 0x40;
		assert(searchindex_open(copy, len) == NULL);
	}
	/* Specifically the snapshot field at offset 8. */
	memcpy(copy, good, len);
	copy[8] = '!';
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

/* A tampered file can claim field lengths larger than the fields they are
 * copied into. The crc does not help - anyone editing the file can recompute
 * it - so the lengths must be checked against their destinations. */
static void test_oversized_lengths_rejected(void)
{
	collection_item c = playlist_of("Small", 1);
	track_page page;
	memset(&page, 0, sizeof page);
	page.collection = c;
	page.total = 1;
	page.count = 1;
	page.items[0] = item(0, "AAAA", "BB", "CC", "");

	searchindex_builder *b = searchindex_builder_new(&c, "snap", 1);
	unsigned char       *blob = NULL;
	size_t               len = 0;
	assert(b && searchindex_builder_add_page(b, &page));
	assert(searchindex_builder_finish(b, &blob, &len));
	searchindex_builder_free(b);

	/* Room behind the claimed lengths, so only the field bound can reject. */
	const size_t big_len = len + 512;
	unsigned char *big = calloc(1, big_len);
	assert(big);
	memcpy(big, blob, len);
	memset(big + len, 'Z', 512);
	free(blob);

	const size_t hdr = 96, rec = 96;
	uint32_t plen = (uint32_t)(big_len - hdr);
	memcpy(big + 80, &plen, 4);

	/* name_len at record offset 4; 200 overflows track_item.name[128]. */
	big[rec + 4] = 200;
	repair_crc(big, big_len);

	searchindex *ix = searchindex_open(big, big_len);
	if (ix) {
		/* Opening may succeed - the walk is what must refuse. */
		track_search_results r;
		track_search_results_init(&r);
		assert(!searchindex_search(ix, &c, "A", &r));
		assert(r.count == 0);
		track_search_results_free(&r);
		searchindex_free(ix);
	} else {
		free(big);
	}
}

/* The format is read off an SD card, so it has to survive arbitrary damage.
 * Mutating a known-good blob and repairing the crc half the time exercises
 * both the cheap header checks and the per-record validation behind them;
 * the point is that nothing here may read outside the blob. Run under the
 * address and undefined sanitizers, which is what actually catches it. */
static void test_fuzz_corrupt_blobs(void)
{
	collection_item c = playlist_of("Fuzz", 8);
	track_page page;
	memset(&page, 0, sizeof page);
	page.collection = c;
	page.total = 8;
	page.count = 8;
	char art[41];
	for (int i = 0; i < 8; i++) {
		char full[64];
		snprintf(full, sizeof full, "%s%s", ART_SCDN, hex40(i, art));
		page.items[i] = item(i, "Song", "Artist", "Album", full);
	}

	searchindex_builder *b = searchindex_builder_new(&c, "snap", 8);
	unsigned char       *good = NULL;
	size_t               len = 0;
	assert(b && searchindex_builder_add_page(b, &page));
	assert(searchindex_builder_finish(b, &good, &len));
	searchindex_builder_free(b);

	unsigned rng = 12345;
	for (int iter = 0; iter < 4000; iter++) {
		unsigned char *copy = malloc(len);
		assert(copy);
		memcpy(copy, good, len);
		const int muts = 1 + (int)((rng = rng * 1103515245u + 12345u) >> 8) % 4;
		for (int m = 0; m < muts; m++) {
			rng = rng * 1103515245u + 12345u;
			const size_t off = (rng >> 8) % len;
			rng = rng * 1103515245u + 12345u;
			copy[off] = (unsigned char)((rng >> 8) & 0xFF);
		}
		rng = rng * 1103515245u + 12345u;
		if ((rng >> 8) & 1)
			repair_crc(copy, len);

		searchindex *ix = searchindex_open(copy, len);
		if (!ix) {
			free(copy);
			continue;
		}
		track_search_results r;
		track_search_results_init(&r);
		searchindex_search(ix, &c, "song", &r);
		track_search_results_free(&r);
		searchindex_free(ix);
	}
	free(good);
}

/* An index built while the metadata request was failing carries no version.
 * It must still be searchable in memory, but it can never be confirmed
 * against Spotify - so nothing may treat a blank version as proof of change,
 * or every later search would rescan the whole collection and never settle. */
static void test_unversioned_index(void)
{
	collection_item c = playlist_of("NoSnapshot", 1);
	track_page page;
	memset(&page, 0, sizeof page);
	page.collection = c;
	page.total = 1;
	page.count = 1;
	page.items[0] = item(0, "Song", "Artist", "Album", "");

	/* Built with an empty snapshot, as the cold path does when
	 * playlist_metadata fails. */
	searchindex *ix = build(&c, &page, 1, "");
	assert(searchindex_snapshot(ix)[0] == '\0');

	/* Still usable for this session. */
	track_search_results r;
	track_search_results_init(&r);
	assert(searchindex_search(ix, &c, "song", &r));
	assert(r.count == 1);
	track_search_results_free(&r);
	searchindex_free(ix);
}

int main(void)
{
	test_round_trip();
	test_equivalence_with_live_scan();
	test_album_collection();
	test_empty_album_on_playlist();
	test_item_total_counts_skipped_rows();
	test_item_total_follows_the_pages();
	test_corruption_is_a_miss();
	test_alignment_and_bounds();
	test_oversized_lengths_rejected();
	test_fuzz_corrupt_blobs();
	test_unversioned_index();
	puts("search index: round-trip, live equivalence, albums, tamper, fuzz, "
	     "and alignment passed");
	return 0;
}
