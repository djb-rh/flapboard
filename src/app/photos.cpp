#include "photos.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace flapboard {
namespace photos {
namespace {

std::string lower(std::string s) {
  for (auto &c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

bool posixList(const std::string &abs, std::vector<DirEntry> *out) {
  DIR *d = opendir(abs.c_str());
  if (!d) return false;
  std::vector<std::string> names;
  while (dirent *e = readdir(d)) names.push_back(e->d_name);   // readdir, not File::openNextFile (Tabulous5: it opens every entry)
  closedir(d);
  for (auto &n : names) {
    if (n[0] == '.') continue;
    struct stat st;
    if (stat((abs + "/" + n).c_str(), &st) != 0) continue;
    out->push_back({n, S_ISDIR(st.st_mode), (int64_t)st.st_mtime});
  }
  return true;
}

void walk(const std::string &abs, const std::string &rel, std::vector<Photo> *out, int depth, const Lister &list) {
  if (depth > 8) return;
  std::vector<DirEntry> es;
  if (!(list ? list(abs, &es) : posixList(abs, &es))) return;
  for (auto &e : es) {
    if (e.name.empty() || e.name[0] == '.') continue;
    if (e.dir) walk(abs + "/" + e.name, rel + "/" + e.name, out, depth + 1, list);
    else if (isPhotoName(e.name)) out->push_back({rel + "/" + e.name, e.mtime});
  }
}

}  // namespace

bool isPhotoName(const std::string &name) {
  const size_t d = name.rfind('.');
  if (d == std::string::npos || name[0] == '.') return false;
  const std::string ext = lower(name.substr(d));
  return ext == ".jpg" || ext == ".jpeg" || ext == ".png";
}

std::vector<Photo> scan(const std::string &abs_root, const std::string &rel, const Lister &list) {
  std::vector<Photo> out;
  walk(abs_root + "/" + rel, rel, &out, 0, list);
  std::sort(out.begin(), out.end(), [](const Photo &a, const Photo &b) { return lower(a.path) < lower(b.path); });
  return out;
}

std::vector<Photo> select(const std::vector<Photo> &all, const std::vector<std::string> &selection) {
  if (selection.empty()) return all;
  std::vector<Photo> out;
  for (auto &p : all) {
    for (const auto &s0 : selection) {
      std::string s = s0;
      while (!s.empty() && s.back() == '/') s.pop_back();
      if (p.path == s || (p.path.size() > s.size() && p.path.compare(0, s.size(), s) == 0 && p.path[s.size()] == '/')) {
        out.push_back(p);
        break;
      }
    }
  }
  return out;
}

std::string next(const std::vector<Photo> &candidates, const std::string &order, const std::string &last, uint32_t rnd) {
  if (candidates.empty()) return "";
  if (order == "random" || order.empty()) {
    if (candidates.size() == 1) return candidates[0].path;
    // Never the same photo twice running: a small library otherwise looks stuck.
    size_t i = rnd % candidates.size();
    if (candidates[i].path == last) i = (i + 1 + rnd / 7 % (candidates.size() - 1)) % candidates.size();
    return candidates[i].path;
  }
  std::vector<Photo> v = candidates;
  const bool by_date = order.rfind("date", 0) == 0, desc = order.find("desc") != std::string::npos;
  std::stable_sort(v.begin(), v.end(), [&](const Photo &a, const Photo &b) {
    if (by_date && a.mtime != b.mtime) return desc ? a.mtime > b.mtime : a.mtime < b.mtime;
    return desc ? lower(a.path) > lower(b.path) : lower(a.path) < lower(b.path);
  });
  // Continue from the previous photo's place in the order, even if it has
  // since been deleted (then: the first one after where it would sit).
  if (last.empty()) return v[0].path;
  for (size_t i = 0; i < v.size(); i++)
    if (v[i].path == last) return v[(i + 1) % v.size()].path;
  for (size_t i = 0; i < v.size(); i++) {
    const bool after = by_date ? false : (desc ? lower(v[i].path) < lower(last) : lower(v[i].path) > lower(last));
    if (after) return v[i].path;
  }
  return v[0].path;
}

}  // namespace photos
}  // namespace flapboard
