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
