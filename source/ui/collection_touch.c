#include "collection_touch.h"
#include <string.h>

void collection_touch_begin(collection_touch *touch, const collection_hit *hits,
                             int count, int id)
{
	memset(touch, 0, sizeof *touch);
	for (int i = 0; i < count; i++) {
		if (hits[i].id != id || !hits[i].uri[0])
			continue;
		touch->active = true;
		touch->action = hits[i].action;
		touch->section = hits[i].section;
		memcpy(touch->uri, hits[i].uri, sizeof touch->uri);
		return;
	}
}

bool collection_touch_matches(const collection_touch *touch,
                               const collection_hit *hits, int count, int id)
{
	if (!touch->active)
		return true;
	for (int i = 0; i < count; i++)
		if (hits[i].id == id)
			return hits[i].action == touch->action &&
			       hits[i].section == touch->section &&
			       strcmp(hits[i].uri, touch->uri) == 0;
	return false;
}

bool collection_touch_find(const collection_touch *touch,
                           const recent_list *recents,
                           const playlist_list *playlists,
                           const album_list *albums, int *section, int *index)
{
	const collection_item *lists[] = {recents->items, playlists->items, albums->items};
	int counts[] = {recents->count, playlists->count, albums->count};
	int preferred = touch->section <= COLLECTION_SECTION_ALBUM ? (int)touch->section : 0;
	for (int pass = 0; pass < 3; pass++) {
		int list = (preferred + pass) % 3;
		for (int i = 0; i < counts[list]; i++)
			if (strcmp(lists[list][i].context_uri, touch->uri) == 0) {
				*section = list;
				*index = i;
				return true;
			}
	}
	return false;
}
