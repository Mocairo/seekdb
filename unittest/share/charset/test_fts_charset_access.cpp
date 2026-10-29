/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "lib/charset/ob_charset.h"
#include "lib/container/ob_array_wrap.h"

namespace oceanbase { namespace common {
TEST(FTSCharsetAccess, compare_matches_collation_callbacks)
{
  ASSERT_EQ(OB_SUCCESS, ObCharset::init_charset());
  const ObCollationType collations[] = {
    CS_TYPE_UTF8MB4_GENERAL_CI, CS_TYPE_UTF8MB4_BIN,
    CS_TYPE_UTF8MB4_UNICODE_CI, CS_TYPE_BINARY
  };
  std::vector<std::string> inputs = {"", " ", "  ", "a", "A", "a ", "a  ", "aa",
    "z", "Z", "café", "CAFÉ", "中文", "全角Ａ", "😀", std::string("a\0b", 3),
    std::string("\0", 1), std::string("\xc3", 1), std::string("\xff\x80", 2),
    std::string(255, 'a'), std::string(256, 'a'), std::string(257, 'a')};
  for (const auto collation : collations) {
    const ObCharsetInfo *cs = ObCharset::get_charset(collation);
    ASSERT_NE(nullptr, cs);
    for (const auto &left : inputs) {
      for (const auto &right : inputs) {
        const auto *l = reinterpret_cast<const unsigned char *>(left.data());
        const auto *r = reinterpret_cast<const unsigned char *>(right.data());
        EXPECT_EQ(cs->coll->strnncoll(cs, l, left.size(), r, right.size(), false),
                  ObCharset::strcmp(collation, left.data(), left.size(), right.data(), right.size()));
        for (bool endspace : {false, true}) {
          EXPECT_EQ(cs->coll->strnncollsp(cs, l, left.size(), r, right.size(), endspace),
                    ObCharset::strcmpsp(collation, left.data(), left.size(),
                                         right.data(), right.size(), endspace));
        }
      }
    }
    EXPECT_EQ(0, ObCharset::strcmp(collation, nullptr, 0, nullptr, 0));
    EXPECT_EQ(0, ObCharset::strcmpsp(collation, nullptr, 0, nullptr, 0, false));
    EXPECT_EQ(0, ObCharset::strcmp(collation, nullptr, 1, "a", 1));
    EXPECT_EQ(0, ObCharset::strcmp(collation, "a", -1, "a", 1));
    EXPECT_EQ(0, ObCharset::strcmpsp(collation, "a", 1, nullptr, 1, true));
    EXPECT_EQ(0, ObCharset::strcmpsp(collation, "a", 1, "a", -1, false));
  }
  // Legacy enum values remain defined but have no retained collation descriptor.
  for (const auto invalid : {CS_TYPE_INVALID, CS_TYPE_MAX,
                            CS_TYPE_LATIN1_SWEDISH_CI, CS_TYPE_UTF16_BIN}) {
    EXPECT_EQ(0, ObCharset::strcmp(invalid, "a", 1, "b", 1));
    EXPECT_EQ(0, ObCharset::strcmpsp(invalid, "a", 1, "b", 1, false));
  }
}

TEST(FTSCharsetAccess, array_access_preserves_references)
{
  for (int64_t count : {1, 3, 64, 256, 2049}) {
    std::vector<int64_t> data(count);
    ObArrayWrap<int64_t> view(data.data(), count);
    const ObIArrayWrap<int64_t> &constant = view;
    for (int64_t i = 0; i < count; ++i) {
      view.at(i) = i * 37;
      EXPECT_EQ(&data[i], &view.at(i));
      EXPECT_EQ(&data[i], &constant.at(i));
      EXPECT_EQ(i * 37, constant.at(i));
    }
  }
}
} }
