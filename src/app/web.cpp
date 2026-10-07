#include "web.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <Preferences.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include "clock.h"
#include "config.h"
#include "content.h"
#include "generated/zones.h"
#include "generated/fonts.h"
#include "generated/web_assets.h"
#include "library.h"
#include "multipart.h"
#include "motion.h"
#include "mqtt.h"
#include "net.h"
#include "power.h"
#include "note.h"
#include "sign.h"
#include "sound.h"
#include "status.h"
#include "weather.h"

namespace flapboard {
namespace web {
namespace {

httpd_handle_t g_server = nullptr;
volatile bool g_reboot = false;
constexpr size_t kChunk = 16384;
char *g_buf = nullptr;   // PSRAM; the server runs one request at a time

// ---- helpers --------------------------------------------------------------

std::string urlDecode(const char *v) {
  std::string out;
  for (size_t i = 0; v[i]; i++) {
    if (v[i] == '%' && isxdigit((unsigned char)v[i + 1]) && isxdigit((unsigned char)v[i + 2])) {
      char h[3] = {v[i + 1], v[i + 2], 0};
      out += (char)strtol(h, nullptr, 16);
      i += 2;
    } else if (v[i] == '+') {
      out += ' ';
    } else {
      out += v[i];
    }
  }
  return out;
}

std::string query(httpd_req_t *req, const char *key) {
  const size_t n = httpd_req_get_url_query_len(req);
  if (!n || n > 2000) return "";
  std::string q(n + 1, '\0');
  if (httpd_req_get_url_query_str(req, &q[0], q.size()) != ESP_OK) return "";
  std::string v(n + 1, '\0');
  if (httpd_query_key_value(q.c_str(), key, &v[0], v.size()) != ESP_OK) return "";
  return urlDecode(v.c_str());
}

esp_err_t sendJson(httpd_req_t *req, int code, const std::string &body) {
  char st[40];
  snprintf(st, sizeof(st), "%d %s", code,
           code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 404 ? "Not Found"
           : code == 409 ? "Conflict" : code == 507 ? "Insufficient Storage" : "Internal Server Error");
  httpd_resp_set_status(req, st);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body.data(), body.size());
}

esp_err_t sendError(httpd_req_t *req, int code, const std::string &why) {
  return sendJson(req, code, "{\"error\":" + library::jsonStr(why) + "}");
}

int httpCode(library::Err e) {
  switch (e) {
    case library::Err::Ok: return 200;
    case library::Err::Unsafe: return 400;
    case library::Err::NotFound: return 404;
    case library::Err::NotDir: return 400;
    case library::Err::Exists: return 409;
    default: return 500;
  }
}

// A small request body (JSON or form), or false if too big / cut off.
bool readBody(httpd_req_t *req, std::string *out, size_t limit = 4096) {
  if (req->content_len > limit) return false;
  out->assign(req->content_len, '\0');
  size_t got = 0;
  while (got < req->content_len) {
    const int r = httpd_req_recv(req, &(*out)[got], req->content_len - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0) return false;
    got += r;
  }
  return true;
}

std::string formValue(const std::string &body, const char *key) {
  const std::string k = std::string(key) + "=";
  size_t p = 0;
  while (p < body.size()) {
    size_t e = body.find('&', p);
    if (e == std::string::npos) e = body.size();
    if (body.compare(p, k.size(), k) == 0) return urlDecode(body.substr(p + k.size(), e - p - k.size()).c_str());
    p = e + 1;
  }
  return "";
}

const char *mimeOf(const std::string &name) {
  const size_t d = name.rfind('.');
  std::string ext = d == std::string::npos ? "" : name.substr(d + 1);
  for (auto &c : ext) c = (char)tolower((unsigned char)c);
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "png") return "image/png";
  if (ext == "gif") return "image/gif";
  if (ext == "webp") return "image/webp";
  if (ext == "bmp") return "image/bmp";
  if (ext == "txt" || ext == "md" || ext == "csv") return "text/plain; charset=utf-8";
  if (ext == "json") return "application/json";
  if (ext == "wav") return "audio/wav";
  if (ext == "ttf" || ext == "otf") return "font/ttf";
  return "application/octet-stream";
}

// ---- web UI ---------------------------------------------------------------

esp_err_t sendAsset(httpd_req_t *req, const char *path) {
  for (const auto &a : web_assets::kAssets) {
    if (strcmp(a.path, path) != 0) continue;
    httpd_resp_set_type(req, a.mime);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)a.gz, a.len);
  }
  return sendError(req, 404, "not found");
}

esp_err_t redirectToSetup(httpd_req_t *req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/setup");
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_sendstr(req, "<a href=\"http://192.168.4.1/setup\">Wi-Fi setup</a>");
}

bool inSetup() { return net::portalActive() && net::state() != net::State::Connected; }

esp_err_t handleRoot(httpd_req_t *req) {
  if (inSetup()) return redirectToSetup(req);
  return sendAsset(req, "/messages.html");   // the first tab
}
esp_err_t redirectTo(httpd_req_t *req, const char *where) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", where);
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_sendstr(req, "Moved");
}
// The Files and Display pages were folded into Messages, Photos and Settings.
esp_err_t handleLibraryPage(httpd_req_t *req) { return redirectTo(req, "/messages#filesec"); }
esp_err_t handleSetup(httpd_req_t *req) { return sendAsset(req, "/setup.html"); }
esp_err_t handleDisplayPage(httpd_req_t *req) { return redirectTo(req, "/messages"); }

// Fonts: the built-in faces and any .ttf in /flapboard/fonts.
esp_err_t handleFontList(httpd_req_t *req) {
  std::string j = "{\"fonts\":[";
  bool first = true;
  for (const auto &f : fonts::kFonts) {
    j += std::string(first ? "" : ",") + "{\"name\":" + library::jsonStr(f.name) + ",\"source\":\"built-in\"}";
    first = false;
  }
  std::vector<library::Entry> e;
  if (library::list("fonts", &e) == library::Err::Ok) {
    for (auto &x : e) {
      if (x.dir || x.name.size() < 5 || strcasecmp(x.name.c_str() + x.name.size() - 4, ".ttf") != 0) continue;
      j += std::string(first ? "" : ",") + "{\"name\":" + library::jsonStr(x.name.substr(0, x.name.size() - 4)) +
           ",\"source\":\"card\"}";
      first = false;
    }
  }
  return sendJson(req, 200, j + "]}");
}

// /fonts/<name>.ttf, so the web preview draws with the sign's own faces.
esp_err_t handleFontFile(httpd_req_t *req) {
  std::string name = urlDecode(req->uri + strlen("/fonts/"));
  const size_t q = name.find('?');
  if (q != std::string::npos) name.resize(q);
  if (name.size() > 4 && name.compare(name.size() - 4, 4, ".ttf") == 0) name.resize(name.size() - 4);
  for (const auto &f : fonts::kFonts) {
    if (name != f.name) continue;
    httpd_resp_set_type(req, "font/ttf");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return httpd_resp_send(req, (const char *)f.data, f.len);
  }
  std::string abs;
  if (name.find('/') != std::string::npos || library::resolve("fonts/" + name + ".ttf", &abs) != library::Err::Ok)
    return sendError(req, 404, "no such font");
  FILE *f = fopen(abs.c_str(), "rb");
  if (!f) return sendError(req, 404, "no such font");
  httpd_resp_set_type(req, "font/ttf");
  size_t n;
  while ((n = fread(g_buf, 1, kChunk, f)) > 0) {
    if (httpd_resp_send_chunk(req, g_buf, n) != ESP_OK) {
      fclose(f);
      return ESP_FAIL;
    }
  }
  fclose(f);
  return httpd_resp_send_chunk(req, nullptr, 0);
}

// Everything else: an embedded file, or (in setup mode) the redirect that
// makes a phone open the setup page -- phones probe /hotspot-detect.html,
// /generate_204 and the like, and DNS points every name at us.
esp_err_t handleOther(httpd_req_t *req, httpd_err_code_t) {
  std::string uri = req->uri;
  const size_t q = uri.find('?');
  if (q != std::string::npos) uri.resize(q);
  for (const auto &a : web_assets::kAssets)
    if (uri == a.path) return sendAsset(req, a.path);
  if (net::portalActive()) return redirectToSetup(req);
  return sendError(req, 404, "not found");
}

// ---- JSON API -------------------------------------------------------------

esp_err_t handleStatus(httpd_req_t *req) { return sendJson(req, 200, status::json()); }

esp_err_t handleConfigGet(httpd_req_t *req) { return sendJson(req, 200, config::toJson()); }

esp_err_t handleConfigPost(httpd_req_t *req) {
  std::string body;
  if (!readBody(req, &body, 32768)) return sendError(req, 400, "request too large or cut off");
  JsonDocument patch;
  if (deserializeJson(patch, body)) return sendError(req, 400, "invalid JSON");
  std::string err;
  if (!config::apply(patch.as<JsonVariantConst>(), &err)) return sendError(req, 400, err);
  note("settings changed from the web");
  return sendJson(req, 200, config::toJson());
}

// Show a message now, over whatever the program is showing: {"text": "...",
// "seconds": 60}. seconds 0 = until cleared; {"clear": true} returns to the program.
esp_err_t handleMessage(httpd_req_t *req) {
  std::string body;
  if (!readBody(req, &body, 2048)) return sendError(req, 400, "request too large");
  JsonDocument d;
  if (deserializeJson(d, body)) return sendError(req, 400, "invalid JSON");
  if (d["clear"] | false) {
    content::clearOverride();
    return sendJson(req, 200, "{\"cleared\":true}");
  }
  if (!d["text"].is<const char *>()) return sendError(req, 400, "expected {\"text\": \"...\"}");
  content::showOverride(d["text"].as<std::string>(), d["seconds"] | 60);
  return sendJson(req, 200, "{\"shown\":true}");
}

// Program state, and {fields} expanded as the sign would now: ?expand=...
esp_err_t handleContent(httpd_req_t *req) {
  const std::string tpl = query(req, "expand");
  if (!tpl.empty()) return sendJson(req, 200, "{\"text\":" + library::jsonStr(content::expand(tpl)) + "}");
  return sendJson(req, 200, content::statusJson());
}

// Overwrite one message file (the library editor). {"file": "x.txt", "text": "..."}
esp_err_t handleMessageSave(httpd_req_t *req) {
  std::string body;
  if (!readBody(req, &body, 64 * 1024)) return sendError(req, 400, "the file is too large (64 KB at most)");
  JsonDocument d;
  if (deserializeJson(d, body)) return sendError(req, 400, "invalid JSON");
  std::string name = d["file"] | "";
  if (name.size() < 5 || strcasecmp(name.c_str() + name.size() - 4, ".txt") != 0) name += ".txt";
  if (library::validateName(name) != library::Err::Ok) return sendError(req, 400, "not a usable file name");
  std::string abs;
  if (library::resolve("messages/" + name, &abs) != library::Err::Ok) return sendError(req, 400, "path not allowed");
  const std::string tmp = abs.substr(0, abs.rfind('/') + 1) + ".save.part";
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f) return sendError(req, 500, "cannot write to the SD card");
  const std::string text = d["text"] | "";
  const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
  fclose(f);
  if (!ok) {
    remove(tmp.c_str());
    return sendError(req, 507, "the SD card is full");
  }
  remove(abs.c_str());
  if (rename(tmp.c_str(), abs.c_str()) != 0) return sendError(req, 500, "could not replace the file");
  content::libraryChanged();
  const auto msgs = content::parseFile(name, text);
  return sendJson(req, 200, "{\"saved\":" + library::jsonStr("messages/" + name) + ",\"messages\":" + std::to_string(msgs.size()) + "}");
}

esp_err_t handleZones(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
  return httpd_resp_sendstr(req, zones::kZones);
}

esp_err_t handleWeather(httpd_req_t *req) {
  if (req->method == HTTP_POST) weather::refreshNow();
  return sendJson(req, 200, weather::statusJson());
}

// {"state": "off"} holds the sign (and the car's lights) off until
// {"state": "on"}; {"relay_test": 3} flips the relay the other way and back 3 times.
esp_err_t handlePower(httpd_req_t *req) {
  if (req->method == HTTP_POST) {
    std::string body;
    if (!readBody(req, &body, 256)) return sendError(req, 400, "request too large");
    JsonDocument d;
    if (deserializeJson(d, body)) return sendError(req, 400, "invalid JSON");
    const std::string st = d["state"] | "";
    if (st == "off") power::requestLatch(true);
    else if (st == "on") power::requestLatch(false);
    if (d["relay_test"].is<int>()) power::testRelay(std::min(10, std::max(1, d["relay_test"].as<int>())));
  }
  return sendJson(req, 200, power::statusJson());
}

// MQTT broker: {"host","port","user","password"}; a blank password keeps the
// stored one (it is never sent back to a browser).
esp_err_t handleMqtt(httpd_req_t *req) {
  if (req->method == HTTP_POST) {
    std::string body;
    if (!readBody(req, &body, 1024)) return sendError(req, 400, "request too large");
    JsonDocument d;
    if (deserializeJson(d, body)) return sendError(req, 400, "invalid JSON");
    mqtt::setServer(d["host"] | "", d["port"] | 1883, d["user"] | "", d["password"] | "");
  }
  return sendJson(req, 200, mqtt::statusJson());
}

esp_err_t handleMotion(httpd_req_t *req) { return sendJson(req, 200, motion::statusJson()); }

// The aiming view: 80x45 brightness bytes then 80x45 changed flags (binary).
// Only produced while someone has the page open; nothing is stored.
esp_err_t handleMotionView(httpd_req_t *req) {
  static uint8_t buf[motion::kGridW * motion::kGridH * 2];
  if (!motion::view(buf, buf + motion::kGridW * motion::kGridH)) return sendError(req, 409, "motion sensing is off");
  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, (const char *)buf, sizeof(buf));
}

// A grey PGM of what the camera sees, for checking it against the room.
esp_err_t handleMotionSnapshot(httpd_req_t *req) {
  static uint8_t *buf = (uint8_t *)heap_caps_malloc(motion::kSnapW * motion::kSnapH * 3, MALLOC_CAP_SPIRAM);
  if (!buf || !motion::snapshot(buf, 2000)) return sendError(req, 409, "the camera is not running");
  char hdr[32];
  const int n = snprintf(hdr, sizeof(hdr), "P6\n%d %d\n255\n", motion::kSnapW, motion::kSnapH);
  httpd_resp_set_type(req, "image/x-portable-pixmap");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send_chunk(req, hdr, n);
  httpd_resp_send_chunk(req, (const char *)buf, motion::kSnapW * motion::kSnapH * 3);
  return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t handleSchedulePage(httpd_req_t *req) { return sendAsset(req, "/schedule.html"); }

esp_err_t handleMessagesPage(httpd_req_t *req) { return sendAsset(req, "/messages.html"); }
esp_err_t handleSettingsPage(httpd_req_t *req) { return sendAsset(req, "/settings.html"); }
esp_err_t handleClockPage(httpd_req_t *req) { return sendAsset(req, "/clock.html"); }
esp_err_t handlePhotosPage(httpd_req_t *req) { return sendAsset(req, "/photos.html"); }

// Volume: {"volume": 0-100} and/or {"enabled": bool}; applied at once.
esp_err_t handleVolume(httpd_req_t *req) {
  std::string body;
  if (!readBody(req, &body, 256)) return sendError(req, 400, "request too large");
  JsonDocument d;
  if (deserializeJson(d, body)) return sendError(req, 400, "invalid JSON");
  if (d["enabled"].is<bool>()) sound::setEnabled(d["enabled"].as<bool>());
  if (d["volume"].is<int>()) sound::setVolume(d["volume"].as<int>(), true);
  char b[64];
  snprintf(b, sizeof(b), "{\"volume\":%d,\"enabled\":%s}", sound::volume(), sound::enabled() ? "true" : "false");
  return sendJson(req, 200, b);
}

esp_err_t handleLog(httpd_req_t *req) {
  std::string j = "[";
  bool first = true;
  // ?prev=1: the end of the run before this start (see note.h)
  for (auto &l : query(req, "prev") == "1" ? previousNotes() : recentNotes()) {
    j += (first ? "" : ",") + library::jsonStr(l);
    first = false;
  }
  return sendJson(req, 200, j + "]");
}

esp_err_t handleReboot(httpd_req_t *req) {
  g_reboot = true;
  return sendJson(req, 200, "{\"rebooting\":true}");
}

esp_err_t handleCoprocUpdate(httpd_req_t *req) {
  if (net::state() != net::State::Connected) return sendError(req, 409, "needs the internet: join a network first");
  net::requestCoprocUpdate();
  return sendJson(req, 200, "{\"updating\":true}");
}

esp_err_t handleNets(httpd_req_t *req) {
  if (query(req, "rescan") == "1") net::requestScan(10000);
  std::string j = "[";
  bool first = true;
  for (auto &n : net::lastScan()) {
    char b[64];
    snprintf(b, sizeof(b), ",\"rssi\":%d,\"open\":%s}", n.rssi, n.open ? "true" : "false");
    j += std::string(first ? "" : ",") + "{\"ssid\":" + library::jsonStr(n.ssid) + b;
    first = false;
  }
  return sendJson(req, 200, j + "]");
}

esp_err_t handleWifi(httpd_req_t *req) {
  std::string body;
  if (!readBody(req, &body, 512)) return sendError(req, 400, "bad form");
  const std::string ssid = formValue(body, "ssid");
  if (ssid.empty()) return sendError(req, 400, "no network name");
  net::requestJoin(ssid, formValue(body, "pass"));
  return sendJson(req, 200, "{\"joining\":true}");
}

esp_err_t handleWifiState(httpd_req_t *req) {
  const net::State st = net::state();
  const char *s = st == net::State::Connected ? "connected" : st == net::State::Connecting ? "joining"
                  : st == net::State::Failed ? "failed" : "off";
  return sendJson(req, 200, std::string("{\"state\":\"") + s + "\",\"ssid\":" + library::jsonStr(net::ssid()) +
                                ",\"ip\":" + library::jsonStr(net::ip()) + ",\"hostname\":" +
                                library::jsonStr(config::hostname() + ".local") + "}");
}

// ---- /library (the Pi frame's API) -----------------------------------------

esp_err_t handleLibList(httpd_req_t *req) {
  const std::string rel = library::normalise(query(req, "path"));
  std::vector<library::Entry> entries;
  const library::Err e = library::list(rel, &entries);
  if (e != library::Err::Ok) return sendError(req, httpCode(e), library::errText(e));
  uint64_t fb = 0, tb = 0;
  library::space(&fb, &tb);
  std::string j = "{\"path\":" + library::jsonStr(rel) + ",\"parent\":";
  if (rel.empty()) j += "null";
  else j += library::jsonStr(rel.find('/') == std::string::npos ? "" : rel.substr(0, rel.rfind('/')));
  j += ",\"entries\":[";
  for (size_t i = 0; i < entries.size(); i++) j += (i ? "," : "") + library::entryJson(entries[i]);
  char b[128];
  snprintf(b, sizeof(b), "],\"free_bytes\":%llu,\"total_bytes\":%llu,\"low_space\":%s}", (unsigned long long)fb,
           (unsigned long long)tb, tb && fb < 200ULL * 1024 * 1024 ? "true" : "false");
  return sendJson(req, 200, j + b);
}

esp_err_t handleLibDownload(httpd_req_t *req) {
  std::string abs;
  if (library::resolve(query(req, "path"), &abs) != library::Err::Ok) return sendError(req, 400, "path not allowed");
  struct stat st;
  if (stat(abs.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) return sendError(req, 404, "no such file");
  FILE *f = fopen(abs.c_str(), "rb");
  if (!f) return sendError(req, 500, "cannot open the file");
  const std::string name = abs.substr(abs.rfind('/') + 1);
  httpd_resp_set_type(req, mimeOf(name));
  const std::string disp = "inline; filename=\"" + name + "\"";
  httpd_resp_set_hdr(req, "Content-Disposition", disp.c_str());
  size_t n;
  while ((n = fread(g_buf, 1, kChunk, f)) > 0) {
    net::markBusy();
    if (httpd_resp_send_chunk(req, g_buf, n) != ESP_OK) {
      fclose(f);
      return ESP_FAIL;
    }
  }
  fclose(f);
  return httpd_resp_send_chunk(req, nullptr, 0);
}

// A photo's thumbnail lives beside it in a hidden .thumbs folder (made by the
// browser at upload time, so the Tab5 never scales a 12 MP photo itself).
std::string thumbOf(const std::string &abs) {
  const size_t s = abs.rfind('/');
  return abs.substr(0, s) + "/.thumbs/" + abs.substr(s + 1);
}

esp_err_t handleLibThumb(httpd_req_t *req) {
  std::string abs;
  if (library::resolve(query(req, "path"), &abs) != library::Err::Ok) return sendError(req, 400, "path not allowed");
  FILE *f = fopen(thumbOf(abs).c_str(), "rb");
  if (!f) return sendError(req, 404, "no thumbnail available");
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
  size_t n;
  while ((n = fread(g_buf, 1, kChunk, f)) > 0)
    if (httpd_resp_send_chunk(req, g_buf, n) != ESP_OK) {
      fclose(f);
      return ESP_FAIL;
    }
  fclose(f);
  return httpd_resp_send_chunk(req, nullptr, 0);
}

// Streams each file part to a hidden temp file in the destination folder,
// then renames it to a name that collides with nothing ("beach (2).jpg").
struct UploadSink : MultipartParser::Handler {
  std::string field, field_value, dest_rel;
  bool thumb = false;   // field thumb=1: this file is a thumbnail for <dest>/<name>
  bool in_file = false;
  FILE *f = nullptr;
  std::string tmp, dest_dir, filename;
  std::vector<std::string> saved, skipped;
  std::string err;
  int code = 200;

  bool setErr(int c, const std::string &e) {
    code = c;
    err = e;
    return false;
  }

  bool partBegin(const std::string &name, const std::string &fname) override {
    field = name;
    field_value.clear();
    in_file = !fname.empty();
    if (!in_file) return true;
    if (library::safeFileName(fname, &filename) != library::Err::Ok) {
      skipped.push_back(fname);
      in_file = false;   // swallow its data
      field.clear();
      return true;
    }
    if (library::resolve(dest_rel, &dest_dir) != library::Err::Ok) return setErr(400, "path not allowed");
    struct stat st;
    if (stat(dest_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
      return setErr(404, "destination folder does not exist");
    if (thumb) {
      dest_dir += "/.thumbs";
      mkdir(dest_dir.c_str(), 0777);
    }
    char t[48];
    snprintf(t, sizeof(t), "/.upload-%lu.part", (unsigned long)millis());
    tmp = dest_dir + t;
    f = fopen(tmp.c_str(), "wb");
    if (!f) return setErr(500, "cannot create the file on the SD card");
    return true;
  }

  bool partData(const uint8_t *d, size_t n) override {
    if (in_file) {
      if (fwrite(d, 1, n, f) != n)
        return setErr(507, errno == ENOSPC ? "the SD card is full" : "writing to the SD card failed");
      return true;
    }
    if (field == "path" && field_value.size() + n < 1024) field_value.append((const char *)d, n);
    return true;
  }

  bool partEnd() override {
    if (!in_file) {
      if (field == "path") dest_rel = library::normalise(field_value);
      if (field == "thumb") thumb = field_value == "1";
      return true;
    }
    fclose(f);
    f = nullptr;
    in_file = false;
    std::string final_abs;
    if (thumb) {   // a thumbnail replaces the old one of that name
      final_abs = dest_dir + "/" + filename;
      remove(final_abs.c_str());
      if (rename(tmp.c_str(), final_abs.c_str()) != 0) {
        remove(tmp.c_str());
        return setErr(500, "could not save the thumbnail");
      }
      saved.push_back(library::relativeOf(final_abs));
      return true;
    }
    if (library::uniqueDestination(dest_dir, filename, &final_abs) != library::Err::Ok ||
        rename(tmp.c_str(), final_abs.c_str()) != 0) {
      remove(tmp.c_str());
      return setErr(500, "could not move the upload into place");
    }
    saved.push_back(library::relativeOf(final_abs));
    return true;
  }

  void abort() {
    if (f) {
      fclose(f);
      f = nullptr;
      remove(tmp.c_str());
    }
  }
};

esp_err_t handleLibUpload(httpd_req_t *req) {
  net::markUploading();   // the speaker stops: a running speaker wedges the Wi-Fi link under uploads
  char ct[160] = "";
  httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct));
  if (req->content_len == 0) return sendError(req, 400, "no file data was received -- please try the upload again");
  UploadSink sink;
  MultipartParser p;
  if (!p.begin(ct, &sink)) return sendError(req, 400, p.error());
  size_t left = req->content_len;
  int timeouts = 0;
  while (left > 0) {
    const int n = httpd_req_recv(req, g_buf, left < kChunk ? left : kChunk);
    // A stalled sender (or a dead link) must end the upload rather than hold
    // the file open forever.
    if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
    if (n <= 0) {
      sink.abort();
      note("upload: connection lost with %u bytes to go", (unsigned)left);
      return ESP_FAIL;
    }
    timeouts = 0;
    net::markBusy();
    net::markUploading();
    left -= n;
    if (!p.feed((const uint8_t *)g_buf, n)) {
      sink.abort();
      return sendError(req, sink.err.empty() ? 400 : sink.code, sink.err.empty() ? p.error() : sink.err);
    }
  }
  sink.abort();   // a file part the body never finished
  if (!p.done()) return sendError(req, 400, "the upload was cut off");
  std::string j = "{\"saved\":[";
  for (size_t i = 0; i < sink.saved.size(); i++) j += (i ? "," : "") + library::jsonStr(sink.saved[i]);
  j += "],\"skipped\":[";
  for (size_t i = 0; i < sink.skipped.size(); i++) j += (i ? "," : "") + library::jsonStr(sink.skipped[i]);
  for (auto &s : sink.saved) {
    note("upload: %s", s.c_str());
    if (s.rfind("sounds/", 0) == 0) sound::reloadClips();   // a new clack takes effect at once
    if (s.rfind("messages/", 0) == 0 || s.rfind("photos/", 0) == 0) content::libraryChanged();
  }
  return sendJson(req, 200, j + "]}");
}

// Uploads in pieces: POST /library/upload_part?path=<folder>&name=<file>
// &id=<upload id>&offset=<n>&total=<size>[&thumb=1], the body being that
// piece (<= 64 KB). The ESP32-C6's SDIO Wi-Fi link wedges on large inbound
// bursts (esp-hosted-mcu#184 class: 60 KB uploads went through, 288 KB ones
// killed the link); a request can never burst more than its own body, so the
// web page sends 32 KB pieces. The last piece moves the file into place.
esp_err_t handleLibUploadPart(httpd_req_t *req) {
  net::markUploading();
  const std::string rel = library::normalise(query(req, "path")), name0 = query(req, "name"), id = query(req, "id");
  const long offset = atol(query(req, "offset").c_str()), total = atol(query(req, "total").c_str());
  const bool thumb = query(req, "thumb") == "1";
  std::string dir, name;
  if (library::resolve(rel, &dir) != library::Err::Ok || library::safeFileName(name0, &name) != library::Err::Ok)
    return sendError(req, 400, "path or name not allowed");
  for (char c : id)
    if (!isalnum((unsigned char)c)) return sendError(req, 400, "bad upload id");
  if (id.empty() || req->content_len > 65536 || offset < 0 || total < offset + (long)req->content_len)
    return sendError(req, 400, "bad piece");
  struct stat st;
  if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return sendError(req, 404, "destination folder does not exist");
  if (thumb) {
    dir += "/.thumbs";
    mkdir(dir.c_str(), 0777);
  }
  const std::string tmp = dir + "/.part-" + id;
  if (offset == 0) remove(tmp.c_str());
  else if (stat(tmp.c_str(), &st) != 0 || st.st_size < offset) {
    // A gap (a restart lost the end of the file): tell the sender where to resume.
    const long have = stat(tmp.c_str(), &st) == 0 ? (long)st.st_size : 0;
    return sendJson(req, 409, "{\"error\":\"resume from an earlier piece\",\"have\":" + std::to_string(have) + "}");
  } else if (st.st_size > offset) {
    // The same piece again (its reply was lost, or the sign restarted while
    // writing it): drop what came after `offset` and take it again.
    if (FILE *t = fopen(tmp.c_str(), "r+b")) {
      ftruncate(fileno(t), offset);
      fclose(t);
    }
  }
  FILE *f = fopen(tmp.c_str(), offset == 0 ? "wb" : "ab");
  if (!f) return sendError(req, 500, "cannot write to the SD card");
  size_t left = req->content_len;
  int timeouts = 0;
  while (left > 0) {
    const int n = httpd_req_recv(req, g_buf, left < kChunk ? left : kChunk);
    if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
    if (n <= 0 || fwrite(g_buf, 1, n, f) != (size_t)n) {
      fclose(f);
      // Truncate back to where this piece started, so a retry of it fits.
      if (FILE *t = fopen(tmp.c_str(), "r+b")) {
        ftruncate(fileno(t), offset);
        fclose(t);
      }
      return n <= 0 ? ESP_FAIL : sendError(req, 507, errno == ENOSPC ? "the SD card is full" : "writing failed");
    }
    left -= n;
    timeouts = 0;
    net::markBusy();
  }
  fclose(f);
  const long have = offset + (long)req->content_len;
  if (have < total) return sendJson(req, 200, "{\"received\":" + std::to_string(have) + "}");
  std::string final_abs;
  if (thumb) {
    final_abs = dir + "/" + name;
    remove(final_abs.c_str());
  } else if (library::uniqueDestination(dir, name, &final_abs) != library::Err::Ok) {
    remove(tmp.c_str());
    return sendError(req, 409, "too many files with that name");
  }
  if (rename(tmp.c_str(), final_abs.c_str()) != 0) {
    remove(tmp.c_str());
    return sendError(req, 500, "could not move the upload into place");
  }
  const std::string saved = library::relativeOf(final_abs);
  if (!thumb) {
    note("upload: %s", saved.c_str());
    if (saved.rfind("sounds/", 0) == 0) sound::reloadClips();
    if (saved.rfind("messages/", 0) == 0 || saved.rfind("photos/", 0) == 0) content::libraryChanged();
  }
  return sendJson(req, 200, "{\"saved\":[" + library::jsonStr(saved) + "],\"skipped\":[]}");
}

// ---- firmware update -------------------------------------------------------------
//
// POST /api/ota?id=<id>&offset=<n>&total=<size>, body = the next piece of the
// .bin (<= 16 KB, the same reason as uploads). Written straight into the
// spare app slot; the last piece checks the image, makes it the boot slot and
// restarts. The bootloader's rollback is on: the new firmware runs on trial
// and main.cpp marks it good once it is healthy, else the next restart goes
// back to the old one.
struct OtaState {
  std::string id;
  esp_ota_handle_t handle = 0;
  const esp_partition_t *part = nullptr;
  long next = 0;
} g_ota;
volatile uint32_t g_ota_seen_ms = 0;   // last piece; 0 = no update under way
volatile bool g_ota_done = false;

esp_err_t handleOta(httpd_req_t *req) {
  net::markUploading();
  g_ota_seen_ms = millis() | 1;
  const std::string id = query(req, "id");
  const long offset = atol(query(req, "offset").c_str()), total = atol(query(req, "total").c_str());
  if (id.empty() || req->content_len > 65536 || total < offset + (long)req->content_len || total > 6 * 1024 * 1024)
    return sendError(req, 400, "bad piece");
  if (offset == 0) {
    if (g_ota.handle) esp_ota_abort(g_ota.handle);
    g_ota = OtaState();
    // The main loop blanks the screen (power.cpp) within ~250 ms; the first
    // erase would otherwise flash the panel white before that.
    for (int i = 0; i < 30 && !power::blankedForUpdate(); i++) vTaskDelay(pdMS_TO_TICKS(20));
    g_ota.part = esp_ota_get_next_update_partition(nullptr);
    if (!g_ota.part) return sendError(req, 500, "no spare firmware slot (partition table)");
    // Sequential writes: each sector is erased as it is reached, not all 6 MB up front.
    if (esp_ota_begin(g_ota.part, OTA_WITH_SEQUENTIAL_WRITES, &g_ota.handle) != ESP_OK)
      return sendError(req, 500, "could not start the update");
    g_ota.id = id;
    note("update: receiving %ld bytes into %s", total, g_ota.part->label);
  } else if (id != g_ota.id || offset != g_ota.next) {
    return sendError(req, 409, "update pieces out of order; start again");
  }
  size_t left = req->content_len;
  int timeouts = 0;
  while (left > 0) {
    const int n = httpd_req_recv(req, g_buf, left < kChunk ? left : kChunk);
    if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
    if (n <= 0) return ESP_FAIL;   // the browser retries this piece (offset unchanged)
    if (esp_ota_write(g_ota.handle, g_buf, n) != ESP_OK) {
      esp_ota_abort(g_ota.handle);
      g_ota = OtaState();
      return sendError(req, 500, "writing the update failed");
    }
    g_ota.next += n;
    left -= n;
    timeouts = 0;
    net::markBusy();
  }
  if (g_ota.next < total) return sendJson(req, 200, "{\"received\":" + std::to_string(g_ota.next) + "}");
  const esp_err_t e = esp_ota_end(g_ota.handle);   // checks the image (magic, size, SHA-256)
  g_ota.handle = 0;
  if (e != ESP_OK) {
    g_ota = OtaState();
    return sendError(req, 400, e == ESP_ERR_OTA_VALIDATE_FAILED ? "that is not a FlapBoard firmware image" : "the update did not verify");
  }
  if (esp_ota_set_boot_partition(g_ota.part) != ESP_OK) return sendError(req, 500, "could not switch to the new firmware");
  note("update: %ld bytes written to %s; restarting into it", total, g_ota.part->label);
  g_ota_done = true;
  g_reboot = true;
  return sendJson(req, 200, "{\"done\":true}");
}

// ---- backup and restore ------------------------------------------------------------

// Settings and message files as one JSON download. No passwords (Wi-Fi and
// MQTT stay on the sign), no photos (too big; copy the card for those).
esp_err_t handleBackup(httpd_req_t *req) {
  JsonDocument d;
  d["flapboard_backup"] = 1;
  d["version"] = FLAPBOARD_VERSION;
  d["device_name"] = config::deviceName();
  JsonDocument cfg;
  deserializeJson(cfg, config::toJson());
  d["settings"] = cfg;
  std::vector<library::Entry> files;
  if (library::list("messages", &files) == library::Err::Ok) {
    for (auto &e : files) {
      if (e.dir || e.size > 64 * 1024) continue;
      std::string abs;
      if (library::resolve(e.path, &abs) != library::Err::Ok) continue;
      FILE *f = fopen(abs.c_str(), "rb");
      if (!f) continue;
      std::string text(e.size, '\0');
      const size_t n = fread(&text[0], 1, e.size, f);
      fclose(f);
      text.resize(n);
      d["messages"][e.name] = text;
    }
  }
  std::string out;
  serializeJson(d, out);
  httpd_resp_set_type(req, "application/json");
  const std::string disp = "attachment; filename=\"" + config::hostname() + "-backup.json\"";
  httpd_resp_set_hdr(req, "Content-Disposition", disp.c_str());
  return httpd_resp_send(req, out.data(), out.size());
}

// {"file": "<library path of an uploaded backup>"}: applies its settings
// (keys this firmware knows) and writes its message files, then deletes it.
esp_err_t handleRestore(httpd_req_t *req) {
  std::string body;
  if (!readBody(req, &body, 1024)) return sendError(req, 400, "request too large");
  JsonDocument q;
  if (deserializeJson(q, body)) return sendError(req, 400, "invalid JSON");
  std::string abs;
  if (library::resolve(q["file"] | "", &abs) != library::Err::Ok) return sendError(req, 400, "path not allowed");
  FILE *f = fopen(abs.c_str(), "rb");
  if (!f) return sendError(req, 404, "the backup file was not found");
  std::string text;
  size_t n;
  while ((n = fread(g_buf, 1, kChunk, f)) > 0 && text.size() < 2 * 1024 * 1024) text.append(g_buf, n);
  fclose(f);
  remove(abs.c_str());
  JsonDocument d;
  if (deserializeJson(d, text) || !(d["flapboard_backup"] | 0)) return sendError(req, 400, "that is not a FlapBoard backup");
  const int applied = config::applyKnown(d["settings"]);
  int written = 0;
  for (JsonPairConst kv : d["messages"].as<JsonObjectConst>()) {
    std::string name = kv.key().c_str(), dest;
    if (library::validateName(name) != library::Err::Ok || library::resolve("messages/" + name, &dest) != library::Err::Ok) continue;
    FILE *o = fopen(dest.c_str(), "wb");
    if (!o) continue;
    const std::string t = kv.value().as<std::string>();
    fwrite(t.data(), 1, t.size(), o);
    fclose(o);
    written++;
  }
  content::libraryChanged();
  note("restore: %d settings, %d message files from a %s backup", applied, written, (const char *)(d["version"] | "?"));
  return sendJson(req, 200, "{\"settings\":" + std::to_string(applied) + ",\"messages\":" + std::to_string(written) + "}");
}

// {"confirm":"ERASE"}: formats the whole card as FAT32 at the next start
// (see main.cpp, formatCardIfAsked) and restarts now.
esp_err_t handleSdFormat(httpd_req_t *req) {
  std::string body;
  if (!readBody(req, &body, 256)) return sendError(req, 400, "request too large");
  JsonDocument q;
  if (deserializeJson(q, body) || std::string(q["confirm"] | "") != "ERASE") return sendError(req, 400, "not confirmed");
  Preferences p;
  p.begin("flapboard", false);
  p.putBool("sd_format", true);
  p.end();
  note("sd: format asked from the web page; restarting to do it");
  g_reboot = true;
  return sendJson(req, 200, "{\"restarting\":true}");
}

esp_err_t handleLibOp(httpd_req_t *req) {
  std::string body;
  if (!readBody(req, &body)) return sendError(req, 400, "request too large");
  JsonDocument d;
  if (body.empty()) body = "{}";
  if (deserializeJson(d, body)) return sendError(req, 400, "invalid JSON body");
  const std::string path = d["path"] | "", name = d["name"] | "";
  const std::string uri = req->uri;
  std::string out;
  library::Err e;
  std::string reply;
  if (uri.rfind("/library/mkdir", 0) == 0) {
    e = library::makeDir(path, name, &out);
    reply = "{\"created\":" + library::jsonStr(out) + "}";
  } else if (uri.rfind("/library/delete", 0) == 0) {
    std::string abs;
    if (library::resolve(path, &abs) == library::Err::Ok) remove(thumbOf(abs).c_str());   // its thumbnail, if any
    e = library::remove(path);
    if (e == library::Err::Ok && library::normalise(path).rfind("sounds", 0) == 0) sound::reloadClips();
    if (e == library::Err::Ok && (library::normalise(path).rfind("messages", 0) == 0 || library::normalise(path).rfind("photos", 0) == 0))
      content::libraryChanged();
    reply = "{\"deleted\":" + library::jsonStr(path) + "}";
  } else {
    std::string old_abs;
    library::resolve(path, &old_abs);
    e = library::rename(path, name, &out);
    std::string new_abs;
    if (e == library::Err::Ok && library::resolve(out, &new_abs) == library::Err::Ok)
      rename(thumbOf(old_abs).c_str(), thumbOf(new_abs).c_str());   // the thumbnail follows
    reply = "{\"renamed\":" + library::jsonStr(out) + "}";
  }
  if (e != library::Err::Ok) return sendError(req, httpCode(e), library::errText(e));
  return sendJson(req, 200, reply);
}

}  // namespace

void begin() {
  g_buf = (char *)heap_caps_malloc(kChunk, MALLOC_CAP_SPIRAM);
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size = 10240;
  cfg.max_uri_handlers = 72;
  cfg.uri_match_fn = httpd_uri_match_wildcard;   // for /fonts/*
  cfg.lru_purge_enable = true;
  cfg.recv_wait_timeout = 20;
  cfg.send_wait_timeout = 20;
  if (httpd_start(&g_server, &cfg) != ESP_OK) {
    note("web: server failed to start");
    g_server = nullptr;
    return;
  }
  const struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*fn)(httpd_req_t *);
  } routes[] = {
      {"/", HTTP_GET, handleRoot},
      {"/library", HTTP_GET, handleLibraryPage},
      {"/setup", HTTP_GET, handleSetup},
      {"/display", HTTP_GET, handleDisplayPage},
      {"/api/fonts", HTTP_GET, handleFontList},
      {"/fonts/*", HTTP_GET, handleFontFile},
      {"/api/status", HTTP_GET, handleStatus},
      {"/api/config", HTTP_GET, handleConfigGet},
      {"/api/config", HTTP_POST, handleConfigPost},
      {"/api/log", HTTP_GET, handleLog},
      {"/api/message", HTTP_POST, handleMessage},
      {"/api/content", HTTP_GET, handleContent},
      {"/api/messages/save", HTTP_POST, handleMessageSave},
      {"/api/zones", HTTP_GET, handleZones},
      {"/api/weather", HTTP_GET, handleWeather},
      {"/api/weather", HTTP_POST, handleWeather},
      {"/messages", HTTP_GET, handleMessagesPage},
      {"/settings", HTTP_GET, handleSettingsPage},
      {"/schedule", HTTP_GET, handleSchedulePage},
      {"/api/power", HTTP_GET, handlePower},
      {"/api/mqtt", HTTP_GET, handleMqtt},
      {"/api/mqtt", HTTP_POST, handleMqtt},
      {"/api/motion", HTTP_GET, handleMotion},
      {"/api/motion/view", HTTP_GET, handleMotionView},
      {"/api/motion/snapshot", HTTP_GET, handleMotionSnapshot},
      {"/api/power", HTTP_POST, handlePower},
      {"/clock", HTTP_GET, handleClockPage},
      {"/photos", HTTP_GET, handlePhotosPage},
      {"/api/volume", HTTP_POST, handleVolume},
      {"/api/reboot", HTTP_POST, handleReboot},
      {"/api/c6update", HTTP_POST, handleCoprocUpdate},
      {"/api/nets", HTTP_GET, handleNets},
      {"/api/wifi", HTTP_POST, handleWifi},
      {"/api/wifistate", HTTP_GET, handleWifiState},
      {"/library/list", HTTP_GET, handleLibList},
      {"/library/download", HTTP_GET, handleLibDownload},
      {"/library/thumb", HTTP_GET, handleLibThumb},
      {"/library/upload", HTTP_POST, handleLibUpload},
      {"/library/upload_part", HTTP_POST, handleLibUploadPart},
      {"/api/ota", HTTP_POST, handleOta},
      {"/api/backup", HTTP_GET, handleBackup},
      {"/api/restore", HTTP_POST, handleRestore},
      {"/api/sdformat", HTTP_POST, handleSdFormat},
      {"/library/mkdir", HTTP_POST, handleLibOp},
      {"/library/delete", HTTP_POST, handleLibOp},
      {"/library/rename", HTTP_POST, handleLibOp},
  };
  for (auto &r : routes) {
    httpd_uri_t u = {};
    u.uri = r.uri;
    u.method = r.method;
    u.handler = r.fn;
    httpd_register_uri_handler(g_server, &u);
  }
  httpd_register_err_handler(g_server, HTTPD_404_NOT_FOUND, handleOther);
}

bool updating() {
  const uint32_t seen = g_ota_seen_ms;
  return g_ota_done || (seen && millis() - seen < 30000);
}

bool takeRebootRequest() {
  const bool r = g_reboot;
  g_reboot = false;
  return r;
}

}  // namespace web
}  // namespace flapboard
