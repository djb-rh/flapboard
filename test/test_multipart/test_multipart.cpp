#include <unity.h>

#include <string>
#include <vector>

#include "../../src/app/multipart.h"

using flapboard::MultipartParser;

struct Part {
  std::string name, filename, data;
  bool ended = false;
};

struct Collect : MultipartParser::Handler {
  std::vector<Part> parts;
  bool refuse = false;
  bool partBegin(const std::string &n, const std::string &f) override {
    if (refuse) return false;
    parts.push_back({n, f, "", false});
    return true;
  }
  bool partData(const uint8_t *d, size_t l) override {
    parts.back().data.append((const char *)d, l);
    return true;
  }
  bool partEnd() override {
    parts.back().ended = true;
    return true;
  }
};

static const char *kCt = "multipart/form-data; boundary=----WebKitFormBoundaryAbC123";

static std::string body(const std::string &file) {
  return "------WebKitFormBoundaryAbC123\r\n"
         "Content-Disposition: form-data; name=\"path\"\r\n\r\n"
         "photos/2026\r\n"
         "------WebKitFormBoundaryAbC123\r\n"
         "Content-Disposition: form-data; name=\"file\"; filename=\"be\\\"ach.jpg\"\r\n"
         "Content-Type: image/jpeg\r\n\r\n" +
         file +
         "\r\n------WebKitFormBoundaryAbC123--\r\n";
}

static std::string binary() {
  // Includes CR, LF, dashes and a near-miss of the delimiter.
  std::string s;
  for (int i = 0; i < 3000; i++) s += (char)(i * 7 + 3);
  s += "\r\n------WebKitFormBoundaryAbC12X";
  s += "\r\n--";
  for (int i = 0; i < 100; i++) s += (char)i;
  return s;
}

static void check(const Collect &c, const std::string &file) {
  TEST_ASSERT_EQUAL(2, (int)c.parts.size());
  TEST_ASSERT_EQUAL_STRING("path", c.parts[0].name.c_str());
  TEST_ASSERT_EQUAL_STRING("", c.parts[0].filename.c_str());
  TEST_ASSERT_EQUAL_STRING("photos/2026", c.parts[0].data.c_str());
  TEST_ASSERT_EQUAL_STRING("file", c.parts[1].name.c_str());
  TEST_ASSERT_EQUAL_STRING("be\"ach.jpg", c.parts[1].filename.c_str());
  TEST_ASSERT_TRUE(c.parts[1].data == file);
  TEST_ASSERT_TRUE(c.parts[1].ended);
}

void test_whole() {
  const std::string f = binary(), b = body(f);
  Collect c;
  MultipartParser p;
  TEST_ASSERT_TRUE(p.begin(kCt, &c));
  TEST_ASSERT_TRUE(p.feed((const uint8_t *)b.data(), b.size()));
  TEST_ASSERT_TRUE(p.done());
  check(c, f);
}

void test_every_split() {
  const std::string f = binary(), b = body(f);
  for (size_t cut = 1; cut < b.size(); cut += 1) {
    Collect c;
    MultipartParser p;
    p.begin(kCt, &c);
    TEST_ASSERT_TRUE(p.feed((const uint8_t *)b.data(), cut));
    TEST_ASSERT_TRUE(p.feed((const uint8_t *)b.data() + cut, b.size() - cut));
    TEST_ASSERT_TRUE(p.done());
    check(c, f);
  }
}

void test_byte_at_a_time() {
  const std::string f = binary(), b = body(f);
  Collect c;
  MultipartParser p;
  p.begin(kCt, &c);
  for (char ch : b) TEST_ASSERT_TRUE(p.feed((const uint8_t *)&ch, 1));
  TEST_ASSERT_TRUE(p.done());
  check(c, f);
}

void test_boundary_parsing() {
  TEST_ASSERT_EQUAL_STRING("abc", MultipartParser::boundaryOf("multipart/form-data; boundary=abc").c_str());
  TEST_ASSERT_EQUAL_STRING("a b", MultipartParser::boundaryOf("Multipart/Form-Data; Boundary=\"a b\"").c_str());
  TEST_ASSERT_EQUAL_STRING("", MultipartParser::boundaryOf("application/json").c_str());
  Collect c;
  MultipartParser p;
  TEST_ASSERT_FALSE(p.begin("application/octet-stream", &c));
}

void test_refused_and_malformed() {
  const std::string b = body("x");
  Collect c;
  c.refuse = true;
  MultipartParser p;
  p.begin(kCt, &c);
  TEST_ASSERT_FALSE(p.feed((const uint8_t *)b.data(), b.size()));
  Collect c2;
  MultipartParser p2;
  p2.begin(kCt, &c2);
  const std::string bad = "------WebKitFormBoundaryAbC123XX\r\n";
  TEST_ASSERT_FALSE(p2.feed((const uint8_t *)bad.data(), bad.size()));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_whole);
  RUN_TEST(test_every_split);
  RUN_TEST(test_byte_at_a_time);
  RUN_TEST(test_boundary_parsing);
  RUN_TEST(test_refused_and_malformed);
  return UNITY_END();
}
