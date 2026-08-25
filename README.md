# Spotify3DS

A Spotify remote for the Nintendo 3DS / New 2DS XL.

- **Top screen:** album cover, artist, album/track name
- **Bottom screen (touch):** previous / play-pause / next, plus a seek scrubber
- **Library:** recently played collections, playlists, saved albums, and their
  individual tracks
- **Lyrics:** synchronized lyrics (from LRCLIB)

Use the L/R shoulder buttons from any screen to decrease or increase active device's volume.

Audio keeps playing on your phone or desktop — this controls Spotify's
*active device* via the official Web API. It is a remote, not a player.

https://github.com/user-attachments/assets/de96f312-1107-42eb-a012-5f5bdfdea7f2

## Screenshots

<p align="center">
  <img src="assets/main-screen.png" alt="Spotify3DS Player screen" width="23%">
  <img src="assets/library-screen.png" alt="Spotify3DS Library screen" width="23%">
  <img src="assets/tracks-screen.png" alt="Spotify3DS Tracks screen" width="23%">
  <img src="assets/volume-overlay.png" alt="Spotify3DS volume overlay" width="23%">
</p>

## Installation

You need Spotify Premium and a homebrew-enabled 3DS. The cross-platform
Spotify3DS Setup app handles authorization and transfers credentials directly
by QR code; Python and SD-card copying are only needed for advanced setup.

Download `Spotify3DS.cia` from the project's
[latest release](https://github.com/avncharlie/spotify3ds/releases/latest).
You can also scan this QR code in FBI under **Remote Install → Scan QR Code**
to install the latest CIA directly:

<a href="https://github.com/avncharlie/spotify3ds/releases/latest/download/Spotify3DS.cia"><img src="assets/latest-release-qr.png" alt="QR code for the latest Spotify3DS CIA" width="240"></a>

To update an existing installation, re-scan the same QR code in FBI. It always
points to the CIA from the latest release.

### Recommended setup app

1. Download `Spotify3DS-Setup` for your computer from the latest release.
2. Follow its illustrated Spotify Developer Dashboard guide and paste your
   public Client ID. The wizard preserves the complete scope set used by the
   advanced bootstrap script.
3. Authorize Spotify in the browser. After validating refresh and API access,
   the wizard displays a credential QR code.
4. Open Spotify3DS and choose **Scan Setup QR**. The app verifies the QR,
   atomically installs `/spotify/creds.cfg`, validates it with Spotify, and
   enters the Player without a relaunch.

The credential QR contains a plaintext bearer credential. Do not photograph,
share, stream, or save it.

### Advanced manual setup

1. Create an app in the [Spotify Developer Dashboard](https://developer.spotify.com/dashboard).
   Select **Web API**, add `http://127.0.0.1:8888/callback` as a redirect URI,
   and copy the app's Client ID. No client secret is needed.
   
   If you're having difficulty with this, look at [this issue](https://github.com/avncharlie/spotify3ds/issues/3#issuecomment-5298331372) to see some screenshots of the process.
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

## Controls

> **Global volume:** Press `L/R` on any screen to
> decrease/increase volume in 5% steps. Hold either button to keep changing it.
> Press `START` from Player, Library, or Tracks to open lyrics for the current
> track.
> Hold `L` and press `START` to exit the application from any screen.

- Player: `A` play/pause, `L/R` volume down/up, `X` Library, and `Y` show/hide
  cover art. Press and release D-pad left/right to skip, or hold either
  direction for repeated 10-second seeks. The touch previous/next controls work
  the same way. Tap `LYRICS` in the header to open lyrics for the current song.
  Tap a Recently Played tile to open its tracks; hold it for 600 ms to start the collection
  immediately. Tracks opened from this shelf return directly to Player with
  `B` or the top-left back control.
- Library: tap a row's play icon to start it immediately, or its right chevron
  to open its tracks. The current playing row shows pause in the same cell.
  D-pad up/down selects a collection, `A` starts it and then toggles play/pause,
  `X` opens its tracks,
  Press and release D-pad left/right to skip the previous/next song, or hold
  either direction for repeated 10-second seeks. `SELECT` toggles play/pause, `L/R` changes volume,
  `ZL/ZR` jumps sections, and `B` returns. Tap `FIND` to filter saved albums and
  playlists by name, artist, or owner using the system keyboard; tap the filter
  strip's `X` to clear it.
- Tracks: tap a row's play icon to start it immediately; the current playing
  track shows pause in the same cell. D-pad up/down selects a song, `A` plays
  it and then toggles play/pause, `L/R` changes volume, `ZL/ZR` changes 50-track
  pages, pressing and releasing D-pad left/right skips the previous/next song,
  and holding either direction performs repeated 10-second seeks. `SELECT` toggles play/pause, `X`
  queues the selected song, and `B` returns to the Library. Tap a
  row's right queue icon to queue it directly. Moving past a page boundary with
  D-pad up/down also loads the adjacent page automatically. Playlist pagination
  wraps: `ZL`/Up on the first page opens the last page, and `ZR`/Down on the last
  page opens the first page. Tap the magnifying glass in the header to search
  the entire album or playlist by track name, artist, then album. Search scans
  50 source items at a time, retains the best 500 matches, and paginates results
  locally. The first `B` clears or cancels search; a second `B` leaves Tracks.
- Lyrics: press `START` from Player, Library, or Tracks. `B` or the top-left
  back control returns to the screen that opened it. Drag or use D-pad/Circle
  Pad up/down to scroll smoothly, then tap `FOLLOW` to resume synchronized
  scrolling. Press and release D-pad left/right to skip tracks, or hold either
  direction for repeated 10-second seeks. Tap a
  timestamped line to seek Spotify to it, and press `X` to retry a failed
  lookup. `SELECT` toggles play/pause and `L/R` changes volume. Plain lyrics
  remain scrollable but cannot seek or auto-follow. `START` has no action while
  Lyrics is open. While LRCLIB is loading, the screen shows downloaded KB and
  an animated bar; responses with a known size show downloaded and total KB.

Volume changes step by five; hold either shoulder button to keep stepping. A
transient bottom-screen overlay shows the current level. Devices
that report no remote-volume support get an explanatory overlay instead of an
API command.
On original 3DS/2DS models without `ZL/ZR`, use touch or D-pad boundary paging;
Library sections remain reachable by scrolling.

Library and Tracks mark Spotify's current context or song with a green title,
edge, row tint, and an equalizer over its artwork. The bars animate while
playing and remain fixed while paused. The same equalizer marks the current
collection on the Player shelf, which is always pinned to the leftmost tile.

Ordinary browsing keeps only the current 50-track page in RAM, always fetched
fresh from Spotify, so playlist edits show up the moment a page is opened.

Searching a collection is the expensive case: it walks every page, which is
about 18 seconds for a 1791-track playlist. Nonmatches are discarded as they
arrive and at most 500 ranked matches are retained (about 383 KiB), with
results appearing as they are found rather than at the end. The searchable text
is also packed into an index - roughly 112 bytes a track, so that same playlist
occupies about 196 KiB - which is kept in memory and written to
`sdmc:/spotify/searchidx`, capped at 100 playlists.

Later searches are answered from that index, and Spotify's `snapshot_id` is
checked afterwards to confirm it still matches the playlist; if it does not,
the collection is walked again and the results on screen are replaced. Nothing
in the store is load-bearing - a missing, damaged or hand-deleted entry simply
reads as a miss - so deleting any or all of those files at any time is safe.

Album-cover thumbnails use the content-addressed SD artwork cache.

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

The embedded roots cover Spotify's API and image hosts plus LRCLIB. DigiCert
**G2 (RSA)** serves `api.spotify.com` and `accounts.spotify.com`, DigiCert
**G3 (ECC)** serves `i.scdn.co`, GlobalSign R3 serves `mosaic.scdn.co`, and GTS
Root R4 serves `lrclib.net`. Omitting one produces a confusing partial failure
where the API works while one class of images or lyrics never loads.

## Lyrics Provider

Spotify's Web API does not expose lyrics. Spotify3DS sends the current track,
artist, album, and duration to the open, keyless [LRCLIB](https://lrclib.net/)
API. Exact matches are preferred, search results are checked against track
metadata and duration, and synchronized lyrics are used only when their LRC
timestamps parse successfully. No Spotify access token is sent to LRCLIB.

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
./dev.sh --hw --ip 192.168.1.x  # build and netload to a 3DS
./tests/run_host_tests.sh  # deterministic cache, HTTP, JSON, and lyrics tests
go test ./...              # setup wizard, scopes, and credential QR protocol
```

### Setup app releases

`.github/workflows/setup-release.yml` packages the setup app for Windows x64
and ARM64, Linux x64 and ARM64, and a universal macOS app. Its default
`artifact` destination is a seven-day dry run that creates no release. The
`draft` destination requires an existing tag and creates or updates an
unpublished draft release.

See [RELEASING.md](RELEASING.md) for the complete beginner-oriented process,
command reference, package verification steps, and troubleshooting guide.

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
This workflow does not upload `Spotify3DS.cia`; attach the CIA separately before
publishing the release as latest so the latest-release install link keeps
working.

For hardware, leave the console at the Homebrew Launcher before running the
`--hw` command. You can also store its address in `.hwip` or export
`SPOTIFY3DS_IP`, then use `./dev.sh --hw`. Do not add `--test` for an interactive
lyrics evaluation; `--test` runs the automated smoketest and exits on its own.

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

### Emulator auth

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

Spotify refresh tokens expire six months after authorization. When Spotify3DS
shows **Authorization expired**, authorize again in Spotify3DS Setup and scan
the replacement QR, or use the advanced bootstrap command and replace
`creds.cfg` manually.

PKCE means **no `client_secret` exists**, so no secret ever reaches the console
or the repo, and the script never sees your password. The 3DS thereafter only
performs `grant_type=refresh_token`.

> **Note:** that refresh token is a plaintext bearer credential on the SD card.
> Homebrew has no secure storage — any encryption key would have to sit beside
> the ciphertext. Revoke anytime at <https://spotify.com/account/apps>.

Spotify3DS vendors the ISC-licensed [quirc](https://github.com/dlbeer/quirc)
decoder for setup QR scanning. The scanner follows the established FBI and
Universal-Updater camera path: outer camera, 400x240 RGB565 preview, grayscale
conversion, and mirrored-code retry.

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

Interactive emulator and hardware runs stay open until they are closed through
HOME or the emulator controls. Only `dev.sh --test` enables automatic exit.

## Layout

```
source/
  main.c          entry point, render loop
  testlog.[ch]    headless test + logging harness
  net/            sockets + mbedTLS transport
  spotify/        auth, API calls, JSON
  ui/             screens, touch hit-testing
tools/            host-side authentication and Spotify API utilities
tests/            host-side cache and HTTP framing tests
```
