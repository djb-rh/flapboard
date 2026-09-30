#include "note.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdarg>

namespace flapboard {
namespace {

constexpr int kKeep = 60;
std::vector<std::string> g_ring;
int g_next = 0;
SemaphoreHandle_t g_mux = xSemaphoreCreateMutex();
volatile bool g_quiet = false;

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
  xSemaphoreGive(g_mux);
}

void setSerialQuiet(bool quiet) { g_quiet = quiet; }

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
