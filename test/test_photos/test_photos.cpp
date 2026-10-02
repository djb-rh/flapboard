#include <unity.h>

#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

#include "../../src/app/photos.h"

using namespace flapboard::photos;
static std::string g_root;

static void touch(const std::string &rel, long mtime) {
  const std::string p = g_root + "/" + rel;
  FILE *f = fopen(p.c_str(), "wb");
  fputs("x", f);
  fclose(f);
  struct utimbuf t = {mtime, mtime};
  utime(p.c_str(), &t);
}

void setUp() {
  char tmpl[] = "/tmp/flapboard_photosXXXXXX";
  g_root = mkdtemp(tmpl);
  for (const char *d : {"photos", "photos/2026", "photos/.thumbs", "photos/Christmas"}) mkdir((g_root + "/" + d).c_str(), 0777);
  touch("photos/b.jpg", 300);
  touch("photos/A.PNG", 100);
  touch("photos/notes.txt", 50);
  touch("photos/._b.jpg", 10);
  touch("photos/.thumbs/b.jpg", 10);
  touch("photos/2026/c.jpeg", 200);
  touch("photos/Christmas/tree.jpg", 400);
}

void tearDown() { system(("rm -rf '" + g_root + "'").c_str()); }

void test_scan_skips_hidden_and_non_photos() {
  auto all = scan(g_root);
  TEST_ASSERT_EQUAL(4, (int)all.size());
  TEST_ASSERT_EQUAL_STRING("photos/2026/c.jpeg", all[0].path.c_str());
  TEST_ASSERT_EQUAL_STRING("photos/A.PNG", all[1].path.c_str());
  TEST_ASSERT_EQUAL_STRING("photos/b.jpg", all[2].path.c_str());
  TEST_ASSERT_EQUAL_STRING("photos/Christmas/tree.jpg", all[3].path.c_str());
}

void test_select() {
  auto all = scan(g_root);
  TEST_ASSERT_EQUAL(4, (int)select(all, {}).size());
  auto xmas = select(all, {"photos/Christmas/"});
  TEST_ASSERT_EQUAL(1, (int)xmas.size());
  auto mixed = select(all, {"photos/2026", "photos/b.jpg"});
  TEST_ASSERT_EQUAL(2, (int)mixed.size());
  TEST_ASSERT_EQUAL(0, (int)select(all, {"photos/Chris"}).size());   // a prefix of a name is not a folder
}

void test_orders_continue_from_last() {
  auto all = scan(g_root);
  TEST_ASSERT_EQUAL_STRING("photos/2026/c.jpeg", next(all, "name_asc", "", 0).c_str());
  TEST_ASSERT_EQUAL_STRING("photos/A.PNG", next(all, "name_asc", "photos/2026/c.jpeg", 0).c_str());
  TEST_ASSERT_EQUAL_STRING("photos/2026/c.jpeg", next(all, "name_asc", "photos/Christmas/tree.jpg", 0).c_str());   // wraps
  TEST_ASSERT_EQUAL_STRING("photos/b.jpg", next(all, "name_asc", "photos/B-deleted.jpg", 0).c_str());   // deleted: next after its place
  TEST_ASSERT_EQUAL_STRING("photos/Christmas/tree.jpg", next(all, "name_desc", "", 0).c_str());
  TEST_ASSERT_EQUAL_STRING("photos/A.PNG", next(all, "date_asc", "", 0).c_str());          // mtime 100
  TEST_ASSERT_EQUAL_STRING("photos/b.jpg", next(all, "date_asc", "photos/2026/c.jpeg", 0).c_str());   // 200 -> 300
  TEST_ASSERT_EQUAL_STRING("photos/Christmas/tree.jpg", next(all, "date_desc", "", 0).c_str());
}

void test_random_never_repeats() {
  auto all = scan(g_root);
  std::set<std::string> seen;
  std::string last;
  for (uint32_t r = 0; r < 200; r++) {
    const std::string n = next(all, "random", last, r * 2654435761u);
    TEST_ASSERT_TRUE(n != last);
    seen.insert(n);
    last = n;
  }
  TEST_ASSERT_EQUAL(4, (int)seen.size());
  auto one = select(all, {"photos/b.jpg"});
  TEST_ASSERT_EQUAL_STRING("photos/b.jpg", next(one, "random", "photos/b.jpg", 5).c_str());   // only one: it repeats
  TEST_ASSERT_EQUAL_STRING("", next({}, "random", "", 1).c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_scan_skips_hidden_and_non_photos);
  RUN_TEST(test_select);
  RUN_TEST(test_orders_continue_from_last);
  RUN_TEST(test_random_never_repeats);
  return UNITY_END();
}
