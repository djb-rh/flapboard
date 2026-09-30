# FlapBoard — split-flap sign + photo frame for the M5Stack Tab5

Name "FlapBoard", in `~/Developer/flapboard` (not the iCloud Desktop).
GPL-3.0-or-later, expected to go public. Plan written 2026-09-30; decisions
settled the same day (§15). Nothing has been built yet.

## 1. What it is

A Tab5 that acts like a physical split-flap board (Solari / Vestaboard):

- The grid is set from the web: rows, columns, flap size, font, colours.
- Optional images on the left and right, uploaded from the web.
- Every flap turns forward through its drum at one fixed speed, so when the
  message changes each cell stops at its own time, the way a real board does.
- Sound that follows the flaps: one clack per flap, which adds up to the
  familiar rattle when many cells turn at once.
- Content comes from a message library on the SD card (random or in order, on
  a timer), a scheduler, MQTT, a clock and weather.
- A **photo mode** that uses the same selection, schedule, random-on-a-timer
  and file manager model as the Pi photo frame
  (`~/Desktop/Claude/pi-zero-photo-frame`).
- MQTT / Home Assistant: send messages, turn the display on and off, switch
  mode.
- A hidden info sheet, opened by touch: IP address, Wi-Fi, firmware versions,
  schedule state and a mode switch. None of it shows on the main screen.

## 2. Hardware and what we reuse

| Part | Tab5 | Notes from earlier projects |
|---|---|---|
| SoC | ESP32-P4, 32 MB PSRAM | Internal RAM is the scarce resource, not PSRAM |
| Display | 5" 1280×720 MIPI-DSI, native portrait | Draw straight to the framebuffer with dirty rects; never push a full-screen PSRAM canvas (750 ms) |
| 2D engine | PPA (scale / rotate / blend / fill) | Proven at 60 fps for NES scaling |
| JPEG | P4 hardware JPEG decoder | Baseline only; progressive files need software decode or resizing on upload (§9) |
| Touch | GT911 or ST7123, depending on unit revision | Never send raw I2C to touch or expander chips |
| Audio | ES8388 codec + amp through M5Unified Speaker | Callback-fed streaming; power-cycle if sound dies after a crash |
| Wi-Fi | ESP32-C6 over SDIO (esp-hosted), 2.4 GHz only | Firmware must match the host (§3); the radio can only be started once per boot |
| RTC | RX8130 | The schedule can run from the RTC at boot once NTP has set it at least once |
| SD | microSD, FAT32 (exFAT is off in the prebuilt IDF) | Use POSIX `readdir`, not `File::openNextFile` |

**Code borrowed from T48-for-Tab5 (`~/Developer/tab5-burner`, GPL-3):**
captive portal, Wi-Fi link watchdog, C6 firmware report and update
(`ESP_HostedOTA`), `sdcard`, web server with uploads and zip handling,
settings, and the `tools/tab5.py` test harness (serial commands, tap,
screenshot, put/get).

**From Tabulous5:** `audio`, `wallclock` (RTC + NTP), `orientation`, and the
panel/framebuffer screenshot commands.

**From the Pi frame (ported from Python to C++):** the config schema,
`scheduler.py` (overrides → rules → default, midnight wrap, clock-trust gate,
"broken rule matches nothing"), sleep rules, the selection tree, sort orders,
the MQTT/HA design (asymmetric OFF latch, discovery, LWT, QoS 1), the file
manager JSON API and its rules, and message behaviour (`{"text","seconds"}`,
not persisted).

**Licence:** GPL-3.0-or-later, because it takes code from GPL-3 projects.

## 3. Phase 0 — check the hardware and firmware first

Done before any feature work, on the actual unit:

1. **P4 chip revision**: `esptool.py chip_id`. Record it; pioarduino builds for
   v1.x and v3.x differ.
2. **C6 (esp-hosted) firmware against the host version**: flash a small probe
   built from the burner's Wi-Fi screen code. It prints both versions. If they
   differ, update over Wi-Fi with `ESP_HostedOTA`, as was done on 2026-09-28
   (1.4.1 → 2.12.x). A mismatch is the known cause of wedged links and failed
   uploads.
3. **Panel and touch controller variant** (ILI9881C+GT911 or ST7123), read
   through M5GFX. Pin the M5GFX / M5Unified versions that drive it.
4. **pioarduino platform and Arduino core versions**: take the current stable
   release and pin it in `platformio.ini`.
5. **SD card**: must be FAT32, with enough free space; check read speed with
   a large photo.
6. **RTC**: confirm it is sane, then set it from NTP.
7. Write down the results in `docs/hardware.md`. The Settings → System page
   shows the same values from then on, including a "C6 update available"
   button.

## 4. Architecture

```
core 1:  render task (60 Hz) ── flap engine ── glyph cache ── framebuffer / PPA
         audio task ─────────── click mixer (fed flip events from the engine)
core 0:  net: web server, MQTT, weather fetch, NTP, link watchdog
         content task (1 Hz): scheduler → playlist → message/photo → renderer queue
         touch/UI: info sheet, wake on tap
```

- **Content priority:** MQTT message (optional `seconds`) → scheduled item →
  default. The HA OFF latch sits above all of them.
- **Storage:**
  - `/littlefs/config.json`: every setting, merged with defaults on load, as
    the Pi does.
  - NVS: secrets (Wi-Fi password, MQTT password) and the HA latch.
  - SD `/flapboard/…`: messages, photos, side images, sounds, fonts, backups.
  - The config lives in flash, so pulling the card does not wipe the sign.
- **Rules carried over:**
  - Keep IDF logging off at runtime.
  - Put big stacks and buffers in PSRAM (`xTaskCreatePinnedToCoreWithCaps`).
  - Keep Wi-Fi on for the whole session and never stop it.
  - The watchdog restarts the device when the link wedges.
  - The main loop runs under the task watchdog.

## 4a. Portability: the sign is a reusable library

The split-flap widget will be reused in other projects, so it lives as its own
library, `lib/flapcore/`, with no Tab5, M5, Arduino or IDF dependencies:

- **flapcore** (plain C++17): drum/charset tables, the per-cell flip state
  machine, timing and jitter, layout maths, the message formatter and
  templates, and the list of flip events. It is unit-tested natively.
- **Backends are small interfaces** the host project provides:
  - `FlapSurface`: blit a rect, fill, and an optional scaled blit. The Tab5
    implements it with the framebuffer and PPA. Other boards can use LovyanGFX
    or M5GFX, or LVGL canvas.
  - `FlapAudioSink`: receives flip events with timestamps (optional).
  - `FlapClock`: monotonic milliseconds.
- **The glyph renderer** (stb_truetype into half-glyph caches) is a separate
  optional module, so a small board can supply pre-rendered bitmap fonts
  instead.
- **The click mixer** is its own module that turns flip events into PCM, with
  no audio driver inside it.
- Scheduler, MQTT, weather and photo code are app code, not part of the
  widget. The scheduler is also plain C++ so it could be shared later.
- Rule: flapcore allocates through a caller-supplied allocator (PSRAM on the
  Tab5) and never logs, so it can drop into ESPHome components, CrowPanels or
  a Mac/iOS build.

## 5. The split-flap engine (the core of the project)

**Drum.** Each cell has an ordered character drum, **defined as data**
(a named charset table in config or on SD), so other character sets can be
added later without code changes. v1 ships the Vestaboard-style set: space,
A–Z, 0–9, `. , : ; ! ? ' " - / & @ # $ % ( ) + =`, plus colour tiles (red,
orange, yellow, green, blue, violet, white, black), written as `{R}` `{O}`
`{Y}` `{G}` `{B}` `{V}` `{W}` `{K}` in messages. Lowercase is not on the v1
drum, but the formatter has a "keep case" path, so a lowercase drum only needs
a new table. Flaps only move **forward**. To go from `S` to
`B`, a flap passes T…Z, the digits and the punctuation, then wraps to A and B.
Characters not on the drum fall back to space.

**Timing.**
- Each flap flip takes a fixed `flip_ms` (default about 70 ms; adjustable).
- A cell's travel time is `(steps forward) × flip_ms`, so cells finish in a
  natural, staggered order.
- Optional realism, each with a setting:
  - a ±3 % speed difference per module, fixed per cell like real motors;
  - a small random start delay;
  - left-to-right "wave" start.
- A cell that is already right does not move. The one exception is a
  "full spin" option that sends every cell round once, which some boards do.

**Look of one flip** (the per-cell state machine uses an angle from 0 to 180°):
- 0–90°: the upper half of the current character falls and foreshortens
  (scaled by cos θ, darkened by angle). The upper half of the next character
  shows behind it.
- 90–180°: the lower half of the next character falls onto the lower half of
  the current one.
- Every frame also draws the hinge gap line, rounded flap corners, a thin
  shadow under the falling flap, and the bezel.

**Rendering cost.**
- On a layout change, each glyph is rendered once (with stb_truetype, at any
  size) into top-half and bottom-half caches in PSRAM. They are stored
  **pre-rotated into panel-native orientation**, so a blit is a plain copy.
- Falling flaps are row-resampled from the cache (or PPA-scaled for large
  flaps).
- Only the rows of cells that are moving are redrawn. A still board costs
  nothing.
- Two framebuffers are swapped on vsync to avoid tearing, the same method as
  the NES path.
- Target: 60 fps with every cell of a 22×6 board flipping. Phase 2 measures
  this at the largest grid.

**Mac first.** The engine (drum, timing, state machine) is plain C++ with no
hardware dependencies:
- `pio test -e native` covers ordering, wrap-around, stop times and jitter
  bounds.
- A Mac runner renders a message change to PNG frames or a GIF, so the look
  can be tuned without flashing. This is the same approach as the Tabulous5
  arcade work.

**Text formatting.**
- Lines are separated by newline or `|`.
- Per-message options: alignment (left / centre / right), vertical centring,
  word wrap, and upper-casing. Lowercase is only kept if the drum has it.
- Template tokens: `{time}`, `{time:%I:%M}`, `{date:%a %b %d}`, `{temp}`,
  `{hi}`, `{lo}`, `{cond}`, `{wind}`, `{hum}`, `{ip}`.
- We write **our own strftime subset**, because `%-I` fails on these
  toolchains, as the pool panel showed.

**Clock and weather are templated messages.**
- Clock mode is a message like `{time}` / `{date}` that re-renders every
  minute. Only the digits that changed flip.
- Weather is the same idea, with layouts such as "NOW 72° SUNNY / HI 81 LO 64".

## 6. Layout (all set from the web)

- Settings: rows, columns, flap size in px or **auto-fit**, gap, bezel,
  theme (Solari black/white, Vestaboard, amber, custom colours), font (bundled
  OFL fonts plus TTFs uploaded to SD), and orientation (**landscape only in
  v1**, either way up; flapcore's layout maths stays orientation-agnostic).
- **Side images:** left and right, each optional.
  - Width is auto (whatever the board doesn't use) or a fixed percentage.
  - Fit is contain or cover, with a background colour.
  - Uploaded PNG/JPEG files are resized in the browser to the exact slot size
    before upload.
- The layout maths lives in one shared module. The web page runs a
  **JavaScript copy of it for a live preview**, so you can see what fits
  before saving. A native test keeps the C++ and JS versions matching.

## 7. Sound

- **Audition before choosing.** Before the mixer is built, put together a
  shortlist to listen to:
  1. **Generated** clacks from `tools/make_sfx.py` (filtered noise burst +
     resonant body + a second "settle" tick), several variations.
  2. **Free samples**: search for CC0 or similarly permissive recordings of
     real split-flap boards (Freesound CC0 filter, and so on). List each
     candidate with its source, licence and size, and **ask before downloading**.
  3. A small local HTML page plays each option alone, and as a simulated
     "22×6 board changing" mix, so they can be compared the way they will
     actually sound.
- The chosen set becomes four to six short clack variants, as WAVs on SD. Only
  a CC0 or generated set is committed to the repo.
- The engine sends a timestamped event for each flap flip. The mixer
  (22.05 kHz, callback-fed) starts a voice for each event, picking a random
  variant with small random gain and pitch changes.
- There is a voice cap of about 16–24. Above the cap, a looped "rattle" bed
  fades in, scaled by the number of flips per second. This keeps 100+
  simultaneous cells from clipping or running out of CPU.
- Settings: volume, mute, a quiet-hours schedule (same rule format), and
  "sound only on message change" (so the clock does not click every minute
  unless you want it to).

## 8. Content, playlists and scheduler

**Message library.**
- Stored as `/flapboard/messages/*.txt`, one message per block separated by
  blank lines, with optional per-message options in a header line.
- Each file is a group, for example "quotes", "menu" or "birthdays".
- The web page has a message editor (with the live preview) and a
  **Send now** button.

**Sources a rule can choose:** message files or individual messages, clock,
weather, a photo folder selection, or a fixed message. Each rule also sets an
order (random / sequential), a dwell time, and **a mode (sign or photo)**, so
the schedule can switch the Tab5 between sign and photo frame.

**Scheduler.**
- Ported from the Pi's `scheduler.py`: one-off date overrides → weekly rules →
  default, with first match winning inside each layer.
- Unchanged rules: windows that cross midnight, a broken time matches nothing,
  and an empty rule is skipped.
- The Pi's tests are ported as native unit tests.
- **Clock trust:** the RTC plus an NVS "NTP has set it at least once" flag.
  This is better than the Pi, which has no RTC: the schedule works right after
  a reboot, even with Wi-Fi down.
- **Sleep rules** turn the screen off: backlight off, and the render task
  paused. A tap can wake it for N minutes if that is enabled.
- The status page shows which rule is active and why, like the Pi.

**Random selection** never repeats the last item. Sequential play continues
from the previous item, not from an index, as the Pi does.

## 9. Photo mode

This is the same model as the Pi frame, so the settings will look familiar:

- Selection tree with checkboxes, sort orders, dwell, schedule rules and
  overrides, sleep rules, and a clock/date overlay.
- MQTT messages appear over the photo.
- Transitions: cut, dissolve and slide, using PPA blend and fill.
- Blur-fill background: PPA scales down to about 1/16 and back up. It is
  cheap, and close to the Pi's blur.

**Decoding.**
- The **browser resizes photos at upload** to at most 1280×720 (or 1920
  wide), as baseline JPEG, and makes the thumbnail too.
- That keeps every file within what the hardware decoder handles, makes
  uploads small (which matters with the esp-hosted upload wedging), and
  handles HEIC from iPhones in Safari.
- The device still decodes anything else it finds on the card, using a
  software fallback (JPEGDEC / PNGdec), so photos copied straight onto the
  card still work.
- The next photo is decoded during the current one's dwell.

**File manager.**
- A port of the Pi's `/library` JSON API: list, download, thumb, upload,
  mkdir, delete and rename.
- Also ported: the web UI (multi-select upload with progress, free space
  shown, and a warning when it is low), `name (2).jpg` on a name collision,
  dot-temp files plus atomic rename, and hiding `.DS_Store` and `._*` files.
- The same manager covers messages, side images, sounds and fonts, as tabs or
  root folders.
- The JS client uploads **one file at a time with retry**, because of the
  known wedge after N uploads.

**v1 shows photos from the SD card only.** A PhotoServer client is left for
later. The Pi frame supports one, and the design keeps a `content_mode` switch
so it can be added.

## 10. Weather, location and timezone

- **Location:** search by city or ZIP using Open-Meteo geocoding (free, no
  key), which gives latitude, longitude and an IANA timezone. You can also
  enter lat/lon by hand.
- **Weather:** Open-Meteo forecast every 15–30 minutes. The last good result
  is kept, so a failed fetch does not blank the fields. Units are °F/°C and
  mph/kmh. Tokens are listed in §5.
- **Timezone:**
  - The ESP32 needs POSIX TZ strings, so the firmware carries an
    IANA → POSIX table (posix_tz_db, MIT). The web page shows IANA names and
    the device stores both.
  - The zone is suggested automatically from the weather location.
  - DST is handled by the POSIX rules.
  - NTP servers can be changed; the RTC is written after each sync.
- **Risk:** HTTPS needs roughly 40 KB of internal RAM for mbedTLS, and Wi-Fi
  DMA competes for the same RAM. Mitigations, in order:
  1. move mbedTLS allocations to PSRAM (needs a pioarduino `custom_sdkconfig`
     build);
  2. run only one TLS session at a time;
  3. fetch weather while nothing else is using the network.

  Phase 5 tests this early.

## 11. MQTT / Home Assistant

Uses the Pi and Lixie pattern with the IDF's esp-mqtt: retained discovery
republished on every connect, LWT availability, QoS 1, 5–60 s backoff, and
publish-on-change plus a 30 s heartbeat.

| Entity | Topic | Behaviour |
|---|---|---|
| light "Display" | `flapboard/<id>/display/set` | ON/OFF with the asymmetric latch: OFF wins over everything and survives a reboot; ON releases the latch |
| text "Message" | `flapboard/<id>/message` | Bare text, or JSON `{text, seconds, align, sound, flip_ms}`; not persisted |
| select "Mode" | `flapboard/<id>/mode/set` | sign / photo / clock / weather / schedule (auto) |
| button "Next" | `…/next` | Moves the playlist on |
| number "Volume" | `…/volume/set` | 0–100 |
| sensors (diagnostic) | — | IP, RSSI, SSID, uptime, free heap, C6 fw, active rule, latch state |

The broker password is never sent back to the web page. A blank field keeps
the stored one.

## 12. Touch and the info sheet

- **Decided: a long press (about 1 s) anywhere** opens the info sheet, so a passing
  touch doesn't. A tap only wakes the screen when it is asleep.
- The sheet shows: hostname`.local` and IP (large, easy to read from across a
  room), a QR code for the web UI, SSID and RSSI, MQTT state, NTP/RTC state,
  the active schedule rule and why, SD free space, P4 and C6 firmware
  versions, and uptime.
- It has buttons: **Sign / Photo / Clock / Auto**, Next, volume, brightness,
  and "Show IP on the board" (which flips the IP onto the flaps).
- It closes itself after 30 s.
- Touch targets are at least 80 px, based on the Tabulous5 lesson.

## 13. Web UI

- A single-page vanilla JS app, gzipped on LittleFS.
- Pages:
  - Status
  - Display (layout + live preview)
  - Messages
  - Schedule (rules, overrides, sleep, quiet hours)
  - Photos (file manager + selection)
  - Clock & Weather
  - Sound
  - MQTT
  - Network
  - System: versions, C6 update, **OTA firmware upload** (ota_0/ota_1
    partitions), backup/restore of config + messages as a zip, and reboot.
- First boot uses the captive portal (`FlapBoard-XXXX`) to choose Wi-Fi and a
  name. The mDNS hostname is `<name>.local`.

## 14. Phases, each checked on hardware

| # | Phase | Done when |
|---|---|---|
| 0 | Hardware/firmware audit (§3) | C6 fw matches host; `docs/hardware.md` written |
| 1 | Skeleton: project, portal, watchdog, SD, web server, config, `tab5.py` harness | Joins Wi-Fi, serves Status, survives 50 sequential uploads |
| 2 | flapcore library (Mac first), then Tab5 surface backend | Native tests pass; Mac GIF looks right; 60 fps at max grid on device (measured) |
| 3 | Sound audition page, then mixer with the chosen clacks | Audio capture (`a`) shows clacks lined up with flips; no dropouts with a full board |
| 4 | Layout config, fonts, side images, live preview | Preview matches the device screenshot for several layouts |
| 5 | Message library, templates, clock, TZ, weather | Clock flips only changed digits; weather fills in; TLS RAM checked |
| 6 | Scheduler + sleep + playlists (ported tests) | Native tests pass; schedule changes mode sign↔photo on the device |
| 7 | MQTT/HA | Entities show in HA; latch behaviour matches the Pi |
| 8 | Info sheet + touch | Long press shows IP; tap wakes; mode buttons work |
| 9 | Photo mode + file manager + transitions | Pi-equivalent features; browser resize path; 12 MP phone JPEG copied straight to the card still shows |
| 10 | OTA, backup/restore, README, web installer (as for T48) | Release build flashed through the web installer |

## 15. Decisions (settled 2026-09-30)

1. Name "FlapBoard", `~/Developer/flapboard`, GPL-3.0-or-later, likely public.
2. The sign widget is a portable library (§4a).
3. Characters: Vestaboard-style with colour tiles to start; the drum is data;
   no lowercase for now, but the option stays open.
4. Sounds: audition generated and free-sample options before choosing (§7).
   Ask before any download.
5. Weather: Open-Meteo for now.
6. Info sheet: long press anywhere.
7. Photos: SD card only for v1.
8. Landscape only for v1.
