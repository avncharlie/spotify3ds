#!/usr/bin/env python3
"""One-time Spotify auth bootstrap for spotify3ds.

Runs the Authorization Code + PKCE flow in your browser and writes the
resulting refresh token to creds.cfg. Run this once on your computer; the 3DS
then only ever performs refresh_token grants.

PKCE needs no client_secret, so no secret ever reaches the console or the repo.
Your password goes to Spotify's own login page — this script never sees it. The
only thing it runs locally is a throwaway listener to catch the redirect.

Usage:
    python3 tools/bootstrap_auth.py --client-id <ID> [--out <path>]

Setup, if you have not done it already:
    1. https://developer.spotify.com/dashboard -> Create app
    2. Add redirect URI exactly:  http://127.0.0.1:8888/callback
    3. Copy the Client ID (it is a public value, not a secret)
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import http.server
import json
import os
import secrets
import stat
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

REDIRECT_HOST = "127.0.0.1"
REDIRECT_PORT = 8888
REDIRECT_URI = f"http://{REDIRECT_HOST}:{REDIRECT_PORT}/callback"

AUTH_URL = "https://accounts.spotify.com/authorize"
TOKEN_URL = "https://accounts.spotify.com/api/token"

# Read state, drive playback, and read (never write) the library well enough to
# populate the recently-played shelf.
#
# Everything here is read-only apart from user-modify-playback-state, which is
# what the transport buttons need. Nothing can alter playlists or the library.
# Note the refresh token this produces lives in plaintext on the SD card, so
# these scopes are also what someone holding that card could read.
SCOPES = (
    "user-read-playback-state "
    "user-modify-playback-state "
    "user-read-currently-playing "
    "user-read-recently-played "
    "user-library-read "
    "playlist-read-private "
    "user-top-read"
)


def b64url(raw: bytes) -> str:
    """Base64url with padding stripped, per RFC 7636."""
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


class CallbackHandler(http.server.BaseHTTPRequestHandler):
    result: dict[str, str] = {}

    def do_GET(self):  # noqa: N802
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path != "/callback":
            self.send_error(404)
            return

        CallbackHandler.result = dict(urllib.parse.parse_qsl(parsed.query))
        ok = "code" in CallbackHandler.result

        body = (
            "<html><body style='font-family:system-ui;text-align:center;"
            "padding-top:4em'><h2>{}</h2><p>{}</p></body></html>"
        ).format(
            "Authorized" if ok else "Authorization failed",
            "You can close this tab and return to the terminal."
            if ok
            else CallbackHandler.result.get("error", "unknown error"),
        ).encode()

        self.send_response(200 if ok else 400)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass  # keep the terminal clean


def post_form(url: str, fields: dict[str, str]) -> dict:
    data = urllib.parse.urlencode(fields).encode()
    req = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        detail = e.read().decode(errors="replace")
        sys.exit(f"token request failed ({e.code}): {detail}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--client-id", required=True, help="Spotify app Client ID")
    ap.add_argument(
        "--out",
        default=os.path.join(os.path.dirname(__file__), "..", "creds.cfg"),
        help="where to write creds.cfg (default: project root)",
    )
    args = ap.parse_args()

    verifier = b64url(secrets.token_bytes(64))
    challenge = b64url(hashlib.sha256(verifier.encode()).digest())
    state = b64url(secrets.token_bytes(16))

    query = urllib.parse.urlencode(
        {
            "client_id": args.client_id,
            "response_type": "code",
            "redirect_uri": REDIRECT_URI,
            "scope": SCOPES,
            "code_challenge_method": "S256",
            "code_challenge": challenge,
            "state": state,
        }
    )

    server = http.server.HTTPServer((REDIRECT_HOST, REDIRECT_PORT), CallbackHandler)
    threading.Thread(target=server.handle_request, daemon=True).start()

    url = f"{AUTH_URL}?{query}"
    print("Opening Spotify authorization in your browser...")
    print(f"If it does not open, visit:\n  {url}\n")
    webbrowser.open(url)

    # handle_request serves exactly one request, then the thread exits.
    for _ in range(1):
        threading.Event().wait(0.1)
    while not CallbackHandler.result:
        threading.Event().wait(0.2)
    server.server_close()

    result = CallbackHandler.result
    if "error" in result:
        sys.exit(f"authorization denied: {result['error']}")
    if result.get("state") != state:
        sys.exit("state mismatch - possible CSRF, aborting")

    code = result.get("code")
    if not code:
        sys.exit("no authorization code returned")

    print("Exchanging code for tokens...")
    tokens = post_form(
        TOKEN_URL,
        {
            "grant_type": "authorization_code",
            "code": code,
            "redirect_uri": REDIRECT_URI,
            "client_id": args.client_id,
            "code_verifier": verifier,
        },
    )

    refresh = tokens.get("refresh_token")
    if not refresh:
        sys.exit(f"no refresh_token in response: {tokens}")

    out = os.path.abspath(args.out)
    with open(out, "w") as f:
        f.write("# spotify3ds credentials - DO NOT COMMIT\n")
        f.write(f"client_id={args.client_id}\n")
        f.write(f"refresh_token={refresh}\n")
    os.chmod(out, stat.S_IRUSR | stat.S_IWUSR)  # 0600

    print(f"\nWrote {out}")
    print("Copy it to your SD card (or Azahar's sdmc) as:  /spotify/creds.cfg")
    print("\nThis file is a bearer credential. It is gitignored; keep it that way.")
    print("Revoke anytime at https://spotify.com/account/apps")


if __name__ == "__main__":
    main()
