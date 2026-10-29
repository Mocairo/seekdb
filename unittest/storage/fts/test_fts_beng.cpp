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
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "share/text_analysis/ob_text_analyzer.h"
#include "storage/fts/ob_beng_ft_parser.h"

namespace oceanbase
{
namespace storage
{
namespace
{
using namespace common;

class BengFreeingAllocator : public ObIAllocator
{
public:
  ~BengFreeingAllocator() override
  {
    for (const auto &allocation : active_) {
      std::free(allocation.first);
    }
  }

  void *alloc(const int64_t size) override
  {
    void *pointer = nullptr;
    if (requests_++ != fail_at_ && size > 0 && nullptr != (pointer = std::malloc(size))) {
      active_[pointer] = size;
    }
    return pointer;
  }
  void *alloc(const int64_t size, const ObMemAttr &) override { return alloc(size); }
  void free(void *pointer) override
  {
    if (nullptr != pointer) {
      const auto found = active_.find(pointer);
      if (found == active_.end()) {
        invalid_free_ = true;
      } else {
        std::memset(pointer, 0xa5, found->second);
        active_.erase(found);
        std::free(pointer);
      }
    }
  }

  bool owns(const ObString &word) const
  {
    const auto found = active_.find(const_cast<char *>(word.ptr()));
    return found != active_.end() && word.length() <= found->second;
  }
  int64_t requests() const { return requests_; }
  void fail_at(const int64_t request) { fail_at_ = request; }
  bool invalid_free() const { return invalid_free_; }

private:
  std::map<void *, int64_t> active_;
  int64_t requests_ = 0;
  int64_t fail_at_ = -1;
  bool invalid_free_ = false;
};

int read_beng_tokens(ObITokenIterator &parser, std::vector<ObString> &tokens)
{
  int ret = OB_SUCCESS;
  const char *word = nullptr;
  int64_t length = 0, characters = 0, frequency = 0;
  while (OB_SUCCESS == (ret = parser.get_next_token(word, length, characters, frequency))) {
    EXPECT_EQ(length, characters);
    EXPECT_EQ(1, frequency);
    tokens.emplace_back(length, word);
  }
  EXPECT_EQ(nullptr, word);
  EXPECT_EQ(0, length);
  EXPECT_EQ(0, characters);
  EXPECT_EQ(0, frequency);
  return OB_ITER_END == ret ? OB_SUCCESS : ret;
}

void check_beng_tokens(const std::vector<ObString> &tokens,
                       const BengFreeingAllocator &allocator)
{
  const char *expected[] = {"first", "second", "123", "first"};
  ASSERT_EQ(ARRAYSIZEOF(expected), tokens.size());
  for (size_t i = 0; i < tokens.size(); ++i) {
    ASSERT_TRUE(allocator.owns(tokens[i]));
    EXPECT_EQ(ObString(expected[i]), tokens[i]);
  }
}

TEST(FtsBengTest, normalized_tokens_keep_caller_lifetime_and_match_original_pipeline)
{
  for (const char *input : {"Alpha BETA alpha 42 co-operate can't ++ ... Z",
      "Mixed\tCASE\nTEXT 123\r\nTest_One",
      "abc\xe4\xb8\xad\xe6\x96\x87 XYZ \xc3\x89" "cole end",
      "bad\xe4\xb8"}) {
    SCOPED_TRACE(input);
    ObArenaAllocator allocator;
    ObArenaAllocator legacy_allocator;
    ObFTParserParam param;
    param.allocator_ = &allocator;
    param.cs_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
    param.fulltext_ = input;
    param.ft_length_ = std::strlen(input);
    std::vector<ObString> retained_tokens;
    std::vector<std::string> expected_tokens;
    {
      share::ObTextAnalysisCtx legacy_ctx;
      legacy_ctx.cs_ = param.cs_;
      legacy_ctx.filter_stopword_ = false;
      legacy_ctx.need_grouping_ = false;
      share::ObEnglishTextAnalyzer legacy;
      ASSERT_EQ(OB_SUCCESS, legacy.init(legacy_ctx, legacy_allocator));
      ObDatum document;
      document.set_string(input, param.ft_length_);
      share::ObITokenStream *reference = nullptr;
      ASSERT_EQ(OB_SUCCESS, legacy.analyze(document, reference));
      ObBEngFTParser parser(allocator);
      ASSERT_EQ(OB_SUCCESS, parser.init(&param));
      while (true) {
        ObDatum expected;
        int64_t expected_frequency = 0;
        const int expected_ret = reference->get_next(expected, expected_frequency);
        const char *word = nullptr;
        int64_t length = 0, characters = 0, frequency = 0;
        const int ret = parser.get_next_token(word, length, characters, frequency);
        ASSERT_EQ(expected_ret, ret);
        if (OB_SUCCESS != ret) {
          break;
        }
        EXPECT_EQ(expected_frequency, frequency);
        EXPECT_EQ(length, characters);
        expected_tokens.emplace_back(expected.ptr_, static_cast<size_t>(expected.len_));
        retained_tokens.emplace_back(length, word);
        EXPECT_EQ(expected_tokens.back(), std::string(word, length));
      }
    }
    // Neither another get_next call nor pipeline destruction may recycle bytes
    // already returned to the grouping map / index row builder.
    allocator.alloc(8192);
    ASSERT_EQ(expected_tokens.size(), retained_tokens.size());
    for (size_t i = 0; i < retained_tokens.size(); ++i) {
      EXPECT_EQ(expected_tokens[i], std::string(retained_tokens[i].ptr(), retained_tokens[i].length()));
    }
  }
}

TEST(FtsBengTest, explicit_normalizer_reuse_retains_output_allocator)
{
  ObArenaAllocator pipeline_allocator, output_allocator;
  share::ObTextAnalysisCtx ctx;
  ctx.cs_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
  ctx.filter_stopword_ = false;
  ctx.token_allocator_ = &output_allocator;
  share::ObEnglishTextAnalyzer analyzer;
  ASSERT_EQ(OB_SUCCESS, analyzer.init(ctx, pipeline_allocator));
  std::vector<ObString> retained;
  for (int iteration = 0; iteration < 3; ++iteration) {
    ObDatum document;
    document.set_string("FIRST second", 12);
    share::ObITokenStream *stream = nullptr;
    ASSERT_EQ(OB_SUCCESS, analyzer.analyze(document, stream));
    ObDatum token;
    int64_t frequency = 0;
    ASSERT_EQ(OB_SUCCESS, stream->get_next(token, frequency));
    retained.push_back(token.get_string());
    ASSERT_EQ(OB_SUCCESS, stream->get_next(token, frequency));
    EXPECT_EQ(ObString("second"), token.get_string());
    EXPECT_EQ(OB_ITER_END, stream->get_next(token, frequency));
  }
  analyzer.reset();
  for (const auto &token : retained) {
    EXPECT_EQ(ObString("first"), token);
  }
}

TEST(FtsBengTest, parser_reset_and_invalid_initialization_preserve_caller_tokens)
{
  BengFreeingAllocator allocator;
  ObFTParserParam param;
  param.allocator_ = &allocator;
  param.cs_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
  param.fulltext_ = "FIRST second 123 FIRST";
  param.ft_length_ = std::strlen(param.fulltext_);
  ObBEngFTParser parser(allocator);
  std::vector<ObString> retained;
  ASSERT_EQ(OB_SUCCESS, parser.init(&param));
  EXPECT_EQ(OB_INIT_TWICE, parser.init(&param));
  ASSERT_EQ(OB_SUCCESS, read_beng_tokens(parser, retained));
  parser.reset();
  check_beng_tokens(retained, allocator);
  EXPECT_EQ(OB_INVALID_ARGUMENT, parser.init(nullptr));
  param.ft_length_ = 0;
  EXPECT_EQ(OB_INVALID_ARGUMENT, parser.init(&param));
  param.ft_length_ = static_cast<int64_t>(UINT32_MAX) + 1;
  EXPECT_EQ(OB_NOT_SUPPORTED, parser.init(&param));
  param.ft_length_ = std::strlen(param.fulltext_);
  param.cs_ = nullptr;
  EXPECT_EQ(OB_INVALID_ARGUMENT, parser.init(&param));
  ASSERT_EQ(OB_NOT_INIT, read_beng_tokens(parser, retained));
  // A new collation and a fresh document must not inherit the old stream's
  // end-of-input state, even after several failed initialization attempts.
  param.cs_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_GENERAL_CI);
  ASSERT_NE(nullptr, param.cs_);
  std::vector<ObString> again;
  ASSERT_EQ(OB_SUCCESS, parser.init(&param));
  ASSERT_EQ(OB_SUCCESS, read_beng_tokens(parser, again));
  parser.reset();
  check_beng_tokens(retained, allocator);
  check_beng_tokens(again, allocator);
  EXPECT_FALSE(allocator.invalid_free());
}

TEST(FtsBengTest, descriptor_allocation_failures_allow_retry_without_releasing_tokens)
{
  ObBasicEnglishFTParserDesc descriptor;
  int64_t requests = 0;
  {
    BengFreeingAllocator allocator;
    ObFTParserParam param;
    param.allocator_ = &allocator;
    param.cs_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
    param.fulltext_ = "FIRST second 123 FIRST";
    param.ft_length_ = std::strlen(param.fulltext_);
    ObITokenIterator *iterator = nullptr;
    std::vector<ObString> retained;
    ASSERT_EQ(OB_SUCCESS, descriptor.segment(&param, iterator));
    ASSERT_NE(nullptr, iterator);
    ASSERT_EQ(OB_SUCCESS, read_beng_tokens(*iterator, retained));
    requests = allocator.requests();
    descriptor.free_token_iter(&param, iterator);
    check_beng_tokens(retained, allocator);
    EXPECT_FALSE(allocator.invalid_free());
  }
  ASSERT_GT(requests, 0);
  for (int64_t failure = 0; failure < requests; ++failure) {
    SCOPED_TRACE(failure);
    BengFreeingAllocator allocator;
    allocator.fail_at(failure);
    ObFTParserParam param;
    param.allocator_ = &allocator;
    param.cs_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
    param.fulltext_ = "FIRST second 123 FIRST";
    param.ft_length_ = std::strlen(param.fulltext_);
    ObITokenIterator *iterator = nullptr;
    std::vector<ObString> retained;
    int ret = descriptor.segment(&param, iterator);
    if (OB_SUCCESS == ret) {
      ASSERT_NE(nullptr, iterator);
      ret = read_beng_tokens(*iterator, retained);
    }
    EXPECT_EQ(OB_ALLOCATE_MEMORY_FAILED, ret);
    if (nullptr != iterator) {
      descriptor.free_token_iter(&param, iterator);
      iterator = nullptr;
    }
    for (const ObString &word : retained) {
      ASSERT_TRUE(allocator.owns(word));
    }
    allocator.fail_at(-1);
    std::vector<ObString> recovered;
    ASSERT_EQ(OB_SUCCESS, descriptor.segment(&param, iterator));
    ASSERT_NE(nullptr, iterator);
    ASSERT_EQ(OB_SUCCESS, read_beng_tokens(*iterator, recovered));
    descriptor.free_token_iter(&param, iterator);
    check_beng_tokens(recovered, allocator);
    for (const ObString &word : retained) {
      ASSERT_TRUE(allocator.owns(word));
    }
    EXPECT_FALSE(allocator.invalid_free());
  }
}
} // namespace
} // namespace storage
} // namespace oceanbase
