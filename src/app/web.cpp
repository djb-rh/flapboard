#include "web.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <sys/stat.h>

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
  return sendAsset(req, "/index.html");
}
esp_err_t handleLibraryPage(httpd_req_t *req) { return sendAsset(req, "/library.html"); }
esp_err_t handleSetup(httpd_req_t *req) { return sendAsset(req, "/setup.html"); }
esp_err_t handleDisplayPage(httpd_req_t *req) { return sendAsset(req, "/display.html"); }

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
// {"state": "on"}; {"relay_test": 3} switches the relay on for 3 s.
esp_err_t handlePower(httpd_req_t *req) {
  if (req->method == HTTP_POST) {
    std::string body;
    if (!readBody(req, &body, 256)) return sendError(req, 400, "request too large");
    JsonDocument d;
    if (deserializeJson(d, body)) return sendError(req, 400, "invalid JSON");
    const std::string st = d["state"] | "";
    if (st == "off") power::requestLatch(true);
    else if (st == "on") power::requestLatch(false);
    if (d["relay_test"].is<int>()) power::testRelay(std::min(30, std::max(1, d["relay_test"].as<int>())));
  }
  return sendJson(req, 200, power::statusJson());
}

esp_err_t handleSchedulePage(httpd_req_t *req) { return sendAsset(req, "/schedule.html"); }

esp_err_t handleMessagesPage(httpd_req_t *req) { return sendAsset(req, "/messages.html"); }
esp_err_t handleClockPage(httpd_req_t *req) { return sendAsset(req, "/clock.html"); }

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
  for (auto &l : recentNotes()) {
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

// Thumbnails arrive with photo mode (Phase 9); until then the page shows an icon.
esp_err_t handleLibThumb(httpd_req_t *req) { return sendError(req, 404, "no thumbnail available"); }

// Streams each file part to a hidden temp file in the destination folder,
// then renames it to a name that collides with nothing ("beach (2).jpg").
struct UploadSink : MultipartParser::Handler {
  std::string field, field_value, dest_rel;
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
      return true;
    }
    fclose(f);
    f = nullptr;
    in_file = false;
    std::string final_abs;
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
    if (s.rfind("messages/", 0) == 0) content::libraryChanged();
  }
  return sendJson(req, 200, j + "]}");
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
    e = library::remove(path);
    if (e == library::Err::Ok && library::normalise(path).rfind("sounds", 0) == 0) sound::reloadClips();
    if (e == library::Err::Ok && library::normalise(path).rfind("messages", 0) == 0) content::libraryChanged();
    reply = "{\"deleted\":" + library::jsonStr(path) + "}";
  } else {
    e = library::rename(path, name, &out);
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
  cfg.max_uri_handlers = 56;
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
      {"/schedule", HTTP_GET, handleSchedulePage},
      {"/api/power", HTTP_GET, handlePower},
      {"/api/power", HTTP_POST, handlePower},
      {"/clock", HTTP_GET, handleClockPage},
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

bool takeRebootRequest() {
  const bool r = g_reboot;
  g_reboot = false;
  return r;
}

}  // namespace web
}  // namespace flapboard
