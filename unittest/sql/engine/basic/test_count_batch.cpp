/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <array>
#include <utility>
#include <vector>
#include "sql/engine/aggregate/ob_aggregate_processor.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
namespace sql
{
namespace
{
using namespace common;

class CountBatch : public testing::Test
{
protected:
  static constexpr int64_t MAX_ROWS = UINT16_MAX;
  static constexpr int64_t INPUT_ROWS = 129;

  CountBatch()
    : exec_ctx_(allocator_), eval_ctx_(exec_ctx_),
      processor_(eval_ctx_, aggr_infos_, "CountBatchTest", monitor_), group_(nullptr),
      skip_words_(ObBitVector::word_count(MAX_ROWS), 0)
  {
    frames_[0] = reinterpret_cast<char *>(input_datums_.data());
    input_expr_.frame_idx_ = 0;
    input_expr_.datum_off_ = 0;
    input_expr_.batch_result_ = true;
    input_expr_.batch_idx_mask_ = UINT64_MAX;
    input_expr_.datum_meta_.type_ = ObIntType;
    input_expr_.obj_meta_.set_int();
    for (int64_t i = 0; i < INPUT_ROWS; ++i) {
      input_values_[i] = i;
      input_datums_[i] = ObDatum(reinterpret_cast<const char *>(&input_values_[i]),
          sizeof(input_values_[i]), false);
      if (0 == i % 3) {
        input_datums_[i].set_null();
      }
    }
  }

  int init(const bool nullable_count = false)
  {
    int ret = OB_SUCCESS;
    exec_ctx_.set_my_session(&session_);
    eval_ctx_.set_max_batch_size(MAX_ROWS);
    eval_ctx_.frames_ = frames_;
    if (OB_FAIL(aggr_infos_.prepare_allocate(2))) {
    } else {
      for (int64_t i = 0; i < aggr_infos_.count(); ++i) {
        count_exprs_[i].type_ = T_FUN_COUNT;
        aggr_infos_.at(i).set_allocator(&allocator_);
        aggr_infos_.at(i).expr_ = &count_exprs_[i];
      }
      if (nullable_count && OB_FAIL(aggr_infos_.at(1).param_exprs_.init(1))) {
      } else if (nullable_count && OB_FAIL(aggr_infos_.at(1).param_exprs_.push_back(&input_expr_))) {
      } else if (OB_FAIL(processor_.init())) {
      } else if (OB_FAIL(processor_.init_one_group())) {
      } else if (OB_FAIL(processor_.get_group_row(0, group_))) {
      }
    }
    return ret;
  }

  ObBitVector &skip() { return *to_bit_vector(skip_words_.data()); }

  template <typename Selector>
  int64_t iterator_count(const Selector &selector) const
  {
    int64_t count = 0;
    for (auto it = selector.begin(); it < selector.end(); selector.next(it)) {
      ++count;
    }
    return count;
  }

  void set_counts(const int64_t value)
  {
    for (int64_t i = 0; i < group_->n_cells_; ++i) {
      group_->aggr_cells_[i].set_row_count(value);
    }
  }

  void expect_counts(const int64_t value)
  {
    for (int64_t i = 0; i < group_->n_cells_; ++i) {
      EXPECT_EQ(value, group_->aggr_cells_[i].get_row_count()) << i;
    }
  }

  ObArenaAllocator allocator_;
  ObSQLSessionInfo session_;
  ObExecContext exec_ctx_;
  ObEvalCtx eval_ctx_;
  ObSEArray<ObAggrInfo, 2> aggr_infos_;
  ObExpr count_exprs_[2];
  ObExpr input_expr_;
  std::array<ObDatum, INPUT_ROWS> input_datums_;
  std::array<int64_t, INPUT_ROWS> input_values_;
  char *frames_[1];
  ObMonitorNode monitor_;
  ObAggregateProcessor processor_;
  ObAggregateProcessor::GroupRow *group_;
  std::vector<uint64_t> skip_words_;
};

TEST_F(CountBatch, slices_match_iterator_at_word_and_batch_boundaries)
{
  ASSERT_EQ(OB_SUCCESS, init());
  const uint16_t bounds[] = {0, 1, 2, 31, 32, 62, 63, 64, 65, 66, 95, 126, 127, 128, 129};
  ObBatchRows brs(skip(), INPUT_ROWS, false);
  for (int pattern = 0; pattern < 5; ++pattern) {
    skip().reset(MAX_ROWS);
    for (int64_t i = 0; i < MAX_ROWS; ++i) {
      if (pattern == 1 || (pattern == 2 && i % 2 == 0)
          || (pattern == 3 && i % 7 == 3)
          || (pattern == 4 && (i < 63 || i >= 65))) {
        skip().set(i);
      }
    }
    // Set padding and out-of-slice bits as well; only the slice may count.
    skip().set(MAX_ROWS);
    for (const uint16_t begin : bounds) {
      for (const uint16_t end : bounds) {
        SCOPED_TRACE(testing::Message() << pattern << ", " << begin << ", " << end);
        const ObAggregateProcessor::ObBatchRowsSlice selector(&brs, begin, end);
        const int64_t expected = iterator_count(selector);
        set_counts(17);
        ASSERT_EQ(OB_SUCCESS, processor_.process_batch(*group_, brs, begin, end));
        expect_counts(17 + expected);
      }
    }
  }
}

TEST_F(CountBatch, empty_and_reversed_slices_need_no_skip_vector)
{
  ASSERT_EQ(OB_SUCCESS, init());
  const ObBatchRows empty;
  for (const std::pair<uint16_t, uint16_t> &bounds : {
      std::pair<uint16_t, uint16_t>{0, 0}, {1, 1}, {65, 64}, {UINT16_MAX, UINT16_MAX},
      {UINT16_MAX, 0}}) {
    SCOPED_TRACE(testing::Message() << bounds.first << ", " << bounds.second);
    const ObAggregateProcessor::ObBatchRowsSlice selector(&empty, bounds.first, bounds.second);
    ASSERT_EQ(0, iterator_count(selector));
    set_counts(23);
    ASSERT_EQ(OB_SUCCESS,
        processor_.process_batch(*group_, empty, bounds.first, bounds.second));
    expect_counts(23);
  }
}

TEST_F(CountBatch, explicit_selections_keep_duplicates_and_ignore_skip_bits)
{
  ASSERT_EQ(OB_SUCCESS, init());
  skip().set_all(MAX_ROWS);
  ObBatchRows brs(skip(), MAX_ROWS, false);
  const uint16_t selected[] = {128, 2, 128, 0, 64, 2, 1};
  const ObAggregateProcessor::ObSelector selector(&brs, selected, ARRAYSIZEOF(selected));
  set_counts(31);
  ASSERT_EQ(OB_SUCCESS, processor_.process_batch(&brs, *group_, selected, ARRAYSIZEOF(selected)));
  expect_counts(31 + iterator_count(selector));
  ASSERT_EQ(OB_SUCCESS, processor_.process_batch(nullptr, *group_, nullptr, 0));
  expect_counts(31 + ARRAYSIZEOF(selected));
}

TEST_F(CountBatch, maximum_batch_and_repeated_calls_accumulate)
{
  ASSERT_EQ(OB_SUCCESS, init());
  ObBatchRows brs(skip(), MAX_ROWS, false);
  set_counts(INT64_C(100000));
  int64_t expected = INT64_C(100000);
  for (int pattern = 0; pattern < 3; ++pattern) {
    skip().reset(MAX_ROWS);
    for (int64_t i = 0; i < MAX_ROWS; ++i) {
      if (pattern == 1 || (pattern == 2 && i % 5 == 1)) {
        skip().set(i);
      }
    }
    skip().set(MAX_ROWS);
    const ObAggregateProcessor::ObBatchRowsSlice selector(&brs, 0, UINT16_MAX);
    expected += iterator_count(selector);
    ASSERT_EQ(OB_SUCCESS, processor_.process_batch(*group_, brs, 0, UINT16_MAX));
    expect_counts(expected);
  }
  // Cardinality remains the number of selection entries when every entry repeats.
  const std::vector<uint16_t> selected(MAX_ROWS, 3);
  const ObAggregateProcessor::ObSelector selector(&brs, selected.data(), UINT16_MAX);
  expected += iterator_count(selector);
  ASSERT_EQ(OB_SUCCESS, processor_.process_batch(&brs, *group_, selected.data(), UINT16_MAX));
  expect_counts(expected);
}

TEST_F(CountBatch, nullable_count_keeps_null_filtering_for_slices_and_selections)
{
  ASSERT_EQ(OB_SUCCESS, init(true));
  skip().reset(MAX_ROWS);
  for (int64_t i = 0; i < INPUT_ROWS; ++i) {
    if (i % 5 == 0) {
      skip().set(i);
    }
  }
  ObBatchRows brs(skip(), INPUT_ROWS, false);
  const ObAggregateProcessor::ObBatchRowsSlice slice(&brs, 1, INPUT_ROWS);
  int64_t expected_star = iterator_count(slice);
  int64_t expected_nullable = 0;
  for (auto it = slice.begin(); it < slice.end(); slice.next(it)) {
    expected_nullable += !input_datums_[it].is_null();
  }
  ASSERT_EQ(OB_SUCCESS, processor_.process_batch(*group_, brs, 1, INPUT_ROWS));
  EXPECT_EQ(expected_star, group_->aggr_cells_[0].get_row_count());
  EXPECT_EQ(expected_nullable, group_->aggr_cells_[1].get_row_count());

  const uint16_t selected[] = {128, 3, 5, 3, 0, 64, 5};
  const ObAggregateProcessor::ObSelector selector(&brs, selected, ARRAYSIZEOF(selected));
  expected_star += iterator_count(selector);
  for (auto it = selector.begin(); it < selector.end(); selector.next(it)) {
    expected_nullable += !input_datums_[selector.get_batch_index(it)].is_null();
  }
  ASSERT_EQ(OB_SUCCESS, processor_.process_batch(&brs, *group_, selected, ARRAYSIZEOF(selected)));
  EXPECT_EQ(expected_star, group_->aggr_cells_[0].get_row_count());
  EXPECT_EQ(expected_nullable, group_->aggr_cells_[1].get_row_count());
}

} // namespace
} // namespace sql
} // namespace oceanbase
