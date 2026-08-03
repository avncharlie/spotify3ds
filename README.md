# spotify3ds

A Spotify remote for the Nintendo 3DS / New 2DS XL.

- **Top screen:** album cover, artist, album/track name
- **Bottom screen (touch):** previous / play-pause / next, plus a seek scrubber

Audio keeps playing on your phone or desktop — this controls Spotify's
*active device* via the official Web API. It is a remote, not a player.

## Requirements

- Spotify **Premium** (the playback-control endpoints return 403 on free accounts)
- A homebrew-enabled 3DS, or the Azahar emulator
- devkitARM + libctru, plus the portlibs listed below

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

## Setup

```sh
sudo dkp-pacman -S 3ds-zlib 3ds-mbedtls 3ds-libjpeg-turbo
./dev.sh          # build, run in Azahar, print pass/fail
./dev.sh --build  # build only
./dev.sh --log    # also dump the guest debug log
```

Auth uses a one-time PKCE bootstrap run on your computer; the 3DS never sees
your password and no `client_secret` is ever needed. The resulting refresh
token goes in `sdmc:/spotify/creds.cfg` (see `creds.cfg.example`).

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
