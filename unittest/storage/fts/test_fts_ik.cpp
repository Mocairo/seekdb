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
#include <cstring>
#include <string>
#include "storage/fts/ik/ob_ik_char_util.h"
#include "storage/fts/ik/ob_ik_cjk_processor.h"
#include "storage/fts/ik/ob_ik_letter_processor.h"
#include "storage/fts/ik/ob_ik_quantifier_processor.h"
#include "storage/fts/ik/ob_ik_surrogate_processor.h"

namespace oceanbase {
namespace storage {
namespace {
using namespace common;
using CharType = ObFTCharUtil::CharType;

TEST(FtsIKTest, utf8_category_ranges_and_precedence)
{
  // The supported Unicode blocks define the existing IK classification,
  // including its treatment of supplementary CJK characters as USELESS.
  struct CategoryRange { ob_wc_t first_; ob_wc_t last_; CharType type_; };
  const CategoryRange ranges[] = {
    {0x41, 0x5A, CharType::ENGLISH_LETTER},
    {0x61, 0x7A, CharType::ENGLISH_LETTER},
    {0xFF21, 0xFF3A, CharType::ENGLISH_LETTER},
    {0xFF41, 0xFF5A, CharType::ENGLISH_LETTER},
    {0x30, 0x39, CharType::ARABIC_LETTER},
    {0xFF10, 0xFF19, CharType::ARABIC_LETTER},
    {0x3400, 0x4DBF, CharType::CHINESE},
    {0x4E00, 0x9FFF, CharType::CHINESE},
    {0xF900, 0xFAFF, CharType::CHINESE},
    {0x1100, 0x11FF, CharType::OTHER_CJK},
    {0x3040, 0x30FF, CharType::OTHER_CJK},
    {0x3130, 0x318F, CharType::OTHER_CJK},
    {0x31F0, 0x31FF, CharType::OTHER_CJK},
    {0xAC00, 0xD7AF, CharType::OTHER_CJK},
    {0xFF00, 0xFFEF, CharType::OTHER_CJK},
  };
  unsigned char encoded[4];
  for (ob_wc_t codepoint = 0; codepoint <= 0x10FFFF; ++codepoint) {
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
      continue; // Surrogates are not valid UTF-8 scalar values.
    }
    CharType expected = CharType::USELESS;
    for (const auto &range : ranges) {
      if (codepoint >= range.first_ && codepoint <= range.last_) {
        expected = range.type_;
        break;
      }
    }
    const int length = ob_charset_encode_unicode<CHARSET_UTF8MB4>(
        codepoint, encoded, encoded + sizeof(encoded));
    ASSERT_GT(length, 0);
    CharType actual = CharType::SURROGATE_LOW;
    ASSERT_EQ(OB_SUCCESS, ObFTCharUtil::classify_first_char(CS_TYPE_UTF8MB4_BIN,
        reinterpret_cast<const char *>(encoded), length, actual)) << codepoint;
    ASSERT_EQ(expected, actual) << codepoint;
  }
}

TEST(FtsIKTest, malformed_utf8_and_unsupported_charset)
{
  struct InvalidInput { const char *text_; int result_; };
  const InvalidInput invalid[] = {
    {"", OB_UNEXPECT_INTERNAL_ERROR},
    {"\xE4\xB8", OB_UNEXPECT_INTERNAL_ERROR},
    // The existing decoder reports illegal scalar values with zero bytes,
    // which the classifier treats as USELESS rather than a truncated input.
    {"\x80", OB_SUCCESS}, {"\xC0\xAF", OB_SUCCESS},
    {"\xED\xA0\x80", OB_SUCCESS}, {"\xF4\x90\x80\x80", OB_SUCCESS}
  };
  CharType type = CharType::CHINESE;
  EXPECT_EQ(OB_INVALID_ARGUMENT,
      ObFTCharUtil::classify_first_char(CS_TYPE_UTF8MB4_BIN, nullptr, 1, type));
  EXPECT_EQ(CharType::USELESS, type);
  for (const auto &input : invalid) {
    type = CharType::CHINESE;
    EXPECT_EQ(input.result_, ObFTCharUtil::classify_first_char(
        CS_TYPE_UTF8MB4_BIN, input.text_, std::strlen(input.text_), type));
    EXPECT_EQ(CharType::USELESS, type);
  }
  EXPECT_EQ(OB_NOT_SUPPORTED,
      ObFTCharUtil::classify_first_char(CS_TYPE_LATIN1_BIN, "a", 1, type));
}

class NoMatchDict final : public ObIFTDict
{
public:
  int init() override { return OB_SUCCESS; }
  int match(const ObString &, bool &matched) const override
  {
    matched = false;
    return OB_SUCCESS;
  }
  int match(const ObString &, ObDATrieHit &hit) const override
  {
    hit.set_unmatch();
    return OB_SUCCESS;
  }
  int match_with_hit(const ObString &, const ObDATrieHit &, ObDATrieHit &hit) const override
  {
    hit.set_unmatch();
    return OB_SUCCESS;
  }
};

TEST(FtsIKTest, shared_character_dispatch_preserves_processor_state_and_tokens)
{
  // Exercise transitions at connectors, fullwidth forms, CJK, numbers and the
  // final character. Both paths must retain the same processor ordering.
  const char *corpus = "AbC-123 1,234.5 中国一二三十公斤 ＡＢＣ１２３ カタカナ 한글 😀 ! x@y_z + 42";
  std::string text;
  for (int i = 0; i < 40; ++i) {
    text += corpus;
  }
  ObArenaAllocator allocator;
  NoMatchDict dict;
  TokenizeContext wrapped(CS_TYPE_UTF8MB4_BIN, allocator, text.data(), text.size(), false);
  TokenizeContext shared(CS_TYPE_UTF8MB4_BIN, allocator, text.data(), text.size(), false);
  ASSERT_EQ(OB_SUCCESS, wrapped.init());
  ASSERT_EQ(OB_SUCCESS, shared.init());
  ObIKLetterProcessor wrapped_letter, shared_letter;
  ObIKQuantifierProcessor wrapped_quantity(dict, allocator), shared_quantity(dict, allocator);
  ObIKCJKProcessor wrapped_cjk(dict, allocator), shared_cjk(dict, allocator);
  ObIKSurrogateProcessor wrapped_surrogate, shared_surrogate;
  ObIIKProcessor *wrapped_processors[] = {
    &wrapped_letter, &wrapped_quantity, &wrapped_cjk, &wrapped_surrogate
  };
  ObIIKProcessor *shared_processors[] = {
    &shared_letter, &shared_quantity, &shared_cjk, &shared_surrogate
  };
  while (!shared.iter_end()) {
    const char *character = nullptr;
    uint8_t length = 0;
    CharType type;
    ASSERT_EQ(OB_SUCCESS, shared.current_char(character, length));
    ASSERT_EQ(OB_SUCCESS, shared.current_char_type(type));
    for (int i = 0; i < 4; ++i) {
      ASSERT_EQ(OB_SUCCESS, wrapped_processors[i]->process(wrapped));
      ASSERT_EQ(OB_SUCCESS, shared_processors[i]->do_process(shared, character, length, type));
      ASSERT_EQ(wrapped.get_cursor(), shared.get_cursor());
    }
    ASSERT_EQ(wrapped.step_next(), shared.step_next());
  }
  auto wrapped_token = wrapped.token_list().tokens().begin();
  auto shared_token = shared.token_list().tokens().begin();
  while (wrapped_token != wrapped.token_list().tokens().end()) {
    ASSERT_NE(shared.token_list().tokens().end(), shared_token);
    EXPECT_EQ(wrapped_token->ptr_, shared_token->ptr_);
    EXPECT_EQ(wrapped_token->offset_, shared_token->offset_);
    EXPECT_EQ(wrapped_token->length_, shared_token->length_);
    EXPECT_EQ(wrapped_token->char_cnt_, shared_token->char_cnt_);
    EXPECT_EQ(wrapped_token->type_, shared_token->type_);
    ++wrapped_token;
    ++shared_token;
  }
  EXPECT_EQ(shared.token_list().tokens().end(), shared_token);
}

} // namespace
} // namespace storage
} // namespace oceanbase
