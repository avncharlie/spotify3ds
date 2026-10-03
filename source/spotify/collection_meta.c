#include "recents.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

bool collection_apply_metadata(collection_item *item, const playlist_meta *meta)
{
	if (item->kind != COLLECTION_PLAYLIST)
		return false;
	collection_item before = *item;
	if (meta->name[0] && meta->name_source != META_NONE &&
	    (item->name_stale || meta->name_source >= item->name_source)) {
		snprintf(item->name, sizeof item->name, "%s", meta->name);
		item->name_source = meta->name_source;
		item->name_stale = false;
		snprintf(item->subtitle, sizeof item->subtitle,
		         meta->owner[0] ? "Playlist \xC2\xB7 %.115s" : "Playlist%s",
		         meta->owner);
	}
	if (meta->art[0] && meta->art_source != META_NONE &&
	    (item->art_stale || meta->art_source >= item->art_source)) {
		snprintf(item->art_url, sizeof item->art_url, "%s", meta->art);
		item->art_source = meta->art_source;
		item->art_stale = false;
	}
	return strcmp(before.name, item->name) != 0 ||
	       strcmp(before.subtitle, item->subtitle) != 0 ||
	       strcmp(before.art_url, item->art_url) != 0 ||
	       before.name_source != item->name_source ||
	       before.art_source != item->art_source ||
	       before.name_stale != item->name_stale || before.art_stale != item->art_stale;
}

static bool contains_ci(const char *text, const char *needle)
{
	if (!needle[0])
		return true;
	for (const char *p = text; *p; p++) {
		int i = 0;
		while (needle[i] && p[i] &&
		       tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
			i++;
		if (!needle[i])
			return true;
	}
	return false;
}

static bool matches(const collection_item *item, const char *query)
{
	return contains_ci(item->name, query) || contains_ci(item->subtitle, query);
}

void collection_filter_lists(const recent_list *recents,
                             const playlist_list *playlists,
                             const album_list *albums, const char *query,
                             recent_list *out_recents,
                             playlist_list *out_playlists,
                             album_list *out_albums)
{
	memset(out_recents, 0, sizeof *out_recents);
	memset(out_playlists, 0, sizeof *out_playlists);
	memset(out_albums, 0, sizeof *out_albums);
	for (int i = 0; i < playlists->count; i++)
		if (matches(&playlists->items[i], query))
			out_playlists->items[out_playlists->count++] = playlists->items[i];
	for (int i = 0; i < albums->count; i++)
		if (matches(&albums->items[i], query))
			out_albums->items[out_albums->count++] = albums->items[i];
	for (int i = 0; i < recents->count; i++) {
		const collection_item *item = &recents->items[i];
		if (!matches(item, query))
			continue;
		bool duplicate = false;
		const collection_item *items = item->kind == COLLECTION_PLAYLIST
		                                   ? out_playlists->items : out_albums->items;
		int count = item->kind == COLLECTION_PLAYLIST ? out_playlists->count
		                                              : out_albums->count;
		for (int k = 0; k < count; k++)
			if (strcmp(item->context_uri, items[k].context_uri) == 0)
				duplicate = true;
		if (!duplicate)
			out_recents->items[out_recents->count++] = *item;
	}
	out_playlists->total = out_playlists->count;
	out_albums->total = out_albums->count;
}
