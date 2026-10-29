/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/table/ob_table_scan_op.h"
#include "sql/das/iter/ob_das_iter.h"

namespace oceanbase {
namespace sql {
namespace {
using namespace common;

struct FtsBatchFrame
{
  static constexpr int64_t CAPACITY = 7;
  FtsBatchFrame()
  {
    MEMSET(memory_, 0, sizeof(memory_));
    expr_.datum_off_ = 0;
    expr_.eval_info_off_ = 128;
    expr_.eval_flags_off_ = 160;
    expr_.res_buf_off_ = 192;
    expr_.res_buf_len_ = 16;
    expr_.batch_result_ = true;
    expr_.batch_idx_mask_ = UINT64_MAX;
    for (int64_t i = 0; i < CAPACITY; ++i) {
      ObDatum *datum = new (memory_ + i * sizeof(ObDatum)) ObDatum();
      datum->ptr_ = memory_ + 192 + i * 16;
      datum->set_null();
    }
  }
  ObExpr expr_;
  alignas(8) char memory_[512];
};

struct FtsBatchDocument
{
  std::string id_;
  std::string text_;
  bool is_null_;
};

// Supplies physical documents through the same scalar DAS interface used by
// the real DDL scan. Token expansion must never request the next document
// after it has already emitted a token into the current batch.
class FtsBatchSource : public ObDASIter
{
public:
  FtsBatchSource(const std::vector<FtsBatchDocument> &documents,
                 ObExpr &text, ObExpr &id, const bool numeric_ids)
      : documents_(documents), text_(text), id_(id), numeric_ids_(numeric_ids), next_(0), calls_(0)
  {}
  int64_t calls() const { return calls_; }
  int64_t documents_read() const { return next_; }
protected:
  int inner_init(ObDASIterParam &) override { return OB_SUCCESS; }
  int inner_reuse() override { next_ = 0; return OB_SUCCESS; }
  int inner_release() override { return OB_SUCCESS; }
  int inner_get_next_rows(int64_t &, int64_t) override { return OB_NOT_SUPPORTED; }
  int inner_get_next_row() override
  {
    ++calls_;
    if (next_ >= documents_.size()) {
      return OB_ITER_END;
    }
    const auto &document = documents_[next_++];
    ObDatum &text = text_.locate_datum_for_write(*eval_ctx_);
    if (document.is_null_) {
      text.set_null();
    } else {
      text.set_string(document.text_.data(), document.text_.size());
    }
    // Storage can overwrite an already initialized datum directly. Restoring
    // its result pointer here would hide numeric ids borrowing the token arena.
    ObDatum &id = id_.locate_expr_datum(*eval_ctx_);
    if (numeric_ids_) {
      id.set_uint(next_);
    } else {
      id.set_string(document.id_.data(), document.id_.size());
    }
    text_.set_evaluated_projected(*eval_ctx_);
    id_.set_evaluated_projected(*eval_ctx_);
    return OB_SUCCESS;
  }
private:
  const std::vector<FtsBatchDocument> &documents_;
  ObExpr &text_;
  ObExpr &id_;
  bool numeric_ids_;
  int64_t next_;
  int64_t calls_;
};

class FtsBatchScan : public ObTableScanOp
{
public:
  FtsBatchScan(ObExecContext &context, const ObTableScanSpec &spec)
      : ObTableScanOp(context, spec, nullptr)
  {}
  int prepare(ObDASIter &source)
  {
    need_init_before_get_row_ = false;
    output_ = &source;
    return fts_index_.init(static_cast<const ObTableScanSpec &>(spec_).is_fts_index_aux_,
        ObString("ngram.1"), ObString("{\"ngram_token_size\":1}"));
  }
  const ObBatchRows &batch() const { return brs_; }
  void sample_documents(const SampleInfo *sample)
  {
    tsc_rtdef_.scan_rtdef_.sample_info_ = sample;
  }
  void restart_after_sampling()
  {
    tsc_rtdef_.scan_rtdef_.sample_info_ = nullptr;
    iter_end_ = false;
  }
};

TEST(FtsIndexBatch, stays_within_document_and_preserves_rows_across_batch_boundaries)
{
  // The final document has one token. The preceding document exceeds a batch,
  // and empty/NULL documents between them must neither end the scan nor leak
  // their document id into the next document's tokens.
  const std::vector<FtsBatchDocument> documents = {
      {"first", "abca", false}, {"empty", "", false},
      {"null", "ignored", true}, {"space", " \t\n", false},
      {"long", "abcdefghijklmnopqrstuvwxyza", false}, {"last", "z", false}};
  using Counts = std::map<std::string, std::pair<uint64_t, uint64_t>>;
  std::map<std::string, Counts> expected;
  expected["first"] = {{"a", {2, 4}}, {"b", {1, 4}}, {"c", {1, 4}}};
  for (char c = 'a'; c <= 'z'; ++c) {
    expected["long"][std::string(1, c)] = {c == 'a' ? 2 : 1, 27};
  }
  expected["last"] = {{"z", {1, 1}}};

  for (const bool numeric_ids : {false, true}) {
    std::map<std::string, Counts> expected_ids;
    for (size_t i = 0; i < documents.size(); ++i) {
      const auto found = expected.find(documents[i].id_);
      if (found != expected.end()) {
        expected_ids[numeric_ids ? std::to_string(i + 1) : documents[i].id_] = found->second;
      }
    }
    for (const bool is_index : {false, true}) {
      for (const int64_t capacity : {int64_t(1), int64_t(3), FtsBatchFrame::CAPACITY}) {
        SCOPED_TRACE(is_index);
        SCOPED_TRACE(capacity);
        SCOPED_TRACE(numeric_ids);
        ObArenaAllocator allocator;
        FtsBatchFrame frames[4];
        char *frame_pointers[4];
        ObExecContext context(allocator);
        ObTableScanSpec spec(allocator, PHY_TABLE_SCAN);
        spec.is_fts_ddl_ = true;
        spec.is_fts_index_aux_ = is_index;
        spec.max_batch_size_ = FtsBatchFrame::CAPACITY;
        ASSERT_EQ(OB_SUCCESS, spec.output_.init(4));
        const int64_t word_idx = is_index ? 0 : 1;
        const int64_t id_idx = is_index ? 1 : 0;
        for (int64_t i = 0; i < 4; ++i) {
          frame_pointers[i] = frames[i].memory_;
          ObExpr &expr = frames[i].expr_;
          expr.frame_idx_ = i;
          expr.type_ = i == word_idx ? T_FUN_SYS_WORD_SEGMENT
              : i == id_idx ? T_REF_COLUMN : i == 2 ? T_FUN_SYS_WORD_COUNT : T_FUN_SYS_DOC_LENGTH;
          if (i == id_idx && numeric_ids) {
            expr.obj_meta_.set_uint64();
            expr.datum_meta_.type_ = ObUInt64Type;
          } else if (i < 2) {
            expr.obj_meta_.set_varchar();
            expr.obj_meta_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
            expr.datum_meta_.type_ = ObVarcharType;
            expr.datum_meta_.cs_type_ = CS_TYPE_UTF8MB4_BIN;
          } else {
            expr.obj_meta_.set_uint64();
            expr.datum_meta_.type_ = ObUInt64Type;
          }
          expr.obj_datum_map_ = ObDatum::get_obj_datum_map_type(expr.obj_meta_.get_type());
          ASSERT_EQ(OB_SUCCESS, spec.output_.push_back(&expr));
        }
        context.set_frames(frame_pointers);
        FtsBatchScan scan(context, spec);
        // DAS iterators live in an arena; this mock borrows its documents and
        // owns no resources needing destruction.
        void *source_memory = allocator.alloc(sizeof(FtsBatchSource));
        ASSERT_NE(nullptr, source_memory);
        auto *source = new (source_memory) FtsBatchSource(documents,
            frames[word_idx].expr_, frames[id_idx].expr_, numeric_ids);
        ObDASIterParam param;
        param.eval_ctx_ = &scan.get_eval_ctx();
        param.exec_ctx_ = &context;
        param.output_ = &spec.output_;
        ASSERT_EQ(OB_SUCCESS, source->init(param));
        ASSERT_EQ(OB_SUCCESS, scan.prepare(*source));
        SampleInfo sample;
        scan.sample_documents(&sample);
        for (int64_t i = 0; i < documents.size(); ++i) {
          ASSERT_EQ(OB_SUCCESS, scan.inner_get_next_batch(capacity));
          EXPECT_EQ(1, scan.batch().size_);
          EXPECT_FALSE(scan.batch().end_);
          EXPECT_EQ(i + 1, source->documents_read());
        }
        ASSERT_EQ(OB_SUCCESS, scan.inner_get_next_batch(capacity));
        EXPECT_TRUE(scan.batch().end_);
        EXPECT_EQ(0, scan.batch().size_);
        ASSERT_EQ(OB_SUCCESS, source->reuse());
        scan.restart_after_sampling();
        std::map<std::string, Counts> actual;
        bool reached_end = false;
        for (int batch = 0; batch < 100 && !reached_end; ++batch) {
          ASSERT_EQ(OB_SUCCESS, scan.inner_get_next_batch(capacity));
          const ObBatchRows &rows = scan.batch();
          ASSERT_LE(rows.size_, capacity);
          reached_end = rows.end_;
          std::string batch_id;
          for (int64_t i = 0; i < rows.size_; ++i) {
            const ObDatum &id = frames[id_idx].expr_.locate_batch_datums(scan.get_eval_ctx())[i];
            const ObDatum &word = frames[word_idx].expr_.locate_batch_datums(scan.get_eval_ctx())[i];
            const std::string id_string = numeric_ids ? std::to_string(id.get_uint())
                : std::string(id.ptr_, id.len_);
            const std::string word_string(word.ptr_, word.len_);
            if (0 == i) { batch_id = id_string; }
            EXPECT_EQ(batch_id, id_string);
            EXPECT_EQ(0, actual[id_string].count(word_string));
            actual[id_string][word_string] = {
                frames[2].expr_.locate_batch_datums(scan.get_eval_ctx())[i].get_uint(),
                frames[3].expr_.locate_batch_datums(scan.get_eval_ctx())[i].get_uint()};
            for (int64_t j = 0; j < 4; ++j) {
              EXPECT_TRUE(frames[j].expr_.get_evaluated_flags(scan.get_eval_ctx()).at(i));
              EXPECT_TRUE(frames[j].expr_.get_eval_info(scan.get_eval_ctx()).projected_);
            }
          }
          if (rows.size_ > 0) {
            ASSERT_FALSE(rows.end_);
            EXPECT_EQ(numeric_ids ? std::to_string(source->documents_read())
                : documents[source->documents_read() - 1].id_, batch_id);
          }
        }
        EXPECT_TRUE(reached_end);
        EXPECT_EQ(expected_ids, actual);
        EXPECT_EQ(documents.size(), source->documents_read());
        const int64_t end_calls = source->calls();
        ASSERT_EQ(OB_SUCCESS, scan.inner_get_next_batch(capacity));
        EXPECT_TRUE(scan.batch().end_);
        EXPECT_EQ(0, scan.batch().size_);
        EXPECT_EQ(end_calls, source->calls());
      }
    }
  }
}
} // namespace
} // namespace sql
} // namespace oceanbase
