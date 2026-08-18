#include "searchcache.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../testlog.h"

#define CACHE_ROOT "sdmc:/spotify/searchidx"
#define MANIFEST   CACHE_ROOT "/index.txt"

/* Playlist ids are 22 base-62 characters; the manifest line adds a timestamp
 * and separators. Generous, since a malformed line is skipped rather than
 * trusted. */
#define LINE_MAX 160

/* One write failure disables the store for the session, as artcache does: a
 * full or read-only card should degrade to "no cache", not retry on every
 * search and stall the worker each time. */
static bool s_writes_disabled;

typedef struct {
	long when; /* unix seconds of the last write */
	char id[40];
} entry;

/* The manifest is a hint about what is on the card, not an authority on its
 * contents: entries are validated by their own header and crc when opened, so
 * a row whose file was deleted by hand simply reads as a miss. */
static entry s_entries[SEARCHCACHE_MAX_ENTRIES];
static int   s_count;
static bool  s_loaded;
/* Highest stamp seen on the card, so a write cannot look older than rows that
 * are already there when the console's clock disagrees. */
static long  s_newest;

/* The 3DS clock can be unset or wound backwards, so this is only ever a hint
 * for ordering. A monotonic floor keeps a fresh write from looking older than
 * rows already on the card, which would otherwise make it the next thing
 * evicted despite being the most recently used. */
static long now_seconds(void)
{
	const time_t t = time(NULL);
	long now = t == (time_t)-1 ? 0 : (long)t;
	if (now < 0)
		now = 0;
	if (now <= s_newest)
		now = s_newest + 1;
	s_newest = now;
	return now;
}

/* Playlist ids are base-62, so the hex-only artcache helper cannot be reused;
 * this only has to reject anything that could escape the directory. */
static bool safe_id(const char *id)
{
	if (!id || !id[0])
		return false;
	int n = 0;
	for (; id[n]; n++) {
		const char c = id[n];
		const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
		                (c >= 'A' && c <= 'Z');
		if (!ok)
			return false;
	}
	return n <= 32;
}

/* Only playlists are stored; albums have no snapshot to validate against. */
static bool playlist_id(const char *context_uri, char *out, int outlen)
{
	if (!context_uri ||
	    strncmp(context_uri, "spotify:playlist:", 17) != 0)
		return false;
	snprintf(out, outlen, "%s", context_uri + 17);
	return safe_id(out);
}

static void entry_path(const char *id, char *out, int outlen)
{
	snprintf(out, outlen, "%s/%s.s3i", CACHE_ROOT, id);
}

static void manifest_load(void)
{
	if (s_loaded)
		return;
	s_loaded = true;
	s_count = 0;

	FILE *f = fopen(MANIFEST, "r");
	if (!f)
		return;
	char line[LINE_MAX];
	while (s_count < SEARCHCACHE_MAX_ENTRIES && fgets(line, sizeof line, f)) {
		long when = 0;
		char id[40] = "";
		if (sscanf(line, "%ld %39s", &when, id) != 2)
			continue; /* a mangled row is dropped, not trusted */
		if (!safe_id(id))
			continue;
		/* A negative stamp cannot come from the clock, only from a damaged or
		 * hand-edited row, and it would make this entry the eviction victim
		 * every time. Treat it as unknown-age rather than infinitely old. */
		if (when < 0)
			when = 0;
		s_entries[s_count].when = when;
		snprintf(s_entries[s_count].id, sizeof s_entries[s_count].id, "%s", id);
		s_count++;
		if (when > s_newest)
			s_newest = when;
	}
	fclose(f);
}

static void manifest_save(void)
{
	if (s_writes_disabled)
		return;
	mkdir(CACHE_ROOT, 0777);
	FILE *f = fopen(MANIFEST, "w");
	if (!f) {
		s_writes_disabled = true;
		tl_log("searchcache: cannot write manifest, caching disabled");
		return;
	}
	for (int i = 0; i < s_count; i++)
		fprintf(f, "%ld %s\n", s_entries[i].when, s_entries[i].id);
	fclose(f);
}

static int manifest_find(const char *id)
{
	for (int i = 0; i < s_count; i++)
		if (strcmp(s_entries[i].id, id) == 0)
			return i;
	return -1;
}

static void manifest_drop(int index)
{
	if (index < 0 || index >= s_count)
		return;
	memmove(&s_entries[index], &s_entries[index + 1],
	        (size_t)(s_count - index - 1) * sizeof *s_entries);
	s_count--;
}

/* Evict the least recently written entry. Recency is tracked by write rather
 * than by read: touching the manifest on every hit would put a ~140ms write on
 * the fast path this cache exists to create. With room for a hundred playlists
 * and a library of dozens, this is a backstop that rarely runs. */
static void evict_oldest(void)
{
	if (s_count <= 0)
		return;
	int oldest = 0;
	for (int i = 1; i < s_count; i++)
		if (s_entries[i].when < s_entries[oldest].when)
			oldest = i;
	char path[128];
	entry_path(s_entries[oldest].id, path, sizeof path);
	remove(path);
	tl_log("searchcache: evicted %s", s_entries[oldest].id);
	manifest_drop(oldest);
	/* Caller saves the manifest as part of the store that triggered this. */
}

/* Delete an entry and forget it, so a discarded file cannot leave a row
 * behind. A phantom row would still count against the entry limit and could
 * absorb an eviction that should have freed a real file. */
static void drop_entry(const char *id, const char *path)
{
	remove(path);
	manifest_load();
	const int at = manifest_find(id);
	if (at >= 0) {
		manifest_drop(at);
		manifest_save();
	}
}

searchindex *searchcache_load(const char *context_uri)
{
	char id[40];
	if (!playlist_id(context_uri, id, sizeof id))
		return NULL;

	char path[128];
	entry_path(id, path, sizeof path);
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;

	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	const long size = ftell(f);
	if (size <= 0 || (size_t)size > SEARCHINDEX_BYTES_MAX ||
	    fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		drop_entry(id, path);
		return NULL;
	}

	unsigned char *blob = malloc((size_t)size);
	if (!blob) {
		fclose(f);
		return NULL;
	}
	const size_t got = fread(blob, 1, (size_t)size, f);
	fclose(f);
	if (got != (size_t)size) {
		free(blob);
		drop_entry(id, path);
		return NULL;
	}

	/* searchindex_open validates magic, version, lengths and crc, so anything
	 * damaged reads exactly like a missing file. Delete it so the next search
	 * rebuilds rather than retrying a broken entry forever. */
	searchindex *ix = searchindex_open(blob, (size_t)got);
	if (!ix) {
		free(blob);
		drop_entry(id, path);
		tl_log("searchcache: discarded unreadable entry %s", id);
		return NULL;
	}
	return ix;
}

void searchcache_store(const char *context_uri, const unsigned char *blob,
                       size_t len)
{
	if (s_writes_disabled || !blob || !len || len > SEARCHINDEX_BYTES_MAX)
		return;
	/* An entry carrying no version can never be checked against Spotify, and
	 * a search reading one later would have to guess. Refuse it here as well
	 * as at the call site, so a future caller cannot reintroduce it. The
	 * snapshot sits at a fixed offset in the header, so this needs no
	 * parsing. */
	if (len <= SEARCHINDEX_SNAPSHOT_OFFSET ||
	    blob[SEARCHINDEX_SNAPSHOT_OFFSET] == '\0')
		return;
	char id[40];
	if (!playlist_id(context_uri, id, sizeof id))
		return;

	manifest_load();
	mkdir(CACHE_ROOT, 0777);

	const int existing = manifest_find(id);
	if (existing < 0 && s_count >= SEARCHCACHE_MAX_ENTRIES)
		evict_oldest();

	char path[128], tmp[128];
	entry_path(id, path, sizeof path);
	snprintf(tmp, sizeof tmp, "%s/%s.tmp", CACHE_ROOT, id);

	/* One write for header and payload together. artcache measured 1716ms
	 * against 60ms for the same bytes split across two calls, because the FS
	 * layer turned the second into a read-modify-write per block. */
	FILE *f = fopen(tmp, "wb");
	if (!f) {
		s_writes_disabled = true;
		tl_log("searchcache: cannot open %s, caching disabled", tmp);
		return;
	}
	const size_t wrote = fwrite(blob, 1, len, f);
	fflush(f);
	fclose(f);
	if (wrote != len) {
		remove(tmp);
		s_writes_disabled = true;
		tl_log("searchcache: short write for %s, caching disabled", id);
		return;
	}
	if (rename(tmp, path) != 0) {
		/* rename does not replace on this filesystem. */
		remove(path);
		if (rename(tmp, path) != 0) {
			remove(tmp);
			tl_log("searchcache: cannot install %s", id);
			return;
		}
	}

	const int slot = existing >= 0 ? existing : s_count++;
	s_entries[slot].when = now_seconds();
	snprintf(s_entries[slot].id, sizeof s_entries[slot].id, "%s", id);
	manifest_save();
	tl_log("searchcache: stored %s (%u bytes)", id, (unsigned)len);
}

bool searchcache_has(const char *context_uri)
{
	char id[40];
	if (!playlist_id(context_uri, id, sizeof id))
		return false;
	char path[128];
	entry_path(id, path, sizeof path);
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	fclose(f);
	return true;
}

void searchcache_evict(const char *context_uri)
{
	char id[40];
	if (!playlist_id(context_uri, id, sizeof id))
		return;
	char path[128];
	entry_path(id, path, sizeof path);
	remove(path);
	manifest_load();
	const int at = manifest_find(id);
	if (at >= 0) {
		manifest_drop(at);
		manifest_save();
	}
}
