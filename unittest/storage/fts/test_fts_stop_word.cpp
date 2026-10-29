/*
 * Copyright (c) 2025 OceanBase.
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
#include <array>
#include <atomic>
#include <string>
#include <thread>
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ob_fts_parser_property.h"

namespace oceanbase
{
namespace storage
{
namespace
{
using namespace common;

int check_word(ObStopWordChecker &checker, ObCollationType collation,
               const ObString &text, bool &is_stopword)
{
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(collation);
  const ObFTWord word(text.length(), text.ptr(), meta);
  return checker.check_stopword(word, is_stopword);
}

TEST(FtsStopWordTest, utf8_input_always_uses_stopword_collation)
{
  ObStopWordChecker checker;
  ASSERT_EQ(OB_SUCCESS, checker.init());
  for (ObCollationType collation : {
      CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI, CS_TYPE_UTF8MB4_UNICODE_CI}) {
    SCOPED_TRACE(collation);
    for (const auto &word : ob_stop_word_list) {
      bool is_stopword = false;
      ASSERT_EQ(OB_SUCCESS, check_word(checker, collation, ObString(word), is_stopword));
      EXPECT_TRUE(is_stopword) << word;
      std::string upper(word);
      for (char &character : upper) {
        if (character >= 'a' && character <= 'z') {
          character -= 'a' - 'A';
        }
      }
      is_stopword = false;
      ASSERT_EQ(OB_SUCCESS, check_word(checker, collation, ObString(upper.c_str()), is_stopword));
      EXPECT_TRUE(is_stopword) << upper;
    }
    // Source BIN collation must not disable the stopword set's accent/case
    // equivalence. Padding must not trigger an incorrect byte-length shortcut.
    for (const char *word : {"\xc3\xa1", "TH\xc3\x89", "the   ",
                             "the                                    "}) {
      bool is_stopword = false;
      ASSERT_EQ(OB_SUCCESS, check_word(checker, collation, ObString(word), is_stopword));
      EXPECT_TRUE(is_stopword) << word;
    }
    for (const char *word : {"search", "database", "theory", "th"}) {
      bool is_stopword = true;
      ASSERT_EQ(OB_SUCCESS, check_word(checker, collation, ObString(word), is_stopword));
      EXPECT_FALSE(is_stopword) << word;
    }
  }
}

TEST(FtsStopWordTest, binary_conversion_and_unsupported_collations)
{
  ObStopWordChecker checker;
  ASSERT_EQ(OB_SUCCESS, checker.init());
  // Binary input takes the original conversion path before GENERAL_CI lookup.
  for (const char *word : {"THE", "th\xc3\xa9", "the   "}) {
    bool is_stopword = false;
    ASSERT_EQ(OB_SUCCESS, check_word(checker, CS_TYPE_BINARY, ObString(word), is_stopword));
    EXPECT_TRUE(is_stopword) << word;
  }
  bool is_stopword = true;
  ASSERT_EQ(OB_SUCCESS, check_word(checker, CS_TYPE_BINARY, ObString("search"), is_stopword));
  EXPECT_FALSE(is_stopword);

  // This seekdb configuration rejects these legacy collations in charset_convert.
  // The fast path must preserve that error and leave the output untouched.
  for (ObCollationType collation : {CS_TYPE_INVALID, CS_TYPE_LATIN1_BIN,
      CS_TYPE_UTF16_BIN, CS_TYPE_UTF16LE_BIN, CS_TYPE_MAX}) {
    SCOPED_TRACE(collation);
    is_stopword = true;
    EXPECT_EQ(OB_INVALID_ARGUMENT, check_word(checker, collation, ObString("the"), is_stopword));
    EXPECT_TRUE(is_stopword);
  }
}

TEST(FtsStopWordTest, initialization_empty_input_and_reinitialization)
{
  ObStopWordChecker checker;
  bool is_stopword = true;
  EXPECT_EQ(OB_NOT_INIT, check_word(checker, CS_TYPE_UTF8MB4_BIN, ObString("the"), is_stopword));
  EXPECT_TRUE(is_stopword);
  EXPECT_EQ(OB_NOT_INIT, check_word(checker, CS_TYPE_INVALID, ObString(), is_stopword));
  ASSERT_EQ(OB_SUCCESS, checker.init());
  EXPECT_EQ(OB_INIT_TWICE, checker.init());
  EXPECT_EQ(OB_INVALID_ARGUMENT, check_word(checker, CS_TYPE_UTF8MB4_BIN, ObString(), is_stopword));
  EXPECT_TRUE(is_stopword);
  is_stopword = false;
  EXPECT_EQ(OB_SUCCESS, check_word(checker, CS_TYPE_UTF8MB4_BIN, ObString("the"), is_stopword));
  EXPECT_TRUE(is_stopword);
  checker.destroy();
  EXPECT_EQ(OB_NOT_INIT, check_word(checker, CS_TYPE_UTF8MB4_BIN, ObString("the"), is_stopword));
  ASSERT_EQ(OB_SUCCESS, checker.init());
  is_stopword = false;
  EXPECT_EQ(OB_SUCCESS, check_word(checker, CS_TYPE_UTF8MB4_BIN, ObString("the"), is_stopword));
  EXPECT_TRUE(is_stopword);
}

TEST(FtsStopWordTest, published_set_supports_concurrent_readers_and_reinitialization)
{
  struct WordCase { const char *text; bool expected; };
  const std::array<WordCase, 8> words = {{{"a", true}, {"\xc3\xa1", true},
      {"THE", true}, {"TH\xc3\x89", true}, {"the   ", true},
      {"search", false}, {"theory", false}, {"\xe4\xb8\xad\xe6\x96\x87", false}}};
  const std::array<ObCollationType, 3> collations = {{CS_TYPE_UTF8MB4_BIN,
      CS_TYPE_UTF8MB4_GENERAL_CI, CS_TYPE_UTF8MB4_UNICODE_CI}};
  ObStopWordChecker checker;
  for (int generation = 0; generation < 2; ++generation) {
    ASSERT_EQ(OB_SUCCESS, checker.init());
    std::atomic<bool> start{false};
    std::atomic<int64_t> failures{0};
    std::array<std::thread, 8> readers;
    for (size_t reader = 0; reader < readers.size(); ++reader) {
      readers[reader] = std::thread([&, reader]() {
        while (!start.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        for (int repeat = 0; repeat < 1000; ++repeat) {
          const ObCollationType collation = collations[(reader + repeat) % collations.size()];
          for (const auto &word : words) {
            bool is_stopword = !word.expected;
            const int ret = check_word(checker, collation, ObString(word.text), is_stopword);
            if (OB_SUCCESS != ret || word.expected != is_stopword) {
              failures.fetch_add(1, std::memory_order_relaxed);
            }
          }
        }
      });
    }
    start.store(true, std::memory_order_release);
    for (auto &reader : readers) { reader.join(); }
    EXPECT_EQ(0, failures.load());
    checker.destroy();
    bool is_stopword = true;
    EXPECT_EQ(OB_NOT_INIT, check_word(checker, CS_TYPE_UTF8MB4_BIN, ObString("the"), is_stopword));
    EXPECT_TRUE(is_stopword);
  }
}

TEST(FtsAddWordTest, grouping_preserves_last_collation_equivalent_key_and_frequencies)
{
  for (ObCollationType collation : {CS_TYPE_UTF8MB4_GENERAL_CI, CS_TYPE_UTF8MB4_BIN}) {
    SCOPED_TRACE(collation);
    ObArenaAllocator allocator;
    ObFTWordMap words;
    ASSERT_EQ(OB_SUCCESS, words.create(16, "FtGroupTest"));
    ObFTParserProperty property;
    ObObjMeta meta;
    meta.set_varchar();
    meta.set_collation_type(collation);
    ObAddWordFlag flag;
    flag.set_groupby_word();
    ObAddWord add_word(property, meta, flag, allocator, words);
    const char *first = "alpha";
    const char *second = "ALPHA";
    const char *last = "\xc3\xa1lpha ";
    ASSERT_EQ(OB_SUCCESS, add_word.process_word(first, STRLEN(first), 5, 7));
    int64_t frequency = 0;
    ASSERT_EQ(OB_SUCCESS, words.get_refactored(ObFTWord(STRLEN(first), first, meta), frequency));
    EXPECT_EQ(1, frequency); // Existing semantics for an aggregated first token.
    ASSERT_EQ(OB_SUCCESS, add_word.process_word(second, STRLEN(second), 5, 3));
    ASSERT_EQ(OB_SUCCESS, add_word.process_word(last, STRLEN(last), 6, 2));
    EXPECT_EQ(12, add_word.get_add_word_count());
    if (CS_TYPE_UTF8MB4_GENERAL_CI == collation) {
      ASSERT_EQ(1, words.size());
      EXPECT_EQ(6, words.begin()->second);
      EXPECT_EQ(std::string(last), std::string(words.begin()->first.get_word().ptr_,
          words.begin()->first.get_word().len_));
    } else {
      ASSERT_EQ(3, words.size());
      for (const auto &entry : words) {
        EXPECT_EQ(1, entry.second);
      }
    }
    EXPECT_EQ(OB_INVALID_ARGUMENT, add_word.process_word(first, STRLEN(first), 5, 0));
    EXPECT_EQ(12, add_word.get_add_word_count());
  }
}

TEST(FtsAddWordTest, non_grouping_duplicate_errors_and_casedown_are_unchanged)
{
  ObArenaAllocator allocator;
  ObFTWordMap words;
  ASSERT_EQ(OB_SUCCESS, words.create(16, "FtGroupTest"));
  ObFTParserProperty property;
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  ObAddWordFlag flag;
  ObAddWord ungrouped(property, meta, flag, allocator, words);
  ASSERT_EQ(OB_SUCCESS, ungrouped.process_word("Alpha", 5, 5, 2));
  EXPECT_EQ(OB_HASH_EXIST, ungrouped.process_word("Alpha", 5, 5, 1));
  EXPECT_EQ(2, ungrouped.get_add_word_count());
  ASSERT_EQ(OB_SUCCESS, words.reuse());
  flag.set_groupby_word();
  flag.set_casedown();
  ObAddWord grouped(property, meta, flag, allocator, words);
  ASSERT_EQ(OB_SUCCESS, grouped.process_word("Alpha", 5, 5, 1));
  ASSERT_EQ(OB_SUCCESS, grouped.process_word("ALPHA", 5, 5, 1));
  ASSERT_EQ(1, words.size());
  EXPECT_EQ(2, words.begin()->second);
  EXPECT_EQ(ObString("alpha"), words.begin()->first.get_word().get_string());
}

TEST(FtsAddWordTest, independent_document_maps_can_group_and_reuse_concurrently)
{
  constexpr int THREAD_COUNT = 8;
  std::array<int, THREAD_COUNT> results{};
  std::array<int64_t, THREAD_COUNT> frequencies{};
  std::array<std::thread, THREAD_COUNT> threads;
  for (int thread = 0; thread < THREAD_COUNT; ++thread) {
    threads[thread] = std::thread([&, thread]() {
      ObArenaAllocator allocator;
      ObFTWordMap words;
      int &ret = results[thread];
      ret = words.create(16, "FtGroupTest");
      ObFTParserProperty property;
      ObObjMeta meta;
      meta.set_varchar();
      meta.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
      ObAddWordFlag flag;
      flag.set_groupby_word();
      for (int document = 0; OB_SUCCESS == ret && document < 10; ++document) {
        ret = words.reuse();
        ObAddWord add_word(property, meta, flag, allocator, words);
        for (int word = 0; OB_SUCCESS == ret && word < 200; ++word) {
          const char *spelling = (0 == word % 2) ? "Alpha" : "ALPHA";
          ret = add_word.process_word(spelling, 5, 5, 1);
        }
        if (OB_SUCCESS == ret) {
          ret = words.get_refactored(ObFTWord(5, "alpha", meta), frequencies[thread]);
        }
        if (OB_SUCCESS == ret && (1 != words.size() || 200 != frequencies[thread]
            || 200 != add_word.get_add_word_count()
            || ObString("ALPHA") != words.begin()->first.get_word().get_string())) {
          ret = OB_ERR_UNEXPECTED;
        }
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  for (int thread = 0; thread < THREAD_COUNT; ++thread) {
    EXPECT_EQ(OB_SUCCESS, results[thread]);
    EXPECT_EQ(200, frequencies[thread]);
  }
}
} // namespace
} // namespace storage
} // namespace oceanbase
