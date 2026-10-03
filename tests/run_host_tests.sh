#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/spotify3ds-tests.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

CC="${CC:-cc}"
CFLAGS=(-std=c11 -Wall -Wextra -Werror -I"$ROOT/source")

"$CC" "${CFLAGS[@]}" \
	"$ROOT/tests/test_artcache_shard.c" \
	"$ROOT/source/spotify/artcache_path.c" \
	"$ROOT/source/spotify/art_scale.c" \
	-o "$TMP/test_artcache_shard"

"$CC" "${CFLAGS[@]}" -I"$ROOT/tests/stubs" -fsanitize=address,undefined \
	-DCACHE_ROOT="\"$TMP/artcache\"" \
	'-DARTCACHE_MAX_BYTES=(512ull*(33+102400))' \
	-Drename=test_sd_rename \
	"$ROOT/tests/test_artcache_quality.c" \
	"$ROOT/source/spotify/artcache.c" \
	"$ROOT/source/spotify/artcache_path.c" \
	-o "$TMP/test_artcache_quality"

"$CC" "${CFLAGS[@]}" \
	"$ROOT/tests/test_http.c" \
	"$ROOT/source/net/http.c" \
	-o "$TMP/test_http"

"$CC" "${CFLAGS[@]}" -fsanitize=address,undefined \
	"$ROOT/tests/test_player_previous.c" \
	"$ROOT/source/spotify/player.c" \
	"$ROOT/source/spotify/json.c" \
	-o "$TMP/test_player_previous"

"$CC" "${CFLAGS[@]}" \
	"$ROOT/tests/test_lyrics.c" \
	"$ROOT/source/spotify/lyrics.c" \
	"$ROOT/source/spotify/json.c" \
	-o "$TMP/test_lyrics"

"$CC" "${CFLAGS[@]}" \
	"$ROOT/tests/test_tracks_search.c" \
	"$ROOT/source/spotify/tracks_search.c" \
	-o "$TMP/test_tracks_search"

"$CC" "${CFLAGS[@]}" \
	"$ROOT/tests/test_setup_qr.c" \
	"$ROOT/source/setup/setup_qr.c" \
	-o "$TMP/test_setup_qr"

# The index is a binary format read back off an SD card, so it is worth
# proving it never reads outside the blob it was handed.
# All fixed-size buffer writes and an index shuffle, so the sanitizers earn
# their place here too.
"$CC" "${CFLAGS[@]}" -I"$ROOT/tests/stubs" -fsanitize=address,undefined \
	-DNAMECACHE_PATH="\"$TMP/names.txt\"" \
	-Drename=test_sd_rename \
	"$ROOT/tests/test_playlist_meta.c" \
	"$ROOT/source/spotify/playlist_meta.c" \
	"$ROOT/source/spotify/collection_meta.c" \
	"$ROOT/source/spotify/namecache.c" \
	"$ROOT/source/spotify/recents.c" \
	"$ROOT/source/spotify/json.c" \
	"$ROOT/source/ui/collection_touch.c" \
	-o "$TMP/test_playlist_meta"

"$CC" "${CFLAGS[@]}" -I"$ROOT/tests/stubs" -fsanitize=address,undefined \
	"$ROOT/tests/test_thumbs.c" \
	"$ROOT/source/ui/thumbs.c" \
	-o "$TMP/test_thumbs"

"$CC" "${CFLAGS[@]}" -fsanitize=address,undefined \
	"$ROOT/tests/test_searchhistory.c" \
	"$ROOT/source/spotify/searchhistory.c" \
	-o "$TMP/test_searchhistory"

"$CC" "${CFLAGS[@]}" -fsanitize=address,undefined \
	"$ROOT/tests/test_searchindex.c" \
	"$ROOT/source/spotify/searchindex.c" \
	"$ROOT/source/spotify/tracks_search.c" \
	-o "$TMP/test_searchindex"

"$TMP/test_artcache_shard"
"$TMP/test_artcache_quality"
"$TMP/test_http"
"$TMP/test_player_previous"
"$TMP/test_lyrics"
"$TMP/test_tracks_search"
"$TMP/test_setup_qr"
"$TMP/test_searchindex"
"$TMP/test_searchhistory"
"$TMP/test_playlist_meta"
"$TMP/test_thumbs"
