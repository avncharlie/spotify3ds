#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "net/http.h"
#include "spotify/playlist_meta.h"
#include "spotify/namecache.h"
#include "spotify/recents.h"
#include "spotify/artcache_path.h"
#include "ui/collection_touch.h"

/* Reproduce the target filesystem's refusal to rename over an existing file.
 * Production and this test are compiled with -Drename=test_sd_rename. */
#undef rename
int rename(const char *from, const char *to);
static bool fail_install;
int test_sd_rename(const char *from, const char *to)
{
	FILE *existing = fopen(to, "r");
	if (existing) {
		fclose(existing);
		errno = EEXIST;
		return -1;
	}
	if (fail_install && strstr(from, ".tmp") && strcmp(to, NAMECACHE_PATH) == 0) {
		errno = EIO;
		return -1;
	}
	return rename(from, to);
}

static time_t clock_now = 1787832000;
time_t time(time_t *out)
{
	if (out)
		*out = clock_now;
	return clock_now;
}
uint64_t osGetTime(void) { return (uint64_t)clock_now * 1000; }
void tl_log(const char *format, ...) { (void)format; }
void tl_timing(const char *format, ...) { (void)format; }
const char *auth_token(char *err, int errlen)
{
	(void)err;
	(void)errlen;
	return "test-bearer";
}

typedef struct {
	const char *host, *path, *body;
	int status;
	bool transport;
	long retry;
} reply;
static reply replies[8];
static int sent, reply_count;

static void reset(void)
{
	sent = reply_count = 0;
	playlist_meta_reset_backoff();
}
static void add(const char *host, const char *path, int status, const char *body)
{
	assert(reply_count < 8);
	replies[reply_count++] = (reply){host, path, body, status, true, -1};
}
bool http_request(const char *host, const char *method, const char *path,
                   const char *bearer, const char *ctype, const char *body,
                   http_response *out, char *err, int errlen)
{
	(void)ctype;
	(void)body;
	(void)err;
	(void)errlen;
	assert(sent < reply_count);
	reply r = replies[sent++];
	assert(strcmp(host, r.host) == 0);
	assert(strcmp(method, "GET") == 0);
	assert(strstr(path, r.path));
	if (strcmp(host, "open.spotify.com") == 0)
		assert(bearer == NULL);
	else
		assert(bearer && strcmp(bearer, "test-bearer") == 0);
	memset(out, 0, sizeof *out);
	if (!r.transport)
		return false;
	out->status = r.status;
	out->retry_after = r.retry;
	if (r.body) {
		out->body_len = strlen(r.body);
		out->body = malloc(out->body_len + 1);
		assert(out->body);
		memcpy(out->body, r.body, out->body_len + 1);
	}
	return true;
}
void http_free(http_response *r)
{
	free(r->body);
	memset(r, 0, sizeof *r);
}

static const char *uri = "spotify:playlist:37i9dQZF1EVHGWrwldPRtj";
static playlist_meta_job job(void)
{
	playlist_meta_job j;
	assert(playlist_meta_begin(&j, uri, NULL));
	for (int i = 0; i < 8; i++)
		if (playlist_meta_step(&j)) {
			assert(sent == reply_count);
			return j;
		}
	assert(!"job did not finish");
	return j;
}

static void test_resolution(void)
{
	reset();
	add("api.spotify.com", "fields=name,images", 200,
	    "{\"name\":\"API name\",\"owner\":{\"display_name\":\"Alice\"},\"images\":[{\"width\":64,\"url\":\"https://i.scdn.co/api\"}]}");
	playlist_meta_job j = job();
	assert(playlist_meta_complete(&j.meta));
	assert(sent == 1 && strcmp(j.meta.name, "API name") == 0);
	assert(j.meta.name_source == META_API && j.meta.art_source == META_API);

	reset();
	add("api.spotify.com", "fields=name,images", 404, "{}");
	add("api.spotify.com", "/images", 200,
	    "[{\"width\":1280,\"url\":\"https://pickasso.spotifycdn.com/large\"},{\"width\":64,\"url\":\"https://pickasso.spotifycdn.com/personal\"},{\"width\":300,\"url\":\"https://pickasso.spotifycdn.com/medium\"}]");
	add("open.spotify.com", "/oembed?url=https%3A%2F%2Fopen.spotify.com%2Fplaylist%2F", 200,
	    "{\"title\":\"Chill Mix\",\"thumbnail_url\":\"https://pickasso.spotifycdn.com/public\"}");
	j = job();
	assert(strcmp(j.meta.name, "Chill Mix") == 0);
	assert(strcmp(j.meta.art, "https://pickasso.spotifycdn.com/personal") == 0);
	assert(j.meta.name_source == META_OEMBED && j.meta.art_source == META_API);
	assert(!j.meta.owner[0]);

	reset();
	add("api.spotify.com", "fields=name,images", 200, "{\"name\":\"API title\",\"images\":[]}");
	add("api.spotify.com", "/images", 404, "{}");
	add("open.spotify.com", "/oembed", 200,
	    "{\"title\":\"Public title\",\"thumbnail_url\":\"https://pickasso.spotifycdn.com/public\"}");
	j = job();
	assert(strcmp(j.meta.name, "API title") == 0 && j.meta.art_source == META_OEMBED);

	reset();
	add("api.spotify.com", "fields=name,images", 404, "{}");
	add("api.spotify.com", "/images", 403, "{}");
	add("open.spotify.com", "/oembed", 404, "{}");
	j = job();
	assert(!playlist_meta_complete(&j.meta) && !j.meta.name[0] && !j.meta.art[0]);

	reset();
	add("api.spotify.com", "fields=name,images", 429, "{}");
	replies[0].retry = 3600;
	add("open.spotify.com", "/oembed", 200, "{\"title\":\"Public title\"}");
	j = job();
	assert(sent == 2 && j.retry_at == clock_now + 3600);
	assert(j.meta.name_source == META_OEMBED);

	reset();
	add("api.spotify.com", "fields=name,images", 200, "");
	replies[0].transport = false;
	add("open.spotify.com", "/oembed", 200, "{\"title\":\"Still works\"}");
	j = job();
	assert(j.retry_at == clock_now + 60 && strcmp(j.meta.name, "Still works") == 0);
}

static void test_parsing(void)
{
	playlist_meta m = {0};
	const char *s = "{\"title\":\"Caf\\u00e9 \\ud83c\\udfb5\",\"thumbnail_url\":\"https://i.scdn.co/x\"}";
	playlist_meta_parse(&m, s, (unsigned)strlen(s), 2);
	assert(strcmp(m.name, "Caf\xC3\xA9 \xF0\x9F\x8E\xB5") == 0);
	memset(&m, 0, sizeof m);
	s = "{\"title\":null,\"thumbnail_url\":\"http://bad.example/x\"}";
	playlist_meta_parse(&m, s, (unsigned)strlen(s), 2);
	assert(!m.name[0] && !m.art[0]);
	s = "{\"title\":\"bad\\nline\",\"thumbnail_url\":42}";
	playlist_meta_parse(&m, s, (unsigned)strlen(s), 2);
	assert(!m.name[0] && !m.art[0]);
	char long_json[512];
	memset(long_json, 'x', sizeof long_json);
	memcpy(long_json, "{\"title\":\"", 10);
	memcpy(long_json + 500, "\"}", 3);
	playlist_meta_parse(&m, long_json, 502, 2);
	assert(!m.name[0]);
	playlist_meta_job j;
	assert(!playlist_meta_begin(&j, "spotify:album:123456789", NULL));
	assert(!playlist_meta_begin(&j, "spotify:playlist:12345678&url=bad", NULL));
}

static void test_cache(void)
{
	remove(NAMECACHE_PATH);
	namecache_reset();
	playlist_meta m = {0};
	strcpy(m.name, "Chill Mix");
	strcpy(m.art, "https://pickasso.spotifycdn.com/personal");
	m.name_source = META_OEMBED;
	m.art_source = META_API;
	namecache_store(uri, &m);
	namecache_flush();
	namecache_reset();
	playlist_meta read;
	assert(namecache_lookup(uri, &read));
	assert(read.name_source == META_OEMBED && read.art_source == META_API);
	assert(namecache_refresh_at(uri) == clock_now + 86400);
	namecache_put_deferred(uri, "API name", "Alice", "");
	assert(namecache_lookup(uri, &read));
	assert(strcmp(read.art, m.art) == 0 && read.name_source == META_API);
	namecache_flush();
	namecache_reset();
	assert(namecache_lookup(uri, &read));
	assert(strcmp(read.name, "API name") == 0); /* second flush replaces SD file */
	namecache_store(uri, &m); /* oEmbed cannot replace a fresh API name */
	assert(namecache_lookup(uri, &read));
	assert(strcmp(read.name, "API name") == 0);
	clock_now += 15 * 86400;
	assert(!namecache_lookup(uri, &read));
	strcpy(m.art, "https://i.scdn.co/content");
	namecache_store(uri, &m);
	clock_now += 2 * 86400;
	assert(namecache_lookup(uri, &read));
	assert(!read.name[0] && read.art_source == META_API);

	FILE *f = fopen(NAMECACHE_PATH, "w");
	assert(f);
	fprintf(f, "%ld %s Legacy name\tOwner\thttps://i.scdn.co/old\n", (long)clock_now, uri);
	fclose(f);
	namecache_reset();
	assert(namecache_lookup(uri, &read));
	assert(strcmp(read.name, "Legacy name") == 0 && read.name_source == META_API);

	/* Art-only cache entries are retained, not replaced by a fake song label. */
	m.name[0] = '\0';
	namecache_store("spotify:playlist:artonly123", &m);
	namecache_flush();
	namecache_reset();
	assert(namecache_lookup("spotify:playlist:artonly123", &read));
	assert(!read.name[0] && read.art[0]);

	/* Failed replacement restores the old valid file, and dirty data can be
	 * installed on a later flush. Recovery also handles a missing main file. */
	strcpy(m.name, "Before failure");
	m.name_source = META_API;
	namecache_store(uri, &m);
	namecache_flush();
	strcpy(m.name, "After recovery");
	namecache_store(uri, &m);
	fail_install = true;
	namecache_flush();
	FILE *old = fopen(NAMECACHE_PATH, "r");
	assert(old);
	char text[1024];
	bool found_old = false;
	while (fgets(text, sizeof text, old))
		if (strstr(text, "Before failure"))
			found_old = true;
	fclose(old);
	assert(found_old);
	fail_install = false;
	namecache_flush();
	namecache_reset();
	assert(namecache_lookup(uri, &read) && strcmp(read.name, "After recovery") == 0);
	assert(test_sd_rename(NAMECACHE_PATH, NAMECACHE_PATH ".bak") == 0);
	namecache_reset();
	assert(namecache_lookup(uri, &read) && strcmp(read.name, "After recovery") == 0);
}

static void test_collections(void)
{
	static recent_list recent, filtered_recent;
	static playlist_list saved, filtered_saved;
	static album_list albums, filtered_albums;
	recent.count = 1;
	collection_item *item = &recent.items[0];
	item->kind = COLLECTION_PLAYLIST;
	strcpy(item->context_uri, uri);
	strcpy(item->name, "Track stand-in");
	strcpy(item->art_url, "https://i.scdn.co/album");
	playlist_meta m = {0};
	strcpy(m.name, "Chill Mix");
	strcpy(m.art, "https://pickasso.spotifycdn.com/personal");
	m.name_source = META_OEMBED;
	m.art_source = META_API;
	assert(collection_apply_metadata(item, &m));
	assert(strcmp(item->subtitle, "Playlist") == 0 && strcmp(item->context_uri, uri) == 0);
	collection_filter_lists(&recent, &saved, &albums, "chill", &filtered_recent, &filtered_saved, &filtered_albums);
	assert(filtered_recent.count == 1 && filtered_saved.count == 0);
	saved.items[0] = *item;
	saved.count = 1;
	collection_filter_lists(&recent, &saved, &albums, "CHILL", &filtered_recent, &filtered_saved, &filtered_albums);
	assert(filtered_recent.count == 0 && filtered_saved.count == 1);
	strcpy(m.name, "Web name");
	strcpy(m.owner, "Alice");
	m.name_source = META_API;
	assert(collection_apply_metadata(item, &m));
	strcpy(m.name, "Public name");
	m.name_source = META_OEMBED;
	collection_apply_metadata(item, &m);
	assert(strcmp(item->name, "Web name") == 0);
	/* Fresh fallback can replace expired API metadata, but expiry alone does
	 * not turn a real playlist name into a temporary song label. */
	item->name_stale = true;
	assert(item->name_source == META_API && strcmp(item->name, "Web name") == 0);
	assert(collection_apply_metadata(item, &m));
	assert(strcmp(item->name, "Public name") == 0 && !item->name_stale);
}

static void test_touch_identity(void)
{
	collection_hit before[] = {
		{1, COLLECTION_TOUCH_PLAY, "spotify:playlist:aaaaaaaa", COLLECTION_SECTION_RECENT},
		{2, COLLECTION_TOUCH_PLAY, "spotify:playlist:bbbbbbbb", COLLECTION_SECTION_RECENT},
	};
	collection_touch touch;
	collection_touch_begin(&touch, before, 2, 2);
	assert(touch.active && strcmp(touch.uri, "spotify:playlist:bbbbbbbb") == 0);
	assert(collection_touch_matches(&touch, before, 2, 2));
	collection_hit after[] = {
		{1, COLLECTION_TOUCH_PLAY, "spotify:playlist:bbbbbbbb", COLLECTION_SECTION_RECENT},
		{2, COLLECTION_TOUCH_PLAY, "spotify:playlist:cccccccc", COLLECTION_SECTION_RECENT},
	};
	assert(!collection_touch_matches(&touch, after, 2, 2));
	assert(collection_touch_matches(&touch, after, 2, 1));
	after[0].action = COLLECTION_TOUCH_OPEN;
	assert(!collection_touch_matches(&touch, after, 2, 1));
	assert(!collection_touch_matches(&touch, after, 0, 1));
	collection_hit duplicate[] = {
		{1, COLLECTION_TOUCH_ROW, "spotify:playlist:bbbbbbbb", COLLECTION_SECTION_RECENT},
		{2, COLLECTION_TOUCH_ROW, "spotify:playlist:bbbbbbbb", COLLECTION_SECTION_PLAYLIST},
	};
	collection_touch_begin(&touch, duplicate, 2, 2);
	assert(touch.section == COLLECTION_SECTION_PLAYLIST);
	assert(collection_touch_matches(&touch, duplicate, 2, 2));
	assert(!collection_touch_matches(&touch, duplicate, 2, 1));
	static recent_list recent;
	static playlist_list saved;
	static album_list albums;
	recent.count = saved.count = 1;
	strcpy(recent.items[0].context_uri, touch.uri);
	strcpy(saved.items[0].context_uri, touch.uri);
	int section, index;
	assert(collection_touch_find(&touch, &recent, &saved, &albums, &section, &index));
	assert(section == COLLECTION_SECTION_PLAYLIST && index == 0);
	saved.count = 0;
	assert(collection_touch_find(&touch, &recent, &saved, &albums, &section, &index));
	assert(section == COLLECTION_SECTION_RECENT && index == 0);
	recent.count = 0;
	assert(!collection_touch_find(&touch, &recent, &saved, &albums, &section, &index));
}

static void test_library_null_fields(void)
{
	remove(NAMECACHE_PATH);
	namecache_reset();
	reset();
	add("api.spotify.com", "/me/playlists?limit=50", 200,
	    "{\"total\":2,\"items\":[{\"uri\":\"spotify:playlist:nullimage1\",\"name\":\"API name\",\"images\":[{\"url\":null}]},{\"uri\":\"spotify:playlist:nullname12\",\"name\":null,\"owner\":{\"display_name\":null},\"images\":[{\"width\":64,\"url\":\"https://i.scdn.co/art\"}]}]}");
	static playlist_list list;
	char err[128];
	assert(playlists_fetch(&list, err, sizeof err) == PLAYER_OK && list.count == 2);
	assert(list.items[0].name_source == META_API && list.items[0].art_source == META_NONE);
	assert(!list.items[0].art_url[0] && list.items[0].metadata_retry_at == 0);
	assert(list.items[1].name_source == META_NONE && list.items[1].art_source == META_API);
	assert(strcmp(list.items[1].name, "Playlist") == 0 && list.items[1].metadata_retry_at == 0);
}

static void test_snapshot_is_api_only(void)
{
	reset();
	add("api.spotify.com", "snapshot_id,items(total)", 404, "{}");
	char name[128], owner[128], art[256], snapshot[128];
	int total = 123;
	assert(!playlist_metadata(uri, name, sizeof name, owner, sizeof owner,
	                          art, sizeof art, snapshot, sizeof snapshot, &total));
	assert(sent == 1 && !snapshot[0] && total == -1);
}

static void test_recent_history(void)
{
	remove(NAMECACHE_PATH);
	namecache_reset();
	playlist_meta m = {0};
	strcpy(m.art, "https://pickasso.spotifycdn.com/personal");
	m.art_source = META_API;
	namecache_store(uri, &m);
	reset();
	const char *body = "{\"items\":[{\"track\":{\"name\":\"Song\",\"artists\":[{\"name\":\"Artist\"}],\"album\":{\"name\":\"Album\",\"uri\":\"spotify:album:album1234\",\"images\":[{\"url\":\"https://i.scdn.co/album\"}]}},\"context\":{\"uri\":\"spotify:playlist:37i9dQZF1EVHGWrwldPRtj\"}}]}";
	add("api.spotify.com", "/recently-played?limit=50", 200, body);
	static recent_list list;
	char err[128];
	assert(recents_fetch(&list, err, sizeof err) == PLAYER_OK);
	assert(sent == 1 && list.count == 1);
	assert(strcmp(list.items[0].name, "Song") == 0);
	assert(list.items[0].name_source == META_NONE);
	assert(list.items[0].art_source == META_API);
	assert(strcmp(list.items[0].art_url, m.art) == 0);
	assert(strcmp(list.items[0].context_uri, uri) == 0);
	strcpy(m.name, "Chill Mix");
	m.name_source = META_OEMBED;
	namecache_store(uri, &m);
	reset();
	add("api.spotify.com", "/recently-played?limit=50", 200, body);
	assert(recents_fetch(&list, err, sizeof err) == PLAYER_OK);
	assert(sent == 1 && strcmp(list.items[0].name, "Chill Mix") == 0);
	assert(strcmp(list.items[0].subtitle, "Playlist") == 0);
	assert(list.items[0].metadata_retry_at > clock_now);
}

int main(void)
{
	test_resolution();
	test_parsing();
	test_cache();
	test_collections();
	test_snapshot_is_api_only();
	test_recent_history();
	test_touch_identity();
	test_library_null_fields();
	remove(NAMECACHE_PATH);
	puts("playlist metadata: field precedence, fallback, cache, library and snapshots passed");
	return 0;
}
