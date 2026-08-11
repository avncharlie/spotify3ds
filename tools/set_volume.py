#!/usr/bin/env python3
"""Set and verify Spotify Connect playback volume using spotify3ds credentials."""

from __future__ import annotations

import argparse
import configparser
import json
import os
import stat
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

TOKEN_URL = "https://accounts.spotify.com/api/token"
API_URL = "https://api.spotify.com/v1"


class SpotifyError(RuntimeError):
    """A transport or Spotify Web API failure."""


def load_credentials(path: Path) -> tuple[str, str]:
    """Read spotify3ds' sectionless key=value credential file."""
    parser = configparser.ConfigParser(interpolation=None)
    try:
        parser.read_string("[spotify]\n" + path.read_text())
        section = parser["spotify"]
        return section["client_id"], section["refresh_token"]
    except (OSError, KeyError, configparser.Error) as error:
        raise SpotifyError(f"Could not read credentials from {path}: {error}") from error


def persist_credentials(path: Path, client_id: str, refresh_token: str) -> None:
    """Atomically persist a refresh-token rotation without exposing it."""
    temp = path.with_name(path.name + ".tmp")
    try:
        temp.write_text(
            "# spotify3ds credentials - DO NOT COMMIT\n"
            f"client_id={client_id}\n"
            f"refresh_token={refresh_token}\n"
        )
        os.chmod(temp, stat.S_IRUSR | stat.S_IWUSR)
        temp.replace(path)
    except OSError as error:
        try:
            temp.unlink(missing_ok=True)
        except OSError:
            pass
        raise SpotifyError(
            f"Spotify rotated the refresh token but {path} could not be updated: "
            f"{error}"
        ) from error


def error_detail(error: urllib.error.HTTPError) -> str:
    raw = error.read().decode(errors="replace")
    try:
        payload = json.loads(raw)
        detail = payload.get("error", payload)
        if isinstance(detail, dict):
            return str(detail.get("message") or detail.get("status") or detail)
        return str(detail)
    except (json.JSONDecodeError, AttributeError):
        return raw.strip() or error.reason


def request(
    url: str,
    *,
    method: str = "GET",
    token: str | None = None,
    form: dict[str, str] | None = None,
) -> tuple[int, bytes]:
    data = urllib.parse.urlencode(form).encode() if form is not None else None
    if method == "PUT" and data is None:
        data = b""  # Explicit Content-Length: 0, as Spotify's player API expects.
    headers = {"Accept": "application/json"}
    if form is not None:
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    if token:
        headers["Authorization"] = f"Bearer {token}"

    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=30) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as error:
        raise SpotifyError(
            f"Spotify returned HTTP {error.code}: {error_detail(error)}"
        ) from error
    except urllib.error.URLError as error:
        raise SpotifyError(f"Could not reach Spotify: {error.reason}") from error


def refresh_access_token(path: Path, client_id: str, refresh_token: str) -> str:
    status, body = request(
        TOKEN_URL,
        method="POST",
        form={
            "grant_type": "refresh_token",
            "refresh_token": refresh_token,
            "client_id": client_id,
        },
    )
    if status != 200:
        raise SpotifyError(f"Token endpoint returned unexpected HTTP {status}")
    try:
        tokens = json.loads(body)
        access_token = tokens["access_token"]
    except (json.JSONDecodeError, KeyError, TypeError) as error:
        raise SpotifyError("Token response did not contain an access token") from error

    rotated = tokens.get("refresh_token")
    if rotated and rotated != refresh_token:
        persist_credentials(path, client_id, rotated)
        print(
            f"Spotify rotated the refresh token in {path}. Re-copy that file to "
            "SD:/spotify/creds.cfg before launching Spotify3DS.",
            file=sys.stderr,
        )
    return access_token


def get_devices(token: str) -> list[dict]:
    status, body = request(f"{API_URL}/me/player/devices", token=token)
    if status != 200:
        raise SpotifyError(f"Device endpoint returned unexpected HTTP {status}")
    try:
        return json.loads(body).get("devices", [])
    except (json.JSONDecodeError, AttributeError) as error:
        raise SpotifyError("Device endpoint returned invalid JSON") from error


def select_device(devices: list[dict], device_id: str | None) -> dict:
    if device_id:
        device = next((item for item in devices if item.get("id") == device_id), None)
        if not device:
            raise SpotifyError(f"Device {device_id!r} is not currently available")
        return device

    device = next((item for item in devices if item.get("is_active")), None)
    if not device:
        raise SpotifyError(
            "No active Spotify device. Start playback first or pass --device-id."
        )
    return device


def device_description(device: dict) -> str:
    name = device.get("name") or "Unnamed device"
    kind = device.get("type") or "unknown type"
    volume = device.get("volume_percent")
    shown_volume = f"{volume}%" if volume is not None else "unknown volume"
    return f"{name} ({kind}, {shown_volume})"


def set_volume(token: str, volume: int, device_id: str) -> None:
    query = urllib.parse.urlencode(
        {"volume_percent": volume, "device_id": device_id}
    )
    status, body = request(
        f"{API_URL}/me/player/volume?{query}", method="PUT", token=token
    )
    if status != 204 or body:
        raise SpotifyError(
            f"Volume endpoint returned unexpected HTTP {status} with {len(body)} bytes"
        )


def verify_volume(token: str, device_id: str, expected: int, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while True:
        devices = get_devices(token)
        device = next((item for item in devices if item.get("id") == device_id), None)
        if device and device.get("volume_percent") == expected:
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.75)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Set the volume of a Spotify Connect playback device."
    )
    parser.add_argument(
        "volume", type=int, nargs="?", help="target volume from 0 to 100"
    )
    parser.add_argument(
        "--get",
        action="store_true",
        help="report the current volume without changing it",
    )
    parser.add_argument(
        "--device-id",
        help="target a specific available device (default: active device)",
    )
    parser.add_argument(
        "--creds",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "creds.cfg",
        help="spotify3ds credentials file (default: project-root creds.cfg)",
    )
    parser.add_argument(
        "--verify-timeout",
        type=float,
        default=5.0,
        metavar="SECONDS",
        help="wait for the device to report the new volume; 0 disables verification",
    )
    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()
    if args.get and args.volume is not None:
        parser.error("volume cannot be supplied with --get")
    if not args.get and args.volume is None:
        parser.error("volume is required unless --get is used")
    if args.volume is not None and not 0 <= args.volume <= 100:
        parser.error("volume must be between 0 and 100")
    if args.verify_timeout < 0:
        parser.error("--verify-timeout cannot be negative")

    try:
        client_id, refresh_token = load_credentials(args.creds)
        token = refresh_access_token(args.creds, client_id, refresh_token)
        device = select_device(get_devices(token), args.device_id)
        print(f"Target: {device_description(device)}")
        if args.get:
            volume = device.get("volume_percent")
            if volume is None:
                raise SpotifyError("Spotify did not report a volume for this device")
            print(f"Current volume: {volume}%")
            return
        if not device.get("supports_volume", False):
            raise SpotifyError(
                f"{device_description(device)} reports supports_volume=false"
            )
        device_id = device.get("id")
        if not device_id:
            raise SpotifyError("Spotify returned a device without an ID")

        set_volume(token, args.volume, device_id)
        print(f"Spotify accepted volume {args.volume}% (HTTP 204).")

        if args.verify_timeout == 0:
            return
        if verify_volume(token, device_id, args.volume, args.verify_timeout):
            print(f"Verified: device now reports {args.volume}%.")
        else:
            raise SpotifyError(
                f"Spotify accepted the command, but the device did not report "
                f"{args.volume}% within {args.verify_timeout:g}s"
            )
    except SpotifyError as error:
        sys.exit(str(error))


if __name__ == "__main__":
    main()
