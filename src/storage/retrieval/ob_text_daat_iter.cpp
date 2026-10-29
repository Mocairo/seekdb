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

#define USING_LOG_PREFIX STORAGE

#include <bit>

#include "lib/utility/utility.h"
#include "ob_text_daat_iter.h"

namespace oceanbase
{
namespace storage
{
TextMembershipUnion::TextMembershipUnion()
  : dim_iters_{nullptr, nullptr, nullptr}, heads_{nullptr, nullptr, nullptr},
    cmp_func_(nullptr), dim_count_(0), pending_mask_(0), is_inited_(false)
{
}

int TextMembershipUnion::init(
    common::ObIArray<ObISRDaaTDimIter *> &dim_iters,
    sql::ObDatumMeta id_meta)
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
  } else if (dim_iters.count() <= 0 || dim_iters.count() > MAX_DIM_COUNT) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters.count(); ++i) {
      if (OB_ISNULL(dim_iters.at(i))) {
        ret = OB_INVALID_ARGUMENT;
      }
    }
    if (OB_SUCC(ret)) {
      cmp_func_ = get_domain_id_cmp_func(id_meta.type_, id_meta.cs_type_);
      if (OB_ISNULL(cmp_func_)) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        dim_count_ = dim_iters.count();
        for (int64_t i = 0; i < dim_count_; ++i) {
          dim_iters_[i] = dim_iters.at(i);
        }
        is_inited_ = true;
        reuse();
      }
    }
  }
  return ret;
}

void TextMembershipUnion::reset()
{
  cmp_func_ = nullptr;
  dim_count_ = 0;
  pending_mask_ = 0;
  is_inited_ = false;
  for (int64_t i = 0; i < MAX_DIM_COUNT; ++i) {
    dim_iters_[i] = nullptr;
    heads_[i] = nullptr;
  }
}

void TextMembershipUnion::reuse()
{
  for (int64_t i = 0; i < MAX_DIM_COUNT; ++i) {
    heads_[i] = nullptr;
  }
  pending_mask_ = is_inited_ ? static_cast<uint8_t>((1U << dim_count_) - 1) : 0;
}

int TextMembershipUnion::get_next_id(const common::ObDatum *&id_datum)
{
  int ret = OB_SUCCESS;
  id_datum = nullptr;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    const int64_t dim_count = dim_count_;
    for (int64_t i = 0; OB_SUCC(ret) && i < dim_count; ++i) {
      if (0 != (pending_mask_ & (1U << i))) {
        if (OB_FAIL(dim_iters_[i]->get_next_row())) {
          if (OB_ITER_END == ret) {
            heads_[i] = nullptr;
            ret = OB_SUCCESS;
          }
        } else if (OB_FAIL(dim_iters_[i]->get_curr_id(heads_[i]))) {
        } else if (OB_ISNULL(heads_[i])) {
          ret = OB_ERR_UNEXPECTED;
        }
      }
    }
    pending_mask_ = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < dim_count; ++i) {
      if (nullptr == heads_[i]) {
      } else if (nullptr == id_datum) {
        id_datum = heads_[i];
        pending_mask_ = static_cast<uint8_t>(1U << i);
      } else {
        int cmp_ret = 0;
        if (OB_FAIL(cmp_func_(*heads_[i], *id_datum, cmp_ret, nullptr))) {
        } else if (cmp_ret < 0) {
          id_datum = heads_[i];
          pending_mask_ = static_cast<uint8_t>(1U << i);
        } else if (0 == cmp_ret) {
          // Match the existing small merger's tie order and consume every
          // posting for this ID before returning another document.
          id_datum = heads_[i];
          pending_mask_ |= static_cast<uint8_t>(1U << i);
        }
      }
    }
    if (OB_SUCC(ret) && nullptr == id_datum) {
      ret = OB_ITER_END;
    }
  }
  if (OB_SUCCESS != ret) {
    id_datum = nullptr;
  }
  return ret;
}

TextMembershipBatchUnion::TextMembershipBatchUnion()
  : dim_iters_{nullptr, nullptr, nullptr}, source_ids_{nullptr, nullptr, nullptr},
    positions_{0, 0, 0}, counts_{0, 0, 0}, source_ended_{false, false, false},
    cmp_func_(nullptr), dim_count_(0), is_inited_(false)
{
}

int TextMembershipBatchUnion::init(
    common::ObIArray<ObISRDaaTDimIter *> &dim_iters, sql::ObDatumMeta id_meta)
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
  } else if (dim_iters.count() <= 0 || dim_iters.count() > MAX_DIM_COUNT) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters.count(); ++i) {
      if (OB_ISNULL(dim_iters.at(i))) {
        ret = OB_INVALID_ARGUMENT;
      }
    }
    if (OB_SUCC(ret)) {
      cmp_func_ = get_domain_id_cmp_func(id_meta.type_, id_meta.cs_type_);
      if (OB_ISNULL(cmp_func_)) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        dim_count_ = dim_iters.count();
        for (int64_t i = 0; i < dim_count_; ++i) {
          dim_iters_[i] = dim_iters.at(i);
        }
        is_inited_ = true;
        reuse();
      }
    }
  }
  return ret;
}

void TextMembershipBatchUnion::reset()
{
  cmp_func_ = nullptr;
  dim_count_ = 0;
  is_inited_ = false;
  for (int64_t i = 0; i < MAX_DIM_COUNT; ++i) {
    dim_iters_[i] = nullptr;
  }
  reuse();
}

void TextMembershipBatchUnion::reuse()
{
  for (int64_t i = 0; i < MAX_DIM_COUNT; ++i) {
    source_ids_[i] = nullptr;
    positions_[i] = counts_[i] = 0;
    source_ended_[i] = false;
  }
}

int TextMembershipBatchUnion::refill_sources()
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < dim_count_; ++i) {
    if (positions_[i] < counts_[i] || source_ended_[i]) {
    } else {
      source_ids_[i] = nullptr;
      positions_[i] = counts_[i] = 0;
      ret = dim_iters_[i]->get_next_doc_id_batch(source_ids_[i], counts_[i]);
      if (OB_ITER_END == ret) {
        source_ended_[i] = true;
        ret = OB_SUCCESS;
      } else if (OB_SUCC(ret) && OB_UNLIKELY(0 == counts_[i])) {
        ret = OB_ERR_UNEXPECTED;
      }
      if (OB_SUCC(ret) && OB_UNLIKELY(counts_[i] < 0
          || (counts_[i] > 0 && nullptr == source_ids_[i]))) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
  }
  return ret;
}

template <bool binary_ids, bool fixed16_ids, bool shared_prefix8>
int TextMembershipBatchUnion::merge_ids(
    const int64_t capacity, const ObDocIdExt **ids, int64_t &output_count)
{
  int ret = OB_SUCCESS;
  // Publish progress once instead of carrying output-reference loads/stores
  // through every pointer append in the merge loop.
  int64_t count = output_count;
  const auto compare = [this](const ObDocIdExt *left, const ObDocIdExt *right, int &cmp)
      __attribute__((always_inline)) {
    if (fixed16_ids) {
      constexpr int64_t offset = shared_prefix8 ? 8 : 0;
      const char *left_bytes = shared_prefix8
          ? left->get_inline_fixed16_data() : left->get_datum().ptr_;
      const char *right_bytes = shared_prefix8
          ? right->get_inline_fixed16_data() : right->get_datum().ptr_;
      const int byte_cmp = MEMCMP(left_bytes + offset, right_bytes + offset, 16 - offset);
      cmp = byte_cmp > 0 ? 1 : (byte_cmp < 0 ? -1 : 0);
      return OB_SUCCESS;
    } else {
      return binary_ids
          ? compare_binary_domain_ids(left->get_datum(), right->get_datum(), cmp, nullptr)
          : cmp_func_(left->get_datum(), right->get_datum(), cmp, nullptr);
    }
  };
  while (OB_SUCC(ret) && count < capacity) {
    int64_t active[MAX_DIM_COUNT];
    int64_t active_count = 0;
    bool need_refill = false;
    for (int64_t i = 0; i < dim_count_; ++i) {
      if (positions_[i] < counts_[i]) {
        active[active_count++] = i;
      } else {
        need_refill |= !source_ended_[i];
      }
    }
    if (need_refill) {
      // Previously returned IDs may borrow the exhausted source's buffer.
      // Only the next get_next_ids() call may refill it.
      break;
    } else if (0 == active_count) {
      ret = 0 == count ? OB_ITER_END : OB_SUCCESS;
      break;
    } else if (1 == active_count) {
      const int64_t dim = active[0];
      const int64_t n = MIN(capacity - count, counts_[dim] - positions_[dim]);
      const ObDocIdExt *current = source_ids_[dim] + positions_[dim];
      for (int64_t i = 0; i < n; ++i) {
        ids[count++] = current++;
      }
      positions_[dim] += n;
    } else if (2 == active_count) {
      const int64_t left = active[0];
      const int64_t right = active[1];
      const ObDocIdExt *l = source_ids_[left] + positions_[left];
      const ObDocIdExt *r = source_ids_[right] + positions_[right];
      const ObDocIdExt *lend = source_ids_[left] + counts_[left];
      const ObDocIdExt *rend = source_ids_[right] + counts_[right];
      while (OB_SUCC(ret) && count < capacity && l < lend && r < rend) {
        int cmp = 0;
        if (OB_FAIL(compare(l, r, cmp))) {
        } else {
          // Active dimensions retain source order, so the right source wins
          // equal IDs while both postings are consumed.
          ids[count++] = cmp < 0 ? l : r;
          l += cmp <= 0;
          r += cmp >= 0;
        }
      }
      positions_[left] = l - source_ids_[left];
      positions_[right] = r - source_ids_[right];
    } else {
      const int64_t first = active[0];
      const int64_t second = active[1];
      const int64_t third = active[2];
      const ObDocIdExt *a = source_ids_[first] + positions_[first];
      const ObDocIdExt *b = source_ids_[second] + positions_[second];
      const ObDocIdExt *c = source_ids_[third] + positions_[third];
      const ObDocIdExt *aend = source_ids_[first] + counts_[first];
      const ObDocIdExt *bend = source_ids_[second] + counts_[second];
      const ObDocIdExt *cend = source_ids_[third] + counts_[third];
      while (OB_SUCC(ret) && count < capacity && a < aend && b < bend && c < cend) {
        if (shared_prefix8) {
          // The proof covers all remaining ordinary sixteen-byte IDs and their
          // common prefix. Load every suffix independently before selecting a
          // minimum, so C's comparison reuses keys instead of rereading next.
          uint64_t a_key = 0;
          uint64_t b_key = 0;
          uint64_t c_key = 0;
          MEMCPY(&a_key, a->get_inline_fixed16_data() + 8, sizeof(a_key));
          MEMCPY(&b_key, b->get_inline_fixed16_data() + 8, sizeof(b_key));
          MEMCPY(&c_key, c->get_inline_fixed16_data() + 8, sizeof(c_key));
          if constexpr (std::endian::native == std::endian::little) {
            a_key = ob_htonll(a_key);
            b_key = ob_htonll(b_key);
            c_key = ob_htonll(c_key);
          }
          const bool a_lt_b = a_key < b_key;
          const uint64_t min_key = a_lt_b ? a_key : b_key;
          const bool c_le_min = c_key <= min_key;
          const bool c_ge_min = c_key >= min_key;
          ids[count++] = c_le_min ? c : (a_lt_b ? a : b);
          a += c_ge_min && a_key <= b_key;
          b += c_ge_min && a_key >= b_key;
          c += c_le_min;
        } else {
          int ab_cmp = 0;
          int c_cmp = 0;
          if (OB_FAIL(compare(a, b, ab_cmp))) {
          } else {
            const ObDocIdExt *next = ab_cmp < 0 ? a : b;
            if (OB_FAIL(compare(c, next, c_cmp))) {
            } else {
              ids[count++] = c_cmp <= 0 ? c : next;
              a += c_cmp >= 0 && ab_cmp <= 0;
              b += c_cmp >= 0 && ab_cmp >= 0;
              c += c_cmp <= 0;
            }
          }
        }
      }
      positions_[first] = a - source_ids_[first];
      positions_[second] = b - source_ids_[second];
      positions_[third] = c - source_ids_[third];
    }
  }
  output_count = count;
  return ret;
}

int TextMembershipBatchUnion::get_next_ids(
    const int64_t capacity, const ObDocIdExt **ids, int64_t &count)
{
  int ret = OB_SUCCESS;
  count = 0;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(capacity < 0 || (capacity > 0 && nullptr == ids))) {
    ret = OB_INVALID_ARGUMENT;
  } else if (0 == capacity) {
  } else if (OB_FAIL(refill_sources())) {
  } else {
    if (cmp_func_ != compare_binary_domain_ids) {
      ret = merge_ids<false, false, false>(capacity, ids, count);
    } else {
      bool fixed16_ids = true;
      for (int64_t i = 0; fixed16_ids && i < dim_count_; ++i) {
        if (positions_[i] < counts_[i] && !dim_iters_[i]->is_fixed16_doc_id_batch()) {
          fixed16_ids = false;
        }
      }
      bool shared_prefix8 = fixed16_ids;
      const char *prefix = nullptr;
      int64_t active_count = 0;
      for (int64_t i = 0; shared_prefix8 && i < dim_count_; ++i) {
        if (positions_[i] < counts_[i]) {
          ++active_count;
          const char *first = source_ids_[i][positions_[i]].get_datum().ptr_;
          const char *last = source_ids_[i][counts_[i] - 1].get_datum().ptr_;
          // Equal endpoint prefixes cover the entire remaining sorted binary
          // span. Different spans must also have the same prefix.
          shared_prefix8 = 0 == MEMCMP(first, last, 8)
              && (nullptr == prefix || 0 == MEMCMP(prefix, first, 8));
          prefix = first;
        }
      }
      // No source can replace its owned span or proof inside this merge.
      if (shared_prefix8 && active_count > 1) {
        ret = merge_ids<true, true, true>(capacity, ids, count);
      } else if (fixed16_ids) {
        ret = merge_ids<true, true, false>(capacity, ids, count);
      } else {
        ret = merge_ids<true, false, false>(capacity, ids, count);
      }
    }
  }
  if (OB_SUCCESS != ret) {
    count = 0;
  }
  return ret;
}

int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_NOT_NULL(param.base_param_->relevance_expr_)
             && OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
    membership_only_ = param.membership_only_
        && param.dim_iters_->count() > 0
        && param.dim_iters_->count() <= TextMembershipUnion::MAX_DIM_COUNT;
    if (membership_only_
        && OB_FAIL(membership_union_.init(*param.dim_iters_, param.base_param_->id_proj_expr_->datum_meta_))) {
    }
    if (OB_SUCC(ret) && membership_only_ && param.base_param_->eval_ctx_->is_vectorized()) {
      membership_ids_.set_allocator(param.allocator_);
      if (OB_FAIL(membership_batch_union_.init(
          *param.dim_iters_, param.base_param_->id_proj_expr_->datum_meta_))) {
      } else if (OB_FAIL(membership_ids_.init(param.base_param_->max_batch_size_))) {
      } else if (OB_FAIL(membership_ids_.prepare_allocate(param.base_param_->max_batch_size_))) {
      }
    }
  }
  return ret;
}

void ObTextDaaTIter::reset()
{
  membership_batch_union_.reset();
  membership_ids_.reset();
  membership_union_.reset();
  membership_only_ = false;
  bm25_param_estimator_.reset();
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  if ((OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRDaaTIterImpl::reuse(switch_tablet);
  membership_union_.reuse();
  membership_batch_union_.reuse();
}

int ObTextDaaTIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (!membership_only_ || !iter_param_->eval_ctx_->is_vectorized()) {
    ret = ObSRDaaTIterImpl::get_next_rows(capacity, count);
  } else if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(capacity <= 0)) {
    count = 0;
  } else if (0 == dim_iters_->count()) {
    count = 0;
    ret = OB_ITER_END;
  } else if (iter_param_->limit_param_->is_valid()
      && output_row_cnt_ >= iter_param_->limit_param_->limit_) {
    count = 0;
    ret = OB_ITER_END;
  } else if (OB_FAIL(pre_process())) {
    count = 0;
  } else {
    count = 0;
    const common::ObLimitParam &limit = *iter_param_->limit_param_;
    const int64_t real_capacity = MIN(capacity, iter_param_->max_batch_size_);
    const int64_t fetch_capacity = limit.is_valid()
        ? MIN(real_capacity, limit.limit_ - output_row_cnt_) : real_capacity;
    int64_t skipped = 0;
    while (OB_SUCC(ret) && 0 == count) {
      int64_t fetched = 0;
      if (OB_FAIL(membership_batch_union_.get_next_ids(
          fetch_capacity, membership_ids_.get_data(), fetched))) {
      } else {
        skipped = limit.is_valid() && input_row_cnt_ < limit.offset_
            ? MIN(fetched, limit.offset_ - input_row_cnt_) : 0;
        input_row_cnt_ += fetched;
        count = fetched - skipped;
        output_row_cnt_ += count;
      }
    }
    if (OB_SUCC(ret) && count > 0) {
      if (OB_FAIL(project_membership_results(membership_ids_.get_data() + skipped, count))) {
      } else if (limit.is_valid() && output_row_cnt_ >= limit.limit_) {
        ret = OB_ITER_END;
      }
    }
  }
  return ret;
}

int ObTextDaaTIter::project_membership_results(
    const ObDocIdExt *const *ids, const int64_t count)
{
  int ret = OB_SUCCESS;
  ObExpr *id_expr = iter_param_->id_proj_expr_;
  ObExpr *relevance_expr = iter_param_->relevance_proj_expr_;
  ObEvalCtx &eval_ctx = *iter_param_->eval_ctx_;
  ObEvalCtx::BatchInfoScopeGuard guard(eval_ctx);
  if (id_expr->is_batch_result()) {
    ObDatum *datums = id_expr->locate_datums_for_update(eval_ctx, count);
    for (int64_t i = 0; i < count; ++i) {
      set_datum_func_(datums[i], *ids[i]);
    }
  } else {
    for (int64_t i = 0; i < count; ++i) {
      guard.set_batch_idx(i);
      set_datum_func_(id_expr->locate_datum_for_write(eval_ctx), *ids[i]);
    }
  }
  id_expr->get_evaluated_flags(eval_ctx).set_all(count);
  id_expr->set_evaluated_projected(eval_ctx);
  if (iter_param_->need_project_relevance()) {
    if (relevance_expr->is_batch_result()) {
      ObDatum *datums = relevance_expr->locate_datums_for_update(eval_ctx, count);
      for (int64_t i = 0; i < count; ++i) {
        datums[i].set_double(1.0);
      }
    } else {
      for (int64_t i = 0; i < count; ++i) {
        guard.set_batch_idx(i);
        relevance_expr->locate_datum_for_write(eval_ctx).set_double(1.0);
      }
    }
    relevance_expr->get_evaluated_flags(eval_ctx).set_all(count);
    relevance_expr->set_evaluated_projected(eval_ctx);
  }
  return ret;
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_NOT_NULL(iter_param_->relevance_expr_)) {
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

int ObTextDaaTIter::do_one_merge_round(int64_t &count)
{
  int ret = OB_SUCCESS;
  if (!membership_only_) {
    ret = ObSRDaaTIterImpl::do_one_merge_round(count);
  } else {
    const ObDatum *id_datum = nullptr;
    if (OB_FAIL(membership_union_.get_next_id(id_datum))) {
    } else if (OB_FAIL(ObSRDaaTIterImpl::cache_result(count, *id_datum, 1.0))) {
    }
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
