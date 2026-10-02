#include "note.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <esp_attr.h>

#include <cstdarg>
#include <cstring>

namespace flapboard {
namespace {

constexpr int kKeep = 60;
std::vector<std::string> g_ring;
int g_next = 0;
SemaphoreHandle_t g_mux = xSemaphoreCreateMutex();
volatile bool g_quiet = false;

// The tail of the log, in RAM a reset leaves alone: what led up to a restart.
constexpr uint32_t kCarryMagic = 0xF1A9B0A7, kCarrySize = 4096;
struct Carry {
  uint32_t magic, head, len;
  char buf[kCarrySize];
};
__NOINIT_ATTR Carry g_carry;
std::string g_prev;

struct CarryInit {
  CarryInit() {   // before setup(): take what the last run left, then start afresh
    if (g_carry.magic == kCarryMagic && g_carry.head < kCarrySize && g_carry.len <= kCarrySize) {
      const uint32_t start = (g_carry.head + kCarrySize - g_carry.len) % kCarrySize;
      for (uint32_t i = 0; i < g_carry.len; i++) g_prev += g_carry.buf[(start + i) % kCarrySize];
    }
    g_carry.magic = kCarryMagic;
    g_carry.head = g_carry.len = 0;
  }
} g_carry_init;

void carry(const char *line) {   // under g_mux
  for (const char *c = line;; c++) {
    g_carry.buf[g_carry.head] = *c ? *c : '\n';
    g_carry.head = (g_carry.head + 1) % kCarrySize;
    if (g_carry.len < kCarrySize) g_carry.len++;
    if (!*c) break;
  }
}

}  // namespace

void note(const char *fmt, ...) {
  char text[240];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  char line[260];
  const uint32_t s = millis() / 1000;
  snprintf(line, sizeof(line), "[%02lu:%02lu:%02lu] %s", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
           (unsigned long)(s % 60), text);
  if (!g_quiet) Serial.println(line);
  xSemaphoreTake(g_mux, portMAX_DELAY);
  if ((int)g_ring.size() < kKeep) g_ring.push_back(line);
  else g_ring[g_next] = line;
  g_next = (g_next + 1) % kKeep;
  carry(line);
  xSemaphoreGive(g_mux);
}

void trace(const char *fmt, ...) {
  if (g_quiet) return;
  char text[240];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  const uint32_t s = millis() / 1000;
  Serial.printf("[%02lu:%02lu:%02lu] %s\n", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
                (unsigned long)(s % 60), text);
}

void setSerialQuiet(bool quiet) { g_quiet = quiet; }

std::vector<std::string> previousNotes() {
  std::vector<std::string> out;
  size_t a = g_prev.find('\n');   // the oldest line is usually cut: skip it
  if (a == std::string::npos) return out;
  for (size_t b; (b = g_prev.find('\n', a + 1)) != std::string::npos; a = b)
    if (b > a + 1) out.push_back(g_prev.substr(a + 1, b - a - 1));
  return out;
}

std::vector<std::string> recentNotes() {
  xSemaphoreTake(g_mux, portMAX_DELAY);
  std::vector<std::string> out;
  if ((int)g_ring.size() < kKeep) out = g_ring;
  else {
    for (int i = 0; i < kKeep; i++) out.push_back(g_ring[(g_next + i) % kKeep]);
  }
  xSemaphoreGive(g_mux);
  return out;
}

}  // namespace flapboard
