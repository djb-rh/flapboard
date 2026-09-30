#include "library.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace flapboard {
namespace library {
namespace {

std::string g_root = "/sdcard/flapboard";
void (*g_space)(uint64_t *, uint64_t *) = nullptr;

bool exists(const std::string &p, struct stat *st = nullptr) {
  struct stat s;
  if (::stat(p.c_str(), &s) != 0) return false;
  if (st) *st = s;
  return true;
}

std::string lower(std::string s) {
  for (auto &c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

std::string trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) a++;
  while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

Err removeTree(const std::string &abs) {
  struct stat st;
  if (!exists(abs, &st)) return Err::NotFound;
  if (!S_ISDIR(st.st_mode)) return ::unlink(abs.c_str()) == 0 ? Err::Ok : Err::Io;
  DIR *d = ::opendir(abs.c_str());
  if (!d) return Err::Io;
  std::vector<std::string> kids;
  while (dirent *e = ::readdir(d)) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    kids.push_back(abs + "/" + e->d_name);
  }
  ::closedir(d);
  for (auto &k : kids) {
    const Err r = removeTree(k);
    if (r != Err::Ok) return r;
  }
  return ::rmdir(abs.c_str()) == 0 ? Err::Ok : Err::Io;
}

}  // namespace

void setRoot(const std::string &abs_root) { g_root = abs_root; }
const std::string &root() { return g_root; }

const char *errText(Err e) {
  switch (e) {
    case Err::Ok: return "ok";
    case Err::Unsafe: return "path not allowed";
    case Err::NotFound: return "no such file or folder";
    case Err::NotDir: return "not a folder";
    case Err::Exists: return "already exists";
    case Err::Io: return "the SD card refused the operation";
  }
  return "error";
}

std::string normalise(const std::string &rel) {
  size_t a = 0, b = rel.size();
  while (a < b && rel[a] == '/') a++;
  while (b > a && rel[b - 1] == '/') b--;
  return rel.substr(a, b - a);
}

Err resolve(const std::string &rel_in, std::string *abs) {
  const std::string rel = trim(rel_in);
  if (!rel.empty() && rel[0] == '/') return Err::Unsafe;   // absolute: not a typo, refuse
  if (rel.find('\0') != std::string::npos || rel.find('\\') != std::string::npos) return Err::Unsafe;
  const std::string n = normalise(rel);
  // Walk the components: "." is harmless noise, ".." is never allowed (FAT
  // has no symlinks, so component checks are enough here).
  std::string out = g_root;
  size_t p = 0;
  while (p <= n.size() && !n.empty()) {
    size_t e = n.find('/', p);
    if (e == std::string::npos) e = n.size();
    const std::string c = n.substr(p, e - p);
    if (c == "..") return Err::Unsafe;
    if (!c.empty() && c != ".") out += "/" + c;
    p = e + 1;
  }
  *abs = out;
  return Err::Ok;
}

Err validateName(const std::string &name_in) {
  const std::string t = trim(name_in);
  if (t.empty() || t == "." || t == "..") return Err::Unsafe;
  if (t.find('/') != std::string::npos || t.find('\\') != std::string::npos) return Err::Unsafe;
  if (t.find('\0') != std::string::npos || t[0] == '.') return Err::Unsafe;
  return Err::Ok;
}

Err safeFileName(const std::string &filename, std::string *out) {
  std::string n = trim(filename);
  std::replace(n.begin(), n.end(), '\\', '/');
  const size_t s = n.rfind('/');
  if (s != std::string::npos) n = trim(n.substr(s + 1));
  if (n.empty() || n == "." || n == ".." || n[0] == '.' || n.find('\0') != std::string::npos) return Err::Unsafe;
  *out = n;
  return Err::Ok;
}

bool isImageName(const std::string &name) {
  const size_t d = name.rfind('.');
  if (d == std::string::npos) return false;
  const std::string ext = lower(name.substr(d));
  return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".gif" || ext == ".bmp" || ext == ".webp";
}

std::string relativeOf(const std::string &abs) {
  if (abs.compare(0, g_root.size(), g_root) != 0) return "";
  return normalise(abs.substr(g_root.size()));
}

Err list(const std::string &rel, std::vector<Entry> *out) {
  std::string abs;
  if (resolve(rel, &abs) != Err::Ok) return Err::Unsafe;
  struct stat st;
  if (!exists(abs, &st)) return Err::NotFound;
  if (!S_ISDIR(st.st_mode)) return Err::NotDir;
  DIR *d = ::opendir(abs.c_str());
  if (!d) return Err::Io;
  std::vector<Entry> dirs, files;
  while (dirent *e = ::readdir(d)) {
    if (e->d_name[0] == '.') continue;   // hidden files are not content
    Entry en;
    en.name = e->d_name;
    const std::string child = abs + "/" + en.name;
    if (!exists(child, &st)) continue;   // vanished mid-listing
    en.path = relativeOf(child);
    en.modified = (int64_t)st.st_mtime;
    en.dir = S_ISDIR(st.st_mode);
    if (!en.dir) {
      en.size = (uint64_t)st.st_size;
      en.is_image = isImageName(en.name);
    }
    (en.dir ? dirs : files).push_back(en);
  }
  ::closedir(d);
  auto byName = [](const Entry &a, const Entry &b) { return lower(a.name) < lower(b.name); };
  std::sort(dirs.begin(), dirs.end(), byName);
  std::sort(files.begin(), files.end(), byName);
  out->clear();
  out->insert(out->end(), dirs.begin(), dirs.end());
  out->insert(out->end(), files.begin(), files.end());
  return Err::Ok;
}

Err makeDir(const std::string &rel_parent, const std::string &name, std::string *created_rel) {
  std::string parent;
  if (resolve(rel_parent, &parent) != Err::Ok || validateName(name) != Err::Ok) return Err::Unsafe;
  struct stat st;
  if (!exists(parent, &st)) return Err::NotFound;
  if (!S_ISDIR(st.st_mode)) return Err::NotDir;
  const std::string target = parent + "/" + trim(name);
  if (exists(target)) return Err::Exists;
  if (::mkdir(target.c_str(), 0777) != 0) return Err::Io;
  if (created_rel) *created_rel = relativeOf(target);
  return Err::Ok;
}

Err remove(const std::string &rel) {
  std::string abs;
  if (resolve(rel, &abs) != Err::Ok || abs == g_root) return Err::Unsafe;   // never the root
  return removeTree(abs);
}

Err rename(const std::string &rel, const std::string &new_name, std::string *moved_rel) {
  std::string abs;
  if (resolve(rel, &abs) != Err::Ok || abs == g_root || validateName(new_name) != Err::Ok) return Err::Unsafe;
  if (!exists(abs)) return Err::NotFound;
  const std::string dest = abs.substr(0, abs.rfind('/') + 1) + trim(new_name);
  if (exists(dest)) return Err::Exists;
  if (::rename(abs.c_str(), dest.c_str()) != 0) return Err::Io;
  if (moved_rel) *moved_rel = relativeOf(dest);
  return Err::Ok;
}

Err uniqueDestination(const std::string &dir_abs, const std::string &filename, std::string *out_abs) {
  std::string base;
  if (safeFileName(filename, &base) != Err::Ok) return Err::Unsafe;
  const size_t dot = base.rfind('.');
  const std::string stem = dot == std::string::npos || dot == 0 ? base : base.substr(0, dot);
  const std::string ext = dot == std::string::npos || dot == 0 ? "" : base.substr(dot);
  std::string cand = dir_abs + "/" + base;
  for (int n = 2; exists(cand); n++) {
    if (n > 1000) return Err::Exists;
    cand = dir_abs + "/" + stem + " (" + std::to_string(n) + ")" + ext;
  }
  *out_abs = cand;
  return Err::Ok;
}

void setSpaceProvider(void (*fn)(uint64_t *, uint64_t *)) { g_space = fn; }
void space(uint64_t *free_b, uint64_t *total_b) {
  *free_b = *total_b = 0;
  if (g_space) g_space(free_b, total_b);
}

std::string jsonStr(const std::string &s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
      o += (char)c;
    } else if (c == '\n') {
      o += "\\n";
    } else if (c < 0x20) {
      char b[8];
      snprintf(b, sizeof(b), "\\u%04x", c);
      o += b;
    } else {
      o += (char)c;
    }
  }
  return o + "\"";
}

std::string entryJson(const Entry &e) {
  char b[96];
  std::string j = "{\"name\":" + jsonStr(e.name) + ",\"path\":" + jsonStr(e.path);
  snprintf(b, sizeof(b), ",\"modified\":%lld,\"type\":\"%s\"", (long long)e.modified, e.dir ? "dir" : "file");
  j += b;
  if (!e.dir) {
    snprintf(b, sizeof(b), ",\"size\":%llu,\"is_image\":%s", (unsigned long long)e.size, e.is_image ? "true" : "false");
    j += b;
  }
  return j + "}";
}

}  // namespace library
}  // namespace flapboard
