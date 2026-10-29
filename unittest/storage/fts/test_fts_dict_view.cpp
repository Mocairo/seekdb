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
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "storage/fts/dict/ob_ft_dict_hub.h"
#include "storage/fts/dict/ob_ft_cache_dict.h"
#include "storage/fts/dict/ob_ft_range_dict.h"
#include "storage/fts/dict/ob_ft_trie.h"
#include "storage/fts/ob_ik_ft_parser.h"
#include "storage/fts/ik/ob_ik_arbitrator.h"
#include "storage/fts/ik/ob_ik_cjk_processor.h"
#include "storage/fts/ik/ob_ik_letter_processor.h"
#include "storage/fts/ik/ob_ik_quantifier_processor.h"
#include "storage/fts/ik/ob_ik_surrogate_processor.h"
#include "data_plane/fts/dict/ob_gen_dic_loader.h"

namespace oceanbase
{
namespace storage
{
namespace
{
using namespace common;

class FtsDictViewTest : public ::testing::Test
{
protected:
  static void SetUpTestCase()
  {
    ASSERT_EQ(OB_SUCCESS, ObKVGlobalCache::get_instance().init(1024, 256L << 20));
    ASSERT_EQ(OB_SUCCESS, ObDictCache::get_instance().init("fts_dict_view_test"));
  }
  static void TearDownTestCase()
  {
    ObDictCache::get_instance().destroy();
    ObKVGlobalCache::get_instance().destroy();
  }
};

struct IKTokenResult
{
  std::string text_;
  int64_t offset_;
  int64_t characters_;
  int64_t frequency_;
  bool operator==(const IKTokenResult &other) const
  {
    return text_ == other.text_ && offset_ == other.offset_
        && characters_ == other.characters_ && frequency_ == other.frequency_;
  }
};

int read_ik_tokens(ObIKFTParser &parser, const char *input,
                    std::vector<IKTokenResult> &tokens)
{
  int ret = OB_SUCCESS;
  const char *word = nullptr;
  int64_t length = 0, characters = 0, frequency = 0;
  while (OB_SUCCESS == (ret = parser.get_next_token(word, length, characters, frequency))) {
    tokens.push_back({std::string(word, length), word - input, characters, frequency});
  }
  return OB_ITER_END == ret ? OB_SUCCESS : ret;
}

// Keep the former virtual processor dispatch as an independent reference for
// full parser output, including arbitration, compounding and batch boundaries.
int read_virtual_ik_tokens(const ObFTParserParam &param, const ObIFTDict &main_dict,
                           const ObIFTDict &quantifier_dict,
                           std::vector<IKTokenResult> &tokens)
{
  int ret = OB_SUCCESS;
  TokenizeContext context(CS_TYPE_UTF8MB4_BIN, *param.allocator_, param.fulltext_,
      param.ft_length_, param.ik_param_.mode_ == ObFTIKParam::Mode::SMART);
  ObIKLetterProcessor letter;
  ObIKQuantifierProcessor quantity(quantifier_dict, *param.allocator_);
  ObIKCJKProcessor cjk(main_dict, *param.allocator_);
  ObIKSurrogateProcessor surrogate;
  ObIIKProcessor *processors[] = {&letter, &quantity, &cjk, &surrogate};
  if (OB_FAIL(context.init())) {
  }
  while (OB_SUCC(ret) && !context.iter_end()) {
    context.reset_resource();
    bool boundary = false;
    while (OB_SUCC(ret) && !context.iter_end() && !boundary) {
      const char *character = nullptr;
      uint8_t length = 0;
      ObFTCharUtil::CharType type = ObFTCharUtil::CharType::USELESS;
      if (OB_FAIL(context.current_char(character, length))) {
      } else if (OB_FAIL(context.current_char_type(type))) {
      } else {
        for (ObIIKProcessor *processor : processors) {
          if (OB_FAIL(processor->do_process(context, character, length, type))) {
            break;
          }
        }
        if (OB_SUCC(ret)) {
          boundary = context.handle_size() > 1000 && type == ObFTCharUtil::CharType::USELESS;
          ret = context.step_next();
        }
      }
    }
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
    }
    if (OB_SUCC(ret)) {
      ObIKArbitrator arbitrator;
      if (OB_FAIL(arbitrator.process(context))) {
      } else if (OB_FAIL(arbitrator.output_result(context))) {
      } else {
        const char *word = nullptr;
        int64_t length = 0, offset = 0, characters = 0;
        while (OB_SUCCESS == (ret = context.get_next_token(word, length, offset, characters))) {
          tokens.push_back({std::string(word + offset, length), offset, characters, 1});
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;
}

class IKFreeingAllocator : public ObIAllocator
{
public:
  ~IKFreeingAllocator() override
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
  int64_t requests() const { return requests_; }
  void fail_at(const int64_t request) { fail_at_ = request; }
  bool invalid_free() const { return invalid_free_; }
  size_t active_count() const { return active_.size(); }
private:
  std::map<void *, int64_t> active_;
  int64_t requests_ = 0;
  int64_t fail_at_ = -1;
  bool invalid_free_ = false;
};

TEST_F(FtsDictViewTest, fixed_ik_pipeline_matches_virtual_dispatch_in_both_modes)
{
  ObFTDictHub hub;
  ASSERT_EQ(OB_SUCCESS, hub.init());
  const ObIFTDict *main_dict = nullptr, *quantifier_dict = nullptr;
  ASSERT_EQ(OB_SUCCESS, hub.get_builtin_dict(ObFTDictType::DICT_IK_MAIN, main_dict));
  ASSERT_EQ(OB_SUCCESS, hub.get_builtin_dict(ObFTDictType::DICT_IK_QUAN, quantifier_dict));
  const std::string mixed = "AbC-123 \xe4\xb8\xad\xe5\x9b\xbd\xe4\xba\xba\xe6\xb0\x91 "
      "42\xe7\xb1\xb3 \xe4\xb8\x80\xe7\x99\xbe\xe4\xba\x8c\xe5\x8d\x81\xe4\xb8\x89 "
      "1,234.5 x@y_z \xef\xbc\xa1\xef\xbc\xa2\xef\xbc\xa3 ! ";
  std::string batches;
  for (int i = 0; i < 80; ++i) {
    batches += mixed;
  }
  const std::vector<std::string> inputs = {
      mixed, batches, "x", "!", "\xe4\xb8\xad\xe5\x9b\xbd", mixed + "\xe4\xb8"};
  for (ObFTIKParam::Mode mode : {ObFTIKParam::Mode::SMART, ObFTIKParam::Mode::MAX_WORD}) {
    SCOPED_TRACE(static_cast<int>(mode));
    for (size_t i = 0; i < inputs.size(); ++i) {
      SCOPED_TRACE(i);
      // Successful output uses the production document arena. The freeing
      // allocator test below checks initialization and processing-error cleanup.
      ObArenaAllocator reference_allocator, actual_allocator;
      ObFTParserParam param;
      param.cs_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
      param.fulltext_ = inputs[i].data();
      param.ft_length_ = inputs[i].size();
      param.ik_param_.mode_ = mode;
      param.allocator_ = &reference_allocator;
      std::vector<IKTokenResult> expected, actual;
      const int expected_ret = read_virtual_ik_tokens(param, *main_dict, *quantifier_dict, expected);
      param.allocator_ = &actual_allocator;
      ObIKFTParser parser(actual_allocator, &hub);
      ASSERT_EQ(OB_SUCCESS, parser.init(param));
      EXPECT_EQ(OB_INIT_TWICE, parser.init(param));
      EXPECT_EQ(expected_ret, read_ik_tokens(parser, inputs[i].data(), actual));
      EXPECT_EQ(expected, actual);
    }
  }
}

TEST_F(FtsDictViewTest, fixed_ik_pipeline_initialization_failures_release_memory_and_retry)
{
  ObFTDictHub hub;
  ASSERT_EQ(OB_SUCCESS, hub.init());
  const ObIFTDict *dict = nullptr;
  for (ObFTDictType type : {ObFTDictType::DICT_IK_MAIN, ObFTDictType::DICT_IK_QUAN,
                            ObFTDictType::DICT_IK_STOP}) {
    ASSERT_EQ(OB_SUCCESS, hub.get_builtin_dict(type, dict));
  }
  for (ObFTIKParam::Mode mode : {ObFTIKParam::Mode::SMART, ObFTIKParam::Mode::MAX_WORD}) {
    for (int64_t failure : {0, 1}) {
      SCOPED_TRACE(failure);
      IKFreeingAllocator allocator;
      {
        ObFTParserParam param;
        param.allocator_ = &allocator;
        param.cs_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
        param.fulltext_ = "X";
        param.ft_length_ = 1;
        param.ik_param_.mode_ = mode;
        ObIKFTParser parser(allocator, &hub);
        allocator.fail_at(failure);
        EXPECT_EQ(OB_ALLOCATE_MEMORY_FAILED, parser.init(param));
        EXPECT_EQ(0, allocator.active_count());
        allocator.fail_at(-1);
        ASSERT_EQ(OB_SUCCESS, parser.init(param));
        // Fail the first letter token allocation. Later processors must not
        // run after that error, and destruction must release both owners.
        const int64_t requests = allocator.requests();
        allocator.fail_at(requests);
        const char *word = nullptr;
        int64_t length = 0, characters = 0, frequency = 0;
        EXPECT_EQ(OB_ALLOCATE_MEMORY_FAILED,
            parser.get_next_token(word, length, characters, frequency));
        EXPECT_EQ(requests + 1, allocator.requests());
      }
      EXPECT_EQ(0, allocator.active_count());
      EXPECT_FALSE(allocator.invalid_free());
    }
    IKFreeingAllocator allocator;
    {
      ObFTParserParam param;
      param.allocator_ = &allocator;
      param.cs_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
      param.fulltext_ = "\xe4\xb8";
      param.ft_length_ = 2;
      param.ik_param_.mode_ = mode;
      ObIKFTParser parser(allocator, &hub);
      EXPECT_NE(OB_SUCCESS, parser.init(param));
      EXPECT_EQ(0, allocator.active_count());
      // Retry on the same parser, then stop on malformed UTF-8 while its
      // context and CJK processor still own pending tokens and active hits.
      const char *malformed = "\xe4\xb8\xad\xe5\x9b\xbd\xe4\xb8\xad\xe4\xb8";
      param.fulltext_ = malformed;
      param.ft_length_ = std::strlen(malformed);
      ASSERT_EQ(OB_SUCCESS, parser.init(param));
      const char *word = nullptr;
      int64_t length = 0, characters = 0, frequency = 0;
      const int ret = parser.get_next_token(word, length, characters, frequency);
      EXPECT_NE(OB_SUCCESS, ret);
      EXPECT_NE(OB_ITER_END, ret);
    }
    EXPECT_EQ(0, allocator.active_count());
    EXPECT_FALSE(allocator.invalid_free());
  }
}

// Independent reference for the original UTF8MB4_BIN linear first-match lookup.
class LegacyRangeLookup final : public ObIFTDict
{
public:
  explicit LegacyRangeLookup(const ObFTCacheRangeContainer &container) : container_(container) {}

  int init() override
  {
    for (const auto *handle : container_.get_handles()) {
      blocks_.push_back(handle->value_->dat_block_);
      dicts_.emplace_back(new ObFTCacheDict(CS_TYPE_UTF8MB4_BIN, blocks_.back()));
    }
    return OB_SUCCESS;
  }

  int match(const ObString &single_word, ObDATrieHit &hit) const override
  {
    int ret = OB_SUCCESS;
    ObFTCacheDict *dict = find(single_word);
    if (nullptr == dict) {
      hit.set_unmatch();
    } else {
      hit.dict_ = dict;
      ret = dict->match(single_word, hit);
    }
    return ret;
  }

  int match(const ObString &words, bool &is_match) const override
  {
    int64_t char_length = 0;
    int ret = ObCharset::first_valid_char(
        CS_TYPE_UTF8MB4_BIN, words.ptr(), words.length(), char_length);
    if (OB_SUCC(ret)) {
      ObFTCacheDict *dict = find(ObString(char_length, words.ptr()));
      if (nullptr == dict) {
        is_match = false;
      } else {
        ret = dict->match(words, is_match);
      }
    }
    return ret;
  }

  int match_with_hit(const ObString &single_word,
                     const ObDATrieHit &last_hit,
                     ObDATrieHit &hit) const override
  {
    return last_hit.dict_->match_with_hit(single_word, last_hit, hit);
  }

private:
  ObFTCacheDict *find(const ObString &word) const
  {
    ObFTCacheDict *dict = nullptr;
    for (size_t i = 0; nullptr == dict && i < blocks_.size(); ++i) {
      if (ObCharset::strcmp(CS_TYPE_UTF8MB4_BIN, blocks_[i]->start_word_.get_word(), word) <= 0
          && ObCharset::strcmp(CS_TYPE_UTF8MB4_BIN, blocks_[i]->end_word_.get_word(), word) >= 0) {
        dict = dicts_[i].get();
      }
    }
    return dict;
  }

  const ObFTCacheRangeContainer &container_;
  std::vector<ObFTDAT *> blocks_;
  std::vector<std::unique_ptr<ObFTCacheDict>> dicts_;
};

int append_test_range(ObArenaAllocator &allocator,
                      ObFTCacheRangeContainer &container,
                      std::vector<std::unique_ptr<ObDictCacheValue>> &values,
                      const std::vector<std::string> &words)
{
  int ret = OB_SUCCESS;
  std::vector<std::string> sorted_words(words);
  std::sort(sorted_words.begin(), sorted_words.end());
  ObFTTrie<void> trie(allocator, CS_TYPE_UTF8MB4_BIN);
  ObFTDATBuilder<void> builder(allocator);
  ObFTDAT *block = nullptr;
  size_t block_size = 0;
  ObFTCacheRangeHandle *handle = nullptr;
  for (const auto &word : sorted_words) {
    if (OB_FAIL(trie.insert(ObString(word.size(), word.data()), {}))) {
      break;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(builder.init(trie))) {
  } else if (OB_FAIL(builder.build_from_trie(trie))) {
  } else if (OB_FAIL(builder.get_mem_block(block, block_size))) {
  } else if (OB_FAIL(container.fetch_info_for_dict(handle))) {
  } else {
    values.emplace_back(new ObDictCacheValue(block));
    handle->value_ = values.back().get();
  }
  return ret;
}

void expect_same_hit(const ObDATrieHit &expected, const ObDATrieHit &actual)
{
  EXPECT_EQ(expected.is_match(), actual.is_match());
  EXPECT_EQ(expected.is_prefix(), actual.is_prefix());
  EXPECT_EQ(expected.char_cnt_, actual.char_cnt_);
  EXPECT_EQ(expected.start_pos_, actual.start_pos_);
  EXPECT_EQ(expected.end_pos_, actual.end_pos_);
  EXPECT_EQ(expected.base_idx_, actual.base_idx_);
  EXPECT_EQ(expected.current_check_, actual.current_check_);
}

void verify_range_lookup(ObArenaAllocator &allocator, ObFTCacheRangeContainer &container)
{
  ObFTDictDesc desc("range_compare_test", ObFTDictType::DICT_IK_MAIN,
                    CHARSET_UTF8MB4, CS_TYPE_UTF8MB4_BIN);
  ObFTRangeDict optimized(allocator, &container, desc);
  LegacyRangeLookup legacy(container);
  ASSERT_EQ(OB_SUCCESS, optimized.init());
  ASSERT_EQ(OB_SUCCESS, legacy.init());
  std::vector<std::string> probes = {
      "", "a", "a ", std::string("a\0", 2), "b", "c", "d", "e", "z",
      "\xe4\xb8\xad", "\xe7\xb1\xb3", "\xf0\x90\x80\x80", "\xf0\x90\x80\x81",
      "\xc0\x80", "\xe4\xb8", "\xed\xa0\x80", "\xff\xff\xff\xff"};
  for (int byte = 0; byte < 256; ++byte) {
    probes.emplace_back(1, static_cast<char>(byte));
  }
  for (const auto *handle : container.get_handles()) {
    for (const ObFTSingleWord *bound : {&handle->value_->dat_block_->start_word_,
                                      &handle->value_->dat_block_->end_word_}) {
      probes.emplace_back(bound->word, bound->word_len);
      if (bound->word_len < ObCharset::MAX_MB_LEN) {
        probes.push_back(probes.back() + ' ');
        probes.push_back(std::string(bound->word, bound->word_len) + '\0');
      }
    }
  }
  for (size_t i = 0; i < probes.size(); ++i) {
    SCOPED_TRACE(i);
    const ObString probe(probes[i].size(), probes[i].data());
    ObDATrieHit expected(&legacy, 7), actual(&optimized, 7);
    EXPECT_EQ(legacy.match(probe, expected), optimized.match(probe, actual));
    expect_same_hit(expected, actual);
    // A selected range remains observable even when its DAT has no matching root.
    EXPECT_EQ(expected.dict_ != &legacy, actual.dict_ != &optimized);
  }
  ObDATrieHit expected_empty(&legacy, 7), actual_empty(&optimized, 7);
  EXPECT_EQ(legacy.match(ObString(), expected_empty), optimized.match(ObString(), actual_empty));
  expect_same_hit(expected_empty, actual_empty);
  EXPECT_EQ(expected_empty.dict_ != &legacy, actual_empty.dict_ != &optimized);

  const std::vector<std::string> words = {
      "", "aa", "ab", "ac", "apple", "bat", "c", "car", "cat", "dog", "e", "z",
      "\xe4\xb8\xad\xe5\x9b\xbd", "\xe4\xb8\xad\xe6\x96\x87", "\xe7\xb1\xb3",
      "\xf0\x90\x80\x80" "a", "\xf0\x90\x80\x81", "a ", std::string("a\0b", 3),
      "\xc0\x80", "\xe4\xb8", "\xff\xfe"};
  for (size_t i = 0; i < words.size(); ++i) {
    SCOPED_TRACE(i);
    const ObString word(words[i].size(), words[i].data());
    for (bool initial : {false, true}) {
      bool expected = initial;
      bool actual = initial;
      EXPECT_EQ(legacy.match(word, expected), optimized.match(word, actual));
      EXPECT_EQ(expected, actual);
    }
    ObDATrieHit expected(&legacy, 7), actual(&optimized, 7);
    for (int64_t offset = 0; offset < word.length();) {
      int64_t char_length = 0;
      if (OB_SUCCESS != ObCharset::first_valid_char(CS_TYPE_UTF8MB4_BIN,
          word.ptr() + offset, word.length() - offset, char_length) || char_length <= 0) {
        break;
      }
      const ObString character(char_length, word.ptr() + offset);
      if (0 == offset) {
        ASSERT_EQ(legacy.match(character, expected), optimized.match(character, actual));
      } else {
        ASSERT_EQ(legacy.match_with_hit(character, expected, expected),
                  optimized.match_with_hit(character, actual, actual));
      }
      expect_same_hit(expected, actual);
      if (expected.is_unmatch() || actual.is_unmatch()) {
        break;
      }
      offset += char_length;
    }
  }
  // Whole-word matching must retain its existing null/invalid-length behavior.
  for (const ObString &word : {ObString(), ObString(-1, "a")}) {
    for (bool initial : {false, true}) {
      bool expected = initial;
      bool actual = initial;
      EXPECT_EQ(legacy.match(word, expected), optimized.match(word, actual));
      EXPECT_EQ(expected, actual);
    }
  }
}

TEST_F(FtsDictViewTest, range_lookup_preserves_binary_boundaries_and_first_match_order)
{
  const std::vector<std::vector<std::vector<std::string>>> cases = {
      {},
      {{"aa", "ac"}},
      {{"aa", "ac"}, {"cat", "dog"},
       {"\xe4\xb8\xad\xe5\x9b\xbd", "\xe4\xb8\xad\xe6\x96\x87", "\xe7\xb1\xb3"},
       {"\xf0\x90\x80\x80" "a", "\xf0\x90\x80\x81"}},
      {{"cat", "dog"}, {"aa", "ac"}},
      {{"apple", "cat"}, {"bat", "dog"}},
      {{"bat", "dog"}, {"apple", "cat"}},
      {{"apple", "cat"}, {"car", "dog"}}};
  for (size_t i = 0; i < cases.size(); ++i) {
    SCOPED_TRACE(i);
    ObArenaAllocator allocator;
    std::vector<std::unique_ptr<ObDictCacheValue>> values;
    ObFTCacheRangeContainer container(allocator);
    for (const auto &words : cases[i]) {
      ASSERT_EQ(OB_SUCCESS, append_test_range(allocator, container, values, words));
    }
    verify_range_lookup(allocator, container);
  }
}

TEST_F(FtsDictViewTest, range_lookup_preserves_empty_and_reversed_range_behavior)
{
  for (bool empty : {false, true}) {
    SCOPED_TRACE(empty);
    ObArenaAllocator allocator;
    std::vector<std::unique_ptr<ObDictCacheValue>> values;
    ObFTCacheRangeContainer container(allocator);
    ASSERT_EQ(OB_SUCCESS, append_test_range(allocator, container, values, {"apple", "cat"}));
    ASSERT_EQ(OB_SUCCESS, values[0]->dat_block_->start_word_.set_word(empty ? "" : "z", empty ? 0 : 1));
    ASSERT_EQ(OB_SUCCESS, values[0]->dat_block_->end_word_.set_word(empty ? "" : "a", empty ? 0 : 1));
    ASSERT_EQ(OB_SUCCESS, append_test_range(allocator, container, values, {"bat", "dog"}));
    verify_range_lookup(allocator, container);
  }
}

TEST_F(FtsDictViewTest, immutable_views_match_legacy_ranges_and_survive_allocator_reuse)
{
  ObFTDictHub hub;
  const ObIFTDict *view = nullptr;
  EXPECT_EQ(OB_NOT_INIT, hub.get_builtin_dict(ObFTDictType::DICT_IK_MAIN, view));
  EXPECT_EQ(nullptr, view);
  ASSERT_EQ(OB_SUCCESS, hub.init());
  EXPECT_EQ(OB_INIT_TWICE, hub.init());
  EXPECT_EQ(OB_INVALID_ARGUMENT, hub.get_builtin_dict(ObFTDictType::DICT_TYPE_INVALID, view));
  EXPECT_EQ(OB_INVALID_ARGUMENT, hub.get_builtin_dict(static_cast<ObFTDictType>(4), view));
  const ObFTDictType types[] = {
    ObFTDictType::DICT_IK_MAIN, ObFTDictType::DICT_IK_QUAN, ObFTDictType::DICT_IK_STOP
  };
  const char *names[] = {"main_dict", "quan_dict", "stopword"};
  for (int i = 0; i < 3; ++i) {
    SCOPED_TRACE(i);
    ASSERT_EQ(OB_SUCCESS, hub.get_builtin_dict(types[i], view));
    ASSERT_NE(nullptr, view);
    const ObIFTDict *again = nullptr;
    ASSERT_EQ(OB_SUCCESS, hub.get_builtin_dict(types[i], again));
    EXPECT_EQ(view, again);
    ObArenaAllocator allocator;
    ObFTCacheRangeContainer container(allocator);
    ObFTDictDesc desc(names[i], types[i], CHARSET_UTF8MB4, CS_TYPE_UTF8MB4_BIN);
    ASSERT_EQ(OB_SUCCESS, hub.load_cache(desc, container));
    ObFTRangeDict legacy(allocator, &container, desc);
    ASSERT_EQ(OB_SUCCESS, legacy.init());
    for (const char *probe : {"\xe4\xb8\xad\xe5\x9b\xbd", "\xe7\xb1\xb3", "\xe7\x9a\x84",
                              "\xe4\xb8\xad\xe5\x9b\xbd\xe4\xba\xba", "the", "nonexistent_fts_word"}) {
      bool expected = false;
      bool actual = false;
      ASSERT_EQ(OB_SUCCESS, legacy.match(ObString(probe), expected));
      ASSERT_EQ(OB_SUCCESS, view->match(ObString(probe), actual));
      EXPECT_EQ(expected, actual) << probe;
      ObDATrieHit legacy_hit(&legacy, 0), shared_hit(view, 0);
      int64_t first_length = 0;
      ASSERT_EQ(OB_SUCCESS, ObCharset::first_valid_char(CS_TYPE_UTF8MB4_BIN,
          probe, std::strlen(probe), first_length));
      ASSERT_EQ(OB_SUCCESS, legacy.match(ObString(first_length, probe), legacy_hit));
      ASSERT_EQ(OB_SUCCESS, view->match(ObString(first_length, probe), shared_hit));
      EXPECT_EQ(legacy_hit.is_match(), shared_hit.is_match());
      EXPECT_EQ(legacy_hit.is_prefix(), shared_hit.is_prefix());
    }
  }

  // A row cache resets its document arena after each parser is destroyed.
  // Shared dictionary views must not refer into that short-lived arena.
  ObArenaAllocator document_allocator;
  const char *text = "AbC-123 \xe4\xb8\xad\xe5\x9b\xbd 42\xe7\xb1\xb3";
  for (ObFTIKParam::Mode mode : {ObFTIKParam::Mode::SMART, ObFTIKParam::Mode::MAX_WORD}) {
    std::vector<std::string> expected;
    for (int iteration = 0; iteration < 5; ++iteration) {
      std::vector<std::string> actual;
      {
        ObFTParserParam param;
        param.allocator_ = &document_allocator;
        param.cs_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
        param.fulltext_ = text;
        param.ft_length_ = std::strlen(text);
        param.ik_param_.mode_ = mode;
        ObIKFTParser parser(document_allocator, &hub);
        ASSERT_EQ(OB_SUCCESS, parser.init(param));
        const char *word = nullptr;
        int64_t length = 0, characters = 0, frequency = 0;
        int ret = OB_SUCCESS;
        while (OB_SUCCESS == (ret = parser.get_next_token(word, length, characters, frequency))) {
          EXPECT_GT(length, 0);
          EXPECT_EQ(1, frequency);
          actual.emplace_back(word, length);
        }
        ASSERT_EQ(OB_ITER_END, ret);
      }
      if (0 == iteration) {
        ASSERT_FALSE(actual.empty());
        expected = actual;
      } else {
        EXPECT_EQ(expected, actual);
      }
      document_allocator.reset();
    }
  }
  ASSERT_EQ(OB_SUCCESS, hub.destroy());
  EXPECT_EQ(OB_NOT_INIT, hub.get_builtin_dict(ObFTDictType::DICT_IK_MAIN, view));
  ASSERT_EQ(OB_SUCCESS, hub.init());
  ASSERT_EQ(OB_SUCCESS, hub.get_builtin_dict(ObFTDictType::DICT_IK_MAIN, view));
  bool matched = false;
  ASSERT_EQ(OB_SUCCESS, view->match(ObString("\xe4\xb8\xad\xe5\x9b\xbd"), matched));
  EXPECT_TRUE(matched);
}

TEST_F(FtsDictViewTest, concurrent_first_use_publishes_one_complete_view)
{
  ObFTDictHub hub;
  ASSERT_EQ(OB_SUCCESS, hub.init());
  constexpr int THREAD_COUNT = 8;
  std::array<const ObIFTDict *, THREAD_COUNT> views{};
  std::array<int, THREAD_COUNT> results{};
  std::array<bool, THREAD_COUNT> matches{};
  std::vector<std::thread> threads;
  for (int i = 0; i < THREAD_COUNT; ++i) {
    threads.emplace_back([&, i]() {
      results[i] = hub.get_builtin_dict(ObFTDictType::DICT_IK_QUAN, views[i]);
      if (OB_SUCCESS == results[i]) {
        results[i] = views[i]->match(ObString("\xe7\xb1\xb3"), matches[i]);
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  ASSERT_NE(nullptr, views[0]);
  for (int i = 0; i < THREAD_COUNT; ++i) {
    EXPECT_EQ(OB_SUCCESS, results[i]);
    EXPECT_EQ(views[0], views[i]);
    EXPECT_TRUE(matches[i]);
  }
}

TEST_F(FtsDictViewTest, failed_view_initialization_does_not_publish_or_prevent_retry)
{
  ObFTDictHub hub;
  ASSERT_EQ(OB_SUCCESS, hub.init());
  ObDictCache::get_instance().destroy();
  const ObIFTDict *view = nullptr;
  const int ret = hub.get_builtin_dict(ObFTDictType::DICT_IK_QUAN, view);
  // Restore the cache even when the assertion fails, so later tests and the
  // fixture teardown still see their expected global state.
  const int cache_ret = ObDictCache::get_instance().init("fts_dict_view_test");
  ASSERT_EQ(OB_NOT_INIT, ret);
  EXPECT_EQ(nullptr, view);
  ASSERT_EQ(OB_SUCCESS, cache_ret);
  ASSERT_EQ(OB_SUCCESS, hub.get_builtin_dict(ObFTDictType::DICT_IK_QUAN, view));
  ASSERT_NE(nullptr, view);
  bool matched = false;
  ASSERT_EQ(OB_SUCCESS, view->match(ObString("\xe7\xb1\xb3"), matched));
  EXPECT_TRUE(matched);
}

TEST(FtsDicLoader, concurrent_first_lookup_and_shared_lookups_pin_one_loader)
{
  ObGenDicLoader &loaders = ObGenDicLoader::get_instance();
  const int init_ret = loaders.init();
  ASSERT_TRUE(OB_SUCCESS == init_ret || OB_INIT_TWICE == init_ret);
  constexpr int THREAD_COUNT = 8;
  std::array<ObDicLoader *, THREAD_COUNT> found{};
  std::array<int, THREAD_COUNT> results{};
  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::vector<std::thread> threads;
  for (int i = 0; i < THREAD_COUNT; ++i) {
    threads.emplace_back([&, i]() {
      ready.fetch_add(1);
      while (!start.load()) { std::this_thread::yield(); }
      for (int iteration = 0; iteration < 200 && OB_SUCCESS == results[i]; ++iteration) {
        ObDicLoaderHandle handle;
        results[i] = loaders.get_dic_loader(ObString("ik"), CHARSET_UTF8MB4, handle);
        if (OB_SUCCESS == results[i]) {
          if (0 == iteration) {
            found[i] = handle.get_loader();
          } else if (found[i] != handle.get_loader()) {
            results[i] = OB_ERR_UNEXPECTED;
          }
        }
      }
    });
  }
  while (THREAD_COUNT != ready.load()) { std::this_thread::yield(); }
  start.store(true);
  for (auto &thread : threads) { thread.join(); }
  ASSERT_NE(nullptr, found[0]);
  for (int i = 0; i < THREAD_COUNT; ++i) {
    EXPECT_EQ(OB_SUCCESS, results[i]);
    EXPECT_EQ(found[0], found[i]);
  }
  ObDicLoaderHandle pinned;
  ASSERT_EQ(OB_SUCCESS, loaders.get_dic_loader(ObString("ik"), CHARSET_UTF8MB4, pinned));
  EXPECT_EQ(found[0], pinned.get_loader());
  EXPECT_EQ(OB_INVALID_ARGUMENT, loaders.get_dic_loader(ObString(), CHARSET_UTF8MB4, pinned));
  EXPECT_EQ(OB_INVALID_ARGUMENT, loaders.get_dic_loader(ObString("ik"), CHARSET_INVALID, pinned));
  EXPECT_EQ(OB_NOT_SUPPORTED, loaders.get_dic_loader(ObString("beng"), CHARSET_UTF8MB4, pinned));
  EXPECT_EQ(found[0], pinned.get_loader());
}
} // namespace
} // namespace storage
} // namespace oceanbase
