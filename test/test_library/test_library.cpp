#include <unity.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../../src/app/library.h"

namespace lib = flapboard::library;
using lib::Err;

static std::string g_root;

static void touch(const std::string &rel, const char *text = "x") {
  FILE *f = fopen((g_root + "/" + rel).c_str(), "wb");
  fputs(text, f);
  fclose(f);
}

static bool exists(const std::string &rel) {
  struct stat st;
  return stat((g_root + "/" + rel).c_str(), &st) == 0;
}

void setUp() {
  char tmpl[] = "/tmp/flapboard_libXXXXXX";
  g_root = mkdtemp(tmpl);
  lib::setRoot(g_root);
}

void tearDown() {
  std::string cmd = "rm -rf '" + g_root + "'";
  system(cmd.c_str());
}

void test_resolve_rejects_escapes() {
  std::string a;
  TEST_ASSERT_TRUE(lib::resolve("", &a) == Err::Ok);
  TEST_ASSERT_EQUAL_STRING(g_root.c_str(), a.c_str());
  TEST_ASSERT_TRUE(lib::resolve("photos/2026/", &a) == Err::Ok);
  TEST_ASSERT_EQUAL_STRING((g_root + "/photos/2026").c_str(), a.c_str());
  TEST_ASSERT_TRUE(lib::resolve("./photos/./x", &a) == Err::Ok);
  TEST_ASSERT_EQUAL_STRING((g_root + "/photos/x").c_str(), a.c_str());
  TEST_ASSERT_TRUE(lib::resolve("../etc", &a) == Err::Unsafe);
  TEST_ASSERT_TRUE(lib::resolve("photos/../../x", &a) == Err::Unsafe);
  TEST_ASSERT_TRUE(lib::resolve("photos/..", &a) == Err::Unsafe);
  TEST_ASSERT_TRUE(lib::resolve("/etc/passwd", &a) == Err::Unsafe);
  TEST_ASSERT_TRUE(lib::resolve("a\\..\\b", &a) == Err::Unsafe);
}

void test_names() {
  std::string n;
  TEST_ASSERT_TRUE(lib::safeFileName("C:\\Users\\me\\beach.jpg", &n) == Err::Ok);
  TEST_ASSERT_EQUAL_STRING("beach.jpg", n.c_str());
  TEST_ASSERT_TRUE(lib::safeFileName("../../x.jpg", &n) == Err::Ok);
  TEST_ASSERT_EQUAL_STRING("x.jpg", n.c_str());
  TEST_ASSERT_TRUE(lib::safeFileName("._beach.jpg", &n) == Err::Unsafe);
  TEST_ASSERT_TRUE(lib::safeFileName("dir/", &n) == Err::Unsafe);
  TEST_ASSERT_TRUE(lib::validateName("ok name") == Err::Ok);
  TEST_ASSERT_TRUE(lib::validateName("../photos") == Err::Unsafe);
  TEST_ASSERT_TRUE(lib::validateName(".hidden") == Err::Unsafe);
  TEST_ASSERT_TRUE(lib::validateName("  ") == Err::Unsafe);
  TEST_ASSERT_TRUE(lib::isImageName("A.JPEG"));
  TEST_ASSERT_FALSE(lib::isImageName("notes.txt"));
}

void test_list_order_and_hidden() {
  mkdir((g_root + "/zeta").c_str(), 0777);
  mkdir((g_root + "/Alpha").c_str(), 0777);
  touch("b.jpg");
  touch("A.txt");
  touch(".DS_Store");
  touch("._b.jpg");
  std::vector<lib::Entry> e;
  TEST_ASSERT_TRUE(lib::list("", &e) == Err::Ok);
  TEST_ASSERT_EQUAL(4, (int)e.size());
  TEST_ASSERT_EQUAL_STRING("Alpha", e[0].name.c_str());
  TEST_ASSERT_TRUE(e[0].dir);
  TEST_ASSERT_EQUAL_STRING("zeta", e[1].name.c_str());
  TEST_ASSERT_EQUAL_STRING("A.txt", e[2].name.c_str());
  TEST_ASSERT_FALSE(e[2].is_image);
  TEST_ASSERT_EQUAL_STRING("b.jpg", e[3].path.c_str());
  TEST_ASSERT_TRUE(e[3].is_image);
  TEST_ASSERT_TRUE(lib::list("b.jpg", &e) == Err::NotDir);
  TEST_ASSERT_TRUE(lib::list("nope", &e) == Err::NotFound);
}

void test_unique_destination() {
  touch("beach.jpg");
  touch("beach (2).jpg");
  std::string d;
  TEST_ASSERT_TRUE(lib::uniqueDestination(g_root, "beach.jpg", &d) == Err::Ok);
  TEST_ASSERT_EQUAL_STRING((g_root + "/beach (3).jpg").c_str(), d.c_str());
  TEST_ASSERT_TRUE(lib::uniqueDestination(g_root, "new.png", &d) == Err::Ok);
  TEST_ASSERT_EQUAL_STRING((g_root + "/new.png").c_str(), d.c_str());
  touch("README");
  TEST_ASSERT_TRUE(lib::uniqueDestination(g_root, "README", &d) == Err::Ok);
  TEST_ASSERT_EQUAL_STRING((g_root + "/README (2)").c_str(), d.c_str());
}

void test_mkdir_rename_delete() {
  std::string r;
  TEST_ASSERT_TRUE(lib::makeDir("", "photos", &r) == Err::Ok);
  TEST_ASSERT_EQUAL_STRING("photos", r.c_str());
  TEST_ASSERT_TRUE(lib::makeDir("", "photos", &r) == Err::Exists);
  TEST_ASSERT_TRUE(lib::makeDir("missing", "x", &r) == Err::NotFound);
  TEST_ASSERT_TRUE(lib::makeDir("photos", "a/b", &r) == Err::Unsafe);
  TEST_ASSERT_TRUE(lib::makeDir("photos", "2026", &r) == Err::Ok);
  touch("photos/2026/one.jpg");
  touch("photos/two.jpg");
  TEST_ASSERT_TRUE(lib::rename("photos/two.jpg", "deux.jpg", &r) == Err::Ok);
  TEST_ASSERT_EQUAL_STRING("photos/deux.jpg", r.c_str());
  TEST_ASSERT_TRUE(lib::rename("photos/deux.jpg", "2026", &r) == Err::Exists);
  TEST_ASSERT_TRUE(lib::rename("", "x", &r) == Err::Unsafe);
  TEST_ASSERT_TRUE(lib::remove("") == Err::Unsafe);
  TEST_ASSERT_TRUE(lib::remove("photos") == Err::Ok);   // recursive
  TEST_ASSERT_FALSE(exists("photos"));
  TEST_ASSERT_TRUE(lib::remove("photos") == Err::NotFound);
}

void test_json() {
  lib::Entry e;
  e.name = "a\"b";
  e.path = "x/a\"b";
  e.size = 12;
  e.is_image = true;
  e.modified = 5;
  TEST_ASSERT_EQUAL_STRING(
      "{\"name\":\"a\\\"b\",\"path\":\"x/a\\\"b\",\"modified\":5,\"type\":\"file\",\"size\":12,\"is_image\":true}",
      lib::entryJson(e).c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_resolve_rejects_escapes);
  RUN_TEST(test_names);
  RUN_TEST(test_list_order_and_hidden);
  RUN_TEST(test_unique_destination);
  RUN_TEST(test_mkdir_rename_delete);
  RUN_TEST(test_json);
  return UNITY_END();
}
