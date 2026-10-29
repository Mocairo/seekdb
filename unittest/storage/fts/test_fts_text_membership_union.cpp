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
#include <memory>
#include <random>
#include <string>
#include <vector>
#include "data_plane/access/ob_tablet_scan.h"
#include "query/ob_sql_define.h"
#include "storage/retrieval/ob_inv_idx_param_estimator.h"
#include "storage/retrieval/ob_sparse_bmw_iter.h"
#include "storage/retrieval/ob_sparse_daat_iter.h"
#include "storage/retrieval/ob_text_retrieval_token_iter.h"
#include "storage/retrieval/ob_text_daat_iter.h"

namespace oceanbase
{
namespace storage
{
class TextMembershipUnionTestPeer
{
public:
  static void set_compare(TextMembershipUnion &merge, common::ObDatumCmpFuncType cmp)
  { merge.cmp_func_ = cmp; }
  static void set_compare(TextMembershipBatchUnion &merge, common::ObDatumCmpFuncType cmp)
  { merge.cmp_func_ = cmp; }
};

namespace
{
using namespace common;

sql::ObDatumMeta membership_id_meta(const bool numeric)
{
  sql::ObDatumMeta meta;
  meta.type_ = numeric ? ObUInt64Type : ObVarcharType;
  meta.cs_type_ = CS_TYPE_BINARY;
  return meta;
}

// The current datum and its backing bytes are overwritten even at EOF, as
// when a real posting iterator refills its document-ID batch.
class MutableMembershipDim : public ObISRDaaTDimIter
{
public:
  MutableMembershipDim(const std::vector<std::string> &ids, const bool numeric,
      const int64_t batch_size = 1)
      : ids_(ids), numeric_(numeric), next_(0), next_calls_(0), id_calls_(0),
        score_calls_(0), fail_next_at_(-1), id_error_(OB_SUCCESS),
        null_id_(false), score_error_(OB_SUCCESS), batch_error_(OB_SUCCESS),
        empty_batch_(false), null_batch_(false), end_with_rows_(false),
        current_(buffer_, 0, false), batch_ids_(batch_size), batch_calls_(0)
  {}
  int get_next_row() override
  {
    ++next_calls_;
    MEMSET(buffer_, 0xdd, sizeof(buffer_));
    current_.set_null();
    if (next_ == fail_next_at_) {
      return OB_TIMEOUT;
    } else if (next_ >= static_cast<int64_t>(ids_.size())) {
      return OB_ITER_END;
    }
    const std::string &id = ids_[next_++];
    if (numeric_) {
      current_.set_uint(std::stoull(id));
    } else if (id.size() > sizeof(buffer_)) {
      return OB_SIZE_OVERFLOW;
    } else {
      MEMCPY(buffer_, id.data(), id.size());
      current_.set_string(buffer_, id.size());
    }
    return OB_SUCCESS;
  }
  int get_curr_id(const ObDatum *&id) const override
  {
    ++id_calls_;
    id = null_id_ ? nullptr : &current_;
    return id_error_;
  }
  int get_curr_score(double &score) const override
  {
    ++score_calls_;
    score = 1.0;
    return score_error_;
  }
  int get_next_doc_id_batch(const ObDocIdExt *&ids, int64_t &count) override
  {
    ++batch_calls_;
    ids = nullptr;
    count = 0;
    // Reset even at EOF and on failure. A premature refill invalidates every
    // borrowed pointer selected earlier in the same output batch.
    for (auto &id : batch_ids_) { id.reset(); }
    if (OB_SUCCESS != batch_error_) {
      return batch_error_;
    } else if (next_ == fail_next_at_) {
      return OB_TIMEOUT;
    } else if (next_ >= static_cast<int64_t>(ids_.size())) {
      return OB_ITER_END;
    } else if (empty_batch_) {
      return OB_SUCCESS;
    }
    while (count < static_cast<int64_t>(batch_ids_.size())
        && next_ < static_cast<int64_t>(ids_.size())) {
      const int ret = get_next_row();
      if (OB_SUCCESS != ret) { return ret; }
      if (OB_SUCCESS != batch_ids_[count].from_datum(current_)) {
        return OB_ERR_UNEXPECTED;
      }
      ++count;
    }
    ids = null_batch_ ? nullptr : batch_ids_.data();
    return end_with_rows_ && next_ == static_cast<int64_t>(ids_.size())
        ? OB_ITER_END : OB_SUCCESS;
  }
  void rewind()
  {
    next_ = 0;
    next_calls_ = id_calls_ = score_calls_ = 0;
    batch_calls_ = 0;
  }
  int64_t next_calls() const { return next_calls_; }
  int64_t score_calls() const { return score_calls_; }
  const ObDatum *current() const { return &current_; }
  const ObDocIdExt *batch_current() const { return batch_ids_.data(); }
  int64_t batch_calls() const { return batch_calls_; }
private:
  std::vector<std::string> ids_;
  bool numeric_;
  int64_t next_;
  int64_t next_calls_;
  mutable int64_t id_calls_;
  mutable int64_t score_calls_;
public:
  int64_t fail_next_at_;
  int id_error_;
  bool null_id_;
  int score_error_;
  int batch_error_;
  bool empty_batch_;
  bool null_batch_;
  bool end_with_rows_;
private:
  alignas(8) char buffer_[40];
  ObDatum current_;
  std::vector<ObDocIdExt> batch_ids_;
  int64_t batch_calls_;
};

class CountingMembershipCollector : public ObSRDaaTInnerProductRelevanceCollector
{
public:
  CountingMembershipCollector() : collect_calls_(0), result_calls_(0) {}
  int collect_one_dim(const int64_t dim, const double relevance) override
  {
    ++collect_calls_;
    return ObSRDaaTInnerProductRelevanceCollector::collect_one_dim(dim, relevance);
  }
  int get_result(double &relevance, bool &valid) override
  {
    ++result_calls_;
    return ObSRDaaTInnerProductRelevanceCollector::get_result(relevance, valid);
  }
  int64_t collect_calls_;
  int64_t result_calls_;
};

template <typename T>
int prepare_membership_slots(ObFixedArray<T, ObIAllocator> &slots,
                             ObIAllocator &allocator, const int64_t count)
{
  int ret = OB_SUCCESS;
  slots.set_allocator(&allocator);
  if (OB_FAIL(slots.init(count))) {
  } else if (OB_FAIL(slots.prepare_allocate(count))) {
  }
  return ret;
}

// Exercise the real base batch loop, heap merge and cache_result without a SQL
// executor. Only projection/filter/pre_process are replaced. Fake dimensions
// must not be installed in ObTextDaaTIter, whose reset casts to concrete tokens.
class MembershipMergeHarness : public ObSRDaaTIterImpl
{
public:
  MembershipMergeHarness(const bool fast, const bool numeric)
      : fast_(fast), numeric_(numeric) {}
  ~MembershipMergeHarness() override { reset(); }
  int prepare(ObIArray<ObISRDaaTDimIter *> &dims, ObIAllocator &allocator,
              const int64_t max_batch_size, const int64_t offset, const int64_t limit)
  {
    int ret = OB_SUCCESS;
    limit_.offset_ = offset;
    limit_.limit_ = limit;
    params_.limit_param_ = &limit_;
    params_.max_batch_size_ = max_batch_size;
    iter_allocator_ = &allocator;
    iter_param_ = &params_;
    dim_iters_ = &dims;
    relevance_collector_ = &collector_;
    if (OB_FAIL(collector_.init())) {
    } else if (OB_FAIL(prepare_membership_slots(iter_domain_ids_, allocator, dims.count()))) {
    } else if (OB_FAIL(prepare_membership_slots(next_round_iter_idxes_, allocator, dims.count()))) {
    } else if (OB_FAIL(prepare_membership_slots(buffered_domain_ids_, allocator, max_batch_size))) {
    } else if (OB_FAIL(prepare_membership_slots(buffered_relevances_, allocator, max_batch_size))) {
    } else if (OB_FAIL(merge_cmp_.init(membership_id_meta(numeric_), &iter_domain_ids_))) {
    } else if (OB_FAIL(init_merge_heap(dims.count()))) {
    } else if (fast_ && OB_FAIL(union_.init(dims, membership_id_meta(numeric_)))) {
    } else {
      for (int64_t i = 0; i < dims.count(); ++i) {
        iter_domain_ids_[i] = nullptr;
        next_round_iter_idxes_[i] = i;
      }
      next_round_cnt_ = dims.count();
      is_inited_ = true;
    }
    return ret;
  }
  void reuse(const bool switch_tablet = false) override
  {
    ObSRDaaTIterImpl::reuse(switch_tablet);
    union_.reuse();
    ids_.clear();
    batch_sizes_.clear();
    collector_.collect_calls_ = collector_.result_calls_ = 0;
  }
  const std::vector<std::string> &ids() const { return ids_; }
  const std::vector<int64_t> &batch_sizes() const { return batch_sizes_; }
  int64_t collector_calls() const
  { return collector_.collect_calls_ + collector_.result_calls_; }
protected:
  int pre_process() override { return OB_SUCCESS; }
  int filter_on_demand(const int64_t, const double, bool &project) override
  {
    project = true;
    return OB_SUCCESS;
  }
  int do_one_merge_round(int64_t &count) override
  {
    if (!fast_) {
      return ObSRDaaTIterImpl::do_one_merge_round(count);
    }
    const ObDatum *id = nullptr;
    int ret = union_.get_next_id(id);
    if (OB_SUCC(ret)) {
      ret = cache_result(count, *id, 1.0);
    }
    return ret;
  }
  int project_results(const int64_t count) override
  {
    // These copies happen after multiple source advances in the batch. They
    // expose cache_result retaining a borrowed ID instead of copying it.
    batch_sizes_.push_back(count);
    for (int64_t i = 0; i < count; ++i) {
      const ObDatum &id = buffered_domain_ids_[i].get_datum();
      ids_.push_back(numeric_ ? std::to_string(id.get_uint()) : std::string(id.ptr_, id.len_));
      EXPECT_GT(buffered_relevances_[i], 0.0);
      if (fast_) {
        EXPECT_DOUBLE_EQ(1.0, buffered_relevances_[i]);
      }
    }
    return OB_SUCCESS;
  }
private:
  bool fast_;
  bool numeric_;
  TextMembershipUnion union_;
  CountingMembershipCollector collector_;
  ObLimitParam limit_;
  ObSparseRetrievalMergeParam params_;
  std::vector<std::string> ids_;
  std::vector<int64_t> batch_sizes_;
};

using MembershipStreams = std::vector<std::vector<std::string>>;

std::vector<std::string> expected_membership_ids(const MembershipStreams &streams, const bool numeric,
                                                const int64_t offset, const int64_t limit)
{
  std::vector<std::string> ids;
  for (const auto &stream : streams) {
    ids.insert(ids.end(), stream.begin(), stream.end());
  }
  std::sort(ids.begin(), ids.end(), [numeric](const std::string &lhs, const std::string &rhs) {
    if (numeric) {
      return std::stoull(lhs) < std::stoull(rhs);
    }
    return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
        [](unsigned char l, unsigned char r) { return l < r; });
  });
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  if (limit >= 0) {
    const size_t begin = std::min(ids.size(), static_cast<size_t>(offset));
    const size_t end = std::min(ids.size(), begin + static_cast<size_t>(limit));
    ids = std::vector<std::string>(ids.begin() + begin, ids.begin() + end);
  }
  return ids;
}

void check_membership_merge(const MembershipStreams &streams, const bool numeric,
                            const int64_t batch_size, const int64_t capacity,
                            const int64_t offset = 0, const int64_t limit = -1)
{
  std::vector<std::string> heap_ids;
  std::vector<int64_t> heap_batches;
  std::vector<int> heap_returns;
  for (const bool fast : {false, true}) {
    ObArenaAllocator allocator;
    ObSEArray<ObISRDaaTDimIter *, 3> dims;
    std::vector<std::unique_ptr<MutableMembershipDim>> sources;
    for (const auto &stream : streams) {
      sources.emplace_back(new MutableMembershipDim(stream, numeric));
      // A hidden score read is a correctness failure in the membership path.
      sources.back()->score_error_ = fast ? OB_ERR_UNEXPECTED : OB_SUCCESS;
      ASSERT_EQ(OB_SUCCESS, dims.push_back(sources.back().get()));
    }
    MembershipMergeHarness merge(fast, numeric);
    ASSERT_EQ(OB_SUCCESS, merge.prepare(dims, allocator, batch_size, offset, limit));
    for (int pass = 0; pass < 2; ++pass) {
      SCOPED_TRACE(::testing::Message() << fast << ", pass=" << pass);
      std::vector<int> returns;
      bool ended = false;
      for (int batch = 0; batch < 256 && !ended; ++batch) {
        int64_t count = 0;
        const int ret = merge.get_next_rows(capacity, count);
        ASSERT_TRUE(OB_SUCCESS == ret || OB_ITER_END == ret) << ret;
        EXPECT_LE(count, std::min(batch_size, capacity));
        returns.push_back(ret);
        ended = OB_ITER_END == ret;
      }
      ASSERT_TRUE(ended);
      EXPECT_EQ(expected_membership_ids(streams, numeric, offset, limit), merge.ids());
      if (!fast) {
        if (!merge.ids().empty()) {
          EXPECT_GT(merge.collector_calls(), 0);
        }
        heap_ids = merge.ids();
        heap_batches = merge.batch_sizes();
        heap_returns = returns;
      } else {
        EXPECT_EQ(heap_ids, merge.ids());
        EXPECT_EQ(heap_batches, merge.batch_sizes());
        EXPECT_EQ(heap_returns, returns);
        EXPECT_EQ(0, merge.collector_calls());
        for (const auto &source : sources) {
          EXPECT_EQ(0, source->score_calls());
        }
      }
      for (int repeat = 0; repeat < 2; ++repeat) {
        int64_t count = 0;
        EXPECT_EQ(OB_ITER_END, merge.get_next_rows(capacity, count));
        EXPECT_EQ(0, count);
      }
      for (const auto &source : sources) { source->rewind(); }
      merge.reuse();
    }
  }
}

TEST(FtsTextMembershipUnion, complete_ids_match_heap_with_one_two_and_three_dimensions)
{
  const std::vector<MembershipStreams> cases = {
      {{}}, {{"1", "3", "9"}}, {{}, {}}, {{"1", "3"}, {}},
      {{"1", "3"}, {"2", "4"}}, {{"1", "3", "9"}, {"1", "7", "9"}},
      {{}, {}, {}}, {{}, {"2", "5"}, {"1", "5", "9"}},
      {{"1", "4", "9"}, {"2", "5", "9"}, {"3", "6", "9"}},
      {{"1", "5", "9"}, {"1", "5", "9"}, {"1", "5", "9"}}};
  for (const auto &streams : cases) {
    for (const bool numeric : {false, true}) {
      for (const int64_t batch_size : {1, 3, 7}) {
        SCOPED_TRACE(::testing::Message() << streams.size() << ", numeric=" << numeric
            << ", batch=" << batch_size);
        check_membership_merge(streams, numeric, batch_size, 11);
      }
    }
  }
}

TEST(FtsTextMembershipUnion, binary_prefixes_unsigned_ids_and_buffer_overwrite)
{
  const std::string nul("\0", 1);
  const std::string high("\x80", 1);
  check_membership_merge({{"", "a", "a ", "ab", high},
                         {nul, "a", std::string("a\0", 2), "abc"},
                         {"a", "a ", "ab", std::string("\xff", 1)}}, false, 7, 3);
  check_membership_merge({{"0", "10", "9223372036854775808"},
                         {"2", "10", "18446744073709551615"},
                         {"2", "7", "9223372036854775808"}}, true, 7, 7);
}

TEST(FtsTextMembershipUnion, batching_offset_limit_and_reuse_match_heap)
{
  MembershipStreams streams(3);
  for (uint64_t id = 1; id <= 43; ++id) {
    streams[id % 3].push_back(std::to_string(id));
    if (id % 4 == 0) { streams[(id + 1) % 3].push_back(std::to_string(id)); }
  }
  for (const int64_t batch_size : {1, 3, 7}) {
    for (const int64_t capacity : {1, 3, 11}) {
      check_membership_merge(streams, true, batch_size, capacity);
      check_membership_merge(streams, true, batch_size, capacity, 5, 11);
      check_membership_merge(streams, true, batch_size, capacity, 0, 0);
      check_membership_merge(streams, true, batch_size, capacity, 99, 3);
    }
  }
}

TEST(FtsTextMembershipUnion, init_reset_empty_eof_and_highest_equal_head)
{
  TextMembershipUnion merge;
  const ObDatum *id = nullptr;
  EXPECT_EQ(OB_NOT_INIT, merge.get_next_id(id));
  ObSEArray<ObISRDaaTDimIter *, 4> dims;
  EXPECT_EQ(OB_INVALID_ARGUMENT, merge.init(dims, membership_id_meta(false)));
  MutableMembershipDim empty({}, false);
  for (int i = 0; i < 4; ++i) { ASSERT_EQ(OB_SUCCESS, dims.push_back(&empty)); }
  EXPECT_EQ(OB_INVALID_ARGUMENT, merge.init(dims, membership_id_meta(false)));
  dims.reuse();
  ASSERT_EQ(OB_SUCCESS, dims.push_back(nullptr));
  EXPECT_EQ(OB_INVALID_ARGUMENT, merge.init(dims, membership_id_meta(false)));
  dims.at(0) = &empty;
  ASSERT_EQ(OB_SUCCESS, merge.init(dims, membership_id_meta(false)));
  EXPECT_EQ(OB_INIT_TWICE, merge.init(dims, membership_id_meta(false)));
  for (int repeat = 0; repeat < 3; ++repeat) {
    EXPECT_EQ(OB_ITER_END, merge.get_next_id(id));
    EXPECT_EQ(nullptr, id);
  }
  EXPECT_EQ(1, empty.next_calls());
  merge.reset();
  EXPECT_EQ(OB_NOT_INIT, merge.get_next_id(id));
  MutableMembershipDim first({"a", "c"}, false), last({"a", "b"}, false);
  dims.reuse();
  ASSERT_EQ(OB_SUCCESS, dims.push_back(&first));
  ASSERT_EQ(OB_SUCCESS, dims.push_back(&last));
  ASSERT_EQ(OB_SUCCESS, merge.init(dims, membership_id_meta(false)));
  ASSERT_EQ(OB_SUCCESS, merge.get_next_id(id));
  EXPECT_EQ(last.current(), id);
  EXPECT_EQ("a", std::string(id->ptr_, id->len_));
  // No source is advanced until the caller has had a chance to copy this ID.
  EXPECT_EQ(1, first.next_calls());
  EXPECT_EQ(1, last.next_calls());
  first.rewind();
  last.rewind();
  merge.reuse();
  ASSERT_EQ(OB_SUCCESS, merge.get_next_id(id));
  EXPECT_EQ(last.current(), id);
  EXPECT_EQ("a", std::string(id->ptr_, id->len_));
}

int fail_membership_compare(const ObDatum &, const ObDatum &, int &, const ObDatumAccessContext *)
{
  return OB_TIMEOUT;
}

TEST(FtsTextMembershipUnion, advance_id_and_comparison_errors_do_not_emit_a_partial_id)
{
  for (const bool after_first_row : {false, true}) {
    for (int fault = 0; fault < 4; ++fault) {
      SCOPED_TRACE(::testing::Message() << after_first_row << ", fault=" << fault);
      MutableMembershipDim first({"a", "b"}, false), second({"a", "c"}, false);
      ObSEArray<ObISRDaaTDimIter *, 3> dims;
      ASSERT_EQ(OB_SUCCESS, dims.push_back(&first));
      ASSERT_EQ(OB_SUCCESS, dims.push_back(&second));
      TextMembershipUnion merge;
      ASSERT_EQ(OB_SUCCESS, merge.init(dims, membership_id_meta(false)));
      const ObDatum *id = first.current();
      if (after_first_row) {
        ASSERT_EQ(OB_SUCCESS, merge.get_next_id(id));
        ASSERT_NE(nullptr, id);
      }
      if (fault == 0) { second.fail_next_at_ = after_first_row ? 1 : 0; }
      if (fault == 1) { second.id_error_ = OB_TIMEOUT; }
      if (fault == 2) { second.null_id_ = true; }
      if (fault == 3) { TextMembershipUnionTestPeer::set_compare(merge, fail_membership_compare); }
      EXPECT_EQ(fault == 2 ? OB_ERR_UNEXPECTED : OB_TIMEOUT, merge.get_next_id(id));
      EXPECT_EQ(nullptr, id);
      EXPECT_EQ(0, first.score_calls());
      EXPECT_EQ(0, second.score_calls());
    }
  }
}

void collect_membership_reference(const MembershipStreams &streams, const bool numeric,
    const int64_t capacity, const bool fast, std::vector<std::string> &result,
    const int64_t offset = 0, const int64_t limit = -1)
{
  ObArenaAllocator allocator;
  ObSEArray<ObISRDaaTDimIter *, 3> dims;
  std::vector<std::unique_ptr<MutableMembershipDim>> sources;
  for (const auto &stream : streams) {
    sources.emplace_back(new MutableMembershipDim(stream, numeric));
    ASSERT_EQ(OB_SUCCESS, dims.push_back(sources.back().get()));
  }
  MembershipMergeHarness merge(fast, numeric);
  ASSERT_EQ(OB_SUCCESS, merge.prepare(dims, allocator, capacity, offset, limit));
  bool ended = false;
  for (int64_t batch = 0; batch < 4096 && !ended; ++batch) {
    int64_t count = 0;
    const int ret = merge.get_next_rows(capacity, count);
    ASSERT_TRUE(OB_SUCCESS == ret || OB_ITER_END == ret) << ret;
    ended = OB_ITER_END == ret;
  }
  ASSERT_TRUE(ended);
  result = merge.ids();
}

void check_membership_batch(const MembershipStreams &streams, const bool numeric,
    const int64_t source_batch_size, const int64_t capacity,
    const std::vector<int64_t> &source_batch_sizes = {}, const uint8_t end_with_rows_mask = 0)
{
  std::vector<std::string> heap_ids;
  std::vector<std::string> row_union_ids;
  collect_membership_reference(streams, numeric, capacity, false, heap_ids);
  collect_membership_reference(streams, numeric, capacity, true, row_union_ids);
  ASSERT_EQ(heap_ids, row_union_ids);
  ASSERT_EQ(expected_membership_ids(streams, numeric, 0, -1), heap_ids);
  ObSEArray<ObISRDaaTDimIter *, 3> dims;
  std::vector<std::unique_ptr<MutableMembershipDim>> sources;
  for (size_t i = 0; i < streams.size(); ++i) {
    sources.emplace_back(new MutableMembershipDim(streams[i], numeric,
        source_batch_sizes.empty() ? source_batch_size : source_batch_sizes.at(i)));
    sources.back()->score_error_ = OB_ERR_UNEXPECTED;
    sources.back()->id_error_ = OB_ERR_UNEXPECTED;
    sources.back()->end_with_rows_ = 0 != (end_with_rows_mask & (1U << i));
    ASSERT_EQ(OB_SUCCESS, dims.push_back(sources.back().get()));
  }
  TextMembershipBatchUnion merge;
  ASSERT_EQ(OB_SUCCESS, merge.init(dims, membership_id_meta(numeric)));
  std::vector<const ObDocIdExt *> ids(capacity, nullptr);
  for (int pass = 0; pass < 2; ++pass) {
    SCOPED_TRACE(::testing::Message() << "source_batch=" << source_batch_size
        << ", capacity=" << capacity << ", pass=" << pass);
    std::vector<std::string> actual;
    bool ended = false;
    for (int64_t batch = 0; batch < 4096 && !ended; ++batch) {
      int64_t count = 999;
      const int ret = merge.get_next_ids(capacity, ids.data(), count);
      ASSERT_TRUE(OB_SUCCESS == ret || OB_ITER_END == ret) << ret;
      ASSERT_LE(count, capacity);
      ended = OB_ITER_END == ret;
      ASSERT_TRUE(ended || count > 0);
      for (int64_t i = 0; i < count; ++i) {
        ASSERT_NE(nullptr, ids[i]);
        // Use the same projection functions as the production batch path.
        // Decode only after the merge has advanced every selected source.
        char buffer[40] = {};
        ObDatum projected(buffer, 0, false);
        if (numeric) {
          ObISparseRetrievalMergeIter::set_datum_int(projected, *ids[i]);
          actual.push_back(std::to_string(projected.get_uint()));
        } else {
          ObISparseRetrievalMergeIter::set_datum_shallow(projected, *ids[i]);
          actual.emplace_back(projected.ptr_, static_cast<size_t>(projected.len_));
        }
      }
    }
    ASSERT_TRUE(ended);
    EXPECT_EQ(heap_ids, actual);
    std::vector<int64_t> calls;
    for (const auto &source : sources) {
      EXPECT_EQ(0, source->score_calls());
      calls.push_back(source->batch_calls());
    }
    for (int repeat = 0; repeat < 3; ++repeat) {
      int64_t count = 99;
      EXPECT_EQ(OB_ITER_END, merge.get_next_ids(capacity, ids.data(), count));
      EXPECT_EQ(0, count);
      for (size_t i = 0; i < sources.size(); ++i) {
        EXPECT_EQ(calls[i], sources[i]->batch_calls());
      }
    }
    for (const auto &source : sources) { source->rewind(); }
    merge.reuse();
  }
}

TEST(FtsTextMembershipBatchUnion, complete_ids_match_heap_and_row_union_across_buffer_boundaries)
{
  const std::vector<MembershipStreams> cases = {
      {{}}, {{"1", "3", "9"}}, {{}, {}}, {{"1", "3"}, {}},
      {{"1", "3"}, {"2", "4"}}, {{"1", "3", "9"}, {"1", "7", "9"}},
      {{}, {}, {}}, {{}, {"2", "5"}, {"1", "5", "9"}},
      {{"1", "4", "9"}, {"2", "5", "9"}, {"3", "6", "9"}},
      {{"1", "5", "9"}, {"1", "5", "9"}, {"1", "5", "9"}}};
  for (const auto &streams : cases) {
    for (const bool numeric : {false, true}) {
      for (const int64_t source_batch : {1, 3, 128}) {
        for (const int64_t capacity : {1, 4, 128}) {
          check_membership_batch(streams, numeric, source_batch, capacity);
        }
      }
    }
  }
  MembershipStreams interleaved(3), dense(3);
  for (uint64_t id = 0; id < 521; ++id) {
    interleaved[id % 3].push_back(std::to_string(id));
    if (id % 5 == 0) { interleaved[(id + 1) % 3].push_back(std::to_string(id)); }
    for (auto &stream : dense) { stream.push_back(std::to_string(id)); }
  }
  for (const int64_t source_batch : {1, 3, 128}) {
    for (const int64_t capacity : {1, 4, 128}) {
      check_membership_batch(interleaved, true, source_batch, capacity);
      check_membership_batch(dense, true, source_batch, capacity);
    }
  }
}

TEST(FtsTextMembershipBatchUnion, binary_ids_unsigned_extremes_and_partial_batches_keep_borrowed_bytes)
{
  const std::string nul("\0", 1), high("\x80", 1);
  check_membership_batch({{"", "a", "a ", "ab", high},
      {nul, "a", std::string("a\0", 2), "abc"},
      {"a", "a ", "ab", std::string("\xff", 1)}}, false, 3, 128);
  check_membership_batch({{"0", "10", "9223372036854775808"},
      {"2", "10", "18446744073709551615"},
      {"2", "7", "9223372036854775808"}}, true, 1, 128);
  MutableMembershipDim first({"a", "b", "c"}, false, 1);
  MutableMembershipDim second({"a", "d"}, false, 2);
  ObSEArray<ObISRDaaTDimIter *, 3> dims;
  ASSERT_EQ(OB_SUCCESS, dims.push_back(&first));
  ASSERT_EQ(OB_SUCCESS, dims.push_back(&second));
  TextMembershipBatchUnion merge;
  ASSERT_EQ(OB_SUCCESS, merge.init(dims, membership_id_meta(false)));
  const ObDocIdExt *ids[8] = {};
  int64_t count = 0;
  ASSERT_EQ(OB_SUCCESS, merge.get_next_ids(8, ids, count));
  ASSERT_EQ(1, count); // Source boundary yields a partial batch, not EOF.
  EXPECT_EQ(second.batch_current(), ids[0]); // Highest-index tie wins.
  EXPECT_EQ("a", std::string(ids[0]->get_datum().ptr_, ids[0]->get_datum().len_));
  EXPECT_EQ(1, first.batch_calls());
  EXPECT_EQ(1, second.batch_calls());
  // Even a zero-capacity call must leave the prior borrowed output intact.
  ASSERT_EQ(OB_SUCCESS, merge.get_next_ids(0, nullptr, count));
  EXPECT_EQ(0, count);
  EXPECT_EQ("a", std::string(ids[0]->get_datum().ptr_, ids[0]->get_datum().len_));
  ASSERT_EQ(OB_SUCCESS, merge.get_next_ids(8, ids, count));
  ASSERT_EQ(1, count);
  EXPECT_EQ("b", std::string(ids[0]->get_datum().ptr_, ids[0]->get_datum().len_));
}

TEST(FtsTextMembershipBatchUnion, generated_sorted_postings_match_reference_with_asymmetric_batches)
{
  std::mt19937_64 random(0x9e3779b97f4a7c15ULL);
  for (const bool numeric : {false, true}) {
    for (int trial = 0; trial < 12; ++trial) {
      MembershipStreams streams(2 + trial % 2);
      std::vector<std::string> pool;
      for (int i = 0; i < 80; ++i) {
        if (numeric) {
          pool.push_back(std::to_string(random()));
        } else {
          // Include empty IDs, embedded zero bytes, unsigned bytes and
          // arbitrary prefixes, rather than only fixed-width document IDs.
          std::string id(random() % 21, '\0');
          for (char &byte : id) { byte = static_cast<char>(random()); }
          pool.push_back(id);
        }
      }
      for (size_t dim = 0; dim < streams.size(); ++dim) {
        const int64_t density = 1 + (trial + dim) % 7;
        for (const std::string &id : pool) {
          if (random() % density == 0) { streams[dim].push_back(id); }
        }
        auto less = [numeric](const std::string &left, const std::string &right) {
          return numeric ? std::stoull(left) < std::stoull(right)
              : std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
                  [](unsigned char l, unsigned char r) { return l < r; });
        };
        std::sort(streams[dim].begin(), streams[dim].end(), less);
        streams[dim].erase(std::unique(streams[dim].begin(), streams[dim].end()), streams[dim].end());
      }
      for (const int64_t capacity : {1, 7, 67}) {
        SCOPED_TRACE(::testing::Message() << "numeric=" << numeric << ", trial=" << trial
            << ", capacity=" << capacity);
        const std::vector<int64_t> batches = streams.size() == 2
            ? std::vector<int64_t>{2, 17} : std::vector<int64_t>{2, 17, 128};
        check_membership_batch(streams, numeric, 1, capacity, batches,
            static_cast<uint8_t>(trial % (1U << streams.size())));
      }
    }
  }
}

TEST(FtsTextMembershipBatchUnion, all_three_tie_masks_choose_last_source_and_bulk_tail_keeps_bytes)
{
  MutableMembershipDim first({"a", "b", "c", "h"}, false, 16);
  MutableMembershipDim second({"a", "b", "d", "h"}, false, 16);
  MutableMembershipDim third({"a", "c", "d", "h", "i", "j", "k"}, false, 16);
  first.end_with_rows_ = second.end_with_rows_ = third.end_with_rows_ = true;
  ObSEArray<ObISRDaaTDimIter *, 3> dims;
  ASSERT_EQ(OB_SUCCESS, dims.push_back(&first));
  ASSERT_EQ(OB_SUCCESS, dims.push_back(&second));
  ASSERT_EQ(OB_SUCCESS, dims.push_back(&third));
  TextMembershipBatchUnion merge;
  ASSERT_EQ(OB_SUCCESS, merge.init(dims, membership_id_meta(false)));
  const ObDocIdExt *ids[16] = {};
  int64_t count = 0;
  ASSERT_EQ(OB_SUCCESS, merge.get_next_ids(16, ids, count));
  ASSERT_EQ(8, count);
  const ObDocIdExt *expected[] = {third.batch_current(), second.batch_current() + 1,
      third.batch_current() + 1, third.batch_current() + 2, third.batch_current() + 3,
      third.batch_current() + 4, third.batch_current() + 5, third.batch_current() + 6};
  for (int64_t i = 0; i < count; ++i) { EXPECT_EQ(expected[i], ids[i]); }
  const std::vector<std::string> values = {"a", "b", "c", "d", "h", "i", "j", "k"};
  ASSERT_EQ(OB_SUCCESS, merge.get_next_ids(0, nullptr, count));
  for (size_t i = 0; i < values.size(); ++i) {
    EXPECT_EQ(values[i], std::string(ids[i]->get_datum().ptr_, ids[i]->get_datum().len_));
  }
  EXPECT_EQ(1, first.batch_calls());
  EXPECT_EQ(1, second.batch_calls());
  EXPECT_EQ(1, third.batch_calls());
  EXPECT_EQ(OB_ITER_END, merge.get_next_ids(16, ids, count));
  EXPECT_EQ(0, count);
}

int64_t membership_batch_compare_calls = 0;
int fail_later_membership_batch_compare(
    const ObDatum &lhs, const ObDatum &rhs, int &cmp, const ObDatumAccessContext *ctx)
{
  return ++membership_batch_compare_calls == 2
      ? OB_TIMEOUT : compare_binary_domain_ids(lhs, rhs, cmp, ctx);
}

int fail_after_six_membership_batch_compares(
    const ObDatum &lhs, const ObDatum &rhs, int &cmp, const ObDatumAccessContext *ctx)
{
  return ++membership_batch_compare_calls == 6
      ? OB_TIMEOUT : compare_binary_domain_ids(lhs, rhs, cmp, ctx);
}

TEST(FtsTextMembershipBatchUnion, fixed_two_and_three_way_comparison_errors_publish_no_rows)
{
  for (const bool three_way : {false, true}) {
    MutableMembershipDim first({"a", "d", "g", "j"}, false, 16);
    MutableMembershipDim second({"b", "e", "h", "k"}, false, 16);
    MutableMembershipDim third({"c", "f", "i", "l"}, false, 16);
    ObSEArray<ObISRDaaTDimIter *, 3> dims;
    ASSERT_EQ(OB_SUCCESS, dims.push_back(&first));
    ASSERT_EQ(OB_SUCCESS, dims.push_back(&second));
    if (three_way) { ASSERT_EQ(OB_SUCCESS, dims.push_back(&third)); }
    TextMembershipBatchUnion merge;
    ASSERT_EQ(OB_SUCCESS, merge.init(dims, membership_id_meta(false)));
    TextMembershipUnionTestPeer::set_compare(merge, fail_after_six_membership_batch_compares);
    membership_batch_compare_calls = 0;
    const ObDocIdExt *ids[16] = {};
    int64_t count = 99;
    EXPECT_EQ(OB_TIMEOUT, merge.get_next_ids(16, ids, count));
    EXPECT_EQ(0, count);
    EXPECT_EQ(6, membership_batch_compare_calls);
    EXPECT_EQ(1, first.batch_calls());
    EXPECT_EQ(1, second.batch_calls());
    EXPECT_EQ(three_way ? 1 : 0, third.batch_calls());
  }
}

TEST(FtsTextMembershipBatchUnion, initialization_eof_with_rows_and_errors)
{
  TextMembershipBatchUnion merge;
  const ObDocIdExt *ids[8] = {};
  int64_t count = 99;
  EXPECT_EQ(OB_NOT_INIT, merge.get_next_ids(8, ids, count));
  EXPECT_EQ(0, count);
  ObSEArray<ObISRDaaTDimIter *, 4> dims;
  EXPECT_EQ(OB_INVALID_ARGUMENT, merge.init(dims, membership_id_meta(false)));
  MutableMembershipDim first({"a", "b"}, false, 2), second({"a", "c"}, false, 2);
  for (int i = 0; i < 4; ++i) { ASSERT_EQ(OB_SUCCESS, dims.push_back(&first)); }
  EXPECT_EQ(OB_INVALID_ARGUMENT, merge.init(dims, membership_id_meta(false)));
  dims.reuse();
  ASSERT_EQ(OB_SUCCESS, dims.push_back(nullptr));
  EXPECT_EQ(OB_INVALID_ARGUMENT, merge.init(dims, membership_id_meta(false)));
  dims.at(0) = &first;
  first.end_with_rows_ = true;
  ASSERT_EQ(OB_SUCCESS, merge.init(dims, membership_id_meta(false)));
  EXPECT_EQ(OB_INIT_TWICE, merge.init(dims, membership_id_meta(false)));
  EXPECT_EQ(OB_INVALID_ARGUMENT, merge.get_next_ids(-1, ids, count));
  EXPECT_EQ(OB_INVALID_ARGUMENT, merge.get_next_ids(1, nullptr, count));
  ASSERT_EQ(OB_SUCCESS, merge.get_next_ids(8, ids, count));
  ASSERT_EQ(2, count);
  EXPECT_EQ("a", std::string(ids[0]->get_datum().ptr_, ids[0]->get_datum().len_));
  EXPECT_EQ("b", std::string(ids[1]->get_datum().ptr_, ids[1]->get_datum().len_));
  ASSERT_EQ(OB_ITER_END, merge.get_next_ids(8, ids, count));
  EXPECT_EQ(1, first.batch_calls());
  merge.reset();
  EXPECT_EQ(OB_NOT_INIT, merge.get_next_ids(8, ids, count));
  dims.reuse();
  ASSERT_EQ(OB_SUCCESS, dims.push_back(&first));
  ASSERT_EQ(OB_SUCCESS, dims.push_back(&second));
  for (int fault = 0; fault < 4; ++fault) {
    SCOPED_TRACE(fault);
    first.rewind();
    second.rewind();
    merge.reset();
    ASSERT_EQ(OB_SUCCESS, merge.init(dims, membership_id_meta(false)));
    second.batch_error_ = fault == 0 ? OB_TIMEOUT : OB_SUCCESS;
    second.empty_batch_ = fault == 1;
    second.null_batch_ = fault == 2;
    if (fault == 3) {
      membership_batch_compare_calls = 0;
      TextMembershipUnionTestPeer::set_compare(merge, fail_later_membership_batch_compare);
    }
    EXPECT_EQ(fault == 0 || fault == 3 ? OB_TIMEOUT : OB_ERR_UNEXPECTED,
        merge.get_next_ids(8, ids, count));
    EXPECT_EQ(0, count); // A comparison error after a selected row publishes none.
    EXPECT_EQ(0, first.score_calls());
    EXPECT_EQ(0, second.score_calls());
  }
}

int reference_fixed16_membership_compare(
    const ObDatum &lhs, const ObDatum &rhs, int &cmp, const ObDatumAccessContext *ctx)
{
  return ObDatumFuncs::get_basic_func(ObVarcharType, CS_TYPE_BINARY)->null_first_cmp_(lhs, rhs, cmp, ctx);
}

class Fixed16ProofSource : public ObISRDaaTDimIter
{
public:
  Fixed16ProofSource(const std::vector<ObDocIdExt> &values, const int64_t batch_size,
      const bool publish_proof, const bool end_with_rows)
      : fail_at_(-1), values_(values), batch_(batch_size), next_(0), calls_(0), publish_proof_(publish_proof),
        end_with_rows_(end_with_rows), fixed16_(false) {}
  int get_next_row() override { return OB_ERR_UNEXPECTED; }
  int get_curr_id(const ObDatum *&) const override { return OB_ERR_UNEXPECTED; }
  int get_curr_score(double &) const override { return OB_ERR_UNEXPECTED; }
  bool is_fixed16_doc_id_batch() const override { return fixed16_; }
  bool owns_current(const ObDocIdExt *id) const
  {
    for (const ObDocIdExt &entry : batch_) { if (&entry == id) { return true; } }
    return false;
  }
  int get_next_doc_id_batch(const ObDocIdExt *&ids, int64_t &count) override
  {
    fixed16_ = false;
    ids = nullptr;
    count = 0;
    // Invalidate the old borrowed payload even on an error or empty EOF.
    for (ObDocIdExt &id : batch_) { id.reset(); }
    if (++calls_ == fail_at_) { return OB_TIMEOUT; }
    bool fixed16 = true;
    while (count < static_cast<int64_t>(batch_.size())
        && next_ < static_cast<int64_t>(values_.size())) {
      batch_[count] = values_[next_++];
      fixed16 &= batch_[count].get_datum().pack_ == 16;
      ++count;
    }
    fixed16_ = publish_proof_ && count > 0 && fixed16;
    ids = count > 0 ? batch_.data() : nullptr;
    return next_ == static_cast<int64_t>(values_.size()) && (end_with_rows_ || count == 0)
        ? OB_ITER_END : OB_SUCCESS;
  }
  void rewind()
  {
    next_ = calls_ = 0;
    fixed16_ = false;
    for (ObDocIdExt &id : batch_) { id.reset(); }
  }
  int64_t fail_at_;
private:
  std::vector<ObDocIdExt> values_;
  std::vector<ObDocIdExt> batch_;
  int64_t next_;
  int64_t calls_;
  bool publish_proof_;
  bool end_with_rows_;
  bool fixed16_;
};

ObDocIdExt fixed16_test_value(const std::string &bytes,
    const ObDatumDesc::FlagType flag = ObDatumDesc::NONE, const bool null = false)
{
  ObDatum datum;
  datum.set_string(bytes.data(), bytes.size());
  if (null) { datum.set_null(); }
  else { datum.set_flag(flag); }
  ObDocIdExt value;
  EXPECT_EQ(OB_SUCCESS, value.from_datum(datum));
  return value;
}

TEST(FtsTextMembershipBatchUnion, fixed16_proven_and_fallback_spans_match_registered_comparison)
{
  const auto compare = [](const ObDocIdExt &l, const ObDocIdExt &r) {
    int result = 0;
    EXPECT_EQ(OB_SUCCESS,
        reference_fixed16_membership_compare(l.get_datum(), r.get_datum(), result, nullptr));
    return result;
  };
  std::vector<ObDocIdExt> fixed;
  for (int position = 0; position < 16; ++position) {
    for (const int byte : {0, 1, 127, 128, 255}) {
      std::string bytes(16, '\0');
      bytes[position] = static_cast<char>(byte);
      bytes[(position + 1) % 16] = static_cast<char>(255 - byte);
      fixed.push_back(fixed16_test_value(bytes));
    }
  }
  for (const bool mixed : {false, true}) {
    for (const int dims_count : {2, 3}) {
      std::vector<std::vector<ObDocIdExt>> values(dims_count);
      for (size_t i = 0; i < fixed.size(); ++i) {
        for (int dim = 0; dim < dims_count; ++dim) {
          if ((i + dim) % 3 != 0 || i % 7 == 0) { values[dim].push_back(fixed[i]); }
        }
      }
      if (mixed) {
        for (int dim = 0; dim < dims_count; ++dim) {
          values[dim].push_back(fixed16_test_value("", ObDatumDesc::NONE, true));
          for (const int width : {0, 1, 8, 15, 17, 40}) {
            values[dim].push_back(fixed16_test_value(std::string(width, static_cast<char>(0xe0 + dim))));
          }
          values[dim].push_back(fixed16_test_value(std::string(16, static_cast<char>(0xe4)), ObDatumDesc::EXT));
          values[dim].push_back(fixed16_test_value(std::string(16, static_cast<char>(0xe5)), ObDatumDesc::OUTROW));
          values[dim].push_back(fixed16_test_value(std::string(16, static_cast<char>(0xe6)), ObDatumDesc::HAS_LOB_HEADER));
        }
      }
      for (auto &stream : values) {
        std::sort(stream.begin(), stream.end(), [&](const ObDocIdExt &l, const ObDocIdExt &r) {
          return compare(l, r) < 0;
        });
        stream.erase(std::unique(stream.begin(), stream.end(), [&](const ObDocIdExt &l, const ObDocIdExt &r) {
          return compare(l, r) == 0;
        }), stream.end());
      }
      // The reference sorts all postings with the registered collation function,
      // then independently keeps the highest source for each equivalent ID.
      std::vector<std::pair<ObDocIdExt, int>> all;
      for (int dim = 0; dim < dims_count; ++dim) {
        for (const ObDocIdExt &id : values[dim]) { all.emplace_back(id, dim); }
      }
      std::stable_sort(all.begin(), all.end(), [&](const auto &l, const auto &r) {
        const int order = compare(l.first, r.first);
        return order < 0 || (order == 0 && l.second < r.second);
      });
      std::vector<ObDocIdExt> expected;
      for (const auto &posting : all) {
        if (expected.empty() || compare(expected.back(), posting.first) != 0) { expected.push_back(posting.first); }
        else { expected.back() = posting.first; }
      }
      for (const int64_t batch_size : {1, 3, 128}) {
        for (const int64_t capacity : {1, 7, 128}) {
          for (const bool end_with_rows : {false, true}) {
            // Mode zero never proves a span; mode two proves it but deliberately
            // selects the generic registered comparator rather than binary dispatch.
            for (int mode = 0; mode < 3; ++mode) {
              SCOPED_TRACE(::testing::Message() << mixed << ", " << dims_count << ", "
                  << batch_size << ", " << capacity << ", " << end_with_rows << ", " << mode);
              std::vector<std::unique_ptr<Fixed16ProofSource>> sources;
              ObSEArray<ObISRDaaTDimIter *, 3> dims;
              for (int dim = 0; dim < dims_count; ++dim) {
                const int64_t size = batch_size == 3 ? (dim == 0 ? 2 : (dim == 1 ? 5 : 3)) : batch_size;
                sources.emplace_back(new Fixed16ProofSource(values[dim], size, mode != 0, end_with_rows));
                ASSERT_EQ(OB_SUCCESS, dims.push_back(sources.back().get()));
              }
              TextMembershipBatchUnion merge;
              ASSERT_EQ(OB_SUCCESS, merge.init(dims, membership_id_meta(false)));
              if (mode == 2) { TextMembershipUnionTestPeer::set_compare(merge, reference_fixed16_membership_compare); }
              std::vector<const ObDocIdExt *> ids(capacity);
              for (int pass = 0; pass < 2; ++pass) {
                size_t next = 0;
                bool ended = false;
                for (int call = 0; call < 1024 && !ended; ++call) {
                  int64_t count = 99;
                  const int ret = merge.get_next_ids(capacity, ids.data(), count);
                  ASSERT_TRUE(ret == OB_SUCCESS || ret == OB_ITER_END) << ret;
                  ASSERT_LE(count, capacity);
                  ended = ret == OB_ITER_END;
                  ASSERT_TRUE(ended || count > 0);
                  for (int64_t i = 0; i < count; ++i) {
                    ASSERT_LT(next, expected.size());
                    const ObDatum &actual = ids[i]->get_datum();
                    const ObDatum &reference = expected[next++].get_datum();
                    EXPECT_EQ(reference.pack_, actual.pack_);
                    if (!reference.is_null() && reference.len_ > 0) {
                      EXPECT_EQ(std::string(reference.ptr_, reference.len_), std::string(actual.ptr_, actual.len_));
                    }
                  }
                }
                EXPECT_TRUE(ended);
                EXPECT_EQ(expected.size(), next);
                for (auto &source : sources) { source->rewind(); }
                merge.reuse();
              }
            }
          }
        }
      }
    }
  }
}

TEST(FtsTextMembershipBatchUnion, fixed16_proof_is_cleared_before_refill_errors)
{
  const std::vector<ObDocIdExt> values = {
      fixed16_test_value(std::string(16, 'a')), fixed16_test_value(std::string(16, 'b'))};
  Fixed16ProofSource first(values, 1, true, false), second(values, 1, true, false);
  ObSEArray<ObISRDaaTDimIter *, 3> dims;
  ASSERT_EQ(OB_SUCCESS, dims.push_back(&first));
  ASSERT_EQ(OB_SUCCESS, dims.push_back(&second));
  TextMembershipBatchUnion merge;
  ASSERT_EQ(OB_SUCCESS, merge.init(dims, membership_id_meta(false)));
  const ObDocIdExt *ids[8] = {};
  int64_t count = 0;
  ASSERT_EQ(OB_SUCCESS, merge.get_next_ids(8, ids, count));
  ASSERT_EQ(1, count);
  EXPECT_TRUE(first.is_fixed16_doc_id_batch());
  EXPECT_TRUE(second.is_fixed16_doc_id_batch());
  first.fail_at_ = 2;
  EXPECT_EQ(OB_TIMEOUT, merge.get_next_ids(8, ids, count));
  EXPECT_EQ(0, count);
  EXPECT_FALSE(first.is_fixed16_doc_id_batch());
  first.fail_at_ = -1;
  first.rewind();
  second.rewind();
  merge.reuse();
  EXPECT_FALSE(first.is_fixed16_doc_id_batch());
  ASSERT_EQ(OB_SUCCESS, merge.get_next_ids(8, ids, count));
  ASSERT_EQ(1, count);
  EXPECT_EQ(std::string(16, 'a'), std::string(ids[0]->get_datum().ptr_, ids[0]->get_datum().len_));
}

// from_obj() deliberately borrows the final batch slot's payload from values_.
// The inherited default proof must remain false despite every pack_ being 16.
class BorrowedFixed16DefaultSource : public ObISRDaaTDimIter
{
public:
  BorrowedFixed16DefaultSource(const std::vector<ObDocIdExt> &values,
      const int64_t batch_size, const bool end_with_rows)
      : values_(values), batch_(batch_size), next_(0), batch_start_(0), batch_count_(0),
        end_with_rows_(end_with_rows) {}
  int get_next_row() override { return OB_ERR_UNEXPECTED; }
  int get_curr_id(const ObDatum *&) const override { return OB_ERR_UNEXPECTED; }
  int get_curr_score(double &) const override { return OB_ERR_UNEXPECTED; }
  int get_next_doc_id_batch(const ObDocIdExt *&ids, int64_t &count) override
  {
    ids = nullptr;
    count = batch_count_ = 0;
    batch_start_ = next_;
    for (ObDocIdExt &id : batch_) { id.reset(); }
    while (count < static_cast<int64_t>(batch_.size())
        && next_ < static_cast<int64_t>(values_.size())) {
      const ObDatum &datum = values_[next_++].get_datum();
      ObObj obj;
      obj.set_varchar(datum.ptr_, datum.len_);
      const int ret = batch_[count].from_obj(obj);
      if (OB_SUCCESS != ret) { return ret; }
      ++count;
    }
    batch_count_ = count;
    ids = count > 0 ? batch_.data() : nullptr;
    return next_ == static_cast<int64_t>(values_.size()) && (end_with_rows_ || count == 0)
        ? OB_ITER_END : OB_SUCCESS;
  }
  bool owns_current(const ObDocIdExt *id) const
  {
    for (int64_t i = 0; i < batch_count_; ++i) { if (&batch_[i] == id) { return true; } }
    return false;
  }
  bool borrows_value_payload(const ObDocIdExt *id) const
  {
    for (int64_t i = 0; i < batch_count_; ++i) {
      if (&batch_[i] == id) {
        return 16 == id->get_datum().pack_
            && id->get_datum().ptr_ == values_[batch_start_ + i].get_datum().ptr_;
      }
    }
    return false;
  }
  void rewind()
  {
    next_ = batch_start_ = batch_count_ = 0;
    for (ObDocIdExt &id : batch_) { id.reset(); }
  }
private:
  std::vector<ObDocIdExt> values_;
  std::vector<ObDocIdExt> batch_;
  int64_t next_;
  int64_t batch_start_;
  int64_t batch_count_;
  bool end_with_rows_;
};

class BorrowedFixed16FalseSource final : public BorrowedFixed16DefaultSource
{
public:
  using BorrowedFixed16DefaultSource::BorrowedFixed16DefaultSource;
  bool is_fixed16_doc_id_batch() const override { return false; }
};


void check_prefix_membership_values(std::vector<std::vector<ObDocIdExt>> values,
    const ObCollationType collation, const std::vector<int64_t> &batch_sizes,
    const int64_t capacity, const int mode, const bool end_with_rows,
    const int borrowed_dimension = -1, const bool explicit_false = false)
{
  const ObDatumCmpFuncType registered = ObDatumFuncs::get_basic_func(ObVarcharType, collation)->null_first_cmp_;
  const auto compare = [&](const ObDocIdExt &l, const ObDocIdExt &r) {
    int result = 0;
    EXPECT_EQ(OB_SUCCESS, registered(l.get_datum(), r.get_datum(), result, nullptr));
    return result;
  };
  for (auto &stream : values) {
    std::stable_sort(stream.begin(), stream.end(), [&](const ObDocIdExt &l, const ObDocIdExt &r) {
      return compare(l, r) < 0;
    });
    stream.erase(std::unique(stream.begin(), stream.end(), [&](const ObDocIdExt &l, const ObDocIdExt &r) {
      return compare(l, r) == 0;
    }), stream.end());
  }
  std::vector<std::pair<ObDocIdExt, int>> all;
  for (size_t dim = 0; dim < values.size(); ++dim) {
    for (const ObDocIdExt &id : values[dim]) { all.emplace_back(id, dim); }
  }
  std::stable_sort(all.begin(), all.end(), [&](const auto &l, const auto &r) {
    const int order = compare(l.first, r.first);
    return order < 0 || (order == 0 && l.second < r.second);
  });
  std::vector<std::pair<ObDocIdExt, int>> expected;
  for (const auto &posting : all) {
    if (expected.empty() || compare(expected.back().first, posting.first) != 0) { expected.push_back(posting); }
    else { expected.back() = posting; }
  }
  std::vector<std::unique_ptr<Fixed16ProofSource>> sources(values.size());
  std::unique_ptr<BorrowedFixed16DefaultSource> borrowed;
  ObSEArray<ObISRDaaTDimIter *, 3> dims;
  for (size_t dim = 0; dim < values.size(); ++dim) {
    if (static_cast<int>(dim) == borrowed_dimension) {
      if (explicit_false) {
        borrowed.reset(new BorrowedFixed16FalseSource(values[dim], batch_sizes[dim], end_with_rows));
      } else {
        borrowed.reset(new BorrowedFixed16DefaultSource(values[dim], batch_sizes[dim], end_with_rows));
      }
      ASSERT_FALSE(borrowed->is_fixed16_doc_id_batch());
      ASSERT_EQ(OB_SUCCESS, dims.push_back(borrowed.get()));
    } else {
      sources[dim].reset(new Fixed16ProofSource(values[dim], batch_sizes[dim], mode != 0, end_with_rows));
      ASSERT_EQ(OB_SUCCESS, dims.push_back(sources[dim].get()));
    }
  }
  sql::ObDatumMeta meta = membership_id_meta(false);
  meta.cs_type_ = collation;
  TextMembershipBatchUnion merge;
  ASSERT_EQ(OB_SUCCESS, merge.init(dims, meta));
  if (mode == 2) { TextMembershipUnionTestPeer::set_compare(merge, registered); }
  std::vector<const ObDocIdExt *> ids(capacity);
  for (int pass = 0; pass < 2; ++pass) {
    size_t next = 0;
    bool ended = false;
    for (int call = 0; call < 1024 && !ended; ++call) {
      int64_t count = 99;
      const int ret = merge.get_next_ids(capacity, ids.data(), count);
      ASSERT_TRUE(ret == OB_SUCCESS || ret == OB_ITER_END) << ret;
      ASSERT_LE(count, capacity);
      ended = ret == OB_ITER_END;
      ASSERT_TRUE(ended || count > 0);
      for (int64_t i = 0; i < count; ++i) {
        ASSERT_LT(next, expected.size());
        const auto &reference = expected[next++];
        const ObDatum &datum = reference.first.get_datum();
        const ObDatum &actual = ids[i]->get_datum();
        if (reference.second == borrowed_dimension) {
          EXPECT_TRUE(borrowed->owns_current(ids[i]));
          EXPECT_TRUE(borrowed->borrows_value_payload(ids[i]));
          EXPECT_FALSE(borrowed->is_fixed16_doc_id_batch());
        } else {
          EXPECT_TRUE(sources[reference.second]->owns_current(ids[i]));
        }
        EXPECT_EQ(datum.pack_, actual.pack_);
        if (!datum.is_null() && datum.len_ > 0) {
          EXPECT_EQ(std::string(datum.ptr_, datum.len_), std::string(actual.ptr_, actual.len_));
        }
      }
    }
    EXPECT_TRUE(ended);
    EXPECT_EQ(expected.size(), next);
    for (auto &source : sources) { if (source) { source->rewind(); } }
    if (borrowed) { borrowed->rewind(); }
    merge.reuse();
  }
}

TEST(FtsTextMembershipBatchUnion, shared_prefix8_keeps_complete_ids_and_dynamic_fallback)
{
  using Streams = std::vector<std::vector<ObDocIdExt>>;
  std::string p0(8, '\0');
  p0[1] = static_cast<char>(0x80);
  p0[6] = static_cast<char>(0xff);
  std::string p1 = p0, p2 = p0;
  p1[0] = '\1';
  p2[0] = '\2';
  const auto value = [](const std::string &prefix, const int suffix) {
    std::string tail(8, '\0');
    tail[0] = static_cast<char>(suffix);
    tail[7] = static_cast<char>(255 - suffix);
    return fixed16_test_value(prefix + tail);
  };
  std::vector<Streams> cases;
  // Positive proof: a data-derived prefix containing NUL/high bytes, with
  // differences at every suffix byte and complete sixteen-byte output.
  Streams same(3);
  int item = 0;
  for (int position = 0; position < 8; ++position) {
    for (const int byte : {0, 1, 127, 128, 255}) {
      std::string tail(8, '\0');
      tail[position] = static_cast<char>(byte);
      tail[(position + 1) % 8] = static_cast<char>(255 - byte);
      const ObDocIdExt id = fixed16_test_value(p0 + tail);
      for (int dim = 0; dim < 3; ++dim) {
        if ((item + dim) % 3 != 0 || item % 5 == 0) { same[dim].push_back(id); }
      }
      ++item;
    }
  }
  cases.push_back(same);
  // Every source has a constant local prefix, but the prefixes differ across
  // sources. Comparing suffixes alone would reverse the full binary ordering.
  cases.push_back({{value(p0, 255)}, {value(p1, 0)}, {value(p0, 128)}});
  // The heads agree but each remaining span crosses a prefix boundary.
  cases.push_back({{value(p0, 0), value(p1, 0)},
      {value(p0, 128), value(p1, 255)}, {value(p0, 255), value(p2, 1)}});
  // Each source changes its high eight bytes at a batch boundary. Capacities
  // and asymmetric source spans move those boundaries independently.
  cases.push_back({{value(p0, 1), value(p0, 10), value(p1, 1), value(p1, 10), value(p2, 1)},
      {value(p0, 2), value(p0, 10), value(p1, 2), value(p1, 10), value(p2, 2)},
      {value(p0, 3), value(p0, 10), value(p1, 3), value(p1, 10), value(p2, 3)}});
  cases.push_back({{value(p0, 1), value(p1, 1), value(p1, 2)},
      {value(p0, 2), value(p1, 1), value(p2, 0)}});
  // Shape failures must retain the fixed16/generic fallback even when the
  // other active source contains a constant-prefix sixteen-byte span.
  cases.push_back({{fixed16_test_value("", ObDatumDesc::NONE, true), value(p0, 10), value(p0, 50)},
      {fixed16_test_value(p0), value(p0, 20), fixed16_test_value(p0 + std::string(9, 'x'))},
      {value(p0, 30), fixed16_test_value(p0 + std::string(8, 'y'), ObDatumDesc::EXT),
          fixed16_test_value(p1 + std::string(8, 'z'), ObDatumDesc::OUTROW)}});
  for (size_t test_case = 0; test_case < cases.size(); ++test_case) {
    for (const int64_t capacity : {1, 2, 7, 128}) {
      for (const std::vector<int64_t> &sizes : {
          std::vector<int64_t>{1, 1, 1}, std::vector<int64_t>{2, 3, 5}, std::vector<int64_t>{128, 128, 128}}) {
        for (int mode = 0; mode < 3; ++mode) {
          SCOPED_TRACE(::testing::Message() << test_case << ", " << capacity << ", " << sizes[0] << ", " << mode);
          check_prefix_membership_values(cases[test_case], CS_TYPE_BINARY, sizes, capacity, mode, sizes[0] != 1);
        }
      }
    }
  }
  // A linguistic collation can consider different byte prefixes equivalent.
  // Its ordering and highest-source ID bytes must remain fully generic.
  const Streams linguistic = {
      {value("AbCdEfGh", 'a'), value("abcdefgh", 'z')},
      {value("aBcDeFgH", 'A'), value("ABCDEFGH", 'm')},
      {value("abcdefgh", 'a'), value("AbCdEfGh", 'Z')}};
  for (const int64_t capacity : {1, 2, 128}) {
    check_prefix_membership_values(linguistic, CS_TYPE_UTF8MB4_GENERAL_CI, {2, 3, 2}, capacity, 1, true);
  }
}

TEST(FtsTextMembershipBatchUnion, three_way_shared_prefix_scalar_keys_match_full_binary_reference)
{
  using Streams = std::vector<std::vector<ObDocIdExt>>;
  std::string prefix(8, '\0');
  prefix[1] = static_cast<char>(0x80);
  prefix[6] = static_cast<char>(0xff);
  std::string next_prefix = prefix;
  next_prefix[0] = '\1';
  const auto value = [](const std::string &head, const uint64_t key) {
    std::string bytes = head;
    // Encode independently, one byte at a time, without the production endian
    // conversion. The registered full binary comparator remains the oracle.
    for (int shift = 56; shift >= 0; shift -= 8) {
      bytes.push_back(static_cast<char>((key >> shift) & 0xff));
    }
    return fixed16_test_value(bytes);
  };
  const std::vector<uint64_t> keys = {
      0ULL, 1ULL, 0x7fULL, 0x80ULL, 0xffULL, 0x100ULL, 0x101ULL,
      0x10000ULL, 0x1000000ULL, 0x100000000ULL, 0x10000000000ULL,
      0x1000000000000ULL, 0x100000000000000ULL, 0x7fffffffffffffffULL,
      0x8000000000000000ULL, 0x8000000000000001ULL,
      0xfffffffffffffffeULL, 0xffffffffffffffffULL};
  Streams interleaved(3), dense(3);
  for (size_t i = 0; i < keys.size(); ++i) {
    const ObDocIdExt id = value(prefix, keys[i]);
    interleaved[i % 3].push_back(id);
    if (i % 4 == 0) { interleaved[(i + 1) % 3].push_back(id); }
    if (i % 5 == 0) { for (auto &stream : interleaved) { stream.push_back(id); } }
    for (auto &stream : dense) { stream.push_back(id); }
  }
  // Both signs of a difference >= 2^63 must use unsigned comparisons, never
  // subtraction/narrowing. Cover every equal pair and the highest-source tie.
  const std::vector<Streams> cases = {
      interleaved, dense,
      {{value(prefix, 0), value(prefix, 0x8000000000000000ULL), value(prefix, 0xffffffffffffffffULL)},
       {value(prefix, 0), value(prefix, 0x7fffffffffffffffULL), value(prefix, 0xffffffffffffffffULL)},
       {value(prefix, 1), value(prefix, 0x8000000000000000ULL), value(prefix, 0xffffffffffffffffULL)}},
      {{value(prefix, 1), value(prefix, 0x8000000000000000ULL)},
       {value(prefix, 0), value(prefix, 0xffffffffffffffffULL)},
       {value(prefix, 0), value(prefix, 0x8000000000000000ULL)}},
      // The common high eight bytes change across batches and may disagree
      // between sources. This must switch between suffix and full comparisons.
      {{value(prefix, 0), value(prefix, 0x8000000000000000ULL),
          value(next_prefix, 0), value(next_prefix, 0xffffffffffffffffULL)},
       {value(prefix, 1), value(prefix, 0xffffffffffffffffULL),
          value(next_prefix, 1), value(next_prefix, 0xffffffffffffffffULL)},
       {value(prefix, 0), value(prefix, 0xffffffffffffffffULL),
          value(next_prefix, 0x8000000000000000ULL), value(next_prefix, 0xffffffffffffffffULL)}}};
  for (size_t test_case = 0; test_case < cases.size(); ++test_case) {
    for (const int64_t capacity : {1, 2, 3, 7, 128}) {
      for (const std::vector<int64_t> &sizes : {
          std::vector<int64_t>{1, 1, 1}, std::vector<int64_t>{2, 3, 5},
          std::vector<int64_t>{7, 11, 13}, std::vector<int64_t>{128, 128, 128}}) {
        for (const bool end_with_rows : {false, true}) {
          for (int mode = 0; mode < 3; ++mode) {
            SCOPED_TRACE(::testing::Message() << test_case << ", " << capacity << ", "
                << sizes[0] << ", " << end_with_rows << ", " << mode);
            check_prefix_membership_values(cases[test_case], CS_TYPE_BINARY, sizes,
                capacity, mode, end_with_rows);
          }
        }
      }
    }
  }
}

TEST(FtsTextMembershipBatchUnion, from_obj_fixed16_default_and_false_proofs_preserve_full_payload)
{
  using Streams = std::vector<std::vector<ObDocIdExt>>;
  std::string prefix(8, '\0');
  prefix[1] = static_cast<char>(0x80);
  prefix[6] = static_cast<char>(0xff);
  const auto value = [&](const int suffix) {
    std::string bytes = prefix + std::string(8, '\0');
    bytes[8] = static_cast<char>(suffix);
    bytes[15] = static_cast<char>(255 - suffix);
    return fixed16_test_value(bytes);
  };
  const std::vector<Streams> cases = {
      {{value(1), value(50), value(200)}, {value(0), value(50), value(255)}},
      {{value(0), value(50), value(255)}, {value(1), value(50), value(200)}},
      {{value(0), value(50), value(255)}, {value(1), value(50), value(200)},
          {value(2), value(50), value(128)}}};
  for (const Streams &streams : cases) {
    for (int borrowed_dim = 0; borrowed_dim < static_cast<int>(streams.size()); ++borrowed_dim) {
      for (const bool explicit_false : {false, true}) {
        for (const bool end_with_rows : {false, true}) {
          for (const int64_t capacity : {1, 2, 128}) {
            for (const std::vector<int64_t> &sizes : {
                std::vector<int64_t>{1, 1, 1}, std::vector<int64_t>{2, 3, 2},
                std::vector<int64_t>{128, 128, 128}}) {
              SCOPED_TRACE(::testing::Message() << streams.size() << ", " << borrowed_dim
                  << ", " << explicit_false << ", " << end_with_rows << ", " << capacity << ", " << sizes[0]);
              check_prefix_membership_values(streams, CS_TYPE_BINARY, sizes, capacity, 1,
                  end_with_rows, borrowed_dim, explicit_false);
            }
          }
        }
      }
    }
  }
}


} // namespace
} // namespace storage
} // namespace oceanbase
