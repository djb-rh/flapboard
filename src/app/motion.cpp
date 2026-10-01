#include "motion.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <driver/i2c_master.h>
#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <fcntl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cmath>
#include <cstring>

#include "config.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "linux/videodev2.h"
#include "note.h"

namespace flapboard {
namespace motion {
namespace {

constexpr int kCells = kGridW * kGridH, kBufs = 2;
constexpr int kEvery = 6;   // of 30 frames a second: 5 looked at

int g_fd = -1;
uint8_t *g_buf[kBufs];
size_t g_len[kBufs];
uint32_t g_w = 0, g_h = 0, g_fmt = 0;
bool g_video_ok = false, g_failed = false;
volatile bool g_enabled = false;
volatile uint32_t g_last_motion = 0;
volatile uint32_t g_pause_until = 0;
volatile int g_threshold = 14;
volatile float g_area = 1.5f;
volatile float g_last_pct = 0;
volatile int g_last_shift = 0;
uint8_t g_grid[kCells], g_changed[kCells];
volatile bool g_snap_req = false, g_snap_done = false;
uint8_t *g_snap = nullptr;
uint32_t g_bpl = 0;   // bytes per line the driver reports
float g_bg[kCells];
bool g_have_bg = false;
SemaphoreHandle_t g_mux = xSemaphoreCreateMutex();
std::string g_status = "off";

bool startCamera() {
  const i2c_port_t port = M5.In_I2C.getPort();
  i2c_master_bus_handle_t bus = nullptr;
  if (i2c_master_get_bus_handle(port, &bus) != ESP_OK || !bus) {
    g_status = "no I2C bus handle for the camera";
    return false;
  }
  static esp_video_init_csi_config_t csi = {};
  csi.sccb_config.init_sccb = false;   // share M5Unified's bus (see docs/hardware.md)
  csi.sccb_config.i2c_handle = bus;
  csi.sccb_config.freq = 400000;
  csi.reset_pin = GPIO_NUM_NC;
  csi.pwdn_pin = GPIO_NUM_NC;
  static esp_video_init_config_t cfg = {};
  cfg.csi = &csi;
  // Only the CSI camera and the ISP: every esp_video device takes one of the
  // VFS table's few slots, and lwIP's sockets need one too.
  const esp_err_t e = esp_video_init_with_flags(&cfg, ESP_VIDEO_INIT_FLAGS_MIPI_CSI | ESP_VIDEO_INIT_FLAGS_ISP);
  if (e != ESP_OK) {
    char b[120];
    snprintf(b, sizeof(b), "the camera did not start (%s; internal RAM %u KB free, largest DMA block %u KB)",
             esp_err_to_name(e), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) / 1024));
    g_status = b;
    return false;
  }
  g_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
  if (g_fd < 0) {
    g_status = "cannot open the camera device";
    return false;
  }
  v4l2_format f = {};
  f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ioctl(g_fd, VIDIOC_G_FMT, &f);
  v4l2_format t = f;
  // RGB565, not "YUV420": the P4 ISP's YUV420 is not the standard planar
  // layout, and reading it as a luma plane gave a texture instead of a picture.
  t.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
  ioctl(g_fd, VIDIOC_S_FMT, &t);
  ioctl(g_fd, VIDIOC_G_FMT, &f);
  g_w = f.fmt.pix.width;
  g_h = f.fmt.pix.height;
  g_fmt = f.fmt.pix.pixelformat;
  g_bpl = f.fmt.pix.bytesperline;
  v4l2_requestbuffers rb = {};
  rb.count = kBufs;
  rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  rb.memory = V4L2_MEMORY_MMAP;
  if (ioctl(g_fd, VIDIOC_REQBUFS, &rb) != 0) {
    g_status = "camera buffers unavailable";
    return false;
  }
  for (int i = 0; i < kBufs; i++) {
    v4l2_buffer b = {};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = i;
    ioctl(g_fd, VIDIOC_QUERYBUF, &b);
    g_buf[i] = (uint8_t *)mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, b.m.offset);
    g_len[i] = b.length;
    ioctl(g_fd, VIDIOC_QBUF, &b);
  }
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(g_fd, VIDIOC_STREAMON, &type) != 0) {
    g_status = "the camera would not stream";
    return false;
  }
  return true;
}

// Mean brightness of each grid cell, from every 4th pixel of every 4th row.
void reduce(const uint8_t *p, uint8_t *grid) {
  const int cw = g_w / kGridW, ch = g_h / kGridH;
  const bool rgb = g_fmt == V4L2_PIX_FMT_RGB565;
  for (int gy = 0; gy < kGridH; gy++)
    for (int gx = 0; gx < kGridW; gx++) {
      uint32_t sum = 0, n = 0;
      for (int y = gy * ch; y < (gy + 1) * ch; y += 4)
        for (int x = gx * cw; x < (gx + 1) * cw; x += 4) {
          if (rgb) {
            const uint16_t v = ((const uint16_t *)p)[y * g_w + x];
            sum += (((v >> 11) << 3) * 77 + (((v >> 5) & 63) << 2) * 150 + ((v & 31) << 3) * 29) >> 8;
          } else {
            sum += p[y * g_w + x];
          }
          n++;
        }
      grid[gy * kGridW + gx] = n ? sum / n : 0;
    }
}

void analyse(const uint8_t *grid) {
  float mean_g = 0, mean_b = 0;
  for (int i = 0; i < kCells; i++) {
    mean_g += grid[i];
    mean_b += g_bg[i];
  }
  mean_g /= kCells;
  mean_b /= kCells;
  const float shift = g_have_bg ? mean_g - mean_b : 0;
  const bool paused = (int32_t)(millis() - g_pause_until) < 0;
  // The whole picture brightened or darkened at once: lights switching, not
  // a person. Re-learn the scene and look again in a moment.
  if (!g_have_bg || paused || std::fabs(shift) > 25) {
    for (int i = 0; i < kCells; i++) g_bg[i] = grid[i];
    g_have_bg = true;
    if (std::fabs(shift) > 25 && !paused) g_pause_until = millis() + 2000;
    xSemaphoreTake(g_mux, portMAX_DELAY);
    memcpy(g_grid, grid, kCells);
    memset(g_changed, 0, kCells);
    xSemaphoreGive(g_mux);
    g_last_shift = (int)shift;
    return;
  }
  int changed = 0;
  uint8_t mask[kCells];
  const int thr = g_threshold;
  for (int i = 0; i < kCells; i++) {
    const float d = std::fabs(grid[i] - g_bg[i] - shift);
    mask[i] = d > thr;
    changed += mask[i];
    g_bg[i] += (grid[i] - g_bg[i]) * 0.05f;   // learn slowly: a parked bag becomes background in ~20 s
  }
  const float pct = changed * 100.0f / kCells;
  g_last_pct = pct;
  g_last_shift = (int)shift;
  if (pct >= g_area) g_last_motion = millis();
  xSemaphoreTake(g_mux, portMAX_DELAY);
  memcpy(g_grid, grid, kCells);
  memcpy(g_changed, mask, kCells);
  xSemaphoreGive(g_mux);
}

void task(void *) {
  uint32_t n = 0;
  static uint8_t grid[kCells];
  for (;;) {
    v4l2_buffer b = {};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    if (ioctl(g_fd, VIDIOC_DQBUF, &b) != 0) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (g_snap_req && g_snap) {
      esp_cache_msync(g_buf[b.index], g_len[b.index], ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
      const uint16_t *p = (const uint16_t *)g_buf[b.index];
      for (int y = 0; y < kSnapH; y++)
        for (int x = 0; x < kSnapW; x++) {
          const uint16_t v = p[(y * 4) * g_w + x * 4];
          uint8_t *o = g_snap + (y * kSnapW + x) * 3;
          o[0] = (v >> 11) << 3;
          o[1] = ((v >> 5) & 63) << 2;
          o[2] = (v & 31) << 3;
        }
      g_snap_req = false;
      g_snap_done = true;
    }
    const bool look = g_enabled && (++n % kEvery) == 0;
    if (look) {
      // The camera wrote this frame to PSRAM by DMA; drop whatever the CPU
      // cache still holds for it, or the "frame" is stale bytes (it was: a
      // static, garbled grid that never changed, so nothing ever moved).
      esp_cache_msync(g_buf[b.index], g_len[b.index], ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
      reduce(g_buf[b.index], grid);
    }
    ioctl(g_fd, VIDIOC_QBUF, &b);
    if (look) analyse(grid);
  }
}

}  // namespace

void beginEarly() {
  bool en;
  {
    config::Reader r;
    en = r.doc()["motion_enabled"] | false;
  }
  if (!en) return;
  if (startCamera()) {
    g_video_ok = true;
    g_last_motion = millis();   // start awake
    g_pause_until = millis() + 3000;
    xTaskCreatePinnedToCoreWithCaps(task, "motion", 6144, nullptr, 1, nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    note("motion: camera on, %lux%lu, looking at 5 frames a second", (unsigned long)g_w, (unsigned long)g_h);
  } else {
    g_failed = true;
    note("motion: %s", g_status.c_str());
  }
}

bool needsRestart() {
  config::Reader r;
  return (r.doc()["motion_enabled"] | false) && !g_video_ok && !g_failed;
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last < 1000) return;
  last = millis();
  bool en;
  {
    config::Reader r;
    en = r.doc()["motion_enabled"] | false;
    g_threshold = r.doc()["motion_threshold"] | 14;
    g_area = r.doc()["motion_area"] | 1.5f;
  }
  if (en && !g_enabled) g_last_motion = millis();   // switching it on counts as motion
  g_enabled = en && g_video_ok;
  if (g_video_ok) g_status = g_enabled ? "watching" : "paused (motion sensing is off)";
  else if (en && !g_failed) g_status = "restart the sign to start the camera";
}

int state(uint32_t timeout_ms) {
  if (!g_enabled) return -1;
  return millis() - g_last_motion < timeout_ms ? 1 : 0;
}

bool active() { return g_enabled && millis() - g_last_motion < 5000; }

void powerChanged() { g_pause_until = millis() + 3000; }

bool view(uint8_t *grid, uint8_t *changed) {
  if (!g_enabled) return false;
  xSemaphoreTake(g_mux, portMAX_DELAY);
  memcpy(grid, g_grid, kCells);
  memcpy(changed, g_changed, kCells);
  xSemaphoreGive(g_mux);
  return true;
}

bool snapshot(uint8_t *out, uint32_t timeout_ms) {
  if (!g_video_ok) return false;
  g_snap = out;
  g_snap_done = false;
  g_snap_req = true;
  for (uint32_t t = 0; t < timeout_ms && !g_snap_done; t += 10) vTaskDelay(pdMS_TO_TICKS(10));
  g_snap_req = false;
  return g_snap_done;
}

std::string statusJson() {
  JsonDocument d;
  d["enabled"] = (bool)g_enabled;
  d["status"] = g_status;
  d["motion_now"] = active();
  d["last_motion_s"] = g_enabled ? (millis() - g_last_motion) / 1000 : 0;
  d["changed_pct"] = g_last_pct;
  d["light_shift"] = (int)g_last_shift;
  char fc[5] = {(char)(g_fmt & 0xFF), (char)((g_fmt >> 8) & 0xFF), (char)((g_fmt >> 16) & 0xFF), (char)(g_fmt >> 24), 0};
  d["format"] = std::string(fc) + " " + std::to_string(g_w) + "x" + std::to_string(g_h) + " bpl " + std::to_string(g_bpl) +
                " buf " + std::to_string(g_len[0]);
  std::string out;
  serializeJson(d, out);
  return out;
}

}  // namespace motion
}  // namespace flapboard
