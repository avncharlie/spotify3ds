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
	-o "$TMP/test_artcache_shard"

"$CC" "${CFLAGS[@]}" \
	"$ROOT/tests/test_http.c" \
	"$ROOT/source/net/http.c" \
	-o "$TMP/test_http"

"$TMP/test_artcache_shard"
"$TMP/test_http"
