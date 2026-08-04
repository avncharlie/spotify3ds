# spotify3ds — handoff

A Spotify **remote** for the Nintendo 3DS / New 2DS XL. Audio plays on the
user's phone or desktop; this app controls Spotify's *active device* through the
official Web API. It is not a player and cannot become one — see "Things that
are impossible" below.

This document is written for another AI instance picking the project up cold.
It covers what exists, how to run and debug it, everything learned the hard way,
what is left to do, and the user's standing instructions.

---

## 1. The user's instructions

Verbatim intent, gathered across the project:

- **"I want you to develop a 3ds homebrew application/project… iterate by
  yourself if I give you a specification."** Autonomous iteration is the point.
- **"No. you need to establish a feedback loop from collecting information when
  running the project on the real 3ds so we can debug effectively."** This was
  said after I guessed at a bug twice and was wrong twice. **Do not guess. Build
  instrumentation and measure.** This is the single most important instruction
  in the project and it has paid off repeatedly.
- **"Keep going until it is all finished."** Don't stop at the easy parts.
- Design changes come from a Claude Design mockup (see §7). Implement what it
  specifies, but say so when the hardware or API makes a spec item impossible
  rather than silently dropping it.
- The user has a hacked New 2DS XL on the LAN and will run things on it. They
  read screenshots and give precise visual feedback (pixel-level: "there is no
  bottom left corner", "more of a gap on right side then left side").

### Working style that has worked

- Measure before optimising, and **verify the measurement means what you think**.
  I once "shrank" a response from 147KB to 540 bytes with `fields=` and nearly
  built on it — the 540 bytes was 50 empty `{}` objects. Check content, not size.
- Screenshot and *look* at UI changes. Several bugs (playlist showing the wrong
  cover, 8x8 thumbnails, unrounded corner) were invisible in logs and obvious on
  screen.
- When the user pushes back on a visual detail, they are almost always right and
  usually more precise than my reading of it.

---

## 2. Current state

**Working and verified on hardware:** playback control (play/pause/next/prev/
seek/shuffle/repeat), album art with SD caching, recently-played shelf with real
artwork, playlist library fetch, top-screen gradient from cover art.

**Last commits:**

```
212646d recents: treat a namecache hit without art as a miss
85ef5c8 art: shelf thumbnails, and fix playlists showing the wrong cover
8e1a39b ui: raise the list row name to 14px
5b40db2 spotify: 50-item recents, real playlist names, playlist library
72ef84a art: size textures per image instead of assuming the hero
bedff19 ui: tighten the gap after the repeat arrowhead
```

~7,300 lines of C across `source/`.

---

## 3. The debug loop — read this before anything else

This is the project's most valuable asset. It was built because guess-based
debugging failed.

### `./dev.sh` — the one entry point

```bash
./dev.sh                      # emulator, interactive, NO timeout (eyeball it)
./dev.sh --test               # emulator, automated, app self-exits, verdict
./dev.sh --hw                 # hardware, interactive, streams console stdout
./dev.sh --hw --test          # hardware, automated
./dev.sh --hw --ip A.B.C.D    # override device address (default 192.168.7.216)
```

Exit codes: `0` pass, `1` a step failed, `2` NOT LAUNCHED (3dslink couldn't
reach the console — it must be sitting on the Homebrew Launcher main menu),
`3` NO OUTPUT (uploaded but printed nothing → early crash before stdout was
redirected; check `SD:/spotify/log.txt`, which is written regardless).

### How the app reports

`source/testlog.[ch]`:

- `tl_log(fmt, ...)` — freeform line
- `tl_timing(fmt, ...)` — a measurement; prefixed `TIMING`
- `tl_step(name, pass, fmt, ...)` — a smoketest assertion, prints
  `PHASE=n STEP=name RESULT=PASS|FAIL detail=...`
- `tl_done()` — writes `DONE`, which `dev.sh` waits for

Output goes to **both** `SD:/spotify/log.txt` *and* stdout. Both matter:
netloading only gives you stdout, and a crash before `link3dsStdio()` only
leaves the file.

### Smoketest mode

Enabled by the presence of `sdmc:/spotify/.smoketest`, which `dev.sh --test`
creates and a bare `./dev.sh` removes. The app then auto-exits at frame 700 and
drives itself through scripted states (see `main.c`: art hidden at frames
480–600, list view at 300–420). **Any new probe must fire before frame 700** or
it never runs — I wrote one at 900 and it silently never reported.

### Emulator specifics (macOS)

- Azahar at `/Applications/Azahar.app`
- SD card root: `~/Library/Application Support/Azahar/sdmc/`
- Guest log: `~/Library/Application Support/Azahar/sdmc/spotify/log.txt`

Screenshotting the emulator, which is how UI work gets verified:

```bash
osascript -e 'tell application "System Events" to set frontmost of process "azahar" to true'
sleep 3
GEOM=$(osascript -e 'tell application "System Events" to tell process "azahar" to get {position, size} of window 1' | tr -d ' ')
IFS=, read -r X Y W H <<< "$GEOM"
screencapture -x -R"$X,$Y,$W,$H" out.png
```

**Gotchas that cost me time:**
- The window is Retina 2x: `screencapture -R` takes *points*, the resulting
  image is 2x pixels. Halve any offset you measure from an image.
- Bring Azahar frontmost and **wait** — otherwise you capture the terminal. This
  happened repeatedly.
- A stale `.smoketest` marker makes the app exit before you can capture. Use
  bare `./dev.sh`, which clears it. `rm .smoketest` in the project dir does
  nothing — the marker lives on the *virtual SD*.

### Emulator vs hardware fidelity

| Thing | Emulator | Hardware |
|---|---|---|
| Font metrics | `height=30 ascent=25` | **identical** — typography is safe to judge in Azahar |
| `threadCreate` on core 1 | allowed | **refused** (syscore; needs `APT_SetAppCpuTimeLimit`) |
| `PS_GenerateRandomBytes` | returns `0xD8E007F7` | works |
| `st_mtime` | — | **always 0**; `utime()` inert |
| SD write, misaligned | 15ms | **1792ms** (read-modify-write per block) |
| Art fetch (cold) | ~1200ms | ~1328ms |
| Art cache read (warm) | ~28ms | ~52ms |

Anything touching threads, the filesystem, or entropy **must** be verified on
hardware. Typography need not be.

---

## 4. Architecture

```
main.c            frame loop, view routing, input, optimistic UI
worker.c/.h       background net thread; everything blocking happens here
net/tls.c         mbedTLS over raw BSD sockets (bypasses sslc)
net/http.c        HTTP/1.1 + keep-alive pool (httppool.c)
spotify/auth      OAuth2 PKCE refresh; client_secret never on device
spotify/player    /v1/me/player + transport commands
spotify/recents   recently-played + /me/playlists
spotify/art       fetch, JPEG decode, Morton tiling, GPU upload
spotify/artcache  SD cache of pre-tiled textures
spotify/namecache SD cache of playlist uri -> name/owner/art
spotify/json      jsmn wrapper, dotted-path lookup
ui/screen_top     top screen (1A cover+text / 2A large-title)
ui/screen_player  bottom screen player (1B)
ui/screen_list    bottom screen list (1C) — to become the Library screen
ui/thumbs         render-thread LRU of thumbnail textures
ui/ui_draw        shared text/shape drawing, type scale
ui/touch          per-frame hit rects
```

### Threading

One worker thread on **core 0** (the app core). Core 1 is the syscore and
`threadCreate` there fails on real hardware while succeeding in Azahar — this
cost a full debugging session and is why `worker_set_fatal()` exists, so a dead
worker renders as a red error instead of an innocuous "Nothing playing".

All shared state is behind a `LightLock`. The render thread never blocks on I/O.
Worker stack is **96KB** and TLS handshakes want most of it — see §6.

### Worker tick order (this encodes priority)

```
poll → do_art → do_playlists → do_recents → do_thumbs
```

`do_playlists` before `do_recents` so it seeds the name cache — on the test
account that resolves every playlist in the history with **zero** extra
requests. `do_thumbs` last so a shelf of cache misses can never delay the hero
cover. **Preserve this order.**

### Optimistic UI

Spotify takes 300ms–1.5s to reflect a command. `opt_field` in `main.c` holds a
local value for `OPTIMISTIC_MS` so buttons feel instant.

---

## 5. Spotify API — measured facts

Endpoints used: `/v1/me/player` (+ `/play` `/pause` `/next` `/previous` `/seek`
`/shuffle` `/repeat`), `/v1/me/player/recently-played`, `/v1/me/playlists`,
`/v1/playlists/{id}`.

Scopes (already in `tools/bootstrap_auth.py`, no re-auth needed):
`user-read-playback-state user-modify-playback-state user-read-currently-playing
user-read-recently-played user-library-read playlist-read-private user-top-read`

### Hard-won specifics

- **`fields=` is IGNORED on `/me/player/recently-played`.** It returns 50 empty
  `{}` objects, not a trimmed payload. The full ~147KB body must come down and
  be parsed on-device (6905 tokens, 35ms measured). `fields=` **does** work on
  `/me/playlists` (52KB → 21KB) and `/playlists/{id}`.
- **50 recently-played tracks dedupe to ~4 distinct collections** on the test
  account. This is why the Library screen needs the playlists section.
- **`/me/playlists` returns `tracks: null`** regardless of `fields=`, so the
  design's "Playlist · 84 tracks" subtitle is unobtainable without one request
  per playlist. Using owner name instead.
- **A playlist's artwork is NOT in the recently-played item.** That item's
  `track.album.images` is the cover of whichever track was playing. The
  playlist's own image only comes from `GET /v1/playlists/{id}`. Getting this
  wrong showed "Good music" under an unrelated album cover.
- **Playlist images are irregular:** three sizes, or one with `width: null`, or
  **none at all** (2 of 49 on the test account). A placeholder path is required.
- Test account: 66 playlists (49 returned per page), 4 distinct recent
  collections.

### CDN / TLS — important

We ship our **own trust store** (`data/*.der`) because `sslc` caps at TLS 1.1.
There is no system CA bundle to fall back on. Different Spotify asset classes
use different CDNs with **different issuers**:

| Host | Root needed |
|---|---|
| `api.spotify.com`, `accounts.spotify.com` | DigiCert G2 (RSA) |
| `i.scdn.co` (album art) | DigiCert G3 (ECC) |
| `mosaic.scdn.co` (generated playlist mosaics) | **GlobalSign Root CA - R3** |

A missing root fails as `verify flags=0x00000008 The certificate is not
correctly signed by the trusted CA` — and only for that asset class, so the app
looks fine while some images silently never load. **If a new image host ever
appears, check its chain first.** Get roots from the macOS system store
(`security find-certificate -a -p /System/Library/Keychains/SystemRootCertificates.keychain`)
and verify against the live chain with `openssl verify -CAfile` before shipping;
don't download a root over an unverified path.

> `README.md` still documents only the two DigiCert roots — **it is stale** and
> should be updated to mention GlobalSign.

---

## 6. Bugs already fixed — do not reintroduce

| Symptom | Cause |
|---|---|
| App did nothing on hardware, "nothing playing" | worker thread on **core 1** (syscore). Azahar allows it, hardware refuses. |
| mbedTLS entropy failure | `PS_GenerateRandomBytes` unavailable in Azahar; made an optional threshold-0 source. `-lctru` must come **after** `-lmbedcrypto`. |
| Cache evicting arbitrary files | `st_mtime` is always 0 on 3DS. Replaced with a `use_seq` counter in the entry header. |
| Corrupt cache entries | implicit struct padding. Now `__attribute__((packed))` + `_Static_assert`. |
| Cache log key useless | used first 8 hex chars, but every art URL starts `ab67616d0000b273...`. Use the **tail**. |
| SD write 1792ms | two `fwrite`s misaligned the payload to block boundaries. One combined buffer → 111ms. |
| Every hardware run "HUNG" | `tl_done()` wrote `DONE` only to the file, unreadable over netload. Now also stdout. |
| Probe output invisible | ran before `link3dsStdio()`. |
| Playlist labelled as album | recently-played gives a context *uri*, never a name. Now `GET /v1/playlists/{id}`. |
| Playlist under wrong cover | used the enclosing item's album art (see §5). |
| Playlist mosaics never loaded | GlobalSign root missing (see §5). |
| Thumbnails decoded to 8x8 | decode loop searched for a scale that *covers* the target; a 60px source against a 64px target never covers, so it fell through to the smallest. Now takes the largest that **fits the texture**. |
| Stuck on wrong cover after the fix | `namecache` hit with an empty art field returned `true`, so the refetch never ran. A hit without art is now treated as a miss, so old caches self-heal. |
| App crashed before logging anything | `playlist_list` is ~32KB on a **96KB** worker stack alongside a TLS handshake. Both list structs are now heap/file-scope. **Never put these on the stack.** |
| Unrounded corner on repeat glyph | `ui_disc` at `r = t/2 = 1` cannot round anything. Use stepped arcs. |

### Standing constraints

- `RECENTS_MAX 16`, `PLAYLISTS_MAX 50` → `recent_list` ~10KB,
  `playlist_list` ~32KB. **Heap or file scope only.**
- json token pool ceiling is **32768** (needed ≥6905 for a 147KB body).
- `ARTCACHE_VERSION` must be bumped when payload layout, texture size, JPEG
  scale or pixel order changes.
- citro2d has **no circle primitive** (hence `ui_disc` triangle fans), **no
  letter-spacing** (hence `ui_text_tracked` per-glyph), and **no scissor**
  (hence the list draws rows first and paints the header over them).

---

## 7. Design source

Claude Design project `703b9c74-a2ca-44b3-a837-3d8c5d5abc63`, file
`Spotify 3DS UI.dc.html`. Access via the `claude_design` MCP
(`mcp__claude_design__read_file`). The project also contains an uploaded
snapshot of the source tree under `uploads/spotify3ds-main/` — **that snapshot
is stale**, read the real repo.

### Type scale (`ui/ui_draw.c`)

Sizes are CSS px converted at runtime via `scale = css_px / finf->height` from
`fontGetInfo` — **never hardcode a scale factor**.

```c
[TY_TITLE_L] = 34, [TY_TITLE] = 21, [TY_ARTIST_L] = 19,
[TY_ALBUM_L] = 15, [TY_ARTIST] = 14, [TY_ROW_NAME] = 14,
[TY_ALBUM]   = 12, [TY_ROW_SUB] = 12, [TY_MICRO] = 7 (overridden to 12 at runtime)
```

**Recurring theme:** the mockup gets hierarchy from **font weight**, and the 3DS
system font has only **one weight**, so size must carry it. Every mockup size
below ~11px has needed raising to stay legible. Expect to do this again.

---

## 8. What is left

### Immediate: the Library screen (the only unfinished mockup item)

`ui/screen_list.c` currently shows a single flat list of recents. The design
calls for two sections. Spec, from the mockup:

- 30px header, `#111`: left-pointing triangle + "Library"
- 20px caption band, `#111`, mono 7px letter-spaced `0.14em`, `#8a8a8a`:
  `RECENTLY PLAYED`
- 42px rows, `#171717` when selected: 30px art at `PAD_X`, then name (11px in
  the mockup — **use `TY_ROW_NAME`**) over subtitle (8px → `TY_ROW_SUB`)
- 1px hairline `#262626`, 6px above the next caption
- second caption band `PLAYLISTS` with a **right-aligned count** in `#5c5c5c`
- scroll indicator: 3px track `#262626` at `right: 3px`, thumb `#7a7a7a`

Behaviour the design states explicitly:
- **"If recently played fits entirely in the tiles, that first section is simply
  omitted and the list opens on the playlists."** With `SHELF_TILES = 4` and 4
  distinct recents on the test account, **this is the common case** — build and
  test it.
- The main shelf stays recents-only even when there are fewer than 4.

Wiring needed: `screen_list_args.art` is already a `const C2D_Image **`; fill it
from `thumbs_get(item.art_url)` exactly as `main.c` does for `pa.shelf[i]`.
`worker_get_playlists()` is ready and unused by the UI so far.

Note the design writes subtitles as `Album · Artist` (middle dot) while the code
uses `Album - Artist`. **Verify the 3DS system font has U+00B7 before
switching** — this is the kind of thing that renders as tofu.

### Then

- **Placeholder art** for the 2-of-49 playlists with no image. Currently draws a
  flat `CLR_THUMB_BG` rect; something better (initial letter? generic glyph?)
  would read as intentional.
- **Hardware verification pass** for phases 12/13/14 — much has only been
  checked in Azahar.
- **Interactive hardware run** to tune `TOUCH_SLOP` (currently 8; resistive
  panels jitter more than capacitive) and judge whether hit rects are findable
  without visible button boxes. This is a *feel* question the harness cannot
  answer — the user must hold the device.
- **Update `README.md`** for the GlobalSign root.

### Known-good latency (do not regress)

| | Emulator | Hardware |
|---|---|---|
| Hero art cold | ~1232ms fetch + 79ms decode | 1328ms |
| Hero art warm (cache hit) | ~28ms | 52ms |
| Thumbnail cold | 28–45ms fetch, 3–5ms decode | not yet measured |
| recently-played parse | 147KB / 6905 tokens / 35ms | not yet measured |
| playlists parse | 21KB / 1111 tokens / 4ms | not yet measured |

Cover-art latency went 4871ms → 1983ms early on purely through measurement.
**Keep measuring.**

---

## 9. Things that are impossible

Don't spend time on these:

- **Playing audio on the 3DS.** Spotify has no streaming API that permits it;
  the Web API only controls playback elsewhere. This is a remote by necessity.
- **Free Spotify accounts.** Playback-control endpoints return 403. Premium only.
- **`fields=` on recently-played** (§5).
- **Track counts from `/me/playlists`** (§5).
- **TLS via the system `sslc`** — caps at TLS 1.1, Spotify needs 1.2+.

---

## 10. Environment

- macOS, zsh. Project at `/Users/alvin/Documents/Tech/3ds/spotify3ds`
- devkitARM 16.1.0 + libctru, citro2d/citro3d, mbedTLS 2.28.8, libjpeg-turbo
- Console IP `192.168.7.216` (New 2DS XL)
- `creds.cfg` (gitignored) holds `client_id` and `refresh_token`; must also be
  copied to `SD:/spotify/creds.cfg` on the console — **3dslink copies only the
  .3dsx**, which is the usual first-run-on-hardware failure.
- `tools/bootstrap_auth.py` does the PKCE dance; `tools/list_recent.py` is the
  reference implementation for the recents/playlists logic — the on-device
  output should match it exactly, and is a good oracle.

Useful one-liner for API experiments (refresh token → access token):

```python
import json, urllib.request, urllib.parse
cfg = dict(l.strip().split('=', 1) for l in open('creds.cfg')
           if '=' in l and not l.startswith('#'))
cfg = {k.strip(): v.strip() for k, v in cfg.items()}
d = urllib.parse.urlencode({'grant_type': 'refresh_token',
                            'refresh_token': cfg['refresh_token'],
                            'client_id': cfg['client_id']}).encode()
tok = json.load(urllib.request.urlopen(urllib.request.Request(
    'https://accounts.spotify.com/api/token', data=d)))['access_token']
```

Note this leaves the account in whatever state you set — I once left repeat-one
on after forcing that state for a screenshot. Put it back.
