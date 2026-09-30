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
