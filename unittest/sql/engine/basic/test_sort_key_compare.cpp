/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include <algorithm>
#include <random>
#include <string>
#include <utility>
#include <vector>
#include "share/datum/ob_datum_cmp_func_def.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/sort/ob_sort_op_impl.h"
#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace oceanbase
{
namespace sql
{
namespace
{
using namespace common;

template <ObCollationType CS, bool END_SPACE>
void check_string_comparisons()
{
  std::vector<std::string> values = {
      "", "a", "A", "a ", "a  ", "b", "abc", "ABC", "abC ",
      "数据库", "数据", "中文搜索", "é", "e", "É", "😀",
      std::string("a\0", 2), std::string("a\0b", 3),
      std::string("\xc0\x80", 2), std::string("\xe4\xb8", 2),
      std::string("\xff\xfe", 2)};
  // Cover the short-key boundary as well as the unchanged long-string path.
  for (int length : {1, 7, 8, 16, 63, 64, 65, 80}) {
    for (int seed : {0, 31, 127, 193}) {
      std::string value;
      for (int i = 0; i < length; ++i) {
        value.push_back(static_cast<char>((seed + i * 29) & 0xff));
      }
      values.push_back(value);
      values.push_back(value + ' ');
    }
  }
  for (size_t i = 0; i < values.size(); ++i) {
    for (size_t j = 0; j < values.size(); ++j) {
      SCOPED_TRACE(::testing::Message() << CS << ", " << END_SPACE << ", " << i << ", " << j);
      // Distinct backing strings also exercise byte equality rather than pointer equality.
      const std::string lhs_value = values[i];
      const std::string rhs_value = values[j];
      ObDatum lhs, rhs;
      lhs.set_string(lhs_value.data(), lhs_value.size());
      rhs.set_string(rhs_value.data(), rhs_value.size());
      const int legacy = ObCharset::strcmpsp(
          CS, lhs.ptr_, lhs.len_, rhs.ptr_, rhs.len_, END_SPACE);
      int actual = 0;
      ASSERT_EQ(OB_SUCCESS, (datum_cmp::ObDatumStrCmp<CS, END_SPACE>::cmp(lhs, rhs, actual)));
      EXPECT_EQ(legacy > 0 ? 1 : (legacy < 0 ? -1 : 0), actual);
      ASSERT_EQ(OB_SUCCESS, (datum_cmp::ObDatumStrCmp<CS, END_SPACE>::cmp(lhs, lhs, actual)));
      EXPECT_EQ(0, actual);
    }
  }
  // Empty strings with a null pointer remain valid ordinary string values.
  ObDatum empty, text;
  empty.set_string(nullptr, 0);
  text.set_string("a", 1);
  int actual = 0;
  ASSERT_EQ(OB_SUCCESS, (datum_cmp::ObDatumStrCmp<CS, END_SPACE>::cmp(empty, text, actual)));
  EXPECT_LT(actual, 0);
}

TEST(SortKeyCompare, short_string_equality_preserves_collation)
{
  check_string_comparisons<CS_TYPE_UTF8MB4_GENERAL_CI, false>();
  check_string_comparisons<CS_TYPE_UTF8MB4_GENERAL_CI, true>();
  check_string_comparisons<CS_TYPE_UTF8MB4_BIN, false>();
  check_string_comparisons<CS_TYPE_UTF8MB4_BIN, true>();
  check_string_comparisons<CS_TYPE_UTF8MB4_UNICODE_CI, false>();
  check_string_comparisons<CS_TYPE_UTF8MB4_UNICODE_CI, true>();
  check_string_comparisons<CS_TYPE_BINARY, false>();
  check_string_comparisons<CS_TYPE_BINARY, true>();

  for (ObCmpNullPos null_pos : {NULL_FIRST, NULL_LAST}) {
    const ObDatumCmpFuncType cmp = ObDatumFuncs::get_nullsafe_cmp_func(
        ObVarcharType, ObVarcharType, null_pos, CS_TYPE_UTF8MB4_GENERAL_CI,
        SCALE_UNKNOWN_YET, false);
    ASSERT_NE(nullptr, cmp);
    ObDatum null_value, text;
    null_value.set_null();
    text.set_string("same", 4);
    int actual = 0;
    ASSERT_EQ(OB_SUCCESS, cmp(null_value, text, actual, nullptr));
    EXPECT_EQ(NULL_FIRST == null_pos ? -1 : 1, actual);
    ASSERT_EQ(OB_SUCCESS, cmp(text, null_value, actual, nullptr));
    EXPECT_EQ(NULL_FIRST == null_pos ? 1 : -1, actual);
    ASSERT_EQ(OB_SUCCESS, cmp(null_value, null_value, actual, nullptr));
    EXPECT_EQ(0, actual);
  }
}

void check_short_general_ci_pair(const ObDatum &lhs, const ObDatum &rhs)
{
  for (bool end_space : {false, true}) {
    const int reference = ObCharset::strcmpsp(
        CS_TYPE_UTF8MB4_GENERAL_CI, lhs.ptr_, lhs.len_, rhs.ptr_, rhs.len_, end_space);
    int actual = 0;
    const int ret = end_space
        ? datum_cmp::ObDatumStrCmp<CS_TYPE_UTF8MB4_GENERAL_CI, true>::cmp(lhs, rhs, actual)
        : datum_cmp::ObDatumStrCmp<CS_TYPE_UTF8MB4_GENERAL_CI, false>::cmp(lhs, rhs, actual);
    ASSERT_EQ(OB_SUCCESS, ret);
    ASSERT_EQ(reference > 0 ? 1 : (reference < 0 ? -1 : 0), actual);
  }
  static const ObDatumCmpFuncType registered[] = {
      ObDatumFuncs::get_nullsafe_cmp_func(
          ObVarcharType, ObVarcharType, NULL_FIRST, CS_TYPE_UTF8MB4_GENERAL_CI,
          SCALE_UNKNOWN_YET, false),
      ObDatumFuncs::get_nullsafe_cmp_func(
          ObVarcharType, ObVarcharType, NULL_LAST, CS_TYPE_UTF8MB4_GENERAL_CI,
          SCALE_UNKNOWN_YET, false)};
  const int reference = ObCharset::strcmpsp(
      CS_TYPE_UTF8MB4_GENERAL_CI, lhs.ptr_, lhs.len_, rhs.ptr_, rhs.len_, false);
  for (const ObDatumCmpFuncType compare : registered) {
    ASSERT_NE(nullptr, compare);
    int actual = 0;
    ASSERT_EQ(OB_SUCCESS, compare(lhs, rhs, actual, nullptr));
    ASSERT_EQ(reference > 0 ? 1 : (reference < 0 ? -1 : 0), actual);
  }
}

TEST(SortKeyCompare, general_ci_short_equality_checks_every_payload_byte)
{
  char left[96], right[96];
  for (int length = 0; length <= 65; ++length) {
    for (int alignment = 0; alignment < 32; ++alignment) {
      SCOPED_TRACE(::testing::Message() << length << ", " << alignment);
      char *l = left + alignment;
      char *r = right + (31 - alignment);
      for (int i = 0; i < length; ++i) {
        l[i] = r[i] = static_cast<char>(i * 31 + alignment * 17);
      }
      ObDatum lhs, rhs;
      lhs.set_string(l, length);
      rhs.set_string(r, length);
      check_short_general_ci_pair(lhs, rhs);
      ASSERT_FALSE(::testing::Test::HasFatalFailure());
      for (int position = 0; position < length; ++position) {
        r[position] ^= static_cast<char>(0x80);
        check_short_general_ci_pair(lhs, rhs);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        check_short_general_ci_pair(rhs, lhs);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        r[position] = l[position];
      }
    }
  }
  for (int byte = 0; byte < 256; ++byte) {
    left[0] = right[0] = static_cast<char>(byte);
    ObDatum lhs, rhs;
    lhs.set_string(left, 1);
    rhs.set_string(right, 1);
    check_short_general_ci_pair(lhs, rhs);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    right[0] ^= static_cast<char>(0x80);
    check_short_general_ci_pair(lhs, rhs);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
  }
}

TEST(SortKeyCompare, general_ci_short_equality_requires_no_readable_prefix_or_suffix)
{
#if defined(__linux__)
  const int64_t page_size = sysconf(_SC_PAGESIZE);
  ASSERT_GT(page_size, 128);
  void *mapping = mmap(nullptr, 3 * page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(MAP_FAILED, mapping);
  struct ShortKeyMappingGuard
  {
    void *mapping_;
    int64_t size_;
    ~ShortKeyMappingGuard() { munmap(mapping_, size_); }
  } cleanup{mapping, 3 * page_size};
  char *readable = static_cast<char *>(mapping) + page_size;
  ASSERT_EQ(0, mprotect(readable, page_size, PROT_READ | PROT_WRITE));
  for (int length = 0; length <= 65; ++length) {
    SCOPED_TRACE(length);
    char *left = readable;
    char *right = readable + page_size - length;
    MEMSET(left, 0x80, length);
    MEMSET(right, 0x80, length);
    ObDatum lhs, rhs;
    lhs.set_string(left, length);
    rhs.set_string(right, length);
    check_short_general_ci_pair(lhs, rhs);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    check_short_general_ci_pair(rhs, lhs);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    for (int position = 0; position < length; ++position) {
      right[position] = static_cast<char>(0xff);
      check_short_general_ci_pair(lhs, rhs);
      ASSERT_FALSE(::testing::Test::HasFatalFailure());
      check_short_general_ci_pair(rhs, lhs);
      ASSERT_FALSE(::testing::Test::HasFatalFailure());
      right[position] = static_cast<char>(0x80);
    }
  }
#endif
}

template <bool END_SPACE>
void check_general_ci_ascii_first_weights()
{
  const ObDatumCmpFuncType null_first = ObDatumFuncs::get_nullsafe_cmp_func(
      ObVarcharType, ObVarcharType, NULL_FIRST, CS_TYPE_UTF8MB4_GENERAL_CI,
      SCALE_UNKNOWN_YET, false);
  const ObDatumCmpFuncType null_last = ObDatumFuncs::get_nullsafe_cmp_func(
      ObVarcharType, ObVarcharType, NULL_LAST, CS_TYPE_UTF8MB4_GENERAL_CI,
      SCALE_UNKNOWN_YET, false);
  ASSERT_NE(nullptr, null_first);
  ASSERT_NE(nullptr, null_last);
  const auto normalized = [](int value) { return value > 0 ? 1 : (value < 0 ? -1 : 0); };
  const auto check_pair = [&](const ObDatum &lhs, const ObDatum &rhs) {
    int reference = 0;
    if (!lhs.is_null() && !rhs.is_null()) {
      reference = normalized(ObCharset::strcmpsp(
          CS_TYPE_UTF8MB4_GENERAL_CI, lhs.ptr_, lhs.len_, rhs.ptr_, rhs.len_, END_SPACE));
      int actual = 0;
      ASSERT_EQ(OB_SUCCESS,
          (datum_cmp::ObDatumStrCmp<CS_TYPE_UTF8MB4_GENERAL_CI, END_SPACE>::cmp(
              lhs, rhs, actual)));
      ASSERT_EQ(reference, actual);
      // The registered SQL null-safe dispatch uses WITH_END_SPACE=false.
      if (END_SPACE) {
        reference = normalized(ObCharset::strcmpsp(
            CS_TYPE_UTF8MB4_GENERAL_CI, lhs.ptr_, lhs.len_, rhs.ptr_, rhs.len_, false));
      }
    }
    for (bool first : {false, true}) {
      const int expected = lhs.is_null() && rhs.is_null() ? 0
          : lhs.is_null() ? (first ? -1 : 1)
          : rhs.is_null() ? (first ? 1 : -1) : reference;
      int actual = 0;
      ASSERT_EQ(OB_SUCCESS, (first ? null_first : null_last)(lhs, rhs, actual, nullptr));
      ASSERT_EQ(expected, actual);
    }
  };
  const std::vector<std::pair<std::string, std::string>> suffixes = {
      {"", ""},
      {"\xe4\xb8\xad\xe6\x96\x87", "\xc3\xa9"},
      {"\xc3\xa9", "\xf0\x9f\x98\x80"},
      {"\xffx", "\xffy"},
      {"\xc0\x80", "\xe4\xb8"},
      {" ", "  "},
      {std::string("\0", 1), ""},
      {std::string(64, 'x'), std::string(65, 'x')}};
  // Exhaustive ASCII pairs include controls, DEL and punctuation that would
  // incorrectly compare equal under unguarded case-bit folding (@/`, [/{, ...).
  for (int l_first = 0; l_first < 128; ++l_first) {
    for (int r_first = 0; r_first < 128; ++r_first) {
      for (size_t i = 0; i < suffixes.size(); ++i) {
        SCOPED_TRACE(::testing::Message() << END_SPACE << ", "
            << l_first << ", " << r_first << ", " << i);
        const std::string l_value = std::string(1, static_cast<char>(l_first)) + suffixes[i].first;
        const std::string r_value = std::string(1, static_cast<char>(r_first)) + suffixes[i].second;
        ObDatum lhs, rhs;
        lhs.set_string(l_value.data(), l_value.size());
        rhs.set_string(r_value.data(), r_value.size());
        check_pair(lhs, rhs);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
      }
    }
  }
  // Non-ASCII first bytes must retain collation equivalence and invalid-byte
  // fallback; unequal lengths must retain space padding and control ordering.
  const std::vector<std::string> values = {
      "", "a", "A", "a ", "a  ", "a\t", std::string("a\0", 2),
      "e", "E", "\xc3\xa9", "\xc3\x89", "\xc3\xa0", "\xc3\x80",
      "\xe4\xb8\xad", "\xf0\x9f\x98\x80", "\xff", "\xc0\x80", "\xe4\xb8",
      "a\xffx", "A\xffy", "a\xc3\xa9", "A\xc3\x89"};
  for (size_t i = 0; i < values.size() + 2; ++i) {
    for (size_t j = 0; j < values.size() + 2; ++j) {
      SCOPED_TRACE(::testing::Message() << END_SPACE << ", edge " << i << ", " << j);
      const auto datum_at = [&](size_t index) {
        ObDatum value;
        if (index < values.size()) {
          value.set_string(values[index].data(), values[index].size());
        } else if (index == values.size()) {
          value.set_string(nullptr, 0);
        } else {
          value.set_null();
        }
        return value;
      };
      check_pair(datum_at(i), datum_at(j));
      ASSERT_FALSE(::testing::Test::HasFatalFailure());
    }
  }
  // Preserve argument validation instead of dereferencing a nonempty nullptr.
  ObDatum invalid, text;
  invalid.set_string(nullptr, 1);
  text.set_string("a", 1);
  check_pair(invalid, text);
  check_pair(text, invalid);
}

TEST(SortKeyCompare, general_ci_ascii_first_weight_preserves_collation_and_dispatch)
{
  check_general_ci_ascii_first_weights<false>();
  check_general_ci_ascii_first_weights<true>();
}

template <bool END_SPACE>
void check_general_ci_three_byte_weights()
{
  const auto encode = [](uint32_t cp) {
    std::string bytes;
    bytes += static_cast<char>(0xe0 | (cp >> 12));
    bytes += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    bytes += static_cast<char>(0x80 | (cp & 0x3f));
    return bytes;
  };
  const auto check = [](const std::string &left, const std::string &right) {
    ObDatum l, r;
    l.set_string(left.data(), left.size());
    r.set_string(right.data(), right.size());
    check_short_general_ci_pair(l, r);
    int actual = 0;
    const int legacy = ObCharset::strcmpsp(CS_TYPE_UTF8MB4_GENERAL_CI,
        l.ptr_, l.len_, r.ptr_, r.len_, END_SPACE);
    ASSERT_EQ(OB_SUCCESS,
        (datum_cmp::ObDatumStrCmp<CS_TYPE_UTF8MB4_GENERAL_CI, END_SPACE>::cmp(l, r, actual)));
    ASSERT_EQ(legacy > 0 ? 1 : (legacy < 0 ? -1 : 0), actual);
  };
  // Every valid three-byte code point, including non-identity weight pages,
  // participates on each side; same-prefix suffixes must still be compared.
  for (uint32_t cp = 0x800; cp <= 0xffff; ++cp) {
    if (cp >= 0xd800 && cp <= 0xdfff) { continue; }
    const uint32_t other = 0x800 + ((cp * 7919U) % 0xf800);
    for (const auto &suffix : std::vector<std::pair<std::string, std::string>>{
        {"", ""}, {" ", "  "}, {"\xffx", "\xc0\x80"},
        {std::string(65, 'x'), std::string(66, 'x')}}) {
      check(encode(cp) + suffix.first, encode(other) + suffix.second);
      check(encode(other) + suffix.second, encode(cp) + suffix.first);
      check(encode(cp) + suffix.first, encode(cp) + suffix.second);
      ASSERT_FALSE(::testing::Test::HasFatalFailure());
    }
  }
  // Exhaust lead/second-byte combinations and continuation boundaries, including
  // overlong encodings, surrogates, malformed suffixes, and truncated prefixes.
  for (int a = 0xe0; a <= 0xef; ++a) {
    for (int b = 0; b < 256; ++b) {
      for (int c : {0, 0x7f, 0x80, 0xbf, 0xc0, 0xff}) {
        std::string value{static_cast<char>(a), static_cast<char>(b), static_cast<char>(c)};
        for (int len : {1, 2, 3}) {
          check(value.substr(0, len), encode(0x4e2d));
          check(encode(0x4e2d), value.substr(0, len));
        }
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
      }
    }
  }
}

TEST(SortKeyCompare, general_ci_three_byte_first_weight_matches_charset)
{
  check_general_ci_three_byte_weights<false>();
  check_general_ci_three_byte_weights<true>();
#if defined(__linux__)
  const int64_t page_size = sysconf(_SC_PAGESIZE);
  ASSERT_GT(page_size, 3);
  void *mapping = mmap(nullptr, 3 * page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(MAP_FAILED, mapping);
  struct ThreeByteMappingGuard
  {
    void *mapping_;
    int64_t size_;
    ~ThreeByteMappingGuard() { munmap(mapping_, size_); }
  } cleanup{mapping, 3 * page_size};
  char *readable = static_cast<char *>(mapping) + page_size;
  ASSERT_EQ(0, mprotect(readable, page_size, PROT_READ | PROT_WRITE));
  for (const std::string &bytes : {
      std::string("\xe4\xb8\xad"), std::string("\xe6\x96\x87"),
      std::string("\xef\xbc\xa1"), std::string("\xef\xbd\x81"),
      std::string("\xe0\x80\x80"), std::string("\xed\xa0\x80")}) {
    for (int len : {1, 2, 3}) {
      MEMCPY(readable, bytes.data(), len);
      MEMCPY(readable + page_size - len, "\xe6\x96\x87", len);
      ObDatum left, right;
      left.set_string(readable, len);
      right.set_string(readable + page_size - len, len);
      check_short_general_ci_pair(left, right);
      check_short_general_ci_pair(right, left);
      ASSERT_FALSE(::testing::Test::HasFatalFailure());
    }
  }
#endif
}

template <bool END_SPACE>
void check_binary_comparison_against_charset()
{
  std::vector<std::string> values = {
      "", "a", "A", "a ", "a  ", " ", "  ", "b", "prefix", "prefix ",
      std::string("a\0", 2), std::string("a\0b", 3), std::string("a\0 ", 3),
      std::string("\xc0\x80", 2), std::string("\xe4\xb8", 2),
      std::string("\xff\xfe", 2)};
  for (int byte = 0; byte < 256; ++byte) {
    values.emplace_back(1, static_cast<char>(byte));
  }
  std::mt19937 random(0x465453);
  for (int length : {2, 3, 7, 8, 15, 16, 31, 32, 63, 64, 65, 127, 128, 1024, 65536}) {
    std::string value(length, '\0');
    for (char &byte : value) {
      byte = static_cast<char>(random() & 0xff);
    }
    values.push_back(value);
    values.push_back(value + ' ');
    values.push_back(value + '\0');
  }
  const auto datum_at = [&](size_t i) {
    ObDatum datum;
    if (i < values.size()) {
      datum.set_string(values[i].data(), values[i].size());
    } else if (i == values.size()) {
      datum.set_string(nullptr, 0);
    } else {
      datum.set_null();
    }
    return datum;
  };
  const ObDatumCmpFuncType null_first = ObDatumFuncs::get_nullsafe_cmp_func(
      ObVarcharType, ObVarcharType, NULL_FIRST, CS_TYPE_BINARY, SCALE_UNKNOWN_YET, false);
  const ObDatumCmpFuncType null_last = ObDatumFuncs::get_nullsafe_cmp_func(
      ObVarcharType, ObVarcharType, NULL_LAST, CS_TYPE_BINARY, SCALE_UNKNOWN_YET, false);
  ASSERT_NE(nullptr, null_first);
  ASSERT_NE(nullptr, null_last);
  for (size_t i = 0; i < values.size() + 2; ++i) {
    const ObDatum lhs = datum_at(i);
    for (size_t j = 0; j < values.size() + 2; ++j) {
      SCOPED_TRACE(::testing::Message() << END_SPACE << ", " << i << ", " << j);
      const ObDatum rhs = datum_at(j);
      int expected = 0;
      if (!lhs.is_null() && !rhs.is_null()) {
        const int reference = ObCharset::strcmpsp(
            CS_TYPE_BINARY, lhs.ptr_, lhs.len_, rhs.ptr_, rhs.len_, END_SPACE);
        expected = reference > 0 ? 1 : (reference < 0 ? -1 : 0);
        int actual = 0;
        ASSERT_EQ(OB_SUCCESS,
            (datum_cmp::ObDatumStrCmp<CS_TYPE_BINARY, END_SPACE>::cmp(lhs, rhs, actual)));
        EXPECT_EQ(expected, actual);
      }
      for (bool first : {false, true}) {
        const int null_expected = lhs.is_null() && rhs.is_null() ? 0
            : lhs.is_null() ? (first ? -1 : 1)
            : rhs.is_null() ? (first ? 1 : -1) : expected;
        int actual = 0;
        ASSERT_EQ(OB_SUCCESS, (first ? null_first : null_last)(lhs, rhs, actual, nullptr));
        EXPECT_EQ(null_expected, actual);
      }
    }
  }
  // A nonempty datum with a null pointer must retain charset validation instead
  // of passing its pointer to memcmp. Datum lengths themselves are unsigned.
  ObDatum invalid, text;
  invalid.set_string(nullptr, 1);
  text.set_string("a", 1);
  for (bool reverse : {false, true}) {
    const ObDatum &lhs = reverse ? text : invalid;
    const ObDatum &rhs = reverse ? invalid : text;
    const int reference = ObCharset::strcmpsp(
        CS_TYPE_BINARY, lhs.ptr_, lhs.len_, rhs.ptr_, rhs.len_, END_SPACE);
    int actual = 0;
    ASSERT_EQ(OB_SUCCESS,
        (datum_cmp::ObDatumStrCmp<CS_TYPE_BINARY, END_SPACE>::cmp(lhs, rhs, actual)));
    EXPECT_EQ(reference > 0 ? 1 : (reference < 0 ? -1 : 0), actual);
  }
}

TEST(SortKeyCompare, binary_comparison_preserves_bytes_padding_and_null_ordering)
{
  check_binary_comparison_against_charset<false>();
  check_binary_comparison_against_charset<true>();
}

void check_binary_fixed_key_pair(const ObDatum &lhs, const ObDatum &rhs)
{
  const int legacy = ObCharset::strcmpsp(
      CS_TYPE_BINARY, lhs.ptr_, lhs.len_, rhs.ptr_, rhs.len_, false);
  const int expected = legacy > 0 ? 1 : (legacy < 0 ? -1 : 0);
  int actual = 0;
  ASSERT_EQ(OB_SUCCESS,
      (datum_cmp::ObDatumStrCmp<CS_TYPE_BINARY, false>::cmp(lhs, rhs, actual)));
  ASSERT_EQ(expected, actual);
  const int legacy_end_space = ObCharset::strcmpsp(
      CS_TYPE_BINARY, lhs.ptr_, lhs.len_, rhs.ptr_, rhs.len_, true);
  ASSERT_EQ(OB_SUCCESS,
      (datum_cmp::ObDatumStrCmp<CS_TYPE_BINARY, true>::cmp(lhs, rhs, actual)));
  ASSERT_EQ(legacy_end_space > 0 ? 1 : (legacy_end_space < 0 ? -1 : 0), actual);
  // Also exercise the dispatch table compiled into the database library.
  static const ObDatumCmpFuncType registered[] = {
      ObDatumFuncs::get_nullsafe_cmp_func(
          ObVarcharType, ObVarcharType, NULL_FIRST, CS_TYPE_BINARY, SCALE_UNKNOWN_YET, false),
      ObDatumFuncs::get_nullsafe_cmp_func(
          ObVarcharType, ObVarcharType, NULL_LAST, CS_TYPE_BINARY, SCALE_UNKNOWN_YET, false)};
  for (const ObDatumCmpFuncType compare : registered) {
    ASSERT_NE(nullptr, compare);
    ASSERT_EQ(OB_SUCCESS, compare(lhs, rhs, actual, nullptr));
    ASSERT_EQ(expected, actual);
  }
}

TEST(SortKeyCompare, fixed_binary_keys_preserve_first_differing_unsigned_byte)
{
  // Unaligned buffers and every possible byte at each position exercise both
  // words, including equal first words and the last significant payload byte.
  char left[16 + 32], right[16 + 32];
  const int boundaries[] = {0, 1, 31, 32, 65, 127, 128, 254, 255};
  for (int position = 0; position < 16; ++position) {
    for (int byte = 0; byte < 256; ++byte) {
      for (const int other : boundaries) {
        SCOPED_TRACE(::testing::Message() << position << ", " << byte << ", " << other);
        const int left_offset = byte & 31;
        const int right_offset = (byte * 7 + position) & 31;
        MEMSET(left, 0, sizeof(left));
        MEMSET(right, 0, sizeof(right));
        for (int i = 0; i < 16; ++i) {
          left[left_offset + i] = right[right_offset + i] = static_cast<char>(i * 19 + 11);
        }
        left[left_offset + position] = static_cast<char>(byte);
        right[right_offset + position] = static_cast<char>(other);
        if (position < 15) {
          // Later bytes must not override the first difference's ordering.
          left[left_offset + 15] = static_cast<char>(other);
          right[right_offset + 15] = static_cast<char>(byte);
        }
        ObDatum lhs, rhs;
        lhs.set_string(left + left_offset, 16);
        rhs.set_string(right + right_offset, 16);
        check_binary_fixed_key_pair(lhs, rhs);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        check_binary_fixed_key_pair(rhs, lhs);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
      }
    }
  }
  // Other widths, flags and invalid pointers retain the existing branch and
  // charset argument handling even when the other operand has width sixteen.
  MEMSET(left, ' ', sizeof(left));
  MEMSET(right, ' ', sizeof(right));
  ObDatum lhs, rhs;
  lhs.set_string(left + 1, 16);
  for (const int length : {0, 1, 15, 16, 17, 31}) {
    rhs.set_string(right + 1, length);
    check_binary_fixed_key_pair(lhs, rhs);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    check_binary_fixed_key_pair(rhs, lhs);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
  }
  rhs.set_string(right + 1, 16);
  rhs.set_outrow();
  check_binary_fixed_key_pair(lhs, rhs);
  ASSERT_FALSE(::testing::Test::HasFatalFailure());
  check_binary_fixed_key_pair(rhs, lhs);
  ASSERT_FALSE(::testing::Test::HasFatalFailure());
  rhs.set_string(nullptr, 16);
  check_binary_fixed_key_pair(lhs, rhs);
  ASSERT_FALSE(::testing::Test::HasFatalFailure());
  check_binary_fixed_key_pair(rhs, lhs);
}

TEST(SortKeyCompare, fixed_binary_keys_require_no_readable_prefix_or_suffix)
{
#if defined(__linux__)
  const int64_t page_size = sysconf(_SC_PAGESIZE);
  ASSERT_GT(page_size, 64);
  void *mapping = mmap(nullptr, 3 * page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(MAP_FAILED, mapping);
  struct FixedKeyMappingGuard
  {
    void *mapping_;
    int64_t size_;
    ~FixedKeyMappingGuard() { munmap(mapping_, size_); }
  } cleanup{mapping, 3 * page_size};
  char *readable = static_cast<char *>(mapping) + page_size;
  ASSERT_EQ(0, mprotect(readable, page_size, PROT_READ | PROT_WRITE));
  for (int left_offset = 0; left_offset < 32; ++left_offset) {
    for (int right_offset = 0; right_offset < 32; ++right_offset) {
      char *left = readable + left_offset;
      char *right = readable + page_size - 16 - right_offset;
      for (int position = 0; position < 16; ++position) {
        MEMSET(left, 0x80, 16);
        MEMSET(right, 0x80, 16);
        ObDatum lhs, rhs;
        lhs.set_string(left, 16);
        rhs.set_string(right, 16);
        check_binary_fixed_key_pair(lhs, rhs);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        right[position] = static_cast<char>(position & 1 ? 0xff : 0x00);
        check_binary_fixed_key_pair(lhs, rhs);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        check_binary_fixed_key_pair(rhs, lhs);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
      }
    }
  }
#endif
}

ObChunkDatumStore::StoredRow *make_sort_row(
    ObIAllocator &allocator, int64_t id, const std::string *word, int64_t rank)
{
  const int64_t size = sizeof(ObChunkDatumStore::StoredRow) + 3 * sizeof(ObDatum)
      + 2 * sizeof(int64_t);
  void *memory = allocator.alloc(size);
  if (nullptr == memory) {
    return nullptr;
  }
  auto *row = new (memory) ObChunkDatumStore::StoredRow();
  row->cnt_ = 3;
  row->row_size_ = size;
  for (int i = 0; i < 3; ++i) {
    new (row->cells() + i) ObDatum();
  }
  char *int_payload = reinterpret_cast<char *>(row->get_extra_payload());
  row->cells()[0].ptr_ = int_payload;
  row->cells()[2].ptr_ = int_payload + sizeof(int64_t);
  row->cells()[0].set_int(id);
  if (nullptr == word) {
    row->cells()[1].set_null();
  } else {
    row->cells()[1].set_string(word->data(), word->size());
  }
  row->cells()[2].set_int(rank);
  return row;
}

int reference_row_compare(const ObChunkDatumStore::StoredRow *lhs,
                          const ObChunkDatumStore::StoredRow *rhs,
                          const ObIArray<ObSortFieldCollation> &collations,
                          const ObIArray<ObSortCmpFunc> &functions,
                          int64_t begin, int64_t end)
{
  int result = 0;
  for (int64_t i = begin; 0 == result && i < end; ++i) {
    const auto &collation = collations.at(i);
    EXPECT_EQ(OB_SUCCESS, functions.at(i).cmp_func_(
        lhs->cells()[collation.field_idx_], rhs->cells()[collation.field_idx_], result, nullptr));
    if (!collation.is_ascending_) {
      result = -result;
    }
  }
  return result;
}

TEST(SortKeyCompare, cached_metadata_preserves_order_ranges_and_merge_adapters)
{
  ObArenaAllocator allocator("SortCmpTest");
  ObExecContext exec_ctx(allocator);
  ObSEArray<ObSortFieldCollation, 3> collations;
  ObSEArray<ObSortCmpFunc, 3> functions;
  ASSERT_EQ(OB_SUCCESS, collations.push_back(
      ObSortFieldCollation(1, CS_TYPE_UTF8MB4_GENERAL_CI, true, NULL_LAST)));
  ASSERT_EQ(OB_SUCCESS, collations.push_back(
      ObSortFieldCollation(2, CS_TYPE_BINARY, false, NULL_FIRST)));
  ASSERT_EQ(OB_SUCCESS, collations.push_back(
      ObSortFieldCollation(0, CS_TYPE_BINARY, true, NULL_FIRST)));
  ObSortCmpFunc word_cmp;
  word_cmp.cmp_func_ = ObDatumFuncs::get_basic_func(
      ObVarcharType, CS_TYPE_UTF8MB4_GENERAL_CI)->null_last_cmp_;
  ObSortCmpFunc int_cmp;
  int_cmp.cmp_func_ = ObDatumFuncs::get_basic_func(ObIntType, CS_TYPE_BINARY)->null_first_cmp_;
  ASSERT_EQ(OB_SUCCESS, functions.push_back(word_cmp));
  ASSERT_EQ(OB_SUCCESS, functions.push_back(int_cmp));
  ASSERT_EQ(OB_SUCCESS, functions.push_back(int_cmp));

  ObSortOpImpl::Compare cmp;
  ASSERT_EQ(OB_SUCCESS, cmp.init(&collations, &functions, &exec_ctx, false));
  std::vector<std::string> words = {"a", "A", "a ", "b", "数据库", "数据", "", "é", "e"};
  std::vector<ObChunkDatumStore::StoredRow *> rows;
  for (size_t i = 0; i < words.size() + 1; ++i) {
    auto *row = make_sort_row(allocator, i, i < words.size() ? &words[i] : nullptr, i % 3);
    ASSERT_NE(nullptr, row);
    rows.push_back(row);
  }
  ObSortOpChunk left_chunk(0), right_chunk(0);
  for (auto *lhs : rows) {
    for (auto *rhs : rows) {
      const int expected = reference_row_compare(lhs, rhs, collations, functions, 0, 3);
      EXPECT_EQ(expected < 0, cmp(lhs, rhs));
      EXPECT_EQ(expected > 0, cmp(&lhs, &rhs));
      left_chunk.row_ = lhs;
      right_chunk.row_ = rhs;
      EXPECT_EQ(expected > 0, cmp(&left_chunk, &right_chunk));
      EXPECT_EQ(-expected, cmp.with_ties_cmp(lhs, rhs));
    }
  }
  std::vector<ObChunkDatumStore::StoredRow *> expected = rows;
  std::sort(expected.begin(), expected.end(), [&](auto *lhs, auto *rhs) {
    return reference_row_compare(lhs, rhs, collations, functions, 0, 3) < 0;
  });
  std::sort(rows.begin(), rows.end(), ObSortOpImpl::CopyableComparer(cmp));
  EXPECT_EQ(expected, rows);
  cmp.set_cmp_range(1, 3);
  for (auto *lhs : rows) {
    for (auto *rhs : rows) {
      EXPECT_EQ(reference_row_compare(lhs, rhs, collations, functions, 1, 3) < 0, cmp(lhs, rhs));
    }
  }
  EXPECT_EQ(OB_SUCCESS, cmp.get_error_code());

  // Prefix sorts use fixed slices; reset/reinitialization must replace cached addresses.
  cmp.reset();
  ObArrayHelper<ObSortFieldCollation> prefix_collations;
  ObArrayHelper<ObSortCmpFunc> prefix_functions;
  prefix_collations.init(2, collations.get_data() + 1, 2);
  prefix_functions.init(2, functions.get_data() + 1, 2);
  ASSERT_EQ(OB_SUCCESS, cmp.init(&prefix_collations, &prefix_functions, &exec_ctx, false));
  for (auto *lhs : rows) {
    for (auto *rhs : rows) {
      EXPECT_EQ(reference_row_compare(lhs, rhs, collations, functions, 1, 3) < 0, cmp(lhs, rhs));
    }
  }
  cmp.reset();
  ObSEArray<ObSortCmpFunc, 3> missing_function;
  EXPECT_EQ(OB_INVALID_ARGUMENT, cmp.init(&collations, &missing_function, &exec_ctx, false));
  EXPECT_FALSE(cmp.is_inited());

  // A fully consumed prefix is represented by an empty sort-key slice.
  ObSEArray<ObSortFieldCollation, 3> empty_collations;
  ASSERT_EQ(OB_SUCCESS, cmp.init(&empty_collations, &missing_function, &exec_ctx, false));
  EXPECT_FALSE(cmp(rows[0], rows[1]));
  EXPECT_EQ(0, cmp.with_ties_cmp(rows[0], rows[1]));
  cmp.reset();

  // Encoded ordering uses the first cell, and its ordinary-key fallback must
  // retain the initialized metadata after the encoding flag is cleared.
  ASSERT_EQ(OB_SUCCESS, cmp.init(&collations, &functions, &exec_ctx, true));
  ObDatum first = rows[0]->cells()[0];
  ObDatum second = rows[1]->cells()[0];
  rows[0]->cells()[0].set_string("a", 1);
  rows[1]->cells()[0].set_string("aa", 2);
  EXPECT_TRUE(cmp(rows[0], rows[1]));
  EXPECT_FALSE(cmp(rows[1], rows[0]));
  EXPECT_FALSE(cmp(rows[0], rows[0]));
  rows[0]->cells()[0] = first;
  rows[1]->cells()[0] = second;
  cmp.enable_encode_sortkey_ = false;
  EXPECT_EQ(reference_row_compare(rows[0], rows[1], collations, functions, 0, 3) < 0,
            cmp(rows[0], rows[1]));
  EXPECT_EQ(OB_SUCCESS, cmp.get_error_code());
}

} // namespace
} // namespace sql
} // namespace oceanbase
