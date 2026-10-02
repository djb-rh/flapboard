// Photo mode's library: which photos to show and in what order, from
// /flapboard/photos on the card. Ported from the Pi frame's local_photos.py
// rules: hidden files and folders (.DS_Store, ._x, .thumbs) are never photos;
// a selection is a list of folders and/or photos (empty = everything);
// random never repeats the last photo when there is a choice; sequential
// orders continue from the previous photo, so adding or deleting photos
// mid-show neither jumps nor skips. Plain POSIX (tested on the Mac).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace flapboard {
namespace photos {

struct Photo {
  std::string path;   // library-relative, e.g. "photos/2026/beach.jpg"
  int64_t mtime = 0;
};

bool isPhotoName(const std::string &name);   // .jpg .jpeg .png
// Every photo under abs_root/rel (recursively), sorted by path.
std::vector<Photo> scan(const std::string &abs_root, const std::string &rel = "photos");
// Those inside the selection (paths equal to, or under, a selected entry).
std::vector<Photo> select(const std::vector<Photo> &all, const std::vector<std::string> &selection);
// orders: random, name_asc, name_desc, date_asc, date_desc. rnd: any random number.
std::string next(const std::vector<Photo> &candidates, const std::string &order, const std::string &last, uint32_t rnd);

}  // namespace photos
}  // namespace flapboard
