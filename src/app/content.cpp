#include "content.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <dirent.h>
#include <esp_random.h>
#include <sys/stat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <flapcore/template.h>
#include <ff.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <cstring>

#include "clock.h"
#include "config.h"
#include "net.h"
#include "photos.h"
#include "schedule.h"
#include "note.h"
#include "sdcard.h"
#include "sign.h"
#include "weather.h"

namespace flapboard {
namespace content {
namespace {

std::vector<Message> g_lib;
bool g_lib_dirty = true;
uint32_t g_lib_loaded = 0;
std::string g_source, g_key;   // program settings in use (a change restarts the program)
bool g_border_messages = false, g_border_clock = false, g_border_weather = false;
bool g_border = false;         // the running program's
Message g_cur;                 // the message being shown
bool g_have = false;
int g_index = -1;              // position in the library (sequential order)
uint32_t g_next_ms = 0;        // when to move to the next message
std::string g_shown;           // the expanded text last sent to the board
std::string g_override;
std::vector<photos::Photo> g_photos;   // everything under photos/, rescanned when the library changes
bool g_photos_dirty = true;
uint32_t g_photos_loaded = 0;
std::string g_last_photo;
std::vector<std::string> g_bad_photos;   // failed twice running: skipped until the library changes
std::string g_failed_once;
uint32_t g_override_until = 0;
bool g_override_on = false;

std::string messagesDir() { return std::string(sdcard::mountPoint()) + "/flapboard/messages"; }

std::string readFile(const std::string &path) {
  std::string s;
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return s;
  char buf[1024];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && s.size() < 256 * 1024) s.append(buf, n);
  fclose(f);
  return s;
}

// First boot: a few messages so the sign has something to say, and the
// file explains its own format.
const char kStarter[] =
    "# Messages are separated by a blank line. A line starting with # is a comment.\n"
    "# Each line is a row; for an empty row inside a message use | (\"DINING||OPEN\").\n"
    "# Options go on a message's first line: @left @right @center @top @hold 45\n"
    "# Fill-in fields: {time} {date} {temp} {hi} {lo} {cond} {place} {name} {ip}\n"
    "# Colour tiles: {R} {O} {Y} {G} {B} {V} {W} {K}\n"
    "\nWELCOME ABOARD\n{name}\n"
    "\nNEXT STOP\nGRAND CENTRAL\n"
    "\n{R}{O}{Y}{G}{B}{V}{R}{O}{Y}{G}{B}{V}{R}{O}{Y}{G}{B}{V}{R}{O}{Y}{G}\n|\nDINING CAR OPEN\nUNTIL 9:30 PM\n|\n"
    "{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}{G}\n"
    "\n@hold 30\nIT IS {time}\n{date:%A %B %-d}\n"
    "\nNOW {temp}\u00B0 {cond}\nHI {hi}  LO {lo}\n";
void seedLibrary() {
  const std::string dir = messagesDir();
  DIR *d = opendir(dir.c_str());
  if (!d) return;
  bool any = false;
  while (dirent *e = readdir(d)) {   // message files only: side-pictures/ lives here too
    const size_t n = strlen(e->d_name);
    any |= e->d_name[0] != '.' && n > 4 && strcasecmp(e->d_name + n - 4, ".txt") == 0;
  }
  closedir(d);
  if (any) return;
  FILE *f = fopen((dir + "/welcome.txt").c_str(), "wb");
  if (!f) return;
  fputs(kStarter, f);
  fclose(f);
  note("content: wrote messages/welcome.txt to start the library");
}

void loadLibrary(const std::vector<std::string> &only) {
  g_lib.clear();
  if (!sdcard::mounted()) return;
  std::vector<std::string> files;
  if (DIR *d = opendir(messagesDir().c_str())) {
    while (dirent *e = readdir(d)) {
      const std::string n = e->d_name;
      if (n[0] == '.' || n.size() < 5 || strcasecmp(n.c_str() + n.size() - 4, ".txt") != 0) continue;
      if (!only.empty() && std::find(only.begin(), only.end(), n) == only.end()) continue;
      files.push_back(n);
    }
    closedir(d);
  }
  std::sort(files.begin(), files.end());
  for (auto &f : files) {
    auto m = parseFile(f, readFile(messagesDir() + "/" + f));
    g_lib.insert(g_lib.end(), m.begin(), m.end());
  }
  g_lib_loaded = millis();
  g_lib_dirty = false;
}

bool lookup(const std::string &name, const std::string &arg, std::string *out) {
  if (name == "time" || name == "date") {
    struct tm t;
    if (!clock::localNow(&t)) {
      *out = name == "time" ? "--:--" : "";
      return true;
    }
    std::string fmt = arg;
    if (fmt.empty()) {
      config::Reader r;
      fmt = name == "time" ? (r.doc()["time_format"] | "%-I:%M %p") : (r.doc()["date_format"] | "%a %b %-d");
    }
    *out = flapcore::formatTime(fmt, t);
    return true;
  }
  if (name == "name") {
    *out = config::deviceName();
    return true;
  }
  if (name == "hostname") {
    *out = config::hostname() + ".local";
    return true;
  }
  if (name == "ip") {
    *out = net::ip().empty() ? "NO WI-FI" : net::ip();
    return true;
  }
  return weather::field(name, out);
}

// The scheduler decides which program runs; the clock and weather layouts
// are global settings.
struct Program {
  std::string source = "messages", order = "random", text, clock_tpl, weather_tpl;
  bool border = false;         // argyle border down each side (per source: messages, clock, weather)
  std::string prefix, suffix;  // added before / after every library message (empty = off); fields allowed
  bool rb_mode = false;        // RB Mode: on Fridays the clock adds friday_text
  std::string friday_text;
  std::vector<std::string> files;
  int dwell = 20;
};

schedule::Program programFrom(JsonVariantConst v) {
  schedule::Program p;
  p.source = v["source"] | "";
  p.order = v["order"] | "random";
  p.text = v["text"] | "";
  p.dwell = v["dwell"] | 20;
  for (JsonVariantConst f : v["files"].as<JsonArrayConst>()) p.files.push_back(f.as<std::string>());
  return p;
}

std::vector<schedule::Rule> rulesFrom(JsonVariantConst arr) {
  std::vector<schedule::Rule> out;
  for (JsonVariantConst v : arr.as<JsonArrayConst>()) {
    schedule::Rule r;
    r.name = v["name"] | "";
    r.date = v["date"] | "";
    r.start = v["start"] | "";
    r.end = v["end"] | "";
    for (JsonVariantConst d : v["days"].as<JsonArrayConst>()) r.days.push_back(d.as<int>());
    r.program = programFrom(v["program"]);
    out.push_back(r);
  }
  return out;
}

std::string g_reason = "default";

Program readProgram() {
  Program p;
  schedule::Program def;
  bool enabled;
  std::vector<schedule::Rule> rules, overrides;
  {
    config::Reader r;
    auto &d = r.doc();
    def.source = d["content_source"] | "messages";
    def.order = d["content_order"] | "random";
    def.dwell = d["content_dwell"] | 20;
    def.text = d["content_text"] | "";
    for (JsonVariantConst v : d["content_files"].as<JsonArrayConst>()) def.files.push_back(v.as<std::string>());
    if (def.source == "photos") {   // photo mode keeps its own selection, order and timing
      def.files.clear();
      for (JsonVariantConst v : d["photo_selection"].as<JsonArrayConst>()) def.files.push_back(v.as<std::string>());
      def.order = d["photo_order"] | "random";
      def.dwell = d["photo_dwell"] | 30;
    }
    p.clock_tpl = d["clock_template"] | "{time}|{date}";
    p.rb_mode = d["clock_rb_mode"] | false;
    if (d["msg_prefix_on"] | false) p.prefix = d["msg_prefix"] | "";
    if (d["msg_suffix_on"] | false) p.suffix = d["msg_suffix"] | "";
    g_border_messages = d["border_messages"] | false;
    g_border_clock = d["border_clock"] | false;
    g_border_weather = d["border_weather"] | false;
    p.friday_text = d["clock_friday_text"] | "";
    p.weather_tpl = d["weather_template"] | "{place}|NOW {temp}\u00B0 {cond}|HI {hi}  LO {lo}";
    enabled = d["schedule_enabled"] | false;
    rules = rulesFrom(d["schedule_rules"]);
    overrides = rulesFrom(d["schedule_overrides"]);
  }
  schedule::Now now;
  struct tm t;
  if (clock::localNow(&t)) {
    now.year = t.tm_year + 1900;
    now.month = t.tm_mon + 1;
    now.day = t.tm_mday;
    now.weekday = (t.tm_wday + 6) % 7;
    now.minutes = t.tm_hour * 60 + t.tm_min;
  }
  const schedule::Choice c = schedule::chooseProgram(def, enabled, rules, overrides, clock::trusted(), now);
  g_reason = c.reason;
  p.source = c.program.source;
  p.order = c.program.order;
  p.text = c.program.text;
  p.files = c.program.files;
  p.dwell = std::max(3, c.program.dwell);
  p.border = p.source == "clock" ? g_border_clock : p.source == "weather" ? g_border_weather : g_border_messages;
  return p;
}

// FatFs's own directory walk: each entry comes with its date, so the scan is
// one pass per folder. (A POSIX stat per file made FatFs search the folder
// from the top every time: with 417 photos in one folder, switching to
// photos took over 15 s and the watchdog restarted the sign.) Paths under
// the mount point map to drive 0; anything else falls back to readdir.
bool fatList(const std::string &abs, std::vector<photos::DirEntry> *out) {
  const std::string mp = sdcard::mountPoint();
  if (abs.rfind(mp, 0) != 0) return false;
  const std::string path = "0:" + abs.substr(mp.size());
  FF_DIR dir;
  FILINFO fi;
  if (f_opendir(&dir, path.c_str()) != FR_OK) return false;
  int n = 0;
  while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
    if (fi.fname[0] == '.') continue;
    photos::DirEntry e;
    e.name = fi.fname;
    e.dir = (fi.fattrib & AM_DIR) != 0;
    struct tm t = {};
    t.tm_year = ((fi.fdate >> 9) & 0x7F) + 80;
    t.tm_mon = ((fi.fdate >> 5) & 0x0F) - 1;
    t.tm_mday = fi.fdate & 0x1F;
    t.tm_hour = (fi.ftime >> 11) & 0x1F;
    t.tm_min = (fi.ftime >> 5) & 0x3F;
    t.tm_sec = (fi.ftime & 0x1F) * 2;
    e.mtime = mktime(&t);
    out->push_back(e);
    if (++n % 200 == 0) esp_task_wdt_reset();   // a big folder on a slow card
  }
  f_closedir(&dir);
  return true;
}

std::string programKey(const Program &p) {
  std::string k = p.source + "|" + p.order + "|" + std::to_string(p.dwell) + "|" + p.text + "|" + p.clock_tpl + "|" + (p.rb_mode ? "rb:" + p.friday_text : "") + "|" +
                  p.weather_tpl + "|" + (p.border ? "border|" : "|") + p.prefix + "|" + p.suffix + "|";
  for (auto &f : p.files) k += f + ",";
  return k;
}

// The next library message: random (never the same twice running, when
// there is a choice) or in file order, continuing from the last one.
bool pickNext(const Program &p) {
  if (g_lib.empty()) return false;
  if (p.order == "sequential") g_index = (g_index + 1) % (int)g_lib.size();
  else if (g_lib.size() == 1) g_index = 0;
  else {
    int n;
    do n = (int)(esp_random() % g_lib.size());
    while (n == g_index);
    g_index = n;
  }
  g_cur = g_lib[g_index];
  return true;
}

void push(bool force) {
  const std::string text = expand(g_cur.text);
  if (!force && text == g_shown) return;
  g_shown = text;
  sign::show(text, g_cur.align, !g_cur.top, g_border);
}

}  // namespace

std::string expand(const std::string &tpl) { return flapcore::expandTemplate(tpl, lookup); }

void begin() {
  if (sdcard::mounted()) seedLibrary();
}

void libraryChanged() {
  g_lib_dirty = true;
  g_photos_dirty = true;
  g_bad_photos.clear();
}

void publishStatus();
void showOverride(const std::string &text, int seconds) {
  g_override = text;
  g_override_on = true;
  g_override_until = seconds > 0 ? millis() + (uint32_t)seconds * 1000 : 0;
  g_shown.clear();
  if (sign::photoMode()) sign::photoCaption(expand(text));
  else sign::show(expand(text), 1, true, g_border_messages);
  publishStatus();
}

void next() { g_next_ms = millis(); }
std::string overrideText() {
  JsonDocument d;
  deserializeJson(d, statusJson());
  return (d["override"] | false) ? std::string(d["override_text"] | "") : std::string();
}

void clearOverride() {
  g_override_on = false;
  g_shown.clear();
  if (sign::photoMode()) sign::photoCaption("");
  publishStatus();
}

void loopInner();
void loop() {
  static uint32_t last = 0, gen = 0;
  // once a second, or straight away after a settings change (a new program
  // shouldn't wait up to a second to start)
  if (millis() - last < 1000 && config::generation() == gen) return;
  last = millis();
  gen = config::generation();
  loopInner();
  publishStatus();
}

void loopInner() {
  const Program p = readProgram();
  const std::string key = programKey(p);
  if (key != g_key) {   // new program: start it now
    g_key = key;
    g_source = p.source;
    g_border = p.border;
    g_lib_dirty = true;
    g_have = false;
    g_index = -1;
  }
  if (g_lib_dirty || millis() - g_lib_loaded > 60000) loadLibrary(p.files);
  const bool photo_program = p.source == "photos";
  if (!photo_program && sign::photoMode()) {   // back to the board
    sign::showBoard();
    g_shown.clear();
    g_have = false;
  }
  if (g_override_on) {
    if (g_override_until && (int32_t)(millis() - g_override_until) >= 0) {
      g_override_on = false;
      g_shown.clear();
      if (sign::photoMode()) sign::photoCaption("");
    } else {
      const std::string t = expand(g_override);   // an override's fields keep ticking too
      if (t != g_shown) {
        g_shown = t;
        if (sign::photoMode()) sign::photoCaption(t);   // over the photo, not instead of it
        else sign::show(t, 1, true, g_border_messages);   // a sent message looks like the library's
      }
      if (!sign::photoMode()) return;   // in photo mode the slideshow carries on underneath
    }
  }
  if (photo_program) {
    // While an upload is arriving, leave the card alone: the SD slot shares
    // the P4's SDIO host with the Wi-Fi chip, and a photo decode or a rescan
    // of the library (after every uploaded file) in the middle of one wedged
    // the link three times in a few minutes. The current photo stays up; the
    // slideshow carries on 3 s after the last piece.
    if (g_have && net::uploading()) return;
    if (g_photos_dirty || millis() - g_photos_loaded > 300000) {
      const uint32_t t0 = millis();
      g_photos = sdcard::mounted() ? photos::scan(std::string(sdcard::mountPoint()) + "/flapboard", "photos", fatList) : std::vector<photos::Photo>();
      note("photos: %u found in %lu ms", (unsigned)g_photos.size(), (unsigned long)(millis() - t0));
      g_photos_dirty = false;
      g_photos_loaded = millis();
    }
    std::string failed;
    if (sign::takePhotoFailure(&failed)) {   // unreadable: straight on to the next; twice running: skip it
      if (failed == g_failed_once) g_bad_photos.push_back(failed);
      g_failed_once = failed;
      g_next_ms = millis();
    }
    if (!g_have || (int32_t)(millis() - g_next_ms) >= 0) {
      auto pool = photos::select(g_photos, p.files);
      pool.erase(std::remove_if(pool.begin(), pool.end(), [](const photos::Photo &x) {
                   return std::find(g_bad_photos.begin(), g_bad_photos.end(), x.path) != g_bad_photos.end();
                 }), pool.end());
      const std::string next = photos::next(pool, p.order, g_last_photo, esp_random());
      g_have = true;
      g_next_ms = millis() + (uint32_t)p.dwell * 1000;
      if (next.empty()) {   // nothing to show: say so on the board
        if (sign::photoMode()) sign::showBoard();
        g_cur = Message{"", "NO PHOTOS YET||ADD THEM AT|{hostname}"};
        g_shown.clear();
        push(true);
      } else {
        g_last_photo = next;
        g_cur = Message{next, ""};
        g_shown = next;
        sign::showPhoto(next);
      }
    }
    return;
  }
  const bool due = !g_have || (int32_t)(millis() - g_next_ms) >= 0;
  if (due) {
    bool ok = true;
    if (p.source == "clock") {
      std::string tpl = p.clock_tpl;
      struct tm t;
      if (p.rb_mode && !p.friday_text.empty() && clock::trusted() && clock::localNow(&t) && t.tm_wday == 5)
        tpl = p.friday_text + "|" + tpl;   // long text wraps onto as many rows as it needs
      g_cur = Message{"", tpl};
    }
    else if (p.source == "weather") g_cur = Message{"", p.weather_tpl};
    else if (p.source == "text") g_cur = Message{"", p.text};
    else {
      ok = pickNext(p);
      // the library's own lines, with the optional lines before and after
      // (their fields expand like the message's: {time}, {date}, ...)
      if (ok && !p.prefix.empty()) g_cur.text = p.prefix + "|" + g_cur.text;
      if (ok && !p.suffix.empty()) g_cur.text = g_cur.text + "|" + p.suffix;
    }
    if (!ok) g_cur = Message{"", "{name}||ADD MESSAGES AT|{hostname}"};   // an empty library says how to fill it
    g_have = true;
    g_next_ms = millis() + (uint32_t)(g_cur.hold_s > 0 ? g_cur.hold_s : p.dwell) * 1000;
    push(true);
  } else {
    push(false);   // fields only: the clock's minute ticks over, the weather updates
  }
}

// The main loop owns g_cur, g_shown and friends; other tasks (the web server,
// the info sheet) read a copy published here under a lock.
SemaphoreHandle_t g_status_mux = xSemaphoreCreateMutex();
std::string g_status_json = "{}";

std::string buildStatus() {
  JsonDocument d;
  d["source"] = g_source;
  d["reason"] = g_reason;
  d["messages"] = (int)g_lib.size();
  d["current"] = g_cur.text;
  d["file"] = g_cur.file;
  d["photos"] = (int)g_photos.size();
  d["shown"] = g_shown;
  d["override"] = g_override_on;
  d["override_text"] = g_override_on ? g_override : "";
  d["clock"] = clock::statusText();
  d["timezone"] = clock::zone();
  d["clock_trusted"] = clock::trusted();
  std::string out;
  serializeJson(d, out);
  return out;
}

void publishStatus() {
  std::string s = buildStatus();
  xSemaphoreTake(g_status_mux, portMAX_DELAY);
  g_status_json.swap(s);
  xSemaphoreGive(g_status_mux);
}

std::string statusJson() {
  xSemaphoreTake(g_status_mux, portMAX_DELAY);
  std::string s = g_status_json;
  xSemaphoreGive(g_status_mux);
  return s;
}

}  // namespace content
}  // namespace flapboard
