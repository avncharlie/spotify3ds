# Spotify3DS

Spotify3DS lets you use your 3DS as a Spotify remote, with playback controls, library browsing, and synchronised lyrics.

https://github.com/user-attachments/assets/de96f312-1107-42eb-a012-5f5bdfdea7f2

- **Top screen:** album cover, artist, album/track name
- **Bottom screen:**
  - **Player** (default view): previous / play-pause / next, plus a seek scrubber
  - **Library:** recently played collections, playlists, saved albums, and their individual tracks. Both your library and any track list can be searched.
  - **Lyrics:** synchronised lyrics (from [LRCLIB](https://lrclib.net/))

You can use the L/R shoulder buttons from any screen to decrease or increase device volume in any screen.

## Screenshots

<p align="center">
  <img src="assets/main-screen.png" alt="Spotify3DS Player screen" width="23%">
  <img src="assets/library-screen.png" alt="Spotify3DS Library screen" width="23%">
  <img src="assets/tracks-screen.png" alt="Spotify3DS Tracks screen" width="23%">
  <img src="assets/volume-overlay.png" alt="Spotify3DS volume overlay" width="23%">
</p>

## Installation

Scan the QR code below in FBI under **Remote Install → Scan QR Code** to install the latest CIA directly.

<a href="https://github.com/avncharlie/spotify3ds/releases/latest/download/Spotify3DS.cia"><img src="assets/latest-release-qr.png" alt="QR code for the latest Spotify3DS CIA" width="240"></a>

Alternatively, you can manually install `Spotify3DS.cia` or `Spotify3DS.3dsx` from the project's [latest release](https://github.com/avncharlie/spotify3ds/releases/latest).

Now, you'll have to do a one-time setup to link Spotify3DS with your Spotify account. You can do this automatically using the setup app or manually with a Python script that generates a credentials file you copy over to 3DS's SD card.

### Automatic setup (recommended) - use the setup app

1. Download the `Spotify3DS-Setup` app for your computer from the [latest release](https://github.com/avncharlie/spotify3ds/releases/latest).
2. Follow the app's instructions to create a Spotify app and get your client ID. It will then show you a setup QR code.
3. Now open Spotify3DS and choose **Scan Setup QR** and scan this code.
   Spotify3DS will then log you in and you can start using the app.

This process writes a `creds.cfg` file to the `spotify` folder on your SD card. It
holds a token for your account rather than your password, but anyone who has it
can read your library and listening history and control your playback, so keep it
to yourself. You can revoke it at any time from your Spotify account settings
under **Apps**.

### Manual setup - use the `bootstrap_auth.py` script

1. Create an app in the [Spotify Developer Dashboard](https://developer.spotify.com/dashboard).
   Select **Web API**, add `http://127.0.0.1:8888/callback` as a redirect URI,
   and copy the app's Client ID. No client secret is needed.
   
   If you're having difficulty with this, look at [this issue](https://github.com/avncharlie/spotify3ds/issues/3#issuecomment-5298331372) to see some screenshots of the process.
2. Clone or download this repo, and run the following command from this project directory to generate your console credentials:

   ```sh
   python3 tools/bootstrap_auth.py --client-id <YOUR_CLIENT_ID>
   ```

   Your browser will open Spotify's authorization page. After approval, the
   script writes `creds.cfg` in the project directory.
3. Create a folder named `spotify` at the root of the 3DS SD card and copy the
   generated file to it. The resulting path must be `/spotify/creds.cfg`.
4. Spotify3DS should now be set up and connected to your account.

## Controls

- On any screen:
  - `L/R` decrease/increase volume
  - Tap D-pad left/right to skip
  - Hold D-pad left/right to seek forwards or backwards
  - `START` opens lyrics for the current track
  - `B` goes to the previous screen
  - Hold `L` and press `START` to exit

- Player
  - `A` play/pause
  - `X` to go to Library
  - `Y` show/hide cover art
  - Tap `LYRICS` in the header to open lyrics for the current song.
  - Tap a Recently Played tile to open its tracks; long press it to start playing it immediately.
- Library:
  - `A` start the selected collection, then play/pause
  - `X` open its tracks
  - `ZL/ZR` jump between recently played / playlists / albums
  - D-pad up/down to select a collection
  - Tap a row's play icon to start it, or its right chevron to open its tracks.
  - Tap magnifying glass in the header to search for a playlist/album in your library.
  - Press and hold the magnifying glass to view previous searches and search them again
- Tracks:
  - `A` play/pause the selected track
  - `X` queue the selected track
  - `ZL/ZR` see the previous / next page of tracks on big playlists. Go back from the start of playlist to go to its last page of tracks
  - D-pad up/down to select a track
  - Tap a row's play icon to start it, or its right queue icon to queue it.
  - Tap magnifying glass in the header to search the whole album or playlist by track name, artist, or album.
- Lyrics:
  - D-pad/Circle Pad up/down to scroll
  - Drag to scroll; tap `FOLLOW` for synchronised lyrics scrolling.
  - Tap a timestamped line to seek Spotify to it.

Upon searching a track within a playlist, the playlist's tracks are cached so that subsequent searches on that playlist are snappy.
A playlist's track cache is invalidated when the playlist is updated in any way.

## Lyrics provider

Spotify's Web API does not expose lyrics. Spotify3DS sends the current track,
artist, album, and duration to the open, keyless [LRCLIB](https://lrclib.net/)
API. Exact matches are preferred, search results are checked against track
metadata and duration, and synchronised lyrics are used only when their LRC
timestamps parse successfully. No Spotify access token is sent to LRCLIB.

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

## Setup app releases

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
