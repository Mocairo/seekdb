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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/basic/ob_file_sql_bridge.h"
#include "common/timezone/ob_time_convert.h"
#include <charconv>
#include <sstream>
#include <locale>
#include "sql/engine/expr/ob_expr_lob_utils.h"


namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFunctionTableSpec, ObOpSpec), value_expr_, column_exprs_, has_correlated_expr_, file_spec_, file_column_ids_);

int ObFunctionTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  already_calc_ = false;
  if (!MY_SPEC.file_spec_.empty()) {
    return open_file();
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType == MY_SPEC.value_expr_->datum_meta_.type_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_udf;
  } else {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_sys_func;
  }
  return ret;
}

int ObFunctionTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    node_idx_ = 0;
    if (!MY_SPEC.file_spec_.empty()) return open_file();
    if (MY_SPEC.has_correlated_expr_) {
      row_count_ = 0;
      col_count_ = 0;
      value_table_ = NULL;
      already_calc_ = false;
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  file_reader_.reset();
  file_names_.clear();
  file_row_allocator_.reset();
  node_idx_ = 0;
  row_count_ = 0;
  col_count_ = 0;
  value_table_ = NULL;
  return ret;
}

void ObFunctionTableOp::destroy()
{
  file_reader_.reset();
  std::vector<std::string>().swap(file_names_);
  // Operators are arena allocated and destroy() does not invoke member
  // destructors. Swap ownership out so standard-library allocations are freed.
  file_sql::Spec empty;
  std::swap(file_spec_, empty);
  file_row_allocator_.reset();
  ObOperator::destroy();
}


int ObFunctionTableOp::get_current_result(ObObj &result)
{
  int ret = OB_SUCCESS;
  void *data = NULL;
  CK (already_calc_);
  if (node_idx_ < 0 || node_idx_ >= row_count_ * col_count_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to get current result in table function",
              K(node_idx_), K(row_count_), K(col_count_));
  }
  do {
    CK (node_idx_ >= 0);
    if (OB_SUCC(ret) && node_idx_ >= row_count_) {
      ret = OB_ITER_END;
    }
    CK (OB_NOT_NULL(value_table_));
    OX (data = value_table_->get_data());
    CK (OB_NOT_NULL(data));
    OX (result = (static_cast<ObObj*>(data))[node_idx_++]);
  } while (OB_SUCC(ret) && result.get_meta().get_type() == ObMaxType);
  return ret;
}

int ObFunctionTableOp::inner_get_next_row()
{
  return (this->*next_row_func_)();
}

int ObFunctionTableOp::inner_get_next_row_udf()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType != MY_SPEC.value_expr_->datum_meta_.type_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(plan_ctx->set_autoinc_id_tmp(0))) {
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    ObDatum *value = nullptr;
    if (!already_calc_) {
      if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
      } else if (value->is_null()) {
        //do nothing
      } else if (OB_ISNULL(value_table_ 
                 = reinterpret_cast<pl::ObPLCollection*>(value->get_ext()))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        row_count_ = value_table_->is_inited() ? value_table_->get_count() : 0;
        col_count_ = value_table_->get_column_count();
      }
      already_calc_ = true;
    }
    if (OB_SUCC(ret) && OB_UNLIKELY(node_idx_ >= row_count_)) {
      ret = OB_ITER_END;
    }
    CK (MY_SPEC.column_exprs_.count() >= col_count_);
    ObObj obj_stack[col_count_];
    if (OB_SUCC(ret)) {
      if (nullptr != value_table_ 
          && ObExtendType == value_table_->get_element_type().get_obj_type()) {
        pl::ObPLComposite *composite = NULL;
        pl::ObPLRecord *record = NULL;
        ObObj record_obj;
        OZ (get_current_result(record_obj));
        if (OB_FAIL(ret)) {
        } else if (ObUserDefinedSQLType == record_obj.get_type()) {
          obj_stack[0] = record_obj;   
        } else if (record_obj.is_pl_extend()) {
          CK (OB_NOT_NULL(composite = reinterpret_cast<pl::ObPLComposite*>(record_obj.get_ext())));
          if (OB_SUCC(ret)) {
            if (composite->is_record()) {
              OX (record = static_cast<pl::ObPLRecord*>(composite));
              CK (record->get_count() == col_count_);
              for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
                OZ (record->get_element(i, obj_stack[i]));
              }
            } else if (composite->is_collection()) {
              OX (obj_stack[0] = record_obj);
            } else {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("unexpected here", K(ret), K(record_obj), K(record_obj.meta_));
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
          if (OB_FAIL(get_current_result(obj_stack[i]))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
        if (obj_stack[i].is_null()) {
          MY_SPEC.column_exprs_.at(i)->locate_datum_for_write(eval_ctx_).set_null();
        } else {
          const ObObjDatumMapType &datum_map = MY_SPEC.column_exprs_.at(i)->obj_datum_map_;
          ObExpr * const &expr = MY_SPEC.column_exprs_.at(i);
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          if (OB_FAIL(datum.from_obj(obj_stack[i], datum_map))) {
          } else if (is_lob_storage(obj_stack[i].get_type()) &&
                     OB_FAIL(ob_adjust_lob_datum(get_exec_ctx(), obj_stack[i],
                                                 expr->obj_meta_, datum_map,
                                                 get_exec_ctx().get_allocator(), datum))) {
          }
        }
        if (OB_SUCC(ret)) {
          MY_SPEC.column_exprs_.at(i)->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_sys_func()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  ObDatum *value = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
    if (OB_ITER_END != ret) {
    }
  } else {
    MY_SPEC.column_exprs_.at(0)->locate_datum_for_write(eval_ctx_).set_datum(*value);
    MY_SPEC.column_exprs_.at(0)->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}


int ObFunctionTableOp::open_file()
{
  int ret = OB_SUCCESS;
  std::string root;
  if (ctx_.get_my_session() == nullptr) return OB_ERR_UNEXPECTED;
  if (OB_FAIL(check_file_sql_access(*ctx_.get_my_session(), root))) return ret;
  try {
    file_spec_ = file_sql::Spec::decode(std::string(MY_SPEC.file_spec_.ptr(), MY_SPEC.file_spec_.length()));
    file_spec_.secure_root = root;
    node_idx_ = 0;
    file_reader_.reset();
    if (file_spec_.options.format == "glob") file_names_ = file_sql::discover(file_spec_.path, root);
    else {
      std::vector<size_t> projection;
      for (int64_t i = 0; i < MY_SPEC.file_column_ids_.count(); ++i) projection.push_back(MY_SPEC.file_column_ids_.at(i));
      // COUNT(*) still advances one source column, without materializing rows.
      if (projection.empty()) projection.push_back(0);
      file_reader_.reset(new file_sql::Reader(file_spec_));
      file_reader_->open(projection);
    }
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_file;
  } catch (const std::exception &e) {
    ret = OB_INVALID_ARGUMENT; LOG_USER_ERROR(OB_INVALID_ARGUMENT, e.what());
    file_reader_.reset();
  }
  return ret;
}

int ObFunctionTableOp::write_file_row(const std::vector<file_sql::Cell> &row)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_exprs_.count(); ++i) {
    const auto &cell = row.at(i);
    ObExpr *expr = MY_SPEC.column_exprs_.at(i);
    ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
    const auto type = file_spec_.columns.at(MY_SPEC.file_column_ids_.at(i)).type;
    if (cell.null || type == file_sql::Type::NIL) datum.set_null();
    else if (type == file_sql::Type::BIGINT) {
      int64_t n = 0; const char *first = cell.text.data() + (cell.text[0] == '+');
      auto parsed = std::from_chars(first, cell.text.data() + cell.text.size(), n);
      if (parsed.ec != std::errc() || parsed.ptr != cell.text.data() + cell.text.size()) ret = OB_INVALID_ARGUMENT;
      else datum.set_int(n);
    } else if (type == file_sql::Type::DOUBLE) {
      double value; std::istringstream in(cell.text); in.imbue(std::locale::classic());
      in >> std::noskipws >> value;
      if (!in || !in.eof() || !std::isfinite(value)) ret = OB_INVALID_ARGUMENT;
      else datum.set_double(value);
    }
    else if (type == file_sql::Type::BOOLEAN) datum.set_int(cell.text == "true" || cell.text == "TRUE");
    else if (type == file_sql::Type::DATE) {
      int32_t date = 0;
      if (OB_FAIL(ObTimeConverter::str_to_date(ObString(int32_t(cell.text.size()), cell.text.data()), date))) {}
      else datum.set_date(date);
    } else if (type == file_sql::Type::DATETIME) {
      int64_t datetime = 0; ObTimeConvertCtx cvrt(nullptr, false);
      if (OB_FAIL(ObTimeConverter::str_to_datetime(ObString(int32_t(cell.text.size()), cell.text.data()), cvrt, datetime))) {}
      else datum.set_datetime(datetime);
    } else {
      char *copy = static_cast<char *>(file_row_allocator_.alloc(std::max<size_t>(cell.text.size(), 1)));
      if (copy == nullptr) ret = OB_ALLOCATE_MEMORY_FAILED;
      else { MEMCPY(copy, cell.text.data(), cell.text.size()); datum.set_string(copy, int32_t(cell.text.size())); }
    }
    if (OB_SUCC(ret)) expr->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_file()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ctx_.check_status())) return ret;
  clear_evaluated_flag(); file_row_allocator_.reuse();
  try {
    std::vector<file_sql::Cell> row;
    if (file_spec_.options.format == "glob") {
      if (size_t(node_idx_) >= file_names_.size()) return OB_ITER_END;
      row.emplace_back(file_names_[node_idx_++]);
    } else if (!file_reader_->next(row)) return OB_ITER_END;
    ret = write_file_row(row);
  } catch (const std::exception &e) { ret = OB_INVALID_ARGUMENT; LOG_USER_ERROR(OB_INVALID_ARGUMENT, e.what()); }
  return ret;
}

int ObFunctionTableOp::inner_get_next_batch(const int64_t max_row_cnt)
{
  if (MY_SPEC.file_spec_.empty()) return ObOperator::inner_get_next_batch(max_row_cnt);
  int ret = OB_SUCCESS; clear_evaluated_flag(); file_row_allocator_.reuse();
  brs_.size_ = 0; brs_.end_ = false;
  const int64_t capacity = MIN(max_row_cnt, MY_SPEC.max_batch_size_);
  if (capacity <= 0) return OB_INVALID_ARGUMENT;
  brs_.skip_->reset(capacity);
  ObEvalCtx::BatchInfoScopeGuard guard(eval_ctx_); guard.set_batch_size(capacity);
  try {
    size_t bytes = 0;
    while (OB_SUCC(ret) && brs_.size_ < capacity && bytes < 1024 * 1024) {
      if (OB_FAIL(ctx_.check_status())) break;
      std::vector<file_sql::Cell> row;
      if (file_spec_.options.format == "glob") {
        if (size_t(node_idx_) >= file_names_.size()) { brs_.end_ = true; break; }
        row.emplace_back(file_names_[node_idx_++]);
      } else if (!file_reader_->next(row)) { brs_.end_ = true; break; }
      guard.set_batch_idx(brs_.size_);
      for (const auto &cell : row) bytes += cell.text.size();
      if (OB_FAIL(write_file_row(row))) break;
      for (auto *expr : MY_SPEC.column_exprs_) expr->get_evaluated_flags(eval_ctx_).set(brs_.size_);
      ++brs_.size_;
    }
  } catch (const std::exception &e) { ret = OB_INVALID_ARGUMENT; LOG_USER_ERROR(OB_INVALID_ARGUMENT, e.what()); }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
