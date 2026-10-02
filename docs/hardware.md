# This Tab5 (Phase 0 audit, 2026-09-30)

Measured with `env:probe` (`src/probe/main.cpp`, driven by `tools/probe.py`).

| Item | Result |
|---|---|
| SoC | ESP32-P4 **rev v1.3**, 2 cores, runs at **360 MHz** (400 MHz is for v3.x silicon) |
| Flash / PSRAM | 16 MB / 32 MB (29.1 MB free at boot) |
| Internal RAM | 351 KB free before Wi-Fi, **~205 KB after**; largest DMA-capable block falls from 247 KB to **~95–103 KB** once Wi-Fi is up |
| Toolchain | pioarduino **55.03.311** (Arduino core 3.3.11, ESP-IDF v5.5.5), M5GFX 0.2.30 from git |
| Display / touch | **ST7123** panel + ST7123 touch (I2C 0x55): the newer Tab5 revision, not ILI9881C + GT911 |
| C6 Wi-Fi (esp-hosted) | was **1.4.1** against host 2.12.11 → **updated to 2.12.11** over Wi-Fi (`ESP_HostedOTA`, from espressif.github.io). Now matches |
| Wi-Fi MAC | 98:A3:16:B8:6A:F0 (P4 base MAC e8:f6:0a:e2:fb:09) |
| RTC | RX8130 present; read 2002-07-24, **set from NTP** to UTC on 2026-09-30 |
| Battery | 2S, ~8.24 V, charging ~620 mA on USB |
| SD card | 122 GB SDHC/SDXC, 4-bit bus at 20 MHz. It held an old openHABian (Pine64) boot image with a 49 MB FAT partition; **reformatted as one FAT32 partition, 32 KB clusters, 122,081 MB** (format took 29 s) |
| SD speed | 32 KB writes/reads: 1 MB written in ~0.71 s, read in ~0.29 s (was 1.7–3.3 s / 0.49 s on the old partition). Fine for photos; 40 MHz may be worth trying later |

## Notes for later phases
- A newer pioarduino (55.03.312-1, 2026-09-22) needs PlatformIO Core >= 6.2; installed is
  6.1.19. Upgrading Core affects Tabulous5 and T48 too, so it was left alone.
- The Wi-Fi join failed on 1 boot of about 7 (and one report showed -92 dBm right after the
  format), then worked again at -55 dBm. Phase 1 needs a join retry loop and the burner's link
  watchdog.
- DMA-capable internal RAM after Wi-Fi (~95 KB largest block) is the budget for TLS (weather),
  MQTT and the web server. Big buffers and stacks go in PSRAM.
- ST7123 panel: M5GFX detects it on its own; touch polling goes through M5GFX's Touch_ST7123.
- Wi-Fi credentials come from `include/secrets.h` (gitignored, copied from Tabulous5). This unit
  had no NVS network saved.

## Phase 1 measurements (2026-09-30)
- Boot to joined Wi-Fi: ~4 s. mDNS name `flapboard.local`.
- 50 sequential uploads (300 KB-2 MB, 59 MB total), each downloaded back and compared: **50/50**,
  0 watchdog restarts, 0 lost router pings. A 400 KB upload takes ~0.6-0.8 s (~500 KB/s).
- After the run: internal free 151 KB, largest DMA block 55 KB (was 82 KB at boot). Watch this as
  features are added.
- macOS waited 5 s on every `.local` lookup for an IPv6 (AAAA) answer. `WiFi.enableIPv6(true)`
  after `WiFi.mode()` gives a link-local address that mDNS advertises; lookups now take ~10 ms.

## Camera spike (2026-09-30, `env:camspike`)
- Sensor: SC202CS driver (SmartSens' name for the "SC2356"), MIPI-CSI, own clock (no XCLK), enabled by
  PI4IOE5V6408 @0x43 pin 6, which M5GFX already sets high at boot.
- SCCB shares M5Unified's internal I2C **port 1** (G31/G32): `i2c_master_get_bus_handle(M5.In_I2C.getPort())`
  passed to `esp_video_init` with `init_sccb = false`. The two sides are not serialized against each
  other (per M5GFX's own comment); 95 s of streaming with `M5.update()` polling touch showed no errors.
  The full feature must keep all other I2C users on the main loop and watch for this.
- `esp_video_init` 40 ms. Formats offered on /dev/video0: RAW8 BGGR, RGB565, RGB888, YUV420, UYVY. Only
  1280x720 (the prebuilt's default sensor mode). Streams YUV420 at a steady **30.0 fps** with the ISP's
  auto exposure (mean luma ~115-120 indoors).
- Cost: ~12 KB internal RAM, DMA largest block 102 KB -> 94 KB; PSRAM 2.7 MB for two frame buffers.
  Reducing a frame to an 80x45 luma grid (every 4th pixel/row): **3.6 ms**.
- Idle scene: motion score 0% at a threshold of 12 levels (no false triggers from sensor noise).
  Detection itself not yet exercised (nobody moved in front of it).

## Phase 2 drawing measurements (2026-09-30, 6x22 board, 53x74 px cells)
Per cell, on-device `bench` (serial):
| Step | us |
|---|---|
| memcpy a glyph face PSRAM -> RAM (7.8 KB) | 53 |
| memcpy one cell into framebuffer columns | 80 |
| cache msync of one cell's panel rows | 12 |
| draw a landed cell (full path) | 133 |
| draw a mid-flip cell (full path) | 235 (was 326) |
| compose mid-flip cell into scratch only | 129 (was 222) |

- M5GFX `pushImage` with rotation 3: ~0.55 ms per cell. Direct framebuffer columns (glyphs stored
  column-major, composed straight into panel memory) plus packed RGB565 shading (2 multiplies, not 3)
  and flapcore at -O2: ~0.23-0.28 ms per cell in real changes.
- Real message changes (random start delay): ~70 cells moving, ~19 ms per frame, 50-60 fps.
  Worst case (all 132 cells mid-flap, e.g. full spin): ~30 fps. Timing is clock-based, so a slow frame
  never slows the board.
- The display's own DMA reads the framebuffer from PSRAM (~110 MB/s at 60 Hz); memory traffic, not
  arithmetic, is now the limit. Half-cell redraws help only once frames outpace flaps.
- Screenshots: a note() printed by another task mid-transfer shifted the picture by one line of text;
  notes are held off serial during shot/get/put.

## Phase 3 sound (2026-09-30)
- M5Unified Speaker, one channel, 512-sample blocks at 22.05 kHz (23 ms), two queued. 256-sample
  blocks underran during busy moments; 512 gives 46 ms of headroom.
- Landings are reported 80 ms ahead (Board::update lookahead), so each clack starts on its own
  sample; 0 late drops measured. A full-board change peaks around 1,000 clacks/s; 64 voices.
- **Flash writes only from the main loop.** A LittleFS write (saving the volume) from the audio task,
  whose stack is in PSRAM, asserted `esp_task_stack_is_sane_cache_disabled()` and reset the board.
  The render and audio tasks keep PSRAM stacks and must never write flash/NVS.
- Serial `audiocap N` records the mixer output to /sdcard/flapboard/capture.wav.

## Phase 7: MQTT, camera motion, and the limits they ran into (2026-10-01)
- **VFS table is 8 entries** in the prebuilt framework (`CONFIG_VFS_MAX_COUNT=8`): /dev/uart,
  /dev/secondary, /dev/null, /dev/console, /sdcard, lwIP sockets, and the camera's /dev/video0 +
  /dev/video20 fill it exactly. The settings moved from LittleFS (a whole VFS entry for one file) to a
  256 KB NVS partition "cfg". Starting the camera before lwIP registered its sockets crashed the boot in
  `esp_vfs_lwip_sockets_register` (ESP_ERR_NO_MEM = no free VFS slot); `esp_netif_init()` now runs first.
  `esp_video_init_with_flags(MIPI_CSI | ISP)` keeps the camera to two devices.
- **The camera must start before Wi-Fi.** Largest DMA-capable internal block: 207 KB before Wi-Fi,
  47 KB after; esp_video then fails with ESP_ERR_NO_MEM. Started in setup() it takes ~19 KB.
- **With the camera on, TLS no longer fits**: internal free ~90 KB, largest block ~34 KB. Weather falls back
  to plain HTTP (Open-Meteo answers on both); the status says which was used.
- **The P4 ISP's "YUV420" is not planar I420.** Reading the first width*height bytes as luma gave a texture,
  not a picture. RGB565 is unambiguous; luma = (77R + 150G + 29B) >> 8.
- **Invalidate the cache before reading a frame** (`esp_cache_msync(..., M2C)`): the camera DMAs into PSRAM
  and the CPU otherwise reads stale lines. Without it every frame looked the same, so the camera-spike's
  "0% motion, no false triggers" was in fact a broken reading. Lesson: check frames against the real scene
  (now `/api/motion/snapshot`, on demand only).
- ISP colour correction logs "Matrix[2][2] out of range" every frame with the default IPA tuning; colour
  cast only, irrelevant to motion; logs are silenced after boot.
- MQTT: esp-mqtt; tested against tools/minibroker.py (a 150-line MQTT 3.1.1 broker) rather than a real
  Home Assistant. Discovery (15 configs), commands, refused password, and the LWT on an unclean drop verified.
- Port A 5 V (red wire, and the M5-Bus 5 V pin) is switched by IO expander 0 pin 2 (`setExtOutput(ext_PA)`);
  the firmware turns it on while a relay pin is configured.

## Uploads wedging the Wi-Fi link (2026-10-01)
- Symptom: large uploads stalled, the router stopped answering pings, the link watchdog restarted the sign.
  Downloads (~730 KB/s) and small requests were fine. Phase 1's firmware on the same access point: 5/5.
- Bisect: Phase 2 borderline (2/3), Phase 3 onwards failing. The ESP32-C6's SDIO link (esp-hosted-mcu#184
  class) wedges on large *inbound bursts*; anything that loads the system makes it likelier:
  - a running speaker (I2S + codec + amp): 1/6 with the speaker merely started, 6/6 with it stopped. M5's
    `playRaw()` restarts an ended speaker by itself ("lazy begin"), so a feed racing `end()` left it running.
    Now the speaker runs only while sound plays (started by the main loop when a board change begins,
    stopped 2 s after the last clack, never during an upload), with a mutex between feeding and `end()`.
  - HTTPS weather (mbedTLS takes ~40 KB of internal RAM here): weather is now plain HTTP.
  - small allocations eating internal RAM: `heap_caps_malloc_extmem_enable(64)` moves them to PSRAM.
- The fix that made uploads reliable: **upload in 16 KB pieces** (`/library/upload_part`). A request never
  bursts more than its body. 32 KB pieces: 10/10 but one wedge (recovered by retries); 16 KB pieces: 12/12,
  no wedges, ~170 KB/s. The multipart `/library/upload` (Pi API) stays for small files.
- Not done (needs a custom ESP-IDF build via pioarduino's `custom_sdkconfig`, i.e. downloading the IDF):
  smaller TCP window, slower SDIO clock, mbedTLS in PSRAM, more VFS slots.
- Joining: the default fast scan took the first access point heard (a far one at -93 dBm with a near one
  available). Now all-channel scan + strongest signal, and roaming when weak (rare scans: they can wedge too).

## Firmware updates and release images (Phase 10, 2026-10-02)
- OTA from the web page in 16 KB pieces (`/api/ota`), sequential writes into the spare slot: ~85-100 KB/s,
  ~35 s for a 3 MB image. Verified ota_0 -> ota_1 -> ota_0 several times.
- **Arduino marks every new app valid before setup()** unless `verifyRollbackLater()` returns true
  (weak, in esp32-hal-misc.c). Without the override the rollback-enabled bootloader never got a say.
  Now the app is confirmed from loop() once up 30 s with Wi-Fi joined; `/api/status` shows `app_trial`.
- Rollback test: an update that `abort()`s 10 s after boot -> panic (reset reason 4) -> bootloader went
  back to the previous slot by itself, ~12 s of downtime.
- **Flash writes make the MIPI-DSI panel flash white** (framebuffer reads stall while the cache is off
  for each sector erase/write). SD-card uploads don't do this. The backlight is now off while an update
  is received (`web::updating()`, cleared 30 s after the last piece if abandoned).
- Release builds define `FLAPBOARD_RELEASE`, which drops the developer Wi-Fi seed in `secrets.h`;
  tools/release.sh fails if the SSID is found in the image (`LC_ALL=C grep -a`: plain grep on the
  binary silently matched nothing). The Arduino libs have no exFAT: SD cards must be FAT32.
- SD: when the mount fails, `sdcard::probe()` initialises the card raw (slot 0, IO-MUX, 1-bit) and reads
  sector 0 (MBR/GPT -> first partition's boot sector) to report none / exfat / ntfs / unformatted / unknown.
  This runs at boot, before Wi-Fi: the SD slot shares the P4's SDIO host with the C6, so `sdmmc_host_deinit`
  (and SD_MMC.end) must not run once Wi-Fi is up. For the same reason the web page's format sets an NVS flag
  and restarts; `formatCardIfAsked()` formats before Wi-Fi and before the task watchdog starts.
- OTA's first piece waits (<= 600 ms) until the main loop has blanked the screen: before that, the first
  sector erase gave two quick white flashes.
