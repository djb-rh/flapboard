// Photo mode's library: which photos to show and in what order, from
// /flapboard/photos on the card. Ported from the Pi frame's local_photos.py
// rules: hidden files and folders (.DS_Store, ._x, .thumbs) are never photos;
// a selection is a list of folders and/or photos (empty = everything);
// random never repeats the last photo when there is a choice; sequential
// orders continue from the previous photo, so adding or deleting photos
// mid-show neither jumps nor skips. Plain POSIX (tested on the Mac).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace flapboard {
namespace photos {

struct Photo {
  std::string path;   // library-relative, e.g. "photos/2026/beach.jpg"
  int64_t mtime = 0;
};

bool isPhotoName(const std::string &name);   // .jpg .jpeg .png
// One directory's entries (hidden ones included; scan skips them). The
// default lists with readdir + stat; the sign passes FatFs's own walk, which
// returns each entry's date with its name instead of a stat per file.
struct DirEntry {
  std::string name;
  bool dir = false;
  int64_t mtime = 0;
};
using Lister = std::function<bool(const std::string &abs_dir, std::vector<DirEntry> *out)>;
// Every photo under abs_root/rel (recursively), sorted by path.
std::vector<Photo> scan(const std::string &abs_root, const std::string &rel = "photos", const Lister &list = Lister());
// Those inside the selection (paths equal to, or under, a selected entry).
std::vector<Photo> select(const std::vector<Photo> &all, const std::vector<std::string> &selection);
// orders: random, name_asc, name_desc, date_asc, date_desc. rnd: any random number.
std::string next(const std::vector<Photo> &candidates, const std::string &order, const std::string &last, uint32_t rnd);

}  // namespace photos
}  // namespace flapboard
