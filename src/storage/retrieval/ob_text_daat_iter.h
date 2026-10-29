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

#ifndef OB_TEXT_DAAT_ITER_H_
#define OB_TEXT_DAAT_ITER_H_

#include "query/ob_sql_define.h"
#include "ob_inv_idx_param_estimator.h"
#include "ob_sparse_bmw_iter.h"
#include "ob_sparse_daat_iter.h"
#include "ob_text_retrieval_token_iter.h"

namespace oceanbase
{
namespace sql
{
  class ObDASScanIter;
}
namespace storage
{

// A natural-language membership query only needs the ordered union of postings.
// Keep the heads until the caller has copied the returned ID: refilling a
// dimension can overwrite its current document-ID buffer.
class TextMembershipUnion
{
  friend class TextMembershipUnionTestPeer;
public:
  static constexpr int64_t MAX_DIM_COUNT = 3;
  TextMembershipUnion();
  int init(common::ObIArray<ObISRDaaTDimIter *> &dim_iters, sql::ObDatumMeta id_meta);
  void reset();
  void reuse();
  int get_next_id(const common::ObDatum *&id_datum);
private:
  ObISRDaaTDimIter *dim_iters_[MAX_DIM_COUNT];
  const common::ObDatum *heads_[MAX_DIM_COUNT];
  common::ObDatumCmpFuncType cmp_func_;
  int64_t dim_count_;
  uint8_t pending_mask_;
  bool is_inited_;
  DISALLOW_COPY_AND_ASSIGN(TextMembershipUnion);
};

// Batch union over each token's owned ID buffer. No source is refilled after
// returning the first ID in a batch, so every output can borrow its bytes until
// the next get_next_ids() call. A source boundary yields a partial batch.
class TextMembershipBatchUnion
{
  friend class TextMembershipUnionTestPeer;
public:
  static constexpr int64_t MAX_DIM_COUNT = TextMembershipUnion::MAX_DIM_COUNT;
  TextMembershipBatchUnion();
  int init(common::ObIArray<ObISRDaaTDimIter *> &dim_iters, sql::ObDatumMeta id_meta);
  void reset();
  void reuse();
  int get_next_ids(const int64_t capacity, const ObDocIdExt **ids, int64_t &count);
private:
  int refill_sources();
  template <bool binary_ids, bool fixed16_ids, bool shared_prefix8>
  int merge_ids(const int64_t capacity, const ObDocIdExt **ids, int64_t &count);
  ObISRDaaTDimIter *dim_iters_[MAX_DIM_COUNT];
  const ObDocIdExt *source_ids_[MAX_DIM_COUNT];
  int64_t positions_[MAX_DIM_COUNT];
  int64_t counts_[MAX_DIM_COUNT];
  bool source_ended_[MAX_DIM_COUNT];
  common::ObDatumCmpFuncType cmp_func_;
  int64_t dim_count_;
  bool is_inited_;
  DISALLOW_COPY_AND_ASSIGN(TextMembershipBatchUnion);
};

struct ObTextDaaTParam
{
  ObTextDaaTParam()
    : dim_iters_(nullptr), base_param_(nullptr), allocator_(nullptr), relevance_collector_(nullptr),
      bm25_param_est_ctx_(), mode_flag_(sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE),
      function_lookup_mode_(false), membership_only_(false) {}
  ~ObTextDaaTParam() {}
  TO_STRING_KV(K_(base_param), KP_(dim_iters), K_(bm25_param_est_ctx), K_(mode_flag),
      K_(function_lookup_mode), K_(membership_only));
  ObIArray<ObISRDaaTDimIter *> *dim_iters_;
  ObSparseRetrievalMergeParam *base_param_;
  common::ObArenaAllocator *allocator_;
  ObSRDaaTRelevanceCollector *relevance_collector_;
  ObBM25ParamEstCtx bm25_param_est_ctx_;
  sql::ObMatchAgainstMode mode_flag_;
  bool function_lookup_mode_;
  bool membership_only_;
};

class ObTextDaaTIter final : public ObSRDaaTIterImpl
{
  friend class TextDaaTIterTestPeer;
public:
  ObTextDaaTIter() : ObSRDaaTIterImpl(),
      bm25_param_estimator_(),
      mode_flag_(sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE),
      function_lookup_mode_(false),
      membership_only_(false),
      membership_union_(),
      membership_batch_union_(),
      membership_ids_() {}
  virtual ~ObTextDaaTIter() { reset(); }

  int init(const ObTextDaaTParam &param);
  virtual int get_next_rows(const int64_t capacity, int64_t &count) override;
  virtual void reuse(const bool switch_tablet = false) override;
  virtual void reset() override;
protected:
  virtual int pre_process() override;
  virtual int do_one_merge_round(int64_t &count) override;
  int project_membership_results(const ObDocIdExt *const *ids, const int64_t count);
protected:
  ObBM25ParamEstimator bm25_param_estimator_;
  sql::ObMatchAgainstMode mode_flag_;
  bool function_lookup_mode_;
  bool membership_only_;
  TextMembershipUnion membership_union_;
  TextMembershipBatchUnion membership_batch_union_;
  ObFixedArray<const ObDocIdExt *, ObIAllocator> membership_ids_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObTextDaaTIter);
};


class ObTextBMWIter final : public ObSRBMWIterImpl
{
public:
  ObTextBMWIter();
  virtual ~ObTextBMWIter() { reset(); }
  virtual void reuse(const bool switch_tablet = false) override;
  virtual void reset() override;
  int init(const ObTextDaaTParam &param);
protected:
  virtual int get_next_rows(const int64_t capacity, int64_t &count) override;
  virtual int init_before_wand_process() override;
protected:
  ObBM25ParamEstimator bm25_param_estimator_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObTextBMWIter);
};

} // namespace storage
} // namespace oceanbase

#endif
