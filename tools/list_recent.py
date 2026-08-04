#!/usr/bin/env python3
"""List distinct albums and playlists from Spotify listening history."""

from __future__ import annotations

import argparse
import configparser
import json
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

TOKEN_URL = "https://accounts.spotify.com/api/token"
API_URL = "https://api.spotify.com/v1"


def request_json(
    url: str,
    *,
    token: str | None = None,
    form: dict[str, str] | None = None,
) -> dict:
    data = urllib.parse.urlencode(form).encode() if form else None
    headers = {"Accept": "application/json"}
    if form:
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    if token:
        headers["Authorization"] = f"Bearer {token}"

    request = urllib.request.Request(url, data=data, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        detail = error.read().decode(errors="replace")
        raise RuntimeError(f"Spotify returned HTTP {error.code}: {detail}") from error
    except urllib.error.URLError as error:
        raise RuntimeError(f"Could not reach Spotify: {error.reason}") from error


def load_credentials(path: Path) -> tuple[str, str]:
    # creds.cfg is key=value rather than an INI section, so add one in memory.
    parser = configparser.ConfigParser(interpolation=None)
    try:
        parser.read_string("[spotify]\n" + path.read_text())
        return parser["spotify"]["client_id"], parser["spotify"]["refresh_token"]
    except (OSError, KeyError, configparser.Error) as error:
        raise RuntimeError(f"Could not read credentials from {path}: {error}") from error


def playlist_name(token: str, uri: str) -> str:
    playlist_id = uri.removeprefix("spotify:playlist:")
    fields = urllib.parse.urlencode({"fields": "name,owner(display_name)"})
    playlist = request_json(
        f"{API_URL}/playlists/{urllib.parse.quote(playlist_id)}?{fields}",
        token=token,
    )
    owner = playlist.get("owner", {}).get("display_name")
    return f"{playlist['name']} - {owner}" if owner else playlist["name"]


def paged_items(token: str, url: str):
    next_page: str | None = url
    while next_page:
        page = request_json(next_page, token=token)
        yield from page.get("items", [])
        next_page = page.get("next")


def print_library(token: str) -> None:
    print("Saved albums:")
    album_count = 0
    for item in paged_items(token, f"{API_URL}/me/albums?limit=50"):
        album = item.get("album", {})
        artists = ", ".join(a["name"] for a in album.get("artists", []))
        print(f"Album     {album.get('name', 'Unknown album')} - {artists}")
        album_count += 1
    if not album_count:
        print("(none)")

    print("\nOwned or followed playlists:")
    playlist_count = 0
    for playlist in paged_items(token, f"{API_URL}/me/playlists?limit=50"):
        owner = playlist.get("owner", {}).get("display_name")
        name = playlist.get("name", "Unknown playlist")
        print(f"Playlist  {name} - {owner}" if owner else f"Playlist  {name}")
        playlist_count += 1
    if not playlist_count:
        print("(none)")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="List distinct albums and playlists in recent Spotify history."
    )
    parser.add_argument(
        "--creds",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "creds.cfg",
        help="credentials file (default: ../creds.cfg relative to this script)",
    )
    parser.add_argument(
        "--limit",
        type=int,
        default=50,
        choices=range(1, 51),
        metavar="1-50",
        help="number of recent tracks to inspect (default: 50)",
    )
    parser.add_argument(
        "--library",
        action="store_true",
        help="list all saved albums and owned/followed playlists instead",
    )
    args = parser.parse_args()

    try:
        client_id, refresh_token = load_credentials(args.creds)
        tokens = request_json(
            TOKEN_URL,
            form={
                "grant_type": "refresh_token",
                "refresh_token": refresh_token,
                "client_id": client_id,
            },
        )
        access_token = tokens["access_token"]
        if args.library:
            print_library(access_token)
            return

        query = urllib.parse.urlencode({"limit": args.limit})
        history = request_json(
            f"{API_URL}/me/player/recently-played?{query}", token=access_token
        )

        seen: set[str] = set()
        rows: list[tuple[str, str, str]] = []
        for item in history.get("items", []):
            track = item.get("track", {})
            album = track.get("album", {})
            context_uri = (item.get("context") or {}).get("uri", "")
            if context_uri.startswith("spotify:playlist:"):
                key = context_uri
                if key in seen:
                    continue
                try:
                    name = playlist_name(access_token, context_uri)
                except (RuntimeError, KeyError):
                    name = context_uri.removeprefix("spotify:playlist:")
                kind = "Playlist"
            else:
                key = album.get("uri", "")
                if not key or key in seen:
                    continue
                artists = ", ".join(a["name"] for a in album.get("artists", []))
                name = f"{album.get('name', 'Unknown album')} - {artists}"
                kind = "Album"

            seen.add(key)
            rows.append((item.get("played_at", ""), kind, name))

        if not rows:
            print("No recently played albums or playlists found.")
            return

        for played_at, kind, name in rows:
            print(f"{played_at}  {kind:<8}  {name}")
    except (RuntimeError, KeyError) as error:
        sys.exit(str(error))


if __name__ == "__main__":
    main()
