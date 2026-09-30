// The file library on the SD card: every path the web API touches goes
// through here. Plain POSIX, no Arduino, so it is unit-tested on the Mac.
//
// Ported from the Pi frame's local_photos.py: same JSON shape, same rules --
// library-relative paths only, no escaping the root, hidden (dot) files are
// not content, uploads never overwrite ("beach (2).jpg"), and the root itself
// cannot be deleted or renamed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace flapboard {
namespace library {

// Absolute directory the library lives in, e.g. "/sdcard/flapboard".
void setRoot(const std::string &abs_root);
const std::string &root();

enum class Err { Ok, Unsafe, NotFound, NotDir, Exists, Io };
const char *errText(Err e);   // for the JSON "error" field

struct Entry {
  std::string name, path;   // path is library-relative
  bool dir = false;
  uint64_t size = 0;
  int64_t modified = 0;
  bool is_image = false;
};

// Library-relative -> absolute, or Err::Unsafe. Rejects absolute input,
// any ".." component, NULs and backslashes. The path need not exist.
Err resolve(const std::string &rel, std::string *abs);
// A user-typed single name: no slashes, not empty, no leading dot.
Err validateName(const std::string &name);
// An uploaded filename reduced to its last component (clients may send
// "C:\\x\\y.jpg"); Err::Unsafe if nothing usable or it starts with a dot.
Err safeFileName(const std::string &filename, std::string *out);
bool isImageName(const std::string &name);
// Strips leading/trailing slashes: "a/b/" -> "a/b".
std::string normalise(const std::string &rel);

// Directories first then files, each case-insensitively by name.
Err list(const std::string &rel, std::vector<Entry> *out);
Err makeDir(const std::string &rel_parent, const std::string &name, std::string *created_rel);
Err remove(const std::string &rel);   // files, or folders recursively
Err rename(const std::string &rel, const std::string &new_name, std::string *moved_rel);
// A non-colliding absolute path for `filename` in the folder `dir_abs`.
Err uniqueDestination(const std::string &dir_abs, const std::string &filename, std::string *out_abs);
std::string relativeOf(const std::string &abs);   // absolute -> library-relative

// Free/total bytes of the filesystem (0/0 if unknown). Set by the platform,
// since statvfs is not available on FAT through ESP-IDF's VFS.
void setSpaceProvider(void (*fn)(uint64_t *free_b, uint64_t *total_b));
void space(uint64_t *free_b, uint64_t *total_b);

// Minimal JSON helpers shared with the web layer.
std::string jsonStr(const std::string &s);
std::string entryJson(const Entry &e);

}  // namespace library
}  // namespace flapboard
