#pragma once
#include <stdbool.h>
#include "../spotify/recents.h"

typedef enum {
	COLLECTION_TOUCH_ROW,
	COLLECTION_TOUCH_PLAY,
	COLLECTION_TOUCH_OPEN,
	COLLECTION_TOUCH_SHELF,
} collection_touch_action;

typedef enum {
	COLLECTION_SECTION_RECENT,
	COLLECTION_SECTION_PLAYLIST,
	COLLECTION_SECTION_ALBUM,
	COLLECTION_SECTION_SHELF,
} collection_touch_section;

typedef struct {
	int id;
	collection_touch_action action;
	char uri[128];
	collection_touch_section section;
} collection_hit;

typedef struct {
	bool active;
	collection_touch_action action;
	char uri[128];
	collection_touch_section section;
} collection_touch;

void collection_touch_begin(collection_touch *touch, const collection_hit *hits,
                             int count, int id);
bool collection_touch_matches(const collection_touch *touch,
                               const collection_hit *hits, int count, int id);
bool collection_touch_find(const collection_touch *touch,
                           const recent_list *recents,
                           const playlist_list *playlists,
                           const album_list *albums, int *section, int *index);
