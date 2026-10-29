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
#include <string>
#include <vector>
#include "storage/retrieval/ob_text_retrieval_token_iter.h"
#include "storage/retrieval/ob_sparse_retrieval_util.h"

namespace oceanbase
{
namespace storage
{
namespace
{
using namespace common;

class TokenAdvanceCache
{
public:
  int init(const std::vector<std::string> &values, const bool numeric,
           const ObCollationType collation = CS_TYPE_BINARY)
  {
    int ret = OB_SUCCESS;
    ids_.resize(values.size());
    count_ = values.size();
    // The linear reference deliberately uses the original datum comparator.
    reference_cmp_ = ObDatumFuncs::get_basic_func(
        numeric ? ObUInt64Type : ObVarcharType, collation)->null_first_cmp_;
    cmp_ = get_domain_id_cmp_func(
        numeric ? ObUInt64Type : ObVarcharType, collation);
    for (int64_t i = 0; OB_SUCC(ret) && i < count_; ++i) {
      uint64_t numeric_value = 0;
      ObDatum datum(reinterpret_cast<char *>(&numeric_value), 0, false);
      if (numeric) {
        datum.set_uint(std::stoull(values[i]));
      } else {
        datum.set_string(values[i].data(), values[i].size());
      }
      ret = ids_[i].from_datum(datum);
    }
    return ret;
  }

  void check(const ObDatum &target, const int64_t begin)
  {
    int64_t expected = begin;
    for (; expected < count_; ++expected) {
      int cmp = 0;
      ASSERT_EQ(OB_SUCCESS, reference_cmp_(
          target, ids_[expected].get_datum(), cmp, nullptr));
      if (cmp <= 0) { break; }
    }
    int64_t actual = -1;
    ASSERT_EQ(OB_SUCCESS, find_cached_text_doc_id(ids_.data(), count_, begin, cmp_, target, actual));
    ASSERT_EQ(expected, actual);
    // The caller passes its cursor as both begin and output.
    int64_t cursor = begin;
    ASSERT_EQ(OB_SUCCESS, find_cached_text_doc_id(ids_.data(), count_, cursor, cmp_, target, cursor));
    EXPECT_EQ(expected, cursor);
  }

  std::vector<ObDocIdExt> ids_;
  int64_t count_ = 0;
  ObDatumCmpFuncType cmp_ = nullptr;
  ObDatumCmpFuncType reference_cmp_ = nullptr;
};

std::vector<int64_t> token_advance_positions(const int64_t count)
{
  std::vector<int64_t> positions;
  for (int64_t i = 0; i <= count; ++i) {
    if (count <= 31 || i <= 2 || i == count / 3 || i == count / 2 || i >= count - 2) {
      positions.push_back(i);
    }
  }
  return positions;
}

TEST(FtsTokenAdvance, unsigned_ids_match_linear_search)
{
  for (const int64_t count : {0, 1, 2, 3, 7, 31, 256}) {
    std::vector<std::string> values;
    for (int64_t i = 0; i < count; ++i) {
      // Equal IDs exercise lower-bound tie handling as well as sparse targets.
      values.push_back(std::to_string((i / 2) * 6));
    }
    TokenAdvanceCache cache;
    ASSERT_EQ(OB_SUCCESS, cache.init(values, true));
    std::vector<uint64_t> targets = {0, 1, UINT64_MAX, uint64_t{1} << 63};
    for (const auto &value : values) {
      const uint64_t id = std::stoull(value);
      targets.push_back(id);
      targets.push_back(id + 1);
      if (id > 0) { targets.push_back(id - 1); }
    }
    for (const int64_t begin : token_advance_positions(count)) {
      for (uint64_t target : targets) {
        SCOPED_TRACE(::testing::Message() << count << ", " << begin << ", " << target);
        ObDatum datum(reinterpret_cast<char *>(&target), 0, false);
        datum.set_uint(target);
        cache.check(datum, begin);
      }
    }
  }
}

TEST(FtsTokenAdvance, binary_and_collation_order_match_the_original_comparator)
{
  const std::vector<std::string> binary_values = {
      "", std::string("\0", 1), "A", "a", std::string("a\0", 2), "a ",
      "a  ", "aa", "ab", "b", "z", std::string("\x7f", 1),
      std::string("\x80", 1), std::string("\xff", 1),
      std::string("\xc3\x84", 2), std::string("\xc3\xa9", 2)};
  for (const ObCollationType collation : {CS_TYPE_BINARY, CS_TYPE_UTF8MB4_BIN,
                                        CS_TYPE_UTF8MB4_GENERAL_CI}) {
    std::vector<std::string> values;
    for (const auto &value : binary_values) {
      // Nonbinary collation order requires valid Unicode; binary IDs may
      // contain arbitrary bytes. Keep the search precondition explicit.
      if (collation == CS_TYPE_BINARY
          || (value != std::string("\x80", 1) && value != std::string("\xff", 1))) {
        values.push_back(value);
      }
    }
    const auto cmp = ObDatumFuncs::get_basic_func(ObVarcharType, collation)->null_first_cmp_;
    std::stable_sort(values.begin(), values.end(), [cmp](const std::string &a, const std::string &b) {
      ObDatum lhs, rhs;
      lhs.set_string(a.data(), a.size());
      rhs.set_string(b.data(), b.size());
      int result = 0;
      EXPECT_EQ(OB_SUCCESS, cmp(lhs, rhs, result, nullptr));
      return result < 0;
    });
    TokenAdvanceCache cache;
    ASSERT_EQ(OB_SUCCESS, cache.init(values, false, collation));
    std::vector<std::string> targets = values;
    targets.push_back("a!");
    targets.push_back("aaa");
    targets.push_back(collation == CS_TYPE_BINARY ? std::string("\xff\xff", 2) : "zz");
    for (const int64_t begin : token_advance_positions(values.size())) {
      for (const auto &target : targets) {
        ObDatum datum;
        datum.set_string(target.data(), target.size());
        cache.check(datum, begin);
      }
    }
    // Only the received rows of a partial final batch may be searched.
    cache.count_ = 3;
    for (const auto &target : targets) {
      ObDatum datum;
      datum.set_string(target.data(), target.size());
      cache.check(datum, 0);
    }
  }
}

int64_t token_advance_compare_calls = 0;
int64_t token_advance_fail_at = -1;
ObDatumCmpFuncType token_advance_reference = nullptr;

int count_token_advance_comparisons(
    const ObDatum &a, const ObDatum &b, int &result, const ObDatumAccessContext *ctx)
{
  ++token_advance_compare_calls;
  return token_advance_compare_calls == token_advance_fail_at ? OB_TIMEOUT
      : token_advance_reference(a, b, result, ctx);
}

TEST(FtsTokenAdvance, nearby_advances_stay_cheap_and_far_advances_are_bounded)
{
  std::vector<std::string> values;
  for (int64_t i = 0; i < 256; ++i) { values.push_back(std::to_string(i * 2)); }
  TokenAdvanceCache cache;
  ASSERT_EQ(OB_SUCCESS, cache.init(values, true));
  token_advance_reference = cache.reference_cmp_;
  cache.cmp_ = count_token_advance_comparisons;
  token_advance_fail_at = -1;
  for (uint64_t target : {uint64_t{40}, uint64_t{42}, uint64_t{510}, UINT64_MAX}) {
    token_advance_compare_calls = 0;
    ObDatum datum(reinterpret_cast<char *>(&target), 0, false);
    datum.set_uint(target);
    int64_t position = -1;
    ASSERT_EQ(OB_SUCCESS, find_cached_text_doc_id(
        cache.ids_.data(), cache.count_, 20, cache.cmp_, datum, position));
    if (target == 40) {
      EXPECT_EQ(1, token_advance_compare_calls);
    } else if (target == 42) {
      EXPECT_EQ(2, token_advance_compare_calls);
    } else {
      EXPECT_LE(token_advance_compare_calls, 18);
    }
  }
  uint64_t target = 126;
  ObDatum datum(reinterpret_cast<char *>(&target), 0, false);
  datum.set_uint(target);
  for (int64_t fail_at : {1, 2, 3, 8}) {
    token_advance_compare_calls = 0;
    token_advance_fail_at = fail_at;
    int64_t position = -1;
    EXPECT_EQ(OB_TIMEOUT, find_cached_text_doc_id(
        cache.ids_.data(), cache.count_, 0, cache.cmp_, datum, position));
    EXPECT_EQ(-1, position);
  }
  token_advance_fail_at = -1;
  int64_t position = -1;
  EXPECT_EQ(OB_ERR_UNEXPECTED, find_cached_text_doc_id(
      cache.ids_.data(), cache.count_, -1, cache.cmp_, datum, position));
}

} // namespace
} // namespace storage
} // namespace oceanbase
