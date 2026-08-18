#include "searchindex.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEARCHINDEX_MAGIC 0x53334900u /* "\0I3S" */

/* Art urls are the one field big enough to matter: a raw track_item reserves
 * 256 bytes for something that is, in practice, a fixed host plus a 40-digit
 * hex id. Measured against a real library (328 cached covers, 65 playlists):
 * every track cover was exactly 40 hex characters, and collection covers were
 * either that or a 160-character mosaic of four such ids. So the host becomes
 * an enum and the hex is packed two digits to the byte - 40 characters in 20
 * bytes - with anything unrecognised stored verbatim rather than rejected. */
typedef enum {
	ART_HOST_NONE = 0,
	ART_HOST_LITERAL,   /* stored as-is; covers hosts we have not seen */
	ART_HOST_SCDN,      /* https://i.scdn.co/image/ */
	ART_HOST_MOSAIC,    /* https://mosaic.scdn.co/60/ */
	ART_HOST_CDN_FA,    /* https://image-cdn-fa.spotifycdn.com/image/ */
	ART_HOST_CDN_AK,    /* https://image-cdn-ak.spotifycdn.com/image/ */
} art_host;

static const char *const s_art_prefix[] = {
	[ART_HOST_NONE] = "",
	[ART_HOST_LITERAL] = "",
	[ART_HOST_SCDN] = "https://i.scdn.co/image/",
	[ART_HOST_MOSAIC] = "https://mosaic.scdn.co/60/",
	[ART_HOST_CDN_FA] = "https://image-cdn-fa.spotifycdn.com/image/",
	[ART_HOST_CDN_AK] = "https://image-cdn-ak.spotifycdn.com/image/",
};

/* Spotify track uris are "spotify:track:" plus a 22-character base-62 id, so
 * only the id is stored. Anything that does not fit that shape keeps its uri
 * verbatim in the text block instead. */
#define TRACK_URI_PREFIX "spotify:track:"
#define TRACK_ID_LEN     22

/* Packed so the on-disk layout is the struct layout. artcache learned this the
 * hard way: leaving it implicit let the compiler insert padding that silently
 * shifted every field after it. */
typedef struct __attribute__((packed)) {
	uint32_t magic;
	uint16_t version;
	uint16_t flags;
	char     snapshot_id[SEARCHINDEX_SNAPSHOT_MAX + 1];
	uint32_t item_total;   /* what Spotify reported for the collection */
	uint32_t record_count; /* records actually packed */
	uint32_t payload_len;
	uint32_t crc32;
	uint32_t built_at;
	uint32_t reserved;
} searchindex_hdr;

/* 4+2+2+64+4+4+4+4+4+4. Asserted rather than assumed: this is the on-disk
 * layout, and a silent change would misread every existing entry. */
_Static_assert(sizeof(searchindex_hdr) == 96,
               "searchindex header layout changed - bump SEARCHINDEX_VERSION");

/* Record header. The text block follows immediately, in the order
 * name/artist/album/uri-or-art, then padding to the next 4-byte boundary so
 * the walk never performs an unaligned load. */
typedef struct __attribute__((packed)) {
	uint16_t      source_index;
	unsigned char flags;      /* bit0 playable, bit1 is_local, bit2 explicit */
	unsigned char art_host;   /* art_host enum */
	unsigned char name_len;
	unsigned char artist_len;
	unsigned char album_len;
	unsigned char art_len;    /* bytes stored, after any prefix/hex packing */
	unsigned char id_len;     /* TRACK_ID_LEN, or 0 when the uri is literal */
	unsigned char uri_len;    /* literal uri length when id_len is 0 */
} record_hdr;

_Static_assert(sizeof(record_hdr) == 10, "record layout changed - bump version");
_Static_assert(offsetof(searchindex_hdr, snapshot_id) ==
                   SEARCHINDEX_SNAPSHOT_OFFSET,
               "SEARCHINDEX_SNAPSHOT_OFFSET no longer matches the header");

#define REC_FLAG_PLAYABLE 0x01
#define REC_FLAG_LOCAL    0x02
#define REC_FLAG_EXPLICIT 0x04

struct searchindex {
	unsigned char *blob;
	size_t         len;
	const unsigned char *payload;
	size_t               payload_len;
	int                  count;
	int                  item_total;
	/* Whether the album column was stored. Taken from the file rather than
	 * re-derived from the caller's collection, so an empty album on a
	 * playlist track stays empty instead of inheriting the playlist name. */
	bool                 match_album;
	char                 snapshot[SEARCHINDEX_SNAPSHOT_MAX + 1];
};

struct searchindex_builder {
	unsigned char *buf;
	size_t         len;
	size_t         cap;
	int            count;
	int            item_total;
	bool           failed;
	bool           match_album;
	char           snapshot[SEARCHINDEX_SNAPSHOT_MAX + 1];
};

/* --- crc32 (reflected, poly 0xEDB88320), matching artcache ------------- */

static uint32_t s_crc_table[256];
static bool     s_crc_ready;

static void crc_init(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++)
			c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		s_crc_table[i] = c;
	}
	s_crc_ready = true;
}

static uint32_t crc32_of(const unsigned char *data, size_t len)
{
	if (!s_crc_ready)
		crc_init();
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < len; i++)
		c = s_crc_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

/* --- hex packing -------------------------------------------------------- */

static int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

static bool all_hex(const char *s, size_t len)
{
	if (!len || (len & 1))
		return false;
	for (size_t i = 0; i < len; i++)
		if (hex_value(s[i]) < 0)
			return false;
	return true;
}

static void hex_pack(const char *src, size_t len, unsigned char *dst)
{
	for (size_t i = 0; i < len; i += 2)
		dst[i / 2] = (unsigned char)((hex_value(src[i]) << 4) |
		                             hex_value(src[i + 1]));
}

static void hex_unpack(const unsigned char *src, size_t bytes, char *dst)
{
	static const char digits[] = "0123456789abcdef";
	for (size_t i = 0; i < bytes; i++) {
		dst[i * 2] = digits[src[i] >> 4];
		dst[i * 2 + 1] = digits[src[i] & 0x0F];
	}
}

/* Split an art url into a known host and its hex tail. Returns ART_HOST_NONE
 * for an empty url, or ART_HOST_LITERAL when the tail is not packable hex. */
static art_host art_split(const char *url, const char **tail, size_t *taillen)
{
	*tail = url;
	*taillen = url ? strlen(url) : 0;
	if (!url || !url[0])
		return ART_HOST_NONE;
	for (int h = ART_HOST_SCDN; h <= ART_HOST_CDN_AK; h++) {
		const size_t plen = strlen(s_art_prefix[h]);
		if (strncmp(url, s_art_prefix[h], plen) != 0)
			continue;
		const char  *rest = url + plen;
		const size_t rlen = strlen(rest);
		if (!all_hex(rest, rlen))
			break; /* known host, odd tail - keep it literal */
		*tail = rest;
		*taillen = rlen;
		return (art_host)h;
	}
	return ART_HOST_LITERAL;
}

/* --- builder ------------------------------------------------------------ */

searchindex_builder *searchindex_builder_new(const collection_item *collection,
                                             const char *snapshot_id,
                                             int item_total)
{
	if (!collection || item_total > SEARCHINDEX_TRACKS_MAX)
		return NULL;
	searchindex_builder *b = calloc(1, sizeof *b);
	if (!b)
		return NULL;
	b->item_total = item_total;
	/* Album collections give every row the same album name, and hit_rank
	 * skips album matching for them, so that column is not stored. */
	b->match_album = collection->kind == COLLECTION_PLAYLIST;
	if (snapshot_id)
		snprintf(b->snapshot, sizeof b->snapshot, "%.*s",
		         SEARCHINDEX_SNAPSHOT_MAX, snapshot_id);

	const size_t guess = (size_t)(item_total > 0 ? item_total : 64) * 136 + 4096;
	b->cap = guess < SEARCHINDEX_BYTES_MAX ? guess : SEARCHINDEX_BYTES_MAX;
	b->buf = malloc(b->cap);
	if (!b->buf) {
		free(b);
		return NULL;
	}
	return b;
}

void searchindex_builder_free(searchindex_builder *b)
{
	if (!b)
		return;
	free(b->buf);
	free(b);
}

static bool builder_reserve(searchindex_builder *b, size_t extra)
{
	if (b->len + extra <= b->cap)
		return true;
	size_t want = b->cap + b->cap / 2;
	while (want < b->len + extra)
		want += want / 2;
	if (want > SEARCHINDEX_BYTES_MAX)
		return false;
	unsigned char *grown = realloc(b->buf, want);
	if (!grown)
		return false;
	b->buf = grown;
	b->cap = want;
	return true;
}

static size_t clamp255(size_t n) { return n > 255 ? 255 : n; }

bool searchindex_builder_add_page(searchindex_builder *b,
                                  const track_page *page)
{
	if (!b || b->failed || !page)
		return false;
	for (int i = 0; i < page->count; i++) {
		const track_item *it = &page->items[i];
		if (it->kind != TRACK_ITEM_TRACK)
			continue;
		if (it->source_index < 0 || it->source_index > SEARCHINDEX_TRACKS_MAX) {
			b->failed = true;
			return false;
		}

		const char  *art_tail = NULL;
		size_t       art_tail_len = 0;
		const art_host host = art_split(it->art_url, &art_tail, &art_tail_len);
		const bool   packed_art = host >= ART_HOST_SCDN;
		const size_t art_stored =
		    packed_art ? art_tail_len / 2 : clamp255(art_tail_len);

		const bool has_id =
		    strncmp(it->uri, TRACK_URI_PREFIX, sizeof TRACK_URI_PREFIX - 1) == 0 &&
		    strlen(it->uri) == sizeof TRACK_URI_PREFIX - 1 + TRACK_ID_LEN;
		const size_t uri_stored = has_id ? 0 : clamp255(strlen(it->uri));

		const size_t name_len = clamp255(strlen(it->name));
		const size_t artist_len = clamp255(strlen(it->artist));
		const size_t album_len =
		    b->match_album ? clamp255(strlen(it->album)) : 0;

		const size_t body = name_len + artist_len + album_len + art_stored +
		                    (has_id ? TRACK_ID_LEN : uri_stored);
		const size_t raw = sizeof(record_hdr) + body;
		const size_t padded = (raw + 3u) & ~(size_t)3u;
		if (!builder_reserve(b, padded)) {
			b->failed = true;
			return false;
		}

		record_hdr rh;
		memset(&rh, 0, sizeof rh);
		rh.source_index = (uint16_t)it->source_index;
		rh.flags = (unsigned char)((it->playable ? REC_FLAG_PLAYABLE : 0) |
		                           (it->is_local ? REC_FLAG_LOCAL : 0) |
		                           (it->explicit_content ? REC_FLAG_EXPLICIT : 0));
		rh.art_host = (unsigned char)host;
		rh.name_len = (unsigned char)name_len;
		rh.artist_len = (unsigned char)artist_len;
		rh.album_len = (unsigned char)album_len;
		rh.art_len = (unsigned char)art_stored;
		rh.id_len = has_id ? TRACK_ID_LEN : 0;
		rh.uri_len = (unsigned char)uri_stored;

		unsigned char *at = b->buf + b->len;
		memcpy(at, &rh, sizeof rh);
		at += sizeof rh;
		memcpy(at, it->name, name_len);
		at += name_len;
		memcpy(at, it->artist, artist_len);
		at += artist_len;
		memcpy(at, it->album, album_len);
		at += album_len;
		if (has_id) {
			memcpy(at, it->uri + sizeof TRACK_URI_PREFIX - 1, TRACK_ID_LEN);
			at += TRACK_ID_LEN;
		} else {
			memcpy(at, it->uri, uri_stored);
			at += uri_stored;
		}
		if (packed_art) {
			hex_pack(art_tail, art_stored * 2, at);
			at += art_stored;
		} else {
			memcpy(at, art_tail, art_stored);
			at += art_stored;
		}
		/* Pad deterministically: the bytes are covered by the crc. */
		const size_t pad = padded - raw;
		if (pad)
			memset(at, 0, pad);

		b->len += padded;
		b->count++;
	}
	return true;
}

int searchindex_builder_count(const searchindex_builder *b)
{
	return b ? b->count : 0;
}

bool searchindex_builder_finish(searchindex_builder *b, unsigned char **out,
                                size_t *outlen)
{
	if (!b || !out || !outlen || b->failed || b->count <= 0)
		return false;
	const size_t total = sizeof(searchindex_hdr) + b->len;
	if (total > SEARCHINDEX_BYTES_MAX)
		return false;

	/* One buffer, so the caller can write header and payload in a single
	 * aligned pass - splitting them cost 1716ms against 60ms in artcache. */
	unsigned char *blob = malloc(total);
	if (!blob)
		return false;

	searchindex_hdr hdr;
	memset(&hdr, 0, sizeof hdr);
	hdr.magic = SEARCHINDEX_MAGIC;
	hdr.version = SEARCHINDEX_VERSION;
	hdr.flags = b->match_album ? 1u : 0u;
	snprintf(hdr.snapshot_id, sizeof hdr.snapshot_id, "%s", b->snapshot);
	hdr.item_total = (uint32_t)(b->item_total > 0 ? b->item_total : b->count);
	hdr.record_count = (uint32_t)b->count;
	hdr.payload_len = (uint32_t)b->len;
	hdr.crc32 = 0;

	/* Cover the header as well as the payload. Leaving it out would let the
	 * snapshot id - the field the whole freshness check rests on - be altered
	 * without detection, turning a damaged entry into a valid-looking index
	 * that claims the wrong version of the playlist. */
	memcpy(blob, &hdr, sizeof hdr);
	memcpy(blob + sizeof hdr, b->buf, b->len);
	hdr.crc32 = crc32_of(blob, total);
	memcpy(blob, &hdr, sizeof hdr);
	*out = blob;
	*outlen = total;
	return true;
}

/* --- reading ------------------------------------------------------------ */

searchindex *searchindex_open(unsigned char *blob, size_t len)
{
	if (!blob || len < sizeof(searchindex_hdr))
		return NULL;
	searchindex_hdr hdr;
	memcpy(&hdr, blob, sizeof hdr);
	if (hdr.magic != SEARCHINDEX_MAGIC || hdr.version != SEARCHINDEX_VERSION)
		return NULL;
	if (hdr.payload_len != len - sizeof(searchindex_hdr))
		return NULL;
	if (hdr.record_count == 0 || hdr.record_count > SEARCHINDEX_TRACKS_MAX)
		return NULL;
	/* Recompute over the whole blob with the crc field zeroed, matching how
	 * it was written. */
	const uint32_t want_crc = hdr.crc32;
	searchindex_hdr probe = hdr;
	probe.crc32 = 0;
	unsigned char saved[sizeof(searchindex_hdr)];
	memcpy(saved, blob, sizeof saved);
	memcpy(blob, &probe, sizeof probe);
	const uint32_t got_crc = crc32_of(blob, len);
	memcpy(blob, saved, sizeof saved);
	if (got_crc != want_crc)
		return NULL;

	searchindex *ix = calloc(1, sizeof *ix);
	if (!ix)
		return NULL;
	ix->blob = blob;
	ix->len = len;
	ix->payload = blob + sizeof(searchindex_hdr);
	ix->payload_len = hdr.payload_len;
	ix->count = (int)hdr.record_count;
	ix->item_total = (int)hdr.item_total;
	ix->match_album = (hdr.flags & 1u) != 0;
	memcpy(ix->snapshot, hdr.snapshot_id, sizeof ix->snapshot);
	ix->snapshot[SEARCHINDEX_SNAPSHOT_MAX] = '\0';
	return ix;
}

void searchindex_free(searchindex *ix)
{
	if (!ix)
		return;
	free(ix->blob);
	free(ix);
}

const char *searchindex_snapshot(const searchindex *ix)
{
	return ix ? ix->snapshot : "";
}

const unsigned char *searchindex_blob(const searchindex *ix)
{
	return ix ? ix->blob : NULL;
}

int searchindex_count(const searchindex *ix) { return ix ? ix->count : 0; }

int searchindex_item_total(const searchindex *ix)
{
	return ix ? ix->item_total : -1;
}

size_t searchindex_bytes(const searchindex *ix) { return ix ? ix->len : 0; }

bool searchindex_age_file_for_test(const char *path)
{
	FILE *f = fopen(path, "r+b");
	if (!f)
		return false;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return false;
	}
	const long size = ftell(f);
	if (size <= (long)sizeof(searchindex_hdr) ||
	    (size_t)size > SEARCHINDEX_BYTES_MAX || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return false;
	}
	unsigned char *blob = malloc((size_t)size);
	if (!blob) {
		fclose(f);
		return false;
	}
	const bool read_ok = fread(blob, 1, (size_t)size, f) == (size_t)size;
	if (!read_ok) {
		free(blob);
		fclose(f);
		return false;
	}

	searchindex_hdr hdr;
	memcpy(&hdr, blob, sizeof hdr);
	/* Any value the real playlist cannot hold. */
	snprintf(hdr.snapshot_id, sizeof hdr.snapshot_id, "aged-for-test");
	hdr.crc32 = 0;
	memcpy(blob, &hdr, sizeof hdr);
	hdr.crc32 = crc32_of(blob, (size_t)size);
	memcpy(blob, &hdr, sizeof hdr);

	const bool ok = fseek(f, 0, SEEK_SET) == 0 &&
	                fwrite(blob, 1, (size_t)size, f) == (size_t)size;
	fflush(f);
	fclose(f);
	free(blob);
	return ok;
}

bool searchindex_search(const searchindex *ix,
                        const collection_item *collection, const char *query,
                        track_search_results *results)
{
	if (!ix || !collection || !query || !query[0] || !results)
		return false;
	const size_t querylen = strlen(query);
	const bool hdr_match_album = ix->match_album;
	const bool match_album = hdr_match_album;
	if (ix->count > results->source_total)
		results->source_total = ix->count;

	size_t at = 0;
	for (int seen = 0; seen < ix->count; seen++) {
		if (at + sizeof(record_hdr) > ix->payload_len)
			return false;
		record_hdr rh;
		memcpy(&rh, ix->payload + at, sizeof rh);
		const size_t body = (size_t)rh.name_len + rh.artist_len + rh.album_len +
		                    rh.art_len + (rh.id_len ? rh.id_len : rh.uri_len);
		const size_t raw = sizeof(record_hdr) + body;
		const size_t padded = (raw + 3u) & ~(size_t)3u;
		if (at + raw > ix->payload_len)
			return false;

		/* Every length is a u8 and can therefore claim up to 255, while the
		 * fields they are copied into are smaller. Checking only that the
		 * bytes exist in the blob is not enough: this file comes off an SD
		 * card and a tamperer can recompute the crc, so the lengths have to
		 * be checked against their destinations too. Reject the whole index
		 * rather than clamping, so a damaged entry behaves like the miss the
		 * header promises. */
		if (rh.name_len >= sizeof ((track_item *)0)->name ||
		    rh.artist_len >= sizeof ((track_item *)0)->artist ||
		    rh.album_len >= sizeof ((track_item *)0)->album ||
		    rh.uri_len >= sizeof ((track_item *)0)->uri ||
		    rh.art_host > ART_HOST_CDN_AK ||
		    (rh.id_len && rh.id_len != TRACK_ID_LEN))
			return false;

		const char *text = (const char *)(ix->payload + at + sizeof(record_hdr));
		const char *name = text;
		const char *artist = name + rh.name_len;
		const char *album = artist + rh.artist_len;
		const char *idp = album + rh.album_len;
		const unsigned char *artp =
		    (const unsigned char *)idp + (rh.id_len ? rh.id_len : rh.uri_len);

		/* An album collection stores no album column; ranking skips it, so
		 * pass a zero length rather than the collection name. */
		const int rank = track_search_rank_fields(
		    name, rh.name_len, artist, rh.artist_len, album, rh.album_len,
		    query, querylen, match_album);
		if (rank >= 0) {
			track_item item;
			memset(&item, 0, sizeof item);
			item.kind = TRACK_ITEM_TRACK;
			item.source_index = rh.source_index;
			item.playable = (rh.flags & REC_FLAG_PLAYABLE) != 0;
			item.is_local = (rh.flags & REC_FLAG_LOCAL) != 0;
			item.explicit_content = (rh.flags & REC_FLAG_EXPLICIT) != 0;
			/* The lengths were validated against these fields above; the
			 * %.*s forms restate the bound so the copies stay safe on their
			 * own terms. */
			snprintf(item.name, sizeof item.name, "%.*s", (int)rh.name_len,
			         name);
			snprintf(item.artist, sizeof item.artist, "%.*s",
			         (int)rh.artist_len, artist);
			if (rh.album_len)
				snprintf(item.album, sizeof item.album, "%.*s",
				         (int)rh.album_len, album);
			else if (!hdr_match_album)
				/* An album collection does not store the column, because
				 * every row repeats the collection name. A playlist track
				 * whose album really is empty keeps it empty. */
				snprintf(item.album, sizeof item.album, "%s",
				         collection->name);
			if (rh.id_len)
				snprintf(item.uri, sizeof item.uri, "%s%.*s", TRACK_URI_PREFIX,
				         (int)rh.id_len, idp);
			else
				snprintf(item.uri, sizeof item.uri, "%.*s", (int)rh.uri_len,
				         idp);

			if (rh.art_host >= ART_HOST_SCDN) {
				char hex[256];
				const size_t chars = (size_t)rh.art_len * 2;
				if (chars < sizeof hex) {
					hex_unpack(artp, rh.art_len, hex);
					hex[chars] = '\0';
					snprintf(item.art_url, sizeof item.art_url, "%s%s",
					         s_art_prefix[rh.art_host], hex);
				}
			} else if (rh.art_host == ART_HOST_LITERAL && rh.art_len) {
				const size_t n = rh.art_len < sizeof item.art_url - 1
				                     ? rh.art_len
				                     : sizeof item.art_url - 1;
				memcpy(item.art_url, artp, n);
			}

			if (!track_search_consider_hit(results, &item, rank))
				return false;
		}
		at += padded;
	}
	results->truncated = results->matched_total > results->count;
	return true;
}
