#include "namecache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "../testlog.h"

#ifndef NAMECACHE_PATH
#define NAMECACHE_PATH "sdmc:/spotify/names.txt"
#endif
#define MAX_ENTRIES 128
#define LINE_MAX 1024

/* Old four-column lines remain readable. New trailing columns record field
 * sources and timestamps independently, so updating a cover cannot renew an
 * old oEmbed title and incomplete API list entries cannot erase good artwork. */
typedef struct {
	long when, name_when, art_when;
	char uri[128];
	playlist_meta meta;
} entry;

static entry s_entries[MAX_ENTRIES];
static int s_count;
static bool s_loaded, s_dirty;

void namecache_reset(void)
{
	s_count = 0;
	s_loaded = s_dirty = false;
}

static long lifetime(metadata_source source, const char *art)
{
	if (art) {
		const char *daylist = "https://daylist.spotifycdn.com/";
		const char *pickasso = "https://pickasso.spotifycdn.com/";
		if (strncmp(art, daylist, strlen(daylist)) == 0)
			return 6 * 3600; /* the image URL changes with the time of day */
		if (strncmp(art, pickasso, strlen(pickasso)) == 0)
			return 86400; /* periodically discover refreshed personalized art */
	}
	return (source == META_OEMBED ? NAMECACHE_FALLBACK_TTL_DAYS
	                             : NAMECACHE_TTL_DAYS) * 86400L;
}

static bool fresh(long when, metadata_source source, const char *art)
{
	long now = (long)time(NULL);
	return source != META_NONE && when > 0 && when <= now &&
	       now - when < lifetime(source, art);
}

static void load(void)
{
	if (s_loaded)
		return;
	s_loaded = true;
	FILE *f = fopen(NAMECACHE_PATH, "r");
	if (!f && errno == ENOENT) {
		/* Recover a power loss between moving the old file aside and installing
		 * the new one. 3DS SD rename cannot replace an existing destination. */
		if (rename(NAMECACHE_PATH ".bak", NAMECACHE_PATH) == 0)
			f = fopen(NAMECACHE_PATH, "r");
		else
			f = fopen(NAMECACHE_PATH ".bak", "r");
	}
	if (!f)
		return;
	char line[LINE_MAX];
	while (s_count < MAX_ENTRIES && fgets(line, sizeof line, f)) {
		char *nl = strchr(line, '\n');
		if (!nl && !feof(f)) {
			int c;
			while ((c = fgetc(f)) != '\n' && c != EOF) {}
			continue;
		}
		if (nl)
			*nl = '\0';
		char *sp1 = strchr(line, ' ');
		char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
		if (!sp2)
			continue;
		*sp1 = *sp2 = '\0';
		char *fields[7] = {sp2 + 1, "", "", NULL, NULL, NULL, NULL};
		for (int i = 1; i < 7; i++) {
			char *tab = strchr(fields[i - 1], '\t');
			if (!tab)
				break;
			*tab = '\0';
			fields[i] = tab + 1;
		}
		if (!sp1[1] || strlen(sp1 + 1) >= sizeof s_entries[0].uri ||
		    strlen(fields[0]) >= sizeof s_entries[0].meta.name ||
		    strlen(fields[1]) >= sizeof s_entries[0].meta.owner ||
		    strlen(fields[2]) >= sizeof s_entries[0].meta.art)
			continue;
		entry e = {0};
		e.when = strtol(line, NULL, 10);
		e.name_when = fields[5] ? strtol(fields[5], NULL, 10) : e.when;
		e.art_when = fields[6] ? strtol(fields[6], NULL, 10) : e.when;
		snprintf(e.uri, sizeof e.uri, "%s", sp1 + 1);
		snprintf(e.meta.name, sizeof e.meta.name, "%s", fields[0]);
		snprintf(e.meta.owner, sizeof e.meta.owner, "%s", fields[1]);
		snprintf(e.meta.art, sizeof e.meta.art, "%s", fields[2]);
		e.meta.name_source = fields[3] ? (metadata_source)atoi(fields[3]) : META_API;
		e.meta.art_source = fields[4] ? (metadata_source)atoi(fields[4]) : META_API;
		if (e.meta.name_source < META_NONE || e.meta.name_source > META_API ||
		    e.meta.art_source < META_NONE || e.meta.art_source > META_API)
			continue;
		if (!e.meta.name[0])
			e.meta.name_source = META_NONE;
		if (!e.meta.art[0])
			e.meta.art_source = META_NONE;
		if (e.meta.name[0] || e.meta.art[0])
			s_entries[s_count++] = e;
	}
	fclose(f);
}

bool namecache_lookup(const char *uri, playlist_meta *meta)
{
	memset(meta, 0, sizeof *meta);
	if (!uri || !uri[0])
		return false;
	load();
	for (int i = 0; i < s_count; i++) {
		entry *e = &s_entries[i];
		if (strcmp(e->uri, uri) != 0)
			continue;
		if (fresh(e->name_when, e->meta.name_source, NULL)) {
			memcpy(meta->name, e->meta.name, sizeof meta->name);
			memcpy(meta->owner, e->meta.owner, sizeof meta->owner);
			meta->name_source = e->meta.name_source;
		}
		if (fresh(e->art_when, e->meta.art_source, e->meta.art)) {
			memcpy(meta->art, e->meta.art, sizeof meta->art);
			meta->art_source = e->meta.art_source;
		}
		return meta->name[0] || meta->art[0];
	}
	return false;
}

bool namecache_get(const char *uri, char *name, int namelen, char *owner,
                   int ownerlen, char *art, int artlen)
{
	if (!name || namelen <= 0)
		return false;
	playlist_meta meta;
	namecache_lookup(uri, &meta);
	snprintf(name, (size_t)namelen, "%s", meta.name);
	if (owner && ownerlen > 0)
		snprintf(owner, (size_t)ownerlen, "%s", meta.owner);
	if (art && artlen > 0)
		snprintf(art, (size_t)artlen, "%s", meta.art);
	return meta.name[0] != '\0';
}

time_t namecache_refresh_at(const char *uri)
{
	playlist_meta meta;
	namecache_lookup(uri, &meta);
	if (!playlist_meta_complete(&meta))
		return 0;
	for (int i = 0; i < s_count; i++) {
		entry *e = &s_entries[i];
		if (strcmp(e->uri, uri) != 0)
			continue;
		time_t a = e->name_when + lifetime(meta.name_source, NULL);
		time_t b = e->art_when + lifetime(meta.art_source, meta.art);
		return a < b ? a : b;
	}
	return 0;
}

void namecache_store(const char *uri, const playlist_meta *meta)
{
	if (!uri || !uri[0] || strlen(uri) >= sizeof s_entries[0].uri ||
	    strpbrk(uri, " \r\n\t") || strpbrk(meta->name, "\r\n\t") ||
	    strpbrk(meta->owner, "\r\n\t") || strpbrk(meta->art, "\r\n\t") ||
	    (!meta->name[0] && !meta->art[0]))
		return;
	load();
	int idx = -1;
	for (int i = 0; i < s_count; i++)
		if (strcmp(s_entries[i].uri, uri) == 0) {
			idx = i;
			break;
		}
	if (idx < 0) {
		if (s_count < MAX_ENTRIES)
			idx = s_count++;
		else {
			idx = 0;
			for (int i = 1; i < s_count; i++)
				if (s_entries[i].when < s_entries[idx].when)
					idx = i;
		}
		memset(&s_entries[idx], 0, sizeof s_entries[idx]);
		snprintf(s_entries[idx].uri, sizeof s_entries[idx].uri, "%s", uri);
	}
	entry *e = &s_entries[idx];
	long now = (long)time(NULL);
	bool changed = false;
	if (meta->name[0] && meta->name_source != META_NONE &&
	    (!fresh(e->name_when, e->meta.name_source, NULL) ||
	     meta->name_source >= e->meta.name_source)) {
		if (meta->name_source == META_OEMBED &&
		    !fresh(e->name_when, e->meta.name_source, NULL))
			e->meta.owner[0] = '\0';
		memcpy(e->meta.name, meta->name, sizeof e->meta.name);
		e->meta.name_source = meta->name_source;
		e->name_when = now;
		changed = true;
	}
	if (meta->art[0] && meta->art_source != META_NONE &&
	    (!fresh(e->art_when, e->meta.art_source, e->meta.art) ||
	     meta->art_source >= e->meta.art_source)) {
		memcpy(e->meta.art, meta->art, sizeof e->meta.art);
		e->meta.art_source = meta->art_source;
		e->art_when = now;
		changed = true;
	}
	if (meta->owner[0])
		memcpy(e->meta.owner, meta->owner, sizeof e->meta.owner);
	if (changed) {
		e->when = now;
		s_dirty = true;
	}
}

void namecache_put_deferred(const char *uri, const char *name,
                            const char *owner, const char *art)
{
	playlist_meta meta = {0};
	snprintf(meta.name, sizeof meta.name, "%s", name ? name : "");
	snprintf(meta.owner, sizeof meta.owner, "%s", owner ? owner : "");
	snprintf(meta.art, sizeof meta.art, "%s", art ? art : "");
	meta.name_source = meta.name[0] ? META_API : META_NONE;
	meta.art_source = meta.art[0] ? META_API : META_NONE;
	namecache_store(uri, &meta);
}

void namecache_put(const char *uri, const char *name, const char *owner,
                   const char *art)
{
	namecache_put_deferred(uri, name, owner, art);
	namecache_flush();
}

void namecache_flush(void)
{
	if (!s_dirty)
		return;
	FILE *f = fopen(NAMECACHE_PATH ".tmp", "w");
	if (!f) {
		tl_log("namecache: cannot write %s", NAMECACHE_PATH);
		return;
	}
	bool ok = true;
	for (int i = 0; i < s_count; i++) {
		entry *e = &s_entries[i];
		if (fprintf(f, "%ld %s %s\t%s\t%s\t%d\t%d\t%ld\t%ld\n", e->when,
		            e->uri, e->meta.name, e->meta.owner, e->meta.art,
		            (int)e->meta.name_source, (int)e->meta.art_source,
		            e->name_when, e->art_when) < 0)
			ok = false;
	}
	if (fclose(f) != 0)
		ok = false;
	if (ok) {
		if (rename(NAMECACHE_PATH ".tmp", NAMECACHE_PATH) == 0) {
			s_dirty = false;
			remove(NAMECACHE_PATH ".bak");
			return;
		}
		/* Keep the previous valid cache until installation succeeds, rather
		 * than unlinking the only copy to satisfy SD's no-replace rename. */
		FILE *current = fopen(NAMECACHE_PATH, "r");
		bool have_current = current != NULL;
		if (current)
			fclose(current);
		if (have_current)
			remove(NAMECACHE_PATH ".bak");
		if (have_current && rename(NAMECACHE_PATH, NAMECACHE_PATH ".bak") == 0) {
			if (rename(NAMECACHE_PATH ".tmp", NAMECACHE_PATH) == 0) {
				s_dirty = false;
				remove(NAMECACHE_PATH ".bak");
				return;
			}
			/* Leave the backup recoverable even if this rollback also fails. */
			rename(NAMECACHE_PATH ".bak", NAMECACHE_PATH);
		}
	}
	if (s_dirty) {
		remove(NAMECACHE_PATH ".tmp");
		tl_log("namecache: write failed for %s", NAMECACHE_PATH);
	}
}
