#!/usr/bin/env python3
"""Start or resume Spotify playback on a selected Connect device."""

from __future__ import annotations

import argparse
import sys
import urllib.parse
from pathlib import Path

from set_volume import (
    API_URL,
    SpotifyError,
    device_description,
    get_devices,
    load_credentials,
    refresh_access_token,
    request,
    select_device,
)


def start_playback(token: str, device_id: str) -> int:
    query = urllib.parse.urlencode({"device_id": device_id})
    status, body = request(
        f"{API_URL}/me/player/play?{query}", method="PUT", token=token
    )
    print(body)
    # Spotify documents 204, but the live endpoint also returns 200 with a
    # response body after successfully starting playback.
    if not 200 <= status < 300:
        detail = body.decode(errors="replace").strip()
        suffix = f": {detail}" if detail else ""
        raise SpotifyError(
            f"Playback endpoint returned unexpected HTTP {status}{suffix}"
        )
    return status


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Start or resume Spotify playback on an available device."
    )
    parser.add_argument(
        "--device-id",
        required=True,
        help="ID reported by tools/list_devices.py",
    )
    parser.add_argument(
        "--creds",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "creds.cfg",
        help="spotify3ds credentials file (default: project-root creds.cfg)",
    )
    return parser


def main() -> None:
    args = build_parser().parse_args()

    try:
        client_id, refresh_token = load_credentials(args.creds)
        token = refresh_access_token(args.creds, client_id, refresh_token)
        device = select_device(get_devices(token), args.device_id)
        if device.get("is_restricted"):
            raise SpotifyError(
                f"{device_description(device)} is restricted and cannot accept commands"
            )

        status = start_playback(token, args.device_id)
        print(
            f"Playback started/resumed on {device_description(device)} "
            f"(HTTP {status})."
        )
    except SpotifyError as error:
        sys.exit(str(error))


if __name__ == "__main__":
    main()
