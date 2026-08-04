#include "namecache.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../testlog.h"

#define NAMECACHE_PATH "sdmc:/spotify/names.txt"

/* One line per entry: "<unix-seconds> <uri> <name>\t<owner>\t<art-url>\n".
 *
 * Space-separated up to the name, which may itself contain spaces, so the
 * later fields are split off by tabs - a character Spotify does not allow in
 * any of them. Entries written before a field existed simply have fewer tabs
 * and read back empty, so an older cache upgrades in place rather than needing
 * to be discarded.
 *
 * A flat file rather than the sharded layout artcache uses: entries are tens of
 * bytes and there are dozens of them, so the whole thing is smaller than a
 * single art entry's header. Rewriting it wholesale on every put costs one
 * short write, which is far cheaper than the seek-heavy alternative and keeps
 * the format greppable when debugging over netload.
 */

#define MAX_ENTRIES 128

/* Must exceed the widest possible line - uri + name + owner + art url plus
 * separators - or fgets would split one entry across two reads and the second
 * half would be discarded as malformed. Mosaic urls alone run to ~200 chars. */
#define LINE_MAX 768

typedef struct {
	long when;
	char uri[128];
	char name[128];
	char owner[128];
	char art[256];
} entry;

/* Loaded lazily and kept in memory: the worker reads this once per playlist it
 * cannot name, and re-reading the file for each would be pointless I/O. */
static entry s_entries[MAX_ENTRIES];
static int   s_count;
static bool  s_loaded;

static long now_seconds(void)
{
	return (long)time(NULL);
}

static void load(void)
{
	if (s_loaded)
		return;
	s_loaded = true;

	FILE *f = fopen(NAMECACHE_PATH, "r");
	if (!f)
		return;

	char line[LINE_MAX];
	while (s_count < MAX_ENTRIES && fgets(line, sizeof line, f)) {
		char *nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';

		/* "<when> <uri> <name>": the name may contain spaces, the first two
		 * fields may not, so split on the first two separators only. */
		char *sp1 = strchr(line, ' ');
		if (!sp1)
			continue;
		*sp1 = '\0';
		char *sp2 = strchr(sp1 + 1, ' ');
		if (!sp2)
			continue;
		*sp2 = '\0';

		const char *when  = line;
		const char *uri   = sp1 + 1;
		char       *name  = sp2 + 1;
		const char *owner = "";
		const char *art   = "";

		/* Both trailing fields are optional; older entries have fewer tabs. */
		char *tab = strchr(name, '\t');
		if (tab) {
			*tab  = '\0';
			owner = tab + 1;

			char *tab2 = strchr(tab + 1, '\t');
			if (tab2) {
				*tab2 = '\0';
				art   = tab2 + 1;
			}
		}

		if (!uri[0] || !name[0])
			continue;

		entry *e = &s_entries[s_count++];
		e->when  = strtol(when, NULL, 10);
		snprintf(e->uri, sizeof e->uri, "%s", uri);
		snprintf(e->name, sizeof e->name, "%s", name);
		snprintf(e->owner, sizeof e->owner, "%s", owner);
		snprintf(e->art, sizeof e->art, "%s", art);
	}

	fclose(f);
}

static void save(void)
{
	FILE *f = fopen(NAMECACHE_PATH, "w");
	if (!f) {
		tl_log("namecache: cannot write %s", NAMECACHE_PATH);
		return;
	}

	for (int i = 0; i < s_count; i++)
		fprintf(f, "%ld %s %s\t%s\t%s\n", s_entries[i].when, s_entries[i].uri,
		        s_entries[i].name, s_entries[i].owner, s_entries[i].art);

	fclose(f);
}

bool namecache_get(const char *uri, char *name, int namelen, char *owner,
                   int ownerlen, char *art, int artlen)
{
	if (!uri || !uri[0] || !name || namelen <= 0)
		return false;

	load();

	const long cutoff = now_seconds() - (long)NAMECACHE_TTL_DAYS * 24 * 3600;

	for (int i = 0; i < s_count; i++) {
		if (strcmp(s_entries[i].uri, uri) != 0)
			continue;

		/* A clock that reads earlier than the entry (the 3DS RTC can be reset,
		 * and was wrong on this very console during bring-up) would make a
		 * fresh entry look infinitely old. Treat only a plausible age as
		 * expiry. */
		if (s_entries[i].when < cutoff)
			return false;

		snprintf(name, namelen, "%s", s_entries[i].name);
		if (owner && ownerlen > 0)
			snprintf(owner, ownerlen, "%s", s_entries[i].owner);
		if (art && artlen > 0)
			snprintf(art, artlen, "%s", s_entries[i].art);
		return true;
	}

	return false;
}

void namecache_put(const char *uri, const char *name, const char *owner,
                   const char *art)
{
	if (!uri || !uri[0] || !name || !name[0])
		return;

	if (!owner)
		owner = "";
	if (!art)
		art = "";

	/* A newline or tab would corrupt the line format, and a space in the uri
	 * would break the field split. Reject rather than mangle: the cost is one
	 * extra request next launch. */
	if (strpbrk(name, "\n\t") || strpbrk(owner, "\n\t") ||
	    strpbrk(art, "\n\t") || strpbrk(uri, " \n\t"))
		return;

	load();

	for (int i = 0; i < s_count; i++) {
		if (strcmp(s_entries[i].uri, uri) == 0) {
			snprintf(s_entries[i].name, sizeof s_entries[i].name, "%s", name);
			snprintf(s_entries[i].owner, sizeof s_entries[i].owner, "%s", owner);
			snprintf(s_entries[i].art, sizeof s_entries[i].art, "%s", art);
			s_entries[i].when = now_seconds();
			save();
			return;
		}
	}

	if (s_count >= MAX_ENTRIES) {
		/* Drop the oldest to make room. At 128 entries this is rare enough not
		 * to justify anything cleverer. */
		int oldest = 0;
		for (int i = 1; i < s_count; i++)
			if (s_entries[i].when < s_entries[oldest].when)
				oldest = i;
		memmove(&s_entries[oldest], &s_entries[oldest + 1],
		        (size_t)(s_count - oldest - 1) * sizeof s_entries[0]);
		s_count--;
	}

	entry *e = &s_entries[s_count++];
	e->when  = now_seconds();
	snprintf(e->uri, sizeof e->uri, "%s", uri);
	snprintf(e->name, sizeof e->name, "%s", name);
	snprintf(e->owner, sizeof e->owner, "%s", owner);
	snprintf(e->art, sizeof e->art, "%s", art);
	save();
}
