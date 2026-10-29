/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "sql/engine/ob_exec_context.h"
#include "sql/das/iter/ob_das_scan_iter.h"
#include "data_plane/access/ob_table_scan_param.h"
#include "storage/retrieval/ob_text_daat_iter.h"

namespace oceanbase
{
namespace storage
{
// Prepare only the cache and its real backing iterator. The production batch
// method itself remains unmodified and must drive the next refill.
class TextTokenBatchTestPeer
{
public:
  static int prepare(ObTextRetrievalDaaTTokenIter &iter,
      ObTextRetrievalTokenIter &source, sql::ObEvalCtx &eval_ctx,
      common::ObArenaAllocator &allocator, const std::vector<std::string> &values,
      const bool numeric, const int64_t valid_count, const int64_t previous_index)
  {
    int ret = common::OB_SUCCESS;
    iter.doc_id_.set_allocator(&allocator);
    if (OB_FAIL(iter.doc_id_.init(values.size()))) {
    } else if (OB_FAIL(iter.doc_id_.prepare_allocate(values.size()))) {
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(values.size()); ++i) {
        uint64_t number = 0;
        common::ObDatum datum(reinterpret_cast<char *>(&number), 0, false);
        if (numeric) {
          datum.set_uint(std::stoull(values[i]));
        } else {
          datum.set_string(values[i].data(), values[i].size());
        }
        ret = iter.doc_id_.at(i).from_datum(datum);
      }
    }
    if (OB_SUCC(ret)) {
      iter.allocator_ = &allocator;
      iter.token_iter_ = &source;
      iter.eval_ctx_ = &eval_ctx;
      iter.max_batch_size_ = MAX(eval_ctx.max_batch_size_, 1);
      iter.cur_idx_ = previous_index;
      iter.count_ = valid_count;
      iter.is_inited_ = true;
    }
    return ret;
  }

  static int64_t index(const ObTextRetrievalDaaTTokenIter &iter) { return iter.cur_idx_; }
  static void set_count(ObTextRetrievalDaaTTokenIter &iter, const int64_t count)
  { iter.count_ = count; }
  static void set_cursor(ObTextRetrievalDaaTTokenIter &iter, const int64_t index)
  { iter.cur_idx_ = index; }
  static void set_source(ObTextRetrievalDaaTTokenIter &iter, ObTextRetrievalTokenIter &source)
  { iter.token_iter_ = &source; }
  static const sql::ObDocIdExt *data(const ObTextRetrievalDaaTTokenIter &iter)
  { return iter.doc_id_.get_data(); }
  static void enable_score(ObTextRetrievalDaaTTokenIter &iter, sql::ObExpr &score)
  { iter.relevance_expr_ = &score; }
  static int save_docids(ObTextRetrievalDaaTTokenIter &iter, sql::ObExpr &expr)
  {
    iter.inv_scan_domain_id_col_ = &expr;
    return iter.save_docids();
  }
};

namespace
{
using namespace common;

void check_real_token_span(const sql::ObDocIdExt *span, const int64_t count,
    const std::vector<std::string> &expected, const int64_t begin, const bool numeric)
{
  ASSERT_NE(nullptr, span);
  for (int64_t i = 0; i < count; ++i) {
    const ObDatum &datum = span[i].get_datum();
    if (numeric) {
      EXPECT_EQ(std::stoull(expected[begin + i]), datum.get_uint());
    } else {
      EXPECT_EQ(expected[begin + i], std::string(datum.ptr_, datum.len_));
    }
  }
}

TEST(FtsRealTokenBatch, returns_cached_unsigned_and_binary_spans_then_refills)
{
  const std::vector<std::string> numeric_values = {
      "0", "1", "7", "42", "9223372036854775808", "18446744073709551614", "18446744073709551615"};
  const std::vector<std::string> binary_values = {
      "", std::string("\0", 1), "a", std::string("a\0b", 3),
      std::string("\x80\xff", 2), std::string(16, 'x'), std::string(40, 'z')};
  for (const bool numeric : {false, true}) {
    const auto &expected = numeric ? numeric_values : binary_values;
    for (const bool vectorized : {false, true}) {
      for (const int64_t valid_count : {1, 3, 7}) {
        // -1 means the first posting has not yet been consumed. A nonnegative
        // cursor is the already-consumed current posting, not the next row.
        for (const int64_t previous_index : std::vector<int64_t>{-1, 0, valid_count - 2}) {
          if (previous_index >= valid_count - 1) { continue; }
          SCOPED_TRACE(::testing::Message() << numeric << ", " << vectorized
              << ", " << valid_count << ", " << previous_index);
          ObArenaAllocator allocator("TokenBatchTest");
          sql::ObExecContext exec_ctx(allocator);
          sql::ObEvalCtx eval_ctx(exec_ctx);
          eval_ctx.set_max_batch_size(vectorized ? 8 : 0);
          ObTextRetrievalTokenIter source;
          ObTextRetrievalDaaTTokenIter iter;
          std::vector<std::string> input = expected;
          ASSERT_EQ(OB_SUCCESS, TextTokenBatchTestPeer::prepare(
              iter, source, eval_ctx, allocator, input, numeric, valid_count, previous_index));
          // Cached IDs must own their bytes independently of the producer's
          // input strings, including embedded NULs and fixed-width IDs.
          for (std::string &value : input) { value.assign(40, '!'); }
          const sql::ObDocIdExt *span = nullptr;
          int64_t count = -1;
          ASSERT_EQ(OB_SUCCESS, iter.get_next_doc_id_batch(span, count));
          const int64_t begin = previous_index + 1;
          ASSERT_EQ(valid_count - begin, count);
          ASSERT_EQ(TextTokenBatchTestPeer::data(iter) + begin, span);
          EXPECT_EQ(valid_count - 1, TextTokenBatchTestPeer::index(iter));
          check_real_token_span(span, count, expected, begin, numeric);

          const ObDatum *current = nullptr;
          ASSERT_EQ(OB_SUCCESS, iter.get_curr_id(current));
          EXPECT_EQ(&span[count - 1].get_datum(), current);
          // Read-only inspection must leave the borrowed span stable.
          check_real_token_span(span, count, expected, begin, numeric);
          const sql::ObDocIdExt *next = span;
          int64_t next_count = 99;
          // The genuine default source returns NOT_INIT. Receiving that error
          // proves the exhausted cache was not returned for a second time.
          EXPECT_EQ(OB_NOT_INIT, iter.get_next_doc_id_batch(next, next_count));
          EXPECT_EQ(nullptr, next);
          EXPECT_EQ(0, next_count);
          next = span;
          next_count = 99;
          EXPECT_EQ(OB_NOT_INIT, iter.get_next_doc_id_batch(next, next_count));
          EXPECT_EQ(nullptr, next);
          EXPECT_EQ(0, next_count);
        }
      }
    }
  }
}

TEST(FtsRealTokenBatch, empty_and_already_consumed_cache_clear_outputs_on_refill_error)
{
  for (const bool vectorized : {false, true}) {
    for (const int64_t valid_count : {0, 1, 3}) {
      ObArenaAllocator allocator("TokenBatchTest");
      sql::ObExecContext exec_ctx(allocator);
      sql::ObEvalCtx eval_ctx(exec_ctx);
      eval_ctx.set_max_batch_size(vectorized ? 8 : 0);
      ObTextRetrievalTokenIter source;
      ObTextRetrievalDaaTTokenIter iter;
      ASSERT_EQ(OB_SUCCESS, TextTokenBatchTestPeer::prepare(iter, source, eval_ctx,
          allocator, {"1", "2", "3", "4"}, true, valid_count, valid_count - 1));
      const sql::ObDocIdExt *ids = TextTokenBatchTestPeer::data(iter);
      int64_t count = 99;
      EXPECT_EQ(OB_NOT_INIT, iter.get_next_doc_id_batch(ids, count));
      EXPECT_EQ(nullptr, ids);
      EXPECT_EQ(0, count);
    }
  }
}

TEST(FtsRealTokenBatch, scoring_iterator_rejects_the_membership_api_without_consuming_cache)
{
  ObArenaAllocator allocator("TokenBatchTest");
  sql::ObExecContext exec_ctx(allocator);
  sql::ObEvalCtx eval_ctx(exec_ctx);
  ObTextRetrievalTokenIter source;
  ObTextRetrievalDaaTTokenIter iter;
  ASSERT_EQ(OB_SUCCESS, TextTokenBatchTestPeer::prepare(
      iter, source, eval_ctx, allocator, {"1", "2", "3"}, true, 3, -1));
  sql::ObExpr score;
  TextTokenBatchTestPeer::enable_score(iter, score);
  const sql::ObDocIdExt *ids = TextTokenBatchTestPeer::data(iter);
  int64_t count = 99;
  EXPECT_EQ(OB_NOT_SUPPORTED, iter.get_next_doc_id_batch(ids, count));
  EXPECT_EQ(nullptr, ids);
  EXPECT_EQ(0, count);
  EXPECT_EQ(-1, TextTokenBatchTestPeer::index(iter));
}

TEST(FtsRealTokenBatch, save_docids_preserves_skip_scalar_and_partial_batch_semantics)
{
  constexpr int64_t capacity = 130;
  for (const bool batch : {false, true}) {
    for (const int64_t count : {0, 1, 3, 63, 64, 65, 129}) {
      for (const int skip_mode : {0, 1, 2}) {
        SCOPED_TRACE(::testing::Message() << batch << ", " << count << ", " << skip_mode);
        ObArenaAllocator allocator("SaveDocIdsTest");
        sql::ObExecContext exec_ctx(allocator);
        sql::ObEvalCtx eval_ctx(exec_ctx);
        eval_ctx.set_max_batch_size(capacity);
        ObDatum datums[capacity];
        char *frames[] = {reinterpret_cast<char *>(datums)};
        eval_ctx.frames_ = frames;
        sql::ObExpr expr;
        expr.frame_idx_ = 0;
        expr.datum_off_ = 0;
        expr.batch_result_ = batch;
        expr.batch_idx_mask_ = batch ? UINT64_MAX : 0;
        ObTableScanParam scan_param;
        // Token init only retains this unused scan. Like production DAS
        // iterators, its object storage has arena lifetime; the base iterator
        // destructor invokes release() and must not run on a default scan.
        void *scan_storage = allocator.alloc(sizeof(sql::ObDASScanIter));
        ASSERT_NE(nullptr, scan_storage);
        sql::ObDASScanIter *scan = new (scan_storage) sql::ObDASScanIter();
        ObTextRetrievalScanIterParam param;
        param.allocator_ = &allocator;
        param.mem_context_ = CURRENT_CONTEXT;
        param.inv_idx_scan_param_ = &scan_param;
        param.inv_idx_scan_iter_ = scan;
        param.eval_ctx_ = &eval_ctx;
        param.inv_scan_domain_id_col_ = &expr;
        ObTextRetrievalTokenIter source;
        ASSERT_EQ(OB_SUCCESS, source.init(param));
        ObTextRetrievalDaaTTokenIter iter;
        const std::vector<std::string> retained(capacity, "unchanged");
        ASSERT_EQ(OB_SUCCESS, TextTokenBatchTestPeer::prepare(
            iter, source, eval_ctx, allocator, retained, false, count, -1));
        std::vector<std::string> values;
        for (int64_t i = 0; i < capacity; ++i) {
          values.push_back(std::string("a\0", 2) + std::to_string(i));
        }
        const std::string oversized(41, 'x');
        for (int64_t i = 0; i < capacity; ++i) {
          // A skipped/unreceived slot deliberately cannot fit in a document
          // ID. Reading it would turn an otherwise successful copy into an
          // error, without relying on a faulting pointer or undefined behavior.
          datums[i].set_string(oversized.data(), oversized.size());
          const bool skipped = skip_mode == 2 || (skip_mode == 1 && i % 2 == 0);
          if (i < count && skipped) { source.get_skip()->set(i); }
          if (batch && i < count && !skipped) {
            datums[i].set_string(values[i].data(), values[i].size());
          }
        }
        if (!batch && (skip_mode != 2 || count == 0)) {
          datums[0].set_string(values[0].data(), values[0].size());
        }
        // Tail bits must not disable the all-false path for a partial batch.
        source.get_skip()->set(count);
        ASSERT_EQ(OB_SUCCESS, TextTokenBatchTestPeer::save_docids(iter, expr));
        EXPECT_EQ(0, TextTokenBatchTestPeer::index(iter));
        for (int64_t i = 0; i < capacity; ++i) {
          const bool skipped = skip_mode == 2 || (skip_mode == 1 && i % 2 == 0);
          const std::string &expected = i >= count || skipped
              ? retained[i] : values[batch ? i : 0];
          const ObDatum &actual = TextTokenBatchTestPeer::data(iter)[i].get_datum();
          EXPECT_EQ(expected, std::string(actual.ptr_, actual.len_));
        }
        if (skip_mode == 0 && count >= 3) {
          // An active bad datum must still propagate its copy error and stop
          // before later output slots are touched, in both no-skip kernels.
          ObTextRetrievalDaaTTokenIter failing_iter;
          ASSERT_EQ(OB_SUCCESS, TextTokenBatchTestPeer::prepare(
              failing_iter, source, eval_ctx, allocator, retained, false, count, -1));
          datums[batch ? 1 : 0].set_string(oversized.data(), oversized.size());
          EXPECT_EQ(OB_BUF_NOT_ENOUGH, TextTokenBatchTestPeer::save_docids(failing_iter, expr));
          const ObDatum &untouched = TextTokenBatchTestPeer::data(failing_iter)[2].get_datum();
          EXPECT_EQ(retained[2], std::string(untouched.ptr_, untouched.len_));
        }
        // Copies retain their own backing bytes after the expression is reused.
        for (std::string &value : values) { value.assign(40, '!'); }
        for (int64_t i = 0; i < count; ++i) {
          const bool skipped = skip_mode == 2 || (skip_mode == 1 && i % 2 == 0);
          const std::string expected = skipped ? retained[i]
              : std::string("a\0", 2) + std::to_string(batch ? i : 0);
          const ObDatum &actual = TextTokenBatchTestPeer::data(iter)[i].get_datum();
          EXPECT_EQ(expected, std::string(actual.ptr_, actual.len_));
        }
      }
    }
  }
}
TEST(FtsRealTokenBatch, fixed16_proof_tracks_complete_owned_copies_and_load_lifetime)
{
  constexpr int64_t capacity = 4;
  constexpr int64_t count = 3;
  for (const bool batch : {false, true}) {
    SCOPED_TRACE(batch);
    ObArenaAllocator allocator("Fixed16Proof");
    sql::ObExecContext exec_ctx(allocator);
    sql::ObEvalCtx eval_ctx(exec_ctx);
    eval_ctx.set_max_batch_size(capacity);
    ObDatum datums[capacity];
    char *frames[] = {reinterpret_cast<char *>(datums)};
    eval_ctx.frames_ = frames;
    sql::ObExpr expr;
    expr.frame_idx_ = 0;
    expr.datum_off_ = 0;
    expr.batch_result_ = batch;
    expr.batch_idx_mask_ = batch ? UINT64_MAX : 0;
    ObTableScanParam scan_param;
    void *scan_storage = allocator.alloc(sizeof(sql::ObDASScanIter));
    ASSERT_NE(nullptr, scan_storage);
    sql::ObDASScanIter *scan = new (scan_storage) sql::ObDASScanIter();
    ObTextRetrievalScanIterParam param;
    param.allocator_ = &allocator;
    param.mem_context_ = CURRENT_CONTEXT;
    param.inv_idx_scan_param_ = &scan_param;
    param.inv_idx_scan_iter_ = scan;
    param.eval_ctx_ = &eval_ctx;
    param.inv_scan_domain_id_col_ = &expr;
    ObTextRetrievalTokenIter source, uninitialized_source;
    ASSERT_EQ(OB_SUCCESS, source.init(param));
    ObTextRetrievalDaaTTokenIter iter;
    ASSERT_FALSE(iter.is_fixed16_doc_id_batch());
    ASSERT_EQ(OB_SUCCESS, TextTokenBatchTestPeer::prepare(
        iter, source, eval_ctx, allocator, std::vector<std::string>(capacity, "old"), false, count, -1));
    std::vector<std::string> original;
    for (int64_t i = 0; i < capacity; ++i) {
      std::string value(16, '\0');
      for (int64_t j = 0; j < 16; ++j) { value[j] = static_cast<char>(i * 71 + j * 19); }
      value[5] = '\0';
      value[13] = static_cast<char>(0xff);
      original.push_back(value);
    }
    std::vector<std::string> values;
    const auto restore = [&]() {
      values = original;
      source.get_skip()->init(capacity);
      TextTokenBatchTestPeer::set_count(iter, count);
      for (int64_t i = 0; i < capacity; ++i) { datums[i].set_string(values[i].data(), values[i].size()); }
    };
    restore();
    ASSERT_EQ(OB_SUCCESS, TextTokenBatchTestPeer::save_docids(iter, expr));
    EXPECT_TRUE(iter.is_fixed16_doc_id_batch());
    for (std::string &value : values) { value.assign(40, '!'); }
    for (int64_t i = 0; i < count; ++i) {
      const sql::ObDocIdExt &owned = TextTokenBatchTestPeer::data(iter)[i];
      const ObDatum &actual = owned.get_datum();
      EXPECT_EQ(owned.get_inline_fixed16_data(), actual.ptr_);
      EXPECT_EQ(original[batch ? i : 0], std::string(actual.ptr_, actual.len_));
    }
    // An unreceived tail slot or skip bit must not affect a shorter span's proof.
    restore();
    datums[count].set_string(nullptr, 0);
    source.get_skip()->set(count);
    ASSERT_EQ(OB_SUCCESS, TextTokenBatchTestPeer::save_docids(iter, expr));
    EXPECT_TRUE(iter.is_fixed16_doc_id_batch());

    const std::string wide(41, static_cast<char>(0x80));
    for (int fault = 0; fault < 9; ++fault) {
      SCOPED_TRACE(fault);
      restore();
      ASSERT_EQ(OB_SUCCESS, TextTokenBatchTestPeer::save_docids(iter, expr));
      ASSERT_TRUE(iter.is_fixed16_doc_id_batch());
      ObDatum &changed = datums[batch ? 1 : 0];
      if (fault == 0) { changed.set_null(); }
      else if (fault == 1) { changed.set_ext(); }
      else if (fault == 2) { changed.set_outrow(); }
      else if (fault == 3) { changed.set_has_lob_header(); }
      else {
        const int lengths[] = {0, 15, 17, 40, 41};
        const int length = lengths[fault - 4];
        changed.set_string(length == 0 ? nullptr : wide.data(), length);
      }
      const int expected_ret = fault == 8 ? OB_BUF_NOT_ENOUGH : OB_SUCCESS;
      ASSERT_EQ(expected_ret, TextTokenBatchTestPeer::save_docids(iter, expr));
      EXPECT_FALSE(iter.is_fixed16_doc_id_batch());
      if (OB_SUCCESS == expected_ret) {
        const ObDatum &actual = TextTokenBatchTestPeer::data(iter)[batch ? 1 : 0].get_datum();
        EXPECT_EQ(changed.pack_, actual.pack_);
        if (!changed.is_null() && changed.len_ > 0) {
          EXPECT_EQ(std::string(changed.ptr_, changed.len_), std::string(actual.ptr_, actual.len_));
        }
      }
    }
    restore();
    ASSERT_EQ(OB_SUCCESS, TextTokenBatchTestPeer::save_docids(iter, expr));
    source.get_skip()->set(1);
    ASSERT_EQ(OB_SUCCESS, TextTokenBatchTestPeer::save_docids(iter, expr));
    EXPECT_FALSE(iter.is_fixed16_doc_id_batch());
    restore();
    ASSERT_EQ(OB_SUCCESS, TextTokenBatchTestPeer::save_docids(iter, expr));
    TextTokenBatchTestPeer::set_count(iter, 0);
    ASSERT_EQ(OB_SUCCESS, TextTokenBatchTestPeer::save_docids(iter, expr));
    EXPECT_FALSE(iter.is_fixed16_doc_id_batch());

    restore();
    ASSERT_EQ(OB_SUCCESS, TextTokenBatchTestPeer::save_docids(iter, expr));
    TextTokenBatchTestPeer::set_cursor(iter, count - 1);
    TextTokenBatchTestPeer::set_source(iter, uninitialized_source);
    const sql::ObDocIdExt *ids = TextTokenBatchTestPeer::data(iter);
    int64_t returned = 99;
    EXPECT_EQ(OB_NOT_INIT, iter.get_next_doc_id_batch(ids, returned));
    EXPECT_EQ(nullptr, ids);
    EXPECT_EQ(0, returned);
    EXPECT_FALSE(iter.is_fixed16_doc_id_batch());
    TextTokenBatchTestPeer::set_source(iter, source);
    restore();
    ASSERT_EQ(OB_SUCCESS, TextTokenBatchTestPeer::save_docids(iter, expr));
    iter.reuse();
    EXPECT_FALSE(iter.is_fixed16_doc_id_batch());
    restore();
    ASSERT_EQ(OB_SUCCESS, TextTokenBatchTestPeer::save_docids(iter, expr));
    iter.reset();
    EXPECT_FALSE(iter.is_fixed16_doc_id_batch());
  }
}

TEST(FtsRealTokenBatch, fixed16_inline_payload_survives_copy_assignment_and_source_overwrite)
{
  for (int position = 0; position < 16; ++position) {
    SCOPED_TRACE(position);
    std::string expected(16, '\0');
    for (int i = 0; i < 16; ++i) { expected[i] = static_cast<char>(position * 71 + i * 19); }
    expected[position] = static_cast<char>(0xff);
    std::string bytes = expected;
    ObDatum datum;
    datum.set_string(bytes.data(), bytes.size());
    sql::ObDocIdExt source;
    ASSERT_EQ(OB_SUCCESS, source.from_datum(datum));
    ASSERT_EQ(source.get_inline_fixed16_data(), source.get_datum().ptr_);
    sql::ObDocIdExt copied(source);
    sql::ObDocIdExt assigned;
    std::string external(16, '!');
    ObObj borrowed;
    borrowed.set_varchar(external.data(), external.size());
    ASSERT_EQ(OB_SUCCESS, assigned.from_obj(borrowed));
    ASSERT_EQ(16, assigned.get_datum().pack_);
    ASSERT_EQ(external.data(), assigned.get_datum().ptr_);
    assigned = source; // Assignment must replace a previous external pointer.
    EXPECT_NE(source.get_datum().ptr_, copied.get_datum().ptr_);
    EXPECT_NE(source.get_datum().ptr_, assigned.get_datum().ptr_);
    EXPECT_NE(copied.get_datum().ptr_, assigned.get_datum().ptr_);
    bytes.assign(40, '?');
    external.assign(40, '#');
    std::string replacement(16, 'x');
    datum.set_string(replacement.data(), replacement.size());
    ASSERT_EQ(OB_SUCCESS, source.from_datum(datum));
    source.reset();
    for (const sql::ObDocIdExt *value : {&copied, &assigned}) {
      EXPECT_EQ(16, value->get_datum().pack_);
      EXPECT_EQ(value->get_inline_fixed16_data(), value->get_datum().ptr_);
      EXPECT_EQ(expected, std::string(value->get_datum().ptr_, 16));
      EXPECT_EQ(expected, std::string(value->get_inline_fixed16_data(), 16));
    }
  }
}


} // namespace
} // namespace storage
} // namespace oceanbase
