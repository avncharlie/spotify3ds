# Spotify3DS

A Spotify remote for the Nintendo 3DS / New 2DS XL.

- **Top screen:** album cover, artist, album/track name
- **Bottom screen (touch):** previous / play-pause / next, plus a seek scrubber

Audio keeps playing on your phone or desktop — this controls Spotify's
*active device* via the official Web API. It is a remote, not a player.

## Installation

You need Spotify Premium, a homebrew-enabled 3DS, Python 3 on your computer,
and access to the console's SD card.

Download `Spotify3DS.cia` from the project's
[latest release](https://github.com/avncharlie/spotify3ds/releases/latest).
You can also scan this QR code in FBI under **Remote Install → Scan QR Code**
to install the latest CIA directly:

<a href="https://github.com/avncharlie/spotify3ds/releases/latest/download/Spotify3DS.cia"><img src="assets/latest-release-qr.png" alt="QR code for the latest Spotify3DS CIA" width="240"></a>

1. Create an app in the [Spotify Developer Dashboard](https://developer.spotify.com/dashboard).
   Select **Web API**, add `http://127.0.0.1:8888/callback` as a redirect URI,
   and copy the app's Client ID. No client secret is needed.
2. From this project directory, generate your console credentials:

   ```sh
   python3 tools/bootstrap_auth.py --client-id <YOUR_CLIENT_ID>
   ```

   Your browser will open Spotify's authorization page. After approval, the
   script writes `creds.cfg` in the project directory.
3. Create a folder named `spotify` at the root of the 3DS SD card and copy the
   generated file to it. The resulting path must be `/spotify/creds.cfg`.
4. Copy `Spotify3DS.cia` to the SD card, put the card back in the console, and
   install the CIA with FBI or another homebrew CIA installer (Or install via
   QR code)

Start playback on a Spotify device before opening Spotify3DS. The app controls
the active device; it does not play audio through the console.

`creds.cfg` contains a plaintext bearer credential. Keep it private and revoke
access at <https://spotify.com/account/apps> if the SD card or file is lost.

## Requirements

- Spotify **Premium** (the playback-control endpoints return 403 on free accounts)
- A homebrew-enabled 3DS, or the Azahar emulator
- For source builds: devkitARM + libctru, the portlibs listed below, `makerom`,
  and `bannertool`

## Why mbedTLS is bundled

The 3DS `sslc` system module maxes out at **TLS 1.1**, and Spotify requires
**TLS 1.2+**. So `httpc`/`sslc` cannot reach the API at all. Instead this app
links mbedTLS and speaks TLS in userspace over raw BSD sockets, bypassing
`sslc` entirely.

Both DigiCert roots must be embedded: **G2 (RSA)** serves `api.spotify.com` and
`accounts.spotify.com`, while **G3 (ECC)** serves the `i.scdn.co` album-art CDN.
Omitting G3 produces a confusing failure where the API works perfectly but
cover art silently never loads. Verified: the two hosts negotiate
`ECDHE-RSA-...` and `ECDHE-ECDSA-...` respectively, so the chains really are
independent.

### Entropy

The packaged mbedTLS is built with `MBEDTLS_NO_PLATFORM_ENTROPY` and
`MBEDTLS_ENTROPY_HARDWARE_ALT`, so its only built-in source is
`mbedtls_hardware_poll()` → `sslcGenerateRandomData()`. That means **`sslcInit()`
must be called** even though we do TLS ourselves — `sslc` is used purely as an
RNG here, and its TLS 1.1 ceiling is irrelevant to that.

`PS_GenerateRandomBytes` looks like the more natural choice but returns
`0xD8E007F7` (unavailable) under Azahar, so it is registered only as an
*optional* extra source with threshold 0. It must not report
`ENTROPY_SOURCE_FAILED` when absent, or it poisons the accumulator and seeding
fails even though the sslc source is healthy.

## Development setup

```sh
sudo dkp-pacman -S 3ds-zlib 3ds-mbedtls 3ds-libjpeg-turbo
./dev.sh          # build, run in Azahar, print pass/fail
./dev.sh --build  # build only
./dev.sh --log    # also dump the guest debug log
```

### Emulator auth (one time)

1. Create an app at <https://developer.spotify.com/dashboard>
2. Add this redirect URI **exactly**: `http://127.0.0.1:8888/callback`
3. Run the bootstrap with your Client ID:

```sh
python3 tools/bootstrap_auth.py --client-id <YOUR_CLIENT_ID>
```

It opens Spotify's own login page in your browser, catches the redirect on a
throwaway local listener, and writes `creds.cfg`. Copy that to the SD card as
`/spotify/creds.cfg` (for Azahar:
`~/Library/Application Support/Azahar/sdmc/spotify/creds.cfg`).

PKCE means **no `client_secret` exists**, so no secret ever reaches the console
or the repo, and the script never sees your password. The 3DS thereafter only
performs `grant_type=refresh_token`.

> **Note:** that refresh token is a plaintext bearer credential on the SD card.
> Homebrew has no secure storage — any encryption key would have to sit beside
> the ciphertext. Revoke anytime at <https://spotify.com/account/apps>.

## Development notes

Verification is headless: the app writes machine-readable verdicts to
`sdmc:/testresult.txt`, which `dev.sh` reads back from the host. A missing
`DONE` sentinel means the app hung or crashed — distinct from a clean failure.

Environment quirks worth knowing, all learned the hard way:

- Launch Azahar via `open -a Azahar.app --args -w <file>.3dsx`. Invoking
  `Contents/MacOS/azahar` directly pops a modal and hangs forever.
- `svcOutputDebugString` / `LOG_DEBUG` is **compiled out** of Azahar release
  builds. It is not a usable debug channel; write to the SD card instead.
- The GDB stub **halts the guest at boot** waiting for a debugger. Keep
  `use_gdbstub=false` unless actively debugging.
- Azahar rewrites `qt-config.ini` on quit, so only edit config while it is
  fully stopped.
- `Failed to find title id for ROM` in the emulator log is normal for `.3dsx`.

Hold **SELECT** at boot to keep the app running instead of auto-exiting, which
is useful when capturing screenshots.

## Layout

```
source/
  main.c          entry point, render loop
  testlog.[ch]    headless test + logging harness
  net/            sockets + mbedTLS transport
  spotify/        auth, API calls, JSON
  ui/             screens, touch hit-testing
tools/            host-side PKCE bootstrap
```
