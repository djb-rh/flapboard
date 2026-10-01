#include <unity.h>

#include "../../src/app/content_parse.h"

using flapboard::content::parseFile;

void test_blocks_comments_options() {
  const auto m = parseFile("a.txt",
                           "# a comment\n"
                           "WELCOME\nABOARD\n"
                           "\n\n"
                           "@left @hold 45 @top\n"
                           "241  BOSTON\r\n"
                           "# comments inside are skipped\n"
                           "118  ALBANY\n"
                           "\n"
                           "{R}{G}\n|\nDINING\n");
  TEST_ASSERT_EQUAL(3, (int)m.size());
  TEST_ASSERT_EQUAL_STRING("WELCOME\nABOARD", m[0].text.c_str());
  TEST_ASSERT_EQUAL(1, m[0].align);
  TEST_ASSERT_EQUAL_STRING("241  BOSTON\n118  ALBANY", m[1].text.c_str());
  TEST_ASSERT_EQUAL(0, m[1].align);
  TEST_ASSERT_EQUAL(45, m[1].hold_s);
  TEST_ASSERT_TRUE(m[1].top);
  TEST_ASSERT_EQUAL_STRING("{R}{G}\n\nDINING", m[2].text.c_str());   // a lone | is one empty row
  TEST_ASSERT_EQUAL_STRING("a.txt", m[2].file.c_str());
}

void test_empty_and_options_only() {
  TEST_ASSERT_EQUAL(0, (int)parseFile("x", "").size());
  TEST_ASSERT_EQUAL(0, (int)parseFile("x", "# only comments\n\n").size());
  TEST_ASSERT_EQUAL(0, (int)parseFile("x", "@right\n").size());   // options with no rows: nothing
  const auto m = parseFile("x", "@right\nHELLO");                  // no trailing newline
  TEST_ASSERT_EQUAL(1, (int)m.size());
  TEST_ASSERT_EQUAL(2, m[0].align);
  TEST_ASSERT_EQUAL_STRING("HELLO", m[0].text.c_str());
}

void test_at_sign_in_text_is_text() {
  const auto m = parseFile("x", "EMAIL US\nHI@EXAMPLE.COM\n");   // @ only means options on a first line
  TEST_ASSERT_EQUAL_STRING("EMAIL US\nHI@EXAMPLE.COM", m[0].text.c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_blocks_comments_options);
  RUN_TEST(test_empty_and_options_only);
  RUN_TEST(test_at_sign_in_text_is_text);
  return UNITY_END();
}
