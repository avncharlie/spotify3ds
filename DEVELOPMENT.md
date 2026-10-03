# Development

This document covers requirements, source builds, testing, release automation,
and implementation details for Spotify3DS contributors.

## Requirements

- Spotify Premium (so we can use the Spotify's playback control API)
- A homebrew-enabled 3DS, or the Azahar emulator
- For source builds: devkitARM + libctru, the portlibs listed below, `makerom`,
  and `bannertool`

## Development setup

```sh
sudo dkp-pacman -S 3ds-zlib 3ds-mbedtls 3ds-libjpeg-turbo
./dev.sh          # build, run in Azahar, print pass/fail
./dev.sh --build  # build only
./dev.sh --log    # also dump the guest debug log
./dev.sh --hw --ip 192.168.1.x  # build and netload to a 3DS
./tests/run_host_tests.sh  # deterministic cache, HTTP, JSON, and lyrics tests
go test ./...              # setup wizard, scopes, and credential QR protocol
```

To test Spotify Connect volume control with the same credentials used by the
3DS, start playback on a Premium account and run:

```sh
python3 tools/set_volume.py 50
python3 tools/set_volume.py --get                    # query active volume
python3 tools/set_volume.py 50 --device-id <DEVICE_ID>  # optional target
```

The script refreshes OAuth, checks that the target supports volume control,
requires Spotify's `204 No Content` response, and verifies the reported volume.
If Spotify reports that it rotated the refresh token, re-copy the updated
`creds.cfg` to `SD:/spotify/creds.cfg` before launching Spotify3DS. Passing the
SD card's credential path with `--creds` updates it in place instead.

To list currently available Spotify Connect devices and start or resume
playback on one of them:

```sh
python3 tools/list_devices.py
python3 tools/start_playback.py --device-id <DEVICE_ID>
```

The listing labels each device's state and displays the device ID needed by the
playback command. Starting playback resumes the account's existing playback
state; it does not select new content.

For hardware, leave the console at the Homebrew Launcher before running the
`--hw` command. You can also store its address in `.hwip` or export
`SPOTIFY3DS_IP`, then use `./dev.sh --hw`. Do not add `--test` for an interactive
lyrics evaluation; `--test` runs the automated smoketest and exits on its own.

## Development notes

Verification is headless: the app writes machine-readable verdicts to
`sdmc:/testresult.txt`, which `dev.sh` reads back from the host. A missing
`DONE` sentinel means the app hung or crashed — distinct from a clean failure.

Environment quirks worth knowing:

- Launch Azahar via `open -a Azahar.app --args -w <file>.3dsx`. Invoking
  `Contents/MacOS/azahar` directly pops a modal and hangs forever.
- `svcOutputDebugString` / `LOG_DEBUG` is **compiled out** of Azahar release
  builds. It is not a usable debug channel; write to the SD card instead.
- The GDB stub **halts the guest at boot** waiting for a debugger. Keep
  `use_gdbstub=false` unless actively debugging.
- Azahar rewrites `qt-config.ini` on quit, so only edit config while it is
  fully stopped.
- `Failed to find title id for ROM` in the emulator log is normal for `.3dsx`.

Interactive emulator and hardware runs stay open until they are closed through
HOME or the emulator controls. Only `dev.sh --test` enables automatic exit.

## Releases

`.github/workflows/setup-release.yml` packages the setup app for Windows x64
and ARM64, Linux x64 and ARM64, and a universal macOS app. It also builds the
3DS application as `Spotify3DS.cia` and `Spotify3DS.3dsx`. Its default `artifact`
destination is a seven-day dry run that creates no release. The `draft`
destination requires an existing tag and creates or updates an unpublished
draft release.

See [RELEASING.md](RELEASING.md) for the complete beginner-oriented process,
command reference, asset review steps, and troubleshooting guide.

```sh
# Dry run from main.
gh workflow run setup-release.yml \
  --ref main \
  -f ref=main -f destination=artifact

# Build a tag and upload the packages to a draft release.
gh workflow run setup-release.yml \
  --ref main \
  -f ref=v1.0.0 -f destination=draft -f release_tag=v1.0.0
```

The draft remains private until it is reviewed and published manually with
`gh release edit v1.0.0 --draft=false --latest` or through GitHub's release UI.
Confirm that both console packages and all five setup-app packages are attached
before publishing the release as latest.

## Why mbedTLS is bundled

The 3DS `sslc` system module maxes out at **TLS 1.1**, and Spotify requires
**TLS 1.2+**. So `httpc`/`sslc` cannot reach the API at all. Instead this app
links mbedTLS and speaks TLS in userspace over raw BSD sockets, bypassing
`sslc` entirely.

The embedded roots cover Spotify's API and image hosts plus LRCLIB. DigiCert
**G2 (RSA)** serves `api.spotify.com`, Starfield G2 serves
`accounts.spotify.com`, DigiCert **G3 (ECC)** serves `i.scdn.co` and
`image-cdn-ak.spotifycdn.com` plus `daylist.spotifycdn.com`, GlobalSign R3 serves
`mosaic.scdn.co` and
`image-cdn-fa.spotifycdn.com` plus `pickasso.spotifycdn.com`, Starfield G2 also
serves `open.spotify.com`, and GTS Root R4 serves `lrclib.net`. Omitting one
produces a confusing partial failure where the API works while one class of
images or lyrics never loads. Spotify can also return previously unknown image
hosts at runtime; add newly observed hosts to the monitor when appropriate.

### Live TLS monitoring

`.github/workflows/live-tls.yml` runs every Monday and can also be started
manually. `tools/tlscheck` reads the runtime root list from `source/net/tls.c`,
loads only those DER files, and checks all ten known hosts without using the
runner's system trust store. It requires TLS 1.2, verifies hostnames and chain
anchors, and fails when a certificate has fewer than 14 valid days remaining.
Transport failures are retried twice before the host is reported unreachable.

Run the same live check locally with:

```sh
go run ./tools/tlscheck
```

### Playlist display metadata

Recent collections and Library rows prefer Web API names and artwork. Missing
artwork is requested from `/v1/playlists/{id}/images`; remaining missing fields
are filled from Spotify's public `open.spotify.com/oembed` JSON (`title` and
`thumbnail_url`). The access token is never sent to oEmbed or image hosts.
Algorithmic mixes can therefore display correctly even when playlist metadata
returns 404. oEmbed does not expose track lists, snapshots, counts, or ownership;
snapshot validation and track browsing continue using the Web API only.

Enrichment runs one request per worker pass after playback work, selecting the
oldest due row so failed lookups cannot starve later entries. Temporary song
labels and album covers are not saved as playlist metadata. Expired real fields
remain visible while they refresh. `names.txt` retains
per-field sources and timestamps: ordinary API fields expire after 14 days and
oEmbed fields after one day. Generated image URLs are rediscovered daily for
Pickasso and every six hours for daylist. Old cache lines remain readable, and
playlist-library cache updates remain bulk-written. Library search includes recent-only
collections and removes duplicates by URI. Touch gestures latch the URI from
the rendered hit areas, so asynchronous search/list changes cannot play a
different row. SD metadata replacement uses a backup/recovery swap because the
console filesystem cannot rename over an existing destination.

Generated Pickasso/daylist artwork is JPEG, but its URL does not end in a content
hash. It uses namespaced full-URL cache keys and image TTLs of five days for
Pickasso and one day for daylist; ordinary content-addressed art retains its
existing cache layout. RAM thumbnail expiry preserves the SD entry's original
deadline, and download/decode/upload failures share bounded retries. Oversized
JPEG thumbnails are streamed down to their
texture dimensions without allocating a full decoded image.

Artwork cache entries record thumbnail versus large-cover decode quality in the
existing header flags. Large requests upgrade thumbnail entries; thumbnail
writes preserve fresh large entries, even when both use the same image URL.
The tier records the decode request, not a minimum pixel dimension, so a small
original does not trigger repeated downloads. Legacy textures larger than 64px
remain usable as large covers; ambiguous smaller entries upgrade once on demand.
The cache layout/version is unchanged, and replacements at a full shard do not
evict unrelated entries or inflate the shard count.

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
