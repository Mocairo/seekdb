/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <map>
#include <string>
#include <utility>
#include "sql/das/ob_das_domain_utils.h"

namespace oceanbase {
namespace sql {
namespace {
using namespace common;
using namespace storage;
using WordCounts = std::map<std::string, std::pair<uint64_t, uint64_t>>;

ObObjMeta fts_row_cache_meta(const ObCollationType collation = CS_TYPE_UTF8MB4_BIN)
{
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(collation);
  return meta;
}

void record_fts_cache_row(const blocksstable::ObDatumRow &row,
                          const bool is_index,
                          const ObDatum &doc_id,
                          WordCounts &words)
{
  ASSERT_EQ(4, row.count_);
  const int64_t word_idx = is_index ? 0 : 1;
  const int64_t doc_id_idx = is_index ? 1 : 0;
  EXPECT_TRUE(ObDatum::binary_equal(doc_id, row.storage_datums_[doc_id_idx]));
  const ObString word = row.storage_datums_[word_idx].get_string();
  const std::string owned_word(word.ptr(), word.length());
  EXPECT_EQ(0, words.count(owned_word));
  words[owned_word] = std::make_pair(row.storage_datums_[2].get_uint(),
                                    row.storage_datums_[3].get_uint());
}

TEST(FtsIndexRowCache, matches_materialized_rows_across_reuse_resize_and_collation)
{
  const ObString parser("ngram.1");
  const ObString properties("{\"ngram_token_size\":1}");
  const std::string documents[] = {
    "AaBb Aa\xc3\xa1\xe4\xb8\xad\xe6\x96\x87",
    std::string(12000, 'X') + " Yz",
    "a",
    "ABab \xe4\xb8\xad\xe4\xb8\xad 123",
  };
  const char doc_id_bytes[] = {'a', '\0', 'b', '\xFF', 'c'};
  ObDatum doc_id;
  doc_id.set_string(doc_id_bytes, sizeof(doc_id_bytes));
  for (const bool is_index : {false, true}) {
    ObFTIndexRowCache cache;
    ASSERT_EQ(OB_SUCCESS, cache.init(is_index, parser, properties));
    for (const ObCollationType collation : {CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI}) {
      const ObObjMeta meta = fts_row_cache_meta(collation);
      for (const std::string &document : documents) {
        SCOPED_TRACE(document.size());
        ObArenaAllocator materialized_allocator("FtsCacheTest");
        ObFTParseHelper helper;
        ObDomainIndexRow materialized;
        ASSERT_EQ(OB_SUCCESS, helper.init(&materialized_allocator, parser, properties));
        const ObString fulltext(document.size(), document.data());
        ASSERT_EQ(OB_SUCCESS, ObDASDomainUtils::generate_fulltext_word_rows(
            materialized_allocator, &helper, meta, doc_id, fulltext, is_index, materialized));
        ASSERT_EQ(OB_SUCCESS, cache.segment(meta, doc_id, fulltext));
        WordCounts expected;
        WordCounts actual;
        for (int64_t i = 0; i < materialized.count(); ++i) {
          record_fts_cache_row(*materialized.at(i), is_index, doc_id, expected);
        }
        blocksstable::ObDatumRow *row = nullptr;
        for (int64_t i = 0; i < materialized.count(); ++i) {
          ASSERT_EQ(OB_SUCCESS, cache.get_next_row(row));
          ASSERT_NE(nullptr, row);
          record_fts_cache_row(*row, is_index, doc_id, actual);
        }
        EXPECT_EQ(expected, actual);
        EXPECT_EQ(OB_ITER_END, cache.get_next_row(row));
        EXPECT_EQ(nullptr, row);
        EXPECT_EQ(OB_ITER_END, cache.get_next_row(row));
      }
    }
  }
}

TEST(FtsIndexRowCache, owns_document_id_and_word_bytes_until_next_document)
{
  ObFTIndexRowCache cache;
  ASSERT_EQ(OB_SUCCESS, cache.init(true, ObString("ngram.1"),
      ObString("{\"ngram_token_size\":1}")));
  char source[] = "AaBbCcDd";
  char source_id[] = {'d', '\0', 'o', 'c'};
  const char expected_id[] = {'d', '\0', 'o', 'c'};
  ObDatum doc_id;
  doc_id.set_string(source_id, sizeof(source_id));
  ASSERT_EQ(OB_SUCCESS, cache.segment(fts_row_cache_meta(), doc_id,
      ObString(sizeof(source) - 1, source)));
  MEMSET(source, '!', sizeof(source) - 1);
  MEMSET(source_id, '!', sizeof(source_id));
  ObDatum expected_doc_id;
  expected_doc_id.set_string(expected_id, sizeof(expected_id));
  blocksstable::ObDatumRow *row = nullptr;
  ASSERT_EQ(OB_SUCCESS, cache.get_next_row(row));
  const blocksstable::ObDatumRow *first_row = row;
  const ObString first_word = row->storage_datums_[0].get_string();
  const std::string saved_first_word(first_word.ptr(), first_word.length());
  WordCounts words;
  record_fts_cache_row(*row, true, expected_doc_id, words);
  for (int i = 1; i < 4; ++i) {
    ASSERT_EQ(OB_SUCCESS, cache.get_next_row(row));
    EXPECT_EQ(first_row, row);
    record_fts_cache_row(*row, true, expected_doc_id, words);
    EXPECT_EQ(saved_first_word, std::string(first_word.ptr(), first_word.length()));
  }
  EXPECT_EQ(4, words.size());
  for (const auto &word : words) {
    EXPECT_EQ(std::make_pair(uint64_t(2), uint64_t(8)), word.second);
  }
  EXPECT_EQ(OB_ITER_END, cache.get_next_row(row));

  // Integer document ids use datum buffers too; retain their original bytes
  // after the source expression's datum is overwritten.
  blocksstable::ObStorageDatum numeric_id;
  numeric_id.set_uint(42);
  ASSERT_EQ(OB_SUCCESS, cache.segment(fts_row_cache_meta(), numeric_id, ObString("aa")));
  numeric_id.set_uint(99);
  ASSERT_EQ(OB_SUCCESS, cache.get_next_row(row));
  EXPECT_EQ(42, row->storage_datums_[1].get_uint());
}

TEST(FtsIndexRowCache, empty_failure_partial_consumption_reset_and_reinitialization)
{
  ObFTIndexRowCache cache;
  blocksstable::ObDatumRow *row = nullptr;
  ObDatum doc_id;
  doc_id.set_null();
  EXPECT_EQ(OB_NOT_INIT, cache.get_next_row(row));
  EXPECT_EQ(OB_NOT_INIT, cache.segment(fts_row_cache_meta(), doc_id, ObString("abc")));
  EXPECT_NE(OB_SUCCESS, cache.init(false, ObString("unknown.1"), ObString()));
  ASSERT_EQ(OB_SUCCESS, cache.init(false, ObString("ngram.1"),
      ObString("{\"ngram_token_size\":1}")));
  EXPECT_EQ(OB_INIT_TWICE, cache.init(false, ObString("ngram.1"), ObString()));
  ASSERT_EQ(OB_SUCCESS, cache.segment(fts_row_cache_meta(), doc_id, ObString("abc")));
  ASSERT_EQ(OB_SUCCESS, cache.get_next_row(row));
  EXPECT_TRUE(row->storage_datums_[0].is_null());
  ASSERT_EQ(OB_SUCCESS, cache.segment(fts_row_cache_meta(), doc_id, ObString("zz")));
  ASSERT_EQ(OB_SUCCESS, cache.get_next_row(row));
  EXPECT_EQ(ObString("z"), row->storage_datums_[1].get_string());
  EXPECT_EQ(OB_ITER_END, cache.get_next_row(row));

  EXPECT_EQ(OB_ITER_END, cache.segment(fts_row_cache_meta(), doc_id, ObString()));
  EXPECT_EQ(OB_ITER_END, cache.get_next_row(row));
  EXPECT_EQ(OB_ITER_END, cache.segment(fts_row_cache_meta(), doc_id, ObString(" ")));
  EXPECT_EQ(OB_ITER_END, cache.get_next_row(row));
  ASSERT_EQ(OB_SUCCESS, cache.segment(fts_row_cache_meta(), doc_id, ObString("abc")));
  EXPECT_EQ(OB_INVALID_ARGUMENT,
      cache.segment(fts_row_cache_meta(CS_TYPE_INVALID), doc_id, ObString("xyz")));
  EXPECT_EQ(OB_ITER_END, cache.get_next_row(row));
  // ObString normalizes a null pointer to an empty string.
  EXPECT_EQ(OB_ITER_END,
      cache.segment(fts_row_cache_meta(), doc_id, ObString(1, static_cast<const char *>(nullptr))));
  EXPECT_EQ(OB_ITER_END, cache.get_next_row(row));
  EXPECT_EQ(OB_INVALID_ARGUMENT,
      cache.segment(fts_row_cache_meta(), doc_id, ObString(-1, "x")));
  EXPECT_EQ(OB_ITER_END, cache.get_next_row(row));
  ASSERT_EQ(OB_SUCCESS, cache.segment(fts_row_cache_meta(), doc_id, ObString("abc")));
  cache.reuse();
  EXPECT_EQ(OB_ITER_END, cache.get_next_row(row));
  cache.reset();
  cache.reset();
  EXPECT_EQ(OB_NOT_INIT, cache.get_next_row(row));
  ASSERT_EQ(OB_SUCCESS, cache.init(true, ObString("ngram.1"),
      ObString("{\"ngram_token_size\":2}")));
  ASSERT_EQ(OB_SUCCESS, cache.segment(fts_row_cache_meta(), doc_id, ObString("abc")));
  int64_t row_count = 0;
  while (OB_SUCCESS == cache.get_next_row(row)) {
    EXPECT_EQ(2, row->storage_datums_[0].get_string().length());
    ++row_count;
  }
  EXPECT_EQ(2, row_count);
}
} // namespace
} // namespace sql
} // namespace oceanbase
