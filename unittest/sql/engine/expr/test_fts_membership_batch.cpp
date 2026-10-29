/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include "sql/engine/ob_exec_context.h"
#include "storage/retrieval/ob_text_daat_iter.h"

namespace oceanbase
{
namespace storage
{
class TextDaaTIterTestPeer
{
public:
  static int prepare(ObTextDaaTIter &iter, ObSparseRetrievalMergeParam &param,
      common::ObIArray<ObISRDaaTDimIter *> &dims, common::ObIAllocator &allocator,
      const bool numeric)
  {
    int ret = OB_SUCCESS;
    iter.iter_param_ = &param;
    iter.dim_iters_ = &dims;
    iter.membership_only_ = true;
    iter.set_datum_func_ = numeric ? ObISparseRetrievalMergeIter::set_datum_int
        : ObISparseRetrievalMergeIter::set_datum_shallow;
    iter.membership_ids_.set_allocator(&allocator);
    if (OB_FAIL(iter.membership_batch_union_.init(dims, param.id_proj_expr_->datum_meta_))) {
    } else if (OB_FAIL(iter.membership_ids_.init(param.max_batch_size_))) {
    } else if (OB_FAIL(iter.membership_ids_.prepare_allocate(param.max_batch_size_))) {
    } else {
      iter.is_inited_ = true;
    }
    return ret;
  }
  static void detach_sources(ObTextDaaTIter &iter) { iter.dim_iters_ = nullptr; }
  static void reuse(ObTextDaaTIter &iter)
  {
    iter.input_row_cnt_ = iter.output_row_cnt_ = 0;
    iter.membership_batch_union_.reuse();
  }
};

namespace
{
using namespace common;

struct MembershipProjectionFrame
{
  MembershipProjectionFrame(const int64_t index, const bool batch)
  {
    MEMSET(memory_, 0, sizeof(memory_));
    expr_.frame_idx_ = index;
    expr_.datum_off_ = 0;
    expr_.eval_info_off_ = 2048;
    expr_.eval_flags_off_ = 2080;
    expr_.res_buf_off_ = 2112;
    expr_.res_buf_len_ = 40;
    expr_.batch_result_ = batch;
    expr_.batch_idx_mask_ = batch ? UINT64_MAX : 0;
    for (int64_t i = 0; i < 128; ++i) {
      new (memory_ + i * sizeof(ObDatum)) ObDatum(memory_ + 2112 + i * 40, 0, false);
    }
  }
  const ObDatum &datum(const int64_t row) const
  {
    return reinterpret_cast<const ObDatum *>(memory_)[expr_.batch_idx_mask_ & row];
  }
  sql::ObExpr expr_;
  alignas(8) char memory_[8192];
};

class MembershipProjectionSource : public ObISRDaaTDimIter
{
public:
  MembershipProjectionSource(const std::vector<std::string> &values,
      const bool numeric, const int64_t batch_size)
    : values_(values), numeric_(numeric), next_(0), calls_(0), ids_(batch_size), fail_at_(-1)
  {}
  int get_next_row() override { return OB_ERR_UNEXPECTED; }
  int get_curr_id(const ObDatum *&) const override { return OB_ERR_UNEXPECTED; }
  int get_curr_score(double &) const override { return OB_ERR_UNEXPECTED; }
  int get_next_doc_id_batch(const ObDocIdExt *&ids, int64_t &count) override
  {
    ids = nullptr;
    count = 0;
    for (auto &id : ids_) { id.reset(); }
    if (++calls_ == fail_at_) { return OB_TIMEOUT; }
    while (count < static_cast<int64_t>(ids_.size())
        && next_ < static_cast<int64_t>(values_.size())) {
      char buffer[40] = {};
      ObDatum datum(buffer, 0, false);
      const std::string &value = values_[next_++];
      if (numeric_) { datum.set_uint(std::stoull(value)); }
      else { datum.set_string(value.data(), value.size()); }
      const int ret = ids_[count].from_datum(datum);
      if (OB_SUCCESS != ret) { return ret; }
      ++count;
    }
    ids = count > 0 ? ids_.data() : nullptr;
    // Exercise the nonempty final source batch carrying EOF as well.
    return next_ == static_cast<int64_t>(values_.size()) ? OB_ITER_END : OB_SUCCESS;
  }
  void rewind() { next_ = calls_ = 0; }
private:
  std::vector<std::string> values_;
  bool numeric_;
  int64_t next_;
  int64_t calls_;
  std::vector<ObDocIdExt> ids_;
public:
  int64_t fail_at_;
};

struct MembershipProjectionIter
{
  ~MembershipProjectionIter()
  {
    // Fake sources are fixture-owned. Prevent the real owner's reset from
    // casting them to concrete token iterators during destruction.
    TextDaaTIterTestPeer::detach_sources(iter_);
  }
  ObTextDaaTIter iter_;
};

void check_membership_projection(const bool numeric, const int64_t source_batch,
    const int64_t max_batch, const int64_t capacity, const int64_t offset,
    const int64_t limit, const bool batch_expr = true, const bool inject_error = false)
{
  ObArenaAllocator allocator;
  sql::ObExecContext exec_ctx(allocator);
  sql::ObEvalCtx eval_ctx(exec_ctx);
  eval_ctx.set_max_batch_size(128);
  MembershipProjectionFrame id_frame(0, batch_expr), score_frame(1, batch_expr);
  id_frame.expr_.datum_meta_.type_ = numeric ? ObUInt64Type : ObVarcharType;
  id_frame.expr_.datum_meta_.cs_type_ = CS_TYPE_BINARY;
  char *frames[] = {id_frame.memory_, score_frame.memory_};
  eval_ctx.frames_ = frames;
  std::vector<std::vector<std::string>> streams(3);
  std::vector<std::string> expected;
  for (uint64_t value = 0; value < 73; ++value) {
    const std::string text = std::to_string(value);
    const std::string id = numeric ? text : "id" + std::string(3 - text.size(), '0') + text;
    streams[value % 3].push_back(id);
    if (value % 5 == 0) { streams[(value + 1) % 3].push_back(id); }
    expected.push_back(id);
  }
  if (limit >= 0) {
    const int64_t begin = std::min<int64_t>(expected.size(), offset);
    const int64_t end = std::min<int64_t>(expected.size(), begin + limit);
    expected = std::vector<std::string>(expected.begin() + begin, expected.begin() + end);
  }
  ObSEArray<ObISRDaaTDimIter *, 3> dims;
  std::vector<std::unique_ptr<MembershipProjectionSource>> sources;
  for (const auto &stream : streams) {
    sources.emplace_back(new MembershipProjectionSource(stream, numeric, source_batch));
    ASSERT_EQ(OB_SUCCESS, dims.push_back(sources.back().get()));
  }
  if (inject_error) { sources.front()->fail_at_ = 2; }
  ObLimitParam limit_param;
  limit_param.offset_ = offset;
  limit_param.limit_ = limit;
  ObSparseRetrievalMergeParam param;
  param.eval_ctx_ = &eval_ctx;
  param.id_proj_expr_ = &id_frame.expr_;
  param.relevance_proj_expr_ = &score_frame.expr_;
  param.max_batch_size_ = max_batch;
  param.limit_param_ = &limit_param;
  MembershipProjectionIter owner;
  ASSERT_EQ(OB_SUCCESS, TextDaaTIterTestPeer::prepare(owner.iter_, param, dims, allocator, numeric));
  for (int pass = 0; pass < 2; ++pass) {
    std::vector<std::string> actual;
    bool ended = false;
    bool ended_with_rows = false;
    bool got_error = false;
    int64_t zero_count = 999;
    ASSERT_EQ(OB_SUCCESS, owner.iter_.get_next_rows(0, zero_count));
    ASSERT_EQ(0, zero_count);
    for (int batch = 0; batch < 512 && !ended; ++batch) {
      int64_t count = 999;
      const int ret = owner.iter_.get_next_rows(capacity, count);
      if (inject_error && OB_TIMEOUT == ret) {
        EXPECT_EQ(0, count);
        got_error = true;
        break;
      }
      ASSERT_TRUE(OB_SUCCESS == ret || OB_ITER_END == ret) << ret;
      ASSERT_LE(count, std::min(max_batch, capacity));
      ended = OB_ITER_END == ret;
      ended_with_rows = ended && count > 0;
      ASSERT_TRUE(ended || count > 0);
      for (int64_t i = 0; i < count; ++i) {
        const ObDatum &id = id_frame.datum(i);
        actual.push_back(numeric ? std::to_string(id.get_uint()) : std::string(id.ptr_, id.len_));
        EXPECT_DOUBLE_EQ(1.0, score_frame.datum(i).get_double());
        EXPECT_TRUE(id_frame.expr_.get_evaluated_flags(eval_ctx).at(i));
        EXPECT_TRUE(score_frame.expr_.get_evaluated_flags(eval_ctx).at(i));
      }
      if (count > 0) {
        EXPECT_TRUE(id_frame.expr_.get_eval_info(eval_ctx).projected_);
        EXPECT_TRUE(score_frame.expr_.get_eval_info(eval_ctx).projected_);
      }
    }
    if (inject_error) {
      EXPECT_TRUE(got_error);
      ASSERT_EQ(1, actual.size());
      EXPECT_EQ(expected.front(), actual.front());
    } else {
      ASSERT_TRUE(ended);
      EXPECT_EQ(expected, actual);
      if (limit > 0 && static_cast<int64_t>(expected.size()) == limit) {
        EXPECT_TRUE(ended_with_rows);
      }
      for (int repeat = 0; repeat < 2; ++repeat) {
        int64_t count = 999;
        EXPECT_EQ(OB_ITER_END, owner.iter_.get_next_rows(capacity, count));
        EXPECT_EQ(0, count);
      }
    }
    for (const auto &source : sources) { source->rewind(); }
    TextDaaTIterTestPeer::reuse(owner.iter_);
  }
}

TEST(FtsMembershipBatchProjection, real_get_next_rows_projects_and_paginates_complete_results)
{
  for (const bool numeric : {false, true}) {
    for (const int64_t source_batch : {1, 4, 128}) {
      for (const int64_t capacity : {1, 4, 128}) {
        SCOPED_TRACE(::testing::Message() << numeric << ", source=" << source_batch
            << ", capacity=" << capacity);
        check_membership_projection(numeric, source_batch, 4, capacity, 0, -1);
        check_membership_projection(numeric, source_batch, 128, capacity, 5, 11);
        check_membership_projection(numeric, source_batch, 128, capacity, 0, 0);
        check_membership_projection(numeric, source_batch, 128, capacity, 99, 3);
      }
    }
    check_membership_projection(numeric, 4, 4, 1, 3, 7, false);
    check_membership_projection(numeric, 1, 128, 128, 0, -1, true, true);
  }
}
} // namespace
} // namespace storage
} // namespace oceanbase
