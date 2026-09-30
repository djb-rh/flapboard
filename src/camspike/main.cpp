// Camera spike (PLAN §11a): can esp_video start the Tab5's SC202CS ("SC2356")
// from Arduino, sharing the internal I2C bus with M5Unified, and what do
// frames cost (fps, CPU, internal RAM) with Wi-Fi up?
//
// Prints a stats line every 2 s: fps, ms per frame spent reducing to an
// 80x45 luma grid, the mean brightness and a crude motion score (percent of
// grid cells that changed by more than 12 levels since the previous frame).
// Serial: 's' stops the stream, 'r' restarts it, 'm' prints memory.
#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <driver/i2c_master.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "esp_video_device.h"
#include "esp_video_init.h"
#include "linux/videodev2.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

namespace {

constexpr int kGridW = 80, kGridH = 45, kBufs = 2;
int g_fd = -1;
uint8_t *g_buf[kBufs];
size_t g_len[kBufs];
uint32_t g_w = 0, g_h = 0, g_fmt = 0;
uint8_t g_prev[kGridW * kGridH], g_cur[kGridW * kGridH];
bool g_have_prev = false;

void mem(const char *tag) {
  Serial.printf("mem %s: internal %u, dma largest %u, psram %u\n", tag,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

const char *fourcc(uint32_t f, char *out) {
  for (int i = 0; i < 4; i++) out[i] = (char)((f >> (8 * i)) & 0xFF);
  out[4] = 0;
  return out;
}

bool startVideo() {
  const i2c_port_t port = M5.In_I2C.getPort();
  i2c_master_bus_handle_t bus = nullptr;
  if (i2c_master_get_bus_handle(port, &bus) != ESP_OK || !bus) {
    Serial.printf("cam: no i2c_master handle for M5's internal port %d\n", (int)port);
    return false;
  }
  Serial.printf("cam: sharing i2c port %d with M5Unified\n", (int)port);
  static esp_video_init_csi_config_t csi = {};
  csi.sccb_config.init_sccb = false;
  csi.sccb_config.i2c_handle = bus;
  csi.sccb_config.freq = 400000;
  csi.reset_pin = GPIO_NUM_NC;
  csi.pwdn_pin = GPIO_NUM_NC;
  static esp_video_init_config_t cfg = {};
  cfg.csi = &csi;
  const uint32_t t0 = millis();
  const esp_err_t e = esp_video_init(&cfg);
  Serial.printf("cam: esp_video_init -> %s (%lu ms)\n", esp_err_to_name(e), (unsigned long)(millis() - t0));
  return e == ESP_OK;
}

bool openStream() {
  g_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
  if (g_fd < 0) {
    Serial.println("cam: cannot open /dev/video0");
    return false;
  }
  v4l2_capability cap = {};
  ioctl(g_fd, VIDIOC_QUERYCAP, &cap);
  Serial.printf("cam: driver %s card %s\n", cap.driver, cap.card);
  char cc[5];
  for (int i = 0;; i++) {
    v4l2_fmtdesc d = {};
    d.index = i;
    d.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(g_fd, VIDIOC_ENUM_FMT, &d) != 0) break;
    Serial.printf("cam: format %d %s (%s)\n", i, fourcc(d.pixelformat, cc), d.description);
  }
  v4l2_format f = {};
  f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ioctl(g_fd, VIDIOC_G_FMT, &f);
  // Luma is all motion detection needs: ask for greyscale, else YUV420 (luma
  // plane first), else keep whatever it gives.
  const uint32_t want[] = {V4L2_PIX_FMT_GREY, V4L2_PIX_FMT_YUV420};
  for (uint32_t w : want) {
    v4l2_format t = f;
    t.fmt.pix.pixelformat = w;
    if (ioctl(g_fd, VIDIOC_S_FMT, &t) == 0) break;
  }
  ioctl(g_fd, VIDIOC_G_FMT, &f);
  g_w = f.fmt.pix.width;
  g_h = f.fmt.pix.height;
  g_fmt = f.fmt.pix.pixelformat;
  Serial.printf("cam: streaming %lux%lu %s\n", (unsigned long)g_w, (unsigned long)g_h, fourcc(g_fmt, cc));
  v4l2_requestbuffers rb = {};
  rb.count = kBufs;
  rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  rb.memory = V4L2_MEMORY_MMAP;
  if (ioctl(g_fd, VIDIOC_REQBUFS, &rb) != 0) {
    Serial.println("cam: REQBUFS failed");
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
    Serial.println("cam: STREAMON failed");
    return false;
  }
  return true;
}

void stopStream() {
  if (g_fd < 0) return;
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ioctl(g_fd, VIDIOC_STREAMOFF, &type);
  close(g_fd);
  g_fd = -1;
  g_have_prev = false;
  Serial.println("cam: stopped");
}

// Mean luma of each grid cell, sampling every 4th pixel of every 4th row.
// For RGB565 the green channel stands in for luma.
void reduce(const uint8_t *p) {
  const int cw = g_w / kGridW, ch = g_h / kGridH;
  const bool rgb = g_fmt == V4L2_PIX_FMT_RGB565;
  for (int gy = 0; gy < kGridH; gy++) {
    for (int gx = 0; gx < kGridW; gx++) {
      uint32_t sum = 0, n = 0;
      for (int y = gy * ch; y < (gy + 1) * ch; y += 4) {
        for (int x = gx * cw; x < (gx + 1) * cw; x += 4) {
          if (rgb) {
            const uint16_t v = ((const uint16_t *)p)[y * g_w + x];
            sum += ((v >> 5) & 63) << 2;
          } else {
            sum += p[y * g_w + x];
          }
          n++;
        }
      }
      g_cur[gy * kGridW + gx] = n ? sum / n : 0;
    }
  }
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  cfg.output_power = false;
  M5.begin(cfg);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  M5.Display.setRotation(3);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextSize(3);
  M5.Display.drawString("camera spike", 40, 40);
  delay(1500);
  mem("boot");
#ifdef WIFI_SSID
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  Serial.printf("wifi: %s %s\n", WiFi.status() == WL_CONNECTED ? "up" : "DOWN", WiFi.localIP().toString().c_str());
#endif
  mem("wifi");
  if (startVideo()) {
    mem("esp_video_init");
    if (openStream()) mem("streaming");
  }
}

void loop() {
  static uint32_t frames = 0, reduce_us = 0, last = millis(), touches = 0;
  static int motion = 0, mean = 0;
  M5.update();   // touch keeps polling the shared bus: that is part of the test
  if (M5.Touch.getCount()) touches++;
  if (Serial.available()) {
    const int c = Serial.read();
    if (c == 's') stopStream();
    else if (c == 'r') openStream();
    else if (c == 'm') mem("now");
  }
  if (g_fd >= 0) {
    v4l2_buffer b = {};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    if (ioctl(g_fd, VIDIOC_DQBUF, &b) == 0) {
      const int64_t t0 = esp_timer_get_time();
      reduce(g_buf[b.index]);
      reduce_us += (uint32_t)(esp_timer_get_time() - t0);
      ioctl(g_fd, VIDIOC_QBUF, &b);
      int changed = 0, sum = 0;
      for (int i = 0; i < kGridW * kGridH; i++) {
        sum += g_cur[i];
        if (g_have_prev && abs((int)g_cur[i] - (int)g_prev[i]) > 12) changed++;
      }
      mean = sum / (kGridW * kGridH);
      motion = changed * 100 / (kGridW * kGridH);
      memcpy(g_prev, g_cur, sizeof(g_prev));
      g_have_prev = true;
      frames++;
    }
  }
  if (millis() - last >= 2000) {
    const float secs = (millis() - last) / 1000.0f;
    Serial.printf("stats: %.1f fps, reduce %.2f ms/frame, mean %d, motion %d%%, touches %lu, dma %u, wifi %s\n",
                  frames / secs, frames ? reduce_us / 1000.0f / frames : 0.f, mean, motion, (unsigned long)touches,
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                  WiFi.status() == WL_CONNECTED ? "up" : "down");
    frames = reduce_us = 0;
    last = millis();
  }
  delay(1);
}
