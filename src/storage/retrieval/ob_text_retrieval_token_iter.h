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

#ifndef OB_TEXT_RETRIEVAL_TOKEN_ITER_H_
#define OB_TEXT_RETRIEVAL_TOKEN_ITER_H_

#include "lib/container/ob_se_array.h"
#include "query/engine/expr/ob_expr.h"
#include "ob_block_max_iter.h"
#include "ob_i_sparse_retrieval_iter.h"
namespace oceanbase
{
namespace sql
{
class ObDASScanIter;
}
namespace storage
{
class ObTableScanParam;
class ObBlockMaxScoreIterator;
struct ObTextRetrievalScanIterParam
{
  ObTextRetrievalScanIterParam()
    : allocator_(nullptr),
      mem_context_(nullptr),
      inv_idx_scan_param_(nullptr),
      inv_idx_agg_param_(nullptr),
      inv_idx_scan_iter_(nullptr),
      inv_idx_agg_iter_(nullptr),
      inv_idx_agg_expr_(nullptr),
      eval_ctx_(nullptr),
      relevance_expr_(nullptr),
      inv_scan_doc_length_col_(nullptr),
      inv_scan_domain_id_col_(nullptr),
      inv_idx_agg_cache_mode_(false)
  {}

  ObArenaAllocator *allocator_;
  lib::MemoryContext mem_context_;
  ObTableScanParam *inv_idx_scan_param_;
  ObTableScanParam *inv_idx_agg_param_;
  sql::ObDASScanIter *inv_idx_scan_iter_;
  sql::ObDASScanIter *inv_idx_agg_iter_;
  sql::ObExpr *inv_idx_agg_expr_;
  sql::ObEvalCtx *eval_ctx_;
  sql::ObExpr *relevance_expr_;
  sql::ObExpr *inv_scan_doc_length_col_;
  sql::ObExpr *inv_scan_domain_id_col_;
  bool inv_idx_agg_cache_mode_;
};

class ObTextRetrievalTokenIter final : public ObISparseRetrievalDimIter
{
public:
  ObTextRetrievalTokenIter();
  virtual ~ObTextRetrievalTokenIter() {}

  int init(const ObTextRetrievalScanIterParam &iter_param);
  void reset();
  void reuse();

  // Sparse Retrieval Dimension Iter Interfaces
  virtual int get_next_row() override;
  virtual int get_next_batch(const int64_t capacity, int64_t &count) override;
  virtual int advance_to(const ObDatum &id_datum) override;
  virtual int update_scan_param(const ObString &token, common::ObArenaAllocator &allocator);
public:
  sql::ObBitVector *get_skip() { return skip_; }
  int get_token_doc_cnt(int64_t &token_doc_cnt) const;
  double get_max_token_relevance() const { return max_token_relevance_; }
private:
  int init_calc_exprs_in_relevance_expr();
  void clear_batch_wise_evaluated_flag(const int64_t count);
  void clear_row_wise_evaluated_flag();
  int fill_token_doc_cnt();
  int fill_token_weight();
  int eval_relevance_expr();
  int batch_eval_relevance_expr(const int64_t count);
  int estimate_token_doc_cnt();
  inline bool need_inv_idx_agg() { return inv_idx_agg_iter_ != nullptr; }
  inline bool need_calc_relevance() { return inv_idx_agg_iter_ != nullptr; }
public:
  static const int64_t INV_IDX_ROWKEY_COL_CNT = 2;
private:
  lib::MemoryContext mem_context_;
  ObArenaAllocator *allocator_;
  ObTableScanParam *inv_idx_scan_param_;
  ObTableScanParam *inv_idx_agg_param_;
  sql::ObDASScanIter *inv_idx_scan_iter_;
  sql::ObDASScanIter *inv_idx_agg_iter_;
  sql::ObExpr *inv_idx_agg_expr_;
  sql::ObEvalCtx *eval_ctx_;
  sql::ObExpr *relevance_expr_;
  sql::ObExpr *inv_scan_doc_length_col_; // read from inv_scan_table
  sql::ObExpr *inv_scan_domain_id_col_; // read from inv_scan_table
  sql::ObExpr *token_weight_expr_; // fill the expr from with max_token_relevance_
  common::ObSEArray<sql::ObExpr *, 2> relevance_calc_exprs_;
  sql::ObBitVector *skip_;
  int64_t max_batch_size_;
  int64_t token_doc_cnt_;
  double max_token_relevance_;
  ObDocIdExt advance_doc_id_;
  bool token_doc_cnt_calculated_;
  bool inv_idx_agg_cache_mode_;
  bool is_inited_;
  DISALLOW_COPY_AND_ASSIGN(ObTextRetrievalTokenIter);
};

// Find the first cached posting at or after a target. The array is ordered by
// cmp_func and remains borrowed for the duration of this call.
OB_INLINE int find_cached_text_doc_id(
    const ObDocIdExt *doc_ids, const int64_t count, const int64_t begin,
    const common::ObDatumCmpFuncType cmp_func, const ObDatum &id_datum, int64_t &position)
{
  int ret = OB_SUCCESS;
  int64_t low = begin;
  int64_t high = count;
  int cmp_ret = 0;
  if (OB_UNLIKELY(low < 0 || low > count)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (low < count
             && OB_FAIL(cmp_func(id_datum, doc_ids[low].get_datum(), cmp_ret, nullptr))) {
  } else if (low == count || cmp_ret <= 0) {
    // Repeated pivots and the current posting need only one comparison.
  } else {
    ++low;
    int64_t step = 1;
    // Keep adjacent advances cheap, while bounding the search for distant
    // pivots. Every posting in a token's cached batch is ordered by doc ID.
    while (OB_SUCC(ret) && low < count) {
      const int64_t probe = low + MIN(step, count - low) - 1;
      if (OB_FAIL(cmp_func(id_datum, doc_ids[probe].get_datum(), cmp_ret, nullptr))) {
      } else if (cmp_ret <= 0) {
        high = probe;
        break;
      } else {
        low = probe + 1;
        const int64_t remaining = count - low;
        step = step <= remaining / 2 ? step * 2 : remaining;
      }
    }
    while (OB_SUCC(ret) && low < high) {
      const int64_t mid = low + (high - low) / 2;
      if (OB_FAIL(cmp_func(id_datum, doc_ids[mid].get_datum(), cmp_ret, nullptr))) {
      } else if (cmp_ret <= 0) {
        high = mid;
      } else {
        low = mid + 1;
      }
    }
  }
  if (OB_SUCC(ret)) {
    position = low;
  }
  return ret;
}

class ObTextRetrievalDaaTTokenIter final : public ObISRDaaTDimIter
{
  friend class TextTokenBatchTestPeer;
public:
  ObTextRetrievalDaaTTokenIter();
  virtual ~ObTextRetrievalDaaTTokenIter() {}
  int init(const ObTextRetrievalScanIterParam &iter_param);
  void reuse();
  void reset();
  virtual int get_next_row() override;
  virtual int get_next_batch(const int64_t capacity, int64_t &count) override;
  virtual int advance_to(const ObDatum &id_datum) override;
  virtual int get_curr_score(double &score) const override;
  virtual int get_curr_id(const ObDatum *&id_datum) const override;
  virtual int get_next_doc_id_batch(const ObDocIdExt *&ids, int64_t &count) override;
  virtual bool is_fixed16_doc_id_batch() const override { return doc_id_batch_fixed16_; }
public:
  int get_token_doc_cnt(int64_t &token_doc_cnt) const { return token_iter_->get_token_doc_cnt(token_doc_cnt); }
  virtual int get_dim_max_score(double &score) override {
    int ret = OB_SUCCESS;
    score = token_iter_->get_max_token_relevance();
    if (OB_UNLIKELY(score < 0.0)) {
      ret = OB_ERR_UNEXPECTED;
    }
    return ret;
  }
private:
  int save_relevances_and_docids();
  int save_docids();
  ObArenaAllocator *allocator_;
  ObTextRetrievalTokenIter *token_iter_;
  sql::ObEvalCtx *eval_ctx_;
  sql::ObExpr *relevance_expr_;
  sql::ObExpr *inv_scan_domain_id_col_;
  int64_t max_batch_size_;
  int64_t cur_idx_;
  int64_t count_;
  ObFixedArray<double, ObIAllocator> relevance_; // when ~ObFixedArray(), wikll destory itself
  ObFixedArray<ObDocIdExt, ObIAllocator> doc_id_;
  common::ObDatumCmpFuncType cmp_func_;
  bool doc_id_batch_fixed16_;
  bool is_inited_;
  DISALLOW_COPY_AND_ASSIGN(ObTextRetrievalDaaTTokenIter);
};

class ObTextRetrievalBlockMaxIter final : public ObISRDimBlockMaxIter
{
public:
  ObTextRetrievalBlockMaxIter();
  ~ObTextRetrievalBlockMaxIter() {}
  int init(
      const ObTextRetrievalScanIterParam &iter_param,
      const ObBlockMaxScoreIterParam &block_max_iter_param,
      ObTableScanParam &scan_param);
  void reset();
  void reuse();

  virtual int get_next_row() override;
  virtual int get_next_batch(const int64_t capacity, int64_t &count) override;
  virtual int advance_to(const ObDatum &id_datum) override;

  virtual int get_curr_score(double &score) const override;
  virtual int get_curr_id(const ObDatum *&id_datum) const override;
  virtual int get_dim_max_score(double &score) override;
  virtual int advance_shallow(const ObDatum &id_datum, const bool inclusive) override;
  virtual int get_curr_block_max_info(const ObMaxScoreTuple *&max_score_tuple) override;
  virtual bool in_shallow_status() const override;
  // currently, for text retrieval, total_doc_cnt and token_doc_cnt is required before block max calculation
  int init_block_max_iter(const int64_t total_doc_cnt, const double avg_doc_token_cnt);
private:
  int calc_dim_max_score(
      const ObBlockMaxScoreIterParam &block_max_iter_param,
      const ObBlockMaxBM25RankingParam &ranking_param,
      ObTableScanParam &scan_param);
private:
  ObTextRetrievalDaaTTokenIter token_iter_;
  ObBlockMaxScoreIterator block_max_iter_;
  const ObBlockMaxScoreIterParam *block_max_iter_param_;
  ObTableScanParam *block_max_scan_param_;
  ObBlockMaxBM25RankingParam ranking_param_;
  const ObDatum *curr_id_;
  const ObMaxScoreTuple *max_score_tuple_;
  double dim_max_score_;
  bool block_max_inited_;
  bool in_shallow_status_;
  bool is_inited_;
  DISALLOW_COPY_AND_ASSIGN(ObTextRetrievalBlockMaxIter);
};

} // namespace storage
} // namespace oceanbase


#endif
