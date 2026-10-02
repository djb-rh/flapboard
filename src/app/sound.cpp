#include "sound.h"

#include <Arduino.h>
#include <M5Unified.h>
#include <dirent.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>

#include <flapcore/clackmixer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <ArduinoJson.h>

#include "config.h"
#include "generated/sounds.h"
#include "note.h"
#include "net.h"
#include "sdcard.h"

namespace flapboard {
namespace sound {
namespace {

using flapcore::Clip;

constexpr uint32_t kRate = sounds::kRate;   // 22050: the clacks are short and bright enough
constexpr int kBlock = 512;                 // 23 ms per block: two queued = 46 ms of headroom
constexpr int kBufs = 4;                    // the speaker holds 2 queued; 4 keeps every buffer alive while used
constexpr uint8_t kChannel = 0;

flapcore::ClackMixer g_mix;
volatile int g_volume = 60;
volatile bool g_enabled = true;
volatile bool g_suppressed = false;
// The speaker runs only while there is sound to play. A running speaker (I2S
// clock, codec, amplifier) wedged the ESP32-C6's SDIO Wi-Fi link under
// upload traffic: measured 1/6 uploads with the speaker merely started,
// 6/6 with it stopped. The main loop starts and stops it (codec setup is
// I2C); the audio task only feeds it while g_spk_on.
volatile bool g_spk_on = false;
volatile uint32_t g_wanted_ms = 0;     // last time sound was wanted (wake, clacks, voices)
volatile bool g_feeding = false;       // the audio task has blocks queued
// Held while feeding (audio task) and while starting/ending (main loop):
// M5's playRaw() restarts an ended speaker by itself ("lazy begin"), so a
// feed racing an end() left the speaker running forever after the first
// board change -- and the Wi-Fi link failing every upload.
SemaphoreHandle_t g_spk_mux = nullptr;
volatile uint32_t g_save_at = 0;   // when to write the changed level to the settings
volatile bool g_reload = false;
std::vector<int16_t *> g_owned;   // clips loaded from the card (PSRAM)
std::string g_source = "built-in";
SemaphoreHandle_t g_mux;
flapcore::ClackMixer::Stats g_last;
uint32_t g_underruns = 0;

// audiocap: blocks are appended here by the audio task, written out when full.
int16_t *g_cap = nullptr;
volatile uint32_t g_cap_len = 0, g_cap_fill = 0;
std::string g_cap_path;

void writeCapture() {
  FILE *f = fopen(g_cap_path.c_str(), "wb");
  if (!f) return;
  auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
  fwrite("RIFF", 1, 4, f); u32(36 + g_cap_fill * 2); fwrite("WAVEfmt ", 1, 8, f);
  u32(16); u16(1); u16(1); u32(kRate); u32(kRate * 2); u16(2); u16(16);
  fwrite("data", 1, 4, f); u32(g_cap_fill * 2);
  fwrite(g_cap, 2, g_cap_fill, f);
  fclose(f);
  note("sound: captured %.1f s to %s", g_cap_fill / (float)kRate, g_cap_path.c_str());
  heap_caps_free(g_cap);
  g_cap = nullptr;
  g_cap_len = g_cap_fill = 0;
}

// A 16-bit PCM WAV from the card, mixed to mono and resampled to kRate.
int16_t *loadWav(const std::string &path, uint32_t *frames_out) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return nullptr;
  uint8_t hdr[12];
  if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
    fclose(f);
    return nullptr;
  }
  uint16_t fmt = 0, ch = 0, bits = 0;
  uint32_t rate = 0;
  int16_t *out = nullptr;
  for (;;) {
    uint8_t c[8];
    if (fread(c, 1, 8, f) != 8) break;
    const uint32_t len = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
    if (!memcmp(c, "fmt ", 4)) {
      uint8_t b[16];
      if (len < 16 || fread(b, 1, 16, f) != 16) break;
      fmt = b[0] | b[1] << 8;
      ch = b[2] | b[3] << 8;
      rate = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
      bits = b[14] | b[15] << 8;
      fseek(f, len - 16 + (len & 1), SEEK_CUR);
    } else if (!memcmp(c, "data", 4)) {
      if (fmt != 1 || bits != 16 || !ch || !rate || len > 2 * 1024 * 1024) break;
      const uint32_t in_frames = len / (2 * ch);
      std::vector<int16_t> raw((size_t)in_frames * ch);
      if (fread(raw.data(), 2, raw.size(), f) != raw.size()) break;
      const uint32_t frames = (uint32_t)((uint64_t)in_frames * kRate / rate);
      out = (int16_t *)heap_caps_malloc((size_t)frames * 2, MALLOC_CAP_SPIRAM);
      if (!out) break;
      for (uint32_t i = 0; i < frames; i++) {   // linear resample, channels averaged
        const double pos = (double)i * rate / kRate;
        const uint32_t a = (uint32_t)pos, b = a + 1 < in_frames ? a + 1 : a;
        const double t = pos - a;
        int32_t s = 0;
        for (int k = 0; k < ch; k++) s += (int32_t)(raw[(size_t)a * ch + k] * (1 - t) + raw[(size_t)b * ch + k] * t);
        out[i] = (int16_t)(s / ch);
      }
      *frames_out = frames;
      break;
    } else {
      fseek(f, len + (len & 1), SEEK_CUR);
    }
  }
  fclose(f);
  return out;
}

// Card clacks (/flapboard/sounds/clack_*.wav) if there are any, else the
// built-in Station Board set.
void loadClips() {
  for (auto *p : g_owned) heap_caps_free(p);
  g_owned.clear();
  std::vector<Clip> clips;
  if (sdcard::mounted()) {
    const std::string dir = std::string(sdcard::mountPoint()) + "/flapboard/sounds";
    if (DIR *d = opendir(dir.c_str())) {
      while (dirent *e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.rfind("clack", 0) != 0 || n.size() < 5 || strcasecmp(n.c_str() + n.size() - 4, ".wav") != 0) continue;
        uint32_t frames = 0;
        if (int16_t *pcm = loadWav(dir + "/" + n, &frames)) {
          g_owned.push_back(pcm);
          clips.push_back({pcm, frames});
        } else {
          note("sound: %s is not a 16-bit PCM WAV; skipped", n.c_str());
        }
      }
      closedir(d);
    }
  }
  g_source = clips.empty() ? "built-in" : "SD card";
  if (clips.empty())
    for (const auto &s : sounds::kSounds) clips.push_back({s.pcm, s.frames});
  g_mix.setClips(clips);
  note("sound: %u clacks (%s)", (unsigned)clips.size(), g_source.c_str());
}

void applyConfig(bool *enabled) {
  if (g_save_at) return;   // a change from the panel or the web is waiting to be saved: it wins
  config::Reader r;
  g_enabled = r.doc()["sound_enabled"] | true;
  g_volume = r.doc()["sound_volume"] | 60;
  *enabled = g_enabled;
  g_mix.setVolume(g_volume / 100.0f);
  g_mix.setOffsetMs(r.doc()["sound_offset_ms"] | 0);
}

void saveIfDue() {
  if (!g_save_at || (int32_t)(millis() - g_save_at) < 0) return;
  g_save_at = 0;
  JsonDocument p;
  p["sound_volume"] = (int)g_volume;
  p["sound_enabled"] = (bool)g_enabled;
  std::string err;
  config::apply(p.as<JsonVariantConst>(), &err);
}

void audioTask(void *) {
  static int16_t buf[kBufs][kBlock];
  int which = 0;
  const double block_ms = kBlock * 1000.0 / kRate;
  double next_ms = millis() + 2 * block_ms;   // board time the next block starts at
  bool enabled = true;
  uint32_t last_cfg = 0, last_stats = millis();
  for (;;) {
    if (g_reload) {
      g_reload = false;
      loadClips();
    }
    if (millis() - last_cfg > 1000) {
      applyConfig(&enabled);
      last_cfg = millis();
    }
    enabled = g_enabled && !g_suppressed;
    if (!enabled || !g_spk_on) {
      // Speaker off (or sound off): keep the clock aligned and let events
      // past their time drain, so a speaker start never plays stale clacks.
      next_ms = millis() + 2 * block_ms;
      g_mix.render(buf[0], kBlock, next_ms - 1000);
      g_feeding = false;
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    // The speaker queue ran dry (or this task stalled): re-anchor the clock
    // so clacks stay matched to their flaps rather than drifting late.
    const double now = millis();
    if (next_ms < now || next_ms > now + 250) {
      if (next_ms < now) g_underruns++;
      next_ms = now + block_ms;
    }
    if (xSemaphoreTake(g_spk_mux, pdMS_TO_TICKS(5)) != pdTRUE) {
      vTaskDelay(1);
      continue;
    }
    while (g_spk_on && M5.Speaker.isPlaying(kChannel) < 2) {
      g_feeding = true;
      g_mix.render(buf[which], kBlock, next_ms);
      if (g_cap && g_cap_fill < g_cap_len) {
        const uint32_t take = std::min<uint32_t>(kBlock, g_cap_len - g_cap_fill);
        memcpy(g_cap + g_cap_fill, buf[which], take * 2);
        g_cap_fill += take;
        if (g_cap_fill >= g_cap_len) writeCapture();
      }
      M5.Speaker.playRaw(buf[which], kBlock, kRate, false, 1, kChannel, false);
      which = (which + 1) % kBufs;
      next_ms += block_ms;
    }
    xSemaphoreGive(g_spk_mux);
    if (millis() - last_stats >= 2000) {
      const auto s = g_mix.takeStats();
      if (s.started) note("sound: %lu clacks, peak %d voices, %lu stolen, %lu late-dropped, %lu underruns",
                          (unsigned long)s.started, s.peak_voices, (unsigned long)s.stolen,
                          (unsigned long)s.dropped, (unsigned long)g_underruns);
      xSemaphoreTake(g_mux, portMAX_DELAY);
      g_last = s;
      xSemaphoreGive(g_mux);
      last_stats = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(3));
  }
}

}  // namespace

void begin() {
  g_mux = xSemaphoreCreateMutex();
  g_spk_mux = xSemaphoreCreateMutex();
  // The speaker is NOT started here: see loop().
  g_mix.setup(kRate, 64);   // a full-board change overlaps ~90 clacks of 90 ms; tails beyond 64 are inaudible
  loadClips();
  // Core 0 (the render task has core 1); above the Arduino loop so a busy
  // web request never starves the speaker.
  xTaskCreatePinnedToCoreWithCaps(audioTask, "sound", 6144, nullptr, 6, nullptr, 0,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void flip(uint32_t at_ms) {
  g_mix.push(at_ms);
  g_wanted_ms = millis();
}
void wake() { g_wanted_ms = millis(); }

void loop() {
  saveIfDue();
  // Start the speaker when sound is wanted, stop it 2 s after the last clack,
  // and never run it during an upload (net::busy: the link wedges otherwise).
  const bool uploading = net::uploading();
  const bool want = g_enabled && !g_suppressed && !uploading && millis() - g_wanted_ms < 2000;
  if (want && !g_spk_on) {
    xSemaphoreTake(g_spk_mux, portMAX_DELAY);
    M5.Speaker.begin();
    M5.Speaker.setAllChannelVolume(255);
    M5.Speaker.setVolume(200);   // the mixer sets the level; this stays fixed
    g_spk_on = true;
    xSemaphoreGive(g_spk_mux);
  } else if (!want && g_spk_on) {
    xSemaphoreTake(g_spk_mux, portMAX_DELAY);   // the audio task is between feeds: no playRaw can follow
    g_spk_on = false;
    g_feeding = false;
    M5.Speaker.stop();
    const auto c = M5.Speaker.config();
    M5.Speaker.end();
    // Deleting the I2S channel leaves its pins routed to the I2S signals;
    // put them back to plain inputs so no clock keeps running on the board.
    for (int pin : {(int)c.pin_mck, (int)c.pin_bck, (int)c.pin_ws, (int)c.pin_data_out})
      if (pin >= 0) gpio_reset_pin((gpio_num_t)pin);
    static bool told = false;
    if (!told) {
      told = true;
      note("sound: speaker pins mck %d bck %d ws %d out %d", c.pin_mck, c.pin_bck, c.pin_ws, c.pin_data_out);
    }
    xSemaphoreGive(g_spk_mux);
  }
}

void setVolume(int volume, bool preview) {
  g_volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
  g_enabled = true;   // changing the volume while muted means "I want to hear it"
  g_mix.setVolume(g_volume / 100.0f);
  g_save_at = millis() + 2000;   // one write after the last tap, not one per tap
  if (preview && g_enabled) {
    g_wanted_ms = millis();
    const uint32_t t = millis() + 120;   // room for the speaker to start
    for (int i = 0; i < 5; i++) g_mix.push(t + i * 70);   // a short run, like one cell turning
  }
}

void setEnabled(bool on) {
  g_enabled = on;
  g_save_at = millis() + 2000;
}

int volume() { return g_volume; }
void setSuppressed(bool quiet) { g_suppressed = quiet; }
bool enabled() { return g_enabled; }

void capture(const std::string &path, float seconds) {
  if (g_cap) return;
  g_cap_path = path;
  const uint32_t n = (uint32_t)(seconds * kRate);
  g_cap_fill = 0;
  g_cap = (int16_t *)heap_caps_malloc((size_t)n * 2, MALLOC_CAP_SPIRAM);
  g_cap_len = g_cap ? n : 0;
}
void reloadClips() { g_reload = true; }

std::string statsJson() {
  xSemaphoreTake(g_mux, portMAX_DELAY);
  const auto s = g_last;
  xSemaphoreGive(g_mux);
  char b[200];
  snprintf(b, sizeof(b), "{\"source\":\"%s\",\"clacks\":%lu,\"peak_voices\":%d,\"stolen\":%lu,\"dropped\":%lu,\"underruns\":%lu}",
           g_source.c_str(), (unsigned long)s.started, s.peak_voices, (unsigned long)s.stolen,
           (unsigned long)s.dropped, (unsigned long)g_underruns);
  return b;
}

}  // namespace sound
}  // namespace flapboard
