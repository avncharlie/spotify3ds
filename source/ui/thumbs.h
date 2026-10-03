#pragma once

#include <citro2d.h>
#include <stdbool.h>

/* Render-thread cache of shelf and list thumbnails.
 *
 * Textures live on the GPU and can only be created on the thread owning the
 * graphics context, so the worker hands over decoded pixels and this turns them
 * into images. Lookups are by URL, which is what the item lists carry.
 *
 * Small on purpose: the shelf shows four and the Library about five rows at a
 * time, so a handful of slots covers everything visible with room to spare
 * while scrolling. At 64px a slot is 16KB.
 */
#define THUMBS_SLOTS 24

/* Release every texture. Call before gfxExit. */
void thumbs_free_all(void);

/* Look up `url`.
 *
 * Returns NULL when not loaded, and queues a fetch for it - so the caller can
 * simply ask every frame and draw a placeholder until an image appears. A NULL
 * or empty url queues nothing, which is what a collection with no artwork
 * (measured: 2 of 49 playlists) gets. */
const C2D_Image *thumbs_get(const char *url);

/* Expire textures and claim/upload completed thumbnails. Once per frame after
 * C3D_FrameBegin(SYNCDRAW) has retired previous draws, before submitting any new
 * image draws. thumbs_get never destroys textures during a draw. */
void thumbs_pump(void);
