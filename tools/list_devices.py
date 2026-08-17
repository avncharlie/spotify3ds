#!/usr/bin/env python3
"""List Spotify Connect devices available to the current user."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from set_volume import (
    SpotifyError,
    get_devices,
    load_credentials,
    refresh_access_token,
)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="List available Spotify Connect playback devices."
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
        devices = get_devices(token)
        if not devices:
            print("No available Spotify devices.")
            return

        print(f"Available Spotify Connect devices: {len(devices)}")
        for index, device in enumerate(devices, start=1):
            name = device.get("name") or "Unnamed device"
            kind = device.get("type") or "unknown"
            volume = device.get("volume_percent")
            shown_volume = f"{volume}%" if volume is not None else "unknown"
            supports_volume = device.get("supports_volume")
            shown_support = (
                "yes"
                if supports_volume is True
                else "no"
                if supports_volume is False
                else "unknown"
            )

            print(f"\n{index}. {name}")
            print(f"   State: {'active' if device.get('is_active') else 'available'}")
            print(f"   Device ID: {device.get('id') or '<not reported>'}")
            print(f"   Type: {kind}")
            print(f"   Volume: {shown_volume}")
            print(f"   Supports volume control: {shown_support}")
            print(f"   Restricted: {'yes' if device.get('is_restricted') else 'no'}")
            print(
                "   Private session: "
                f"{'yes' if device.get('is_private_session') else 'no'}"
            )

        print("\nStart or resume playback with:")
        print("  python3 tools/start_playback.py --device-id <DEVICE_ID>")
    except SpotifyError as error:
        sys.exit(str(error))


if __name__ == "__main__":
    main()
