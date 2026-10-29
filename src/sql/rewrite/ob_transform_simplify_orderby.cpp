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

#define USING_LOG_PREFIX SQL_REWRITE
#include "sql/rewrite/ob_transform_simplify_orderby.h"
#include "sql/rewrite/ob_transform_utils.h"
#include "sql/rewrite/ob_transform_project_pruning.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
using namespace oceanbase::sql;

/*@brief, remove_stmt_order_by removes order by from stmt that can be removed, eg:
* select * from t1 where c1 in (select c1 from t2 order by c2);
* ==>
* select * from t1 where c1 in (select c1 from t2);
*
* select distinct c1 from (select c1 from t1 order by c2);
* ==>
* select distinct c1 from (select c1 from t1);
*
* select  * from (select * from t1 order by c1) order by c2;
* ==>
* select  * from (select * from t1) order by c2;
*
* select c1 from (select c1 from t1 order by c2) group by c1;
* ==>
* select c1 from (select c1 from t1) group by c1;
*
* select c1 from t1 where c2 in (select c2 from t2 order by c1);
* ==>
* select c1 from t1 where c2 in (select c2 from t2);
*/
int ObTransformSimplifyOrderby::transform_one_stmt(common::ObIArray<ObParentDMLStmt> &parent_stmts,
                                                   ObDMLStmt *&stmt,
                                                   bool &trans_happened)
{
  int ret = OB_SUCCESS;
  bool remove_duplicates = false;
  bool subquery_happened = false;
  bool view_happened = false;
  bool set_happened = false;
  bool force_serial_set_order = false;
  trans_happened = false;
  UNUSED(parent_stmts);
  if (OB_ISNULL(stmt) || OB_ISNULL(ctx_) || OB_ISNULL(ctx_->session_info_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_->session_info_->is_serial_set_order_forced(force_serial_set_order))) {
  } else if (OB_FAIL(remove_order_by_for_subquery(stmt, subquery_happened))) {
  } else if (OB_FAIL(remove_order_by_for_view_stmt(stmt, view_happened, force_serial_set_order))) {
  } else if (OB_FAIL(remove_order_by_for_set_stmt(stmt, set_happened, force_serial_set_order))) {
  } else if (OB_FAIL(remove_order_by_duplicates(stmt, remove_duplicates))) {
  } else {
    trans_happened = (subquery_happened || view_happened || remove_duplicates || set_happened);
    OPT_TRACE("remove order by for subquery:", subquery_happened);
    OPT_TRACE("remove order by for view:", view_happened);
    OPT_TRACE("remove order by duplicates:", remove_duplicates);
    OPT_TRACE("remove order by for set stmt:", set_happened);
  }
  if (OB_SUCC(ret) && trans_happened) {
    if (OB_FAIL(add_transform_hint(*stmt))) {
    }
  }
  return ret;
}

// for select/update/delete/insert
// Remove order by clause from subquery
int ObTransformSimplifyOrderby::remove_order_by_for_subquery(ObDMLStmt *stmt, bool &trans_happened)
{
  int ret = OB_SUCCESS;
  trans_happened = false;
  if (OB_ISNULL(stmt)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObIArray<ObQueryRefRawExpr*> &subquery_exprs = stmt->get_subquery_exprs();
    ObSelectStmt *subquery = NULL;
    bool happened = false;
    for (int64_t i = 0; OB_SUCC(ret) && i < subquery_exprs.count(); ++i) {
      ObQueryRefRawExpr *query_ref = subquery_exprs.at(i);
      if (OB_ISNULL(query_ref) || OB_ISNULL(subquery = query_ref->get_ref_stmt())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (!subquery->has_limit() &&
                 !subquery->is_contains_assignment() &&
                 subquery->has_order_by() &&
                 OB_FAIL(do_remove_stmt_order_by(subquery, happened))) {
      } else {
        trans_happened |= happened;
      }
    }
  }
  return ret;
}

namespace
{
bool count_star_order_expr_has_no_effects(const ObRawExpr *expr)
{
  return nullptr != expr && expr->is_deterministic()
      && !expr->has_flag(CNT_SUB_QUERY)
      && !expr->has_flag(CNT_STATE_FUNC)
      && !expr->has_flag(CNT_RAND_FUNC)
      && !expr->has_flag(CNT_PL_UDF)
      && !expr->has_flag(CNT_DYNAMIC_USER_VARIABLE)
      && !expr->has_flag(CNT_ASSIGN_EXPR)
      && !expr->has_flag(CNT_PSEUDO_COLUMN)
      && !expr->has_flag(CNT_OP_PSEUDO_COLUMN)
      && !expr->has_flag(CNT_LAST_INSERT_ID)
      && !expr->has_flag(CNT_VOLATILE_CONST)
      && !expr->has_flag(CNT_OBJ_ACCESS_EXPR);
}

bool count_star_order_leaf_is_safe(const ObRawExpr *expr, const uint64_t table_id)
{
  bool is_safe = false;
  if (!count_star_order_expr_has_no_effects(expr)) {
  } else if (expr->is_const_raw_expr()) {
    is_safe = true;
  } else if (expr->is_column_ref_expr()) {
    const ObColumnRefRawExpr *column = static_cast<const ObColumnRefRawExpr *>(expr);
    is_safe = column->get_table_id() == table_id && !column->is_generated_column()
        && nullptr == column->get_dependant_expr();
  }
  return is_safe;
}

bool count_star_order_match_is_safe(const ObRawExpr *expr, const uint64_t table_id)
{
  bool is_safe = false;
  if (count_star_order_expr_has_no_effects(expr)
      && expr->get_expr_type() == T_FUN_MATCH_AGAINST) {
    const ObMatchFunRawExpr *match = static_cast<const ObMatchFunRawExpr *>(expr);
    const ObRawExpr *search_key = match->get_search_key();
    is_safe = match->get_mode_flag() == NATURAL_LANGUAGE_MODE
        && match->get_columns_boosts().empty() && nullptr == match->get_param_text_expr()
        && nullptr != search_key && search_key->is_const_raw_expr()
        && count_star_order_expr_has_no_effects(search_key)
        && !match->get_match_columns().empty();
    for (int64_t i = 0; is_safe && i < match->get_match_columns().count(); ++i) {
      const ObRawExpr *column = match->get_match_columns().at(i);
      is_safe = nullptr != column && column->is_column_ref_expr()
          && count_star_order_leaf_is_safe(column, table_id);
    }
  }
  return is_safe;
}

const ObRawExpr *count_star_order_membership_predicate(const ObRawExpr *expr,
                                                     const uint64_t table_id)
{
  const ObRawExpr *match = nullptr;
  if (count_star_order_expr_has_no_effects(expr)) {
    // WHERE MATCH is still unwrapped at some rewrite stages. Both forms use
    // only its truth value; no score comparison or compound predicate qualifies.
    const ObRawExpr *candidate = expr;
    if (expr->get_expr_type() == T_OP_BOOL && expr->get_param_count() == 1) {
      candidate = expr->get_param_expr(0);
    }
    if (count_star_order_match_is_safe(candidate, table_id)) {
      match = candidate;
    }
  }
  return match;
}

bool count_star_order_stmt_has_no_effects(const ObSelectStmt &stmt)
{
  return !stmt.is_set_stmt() && !stmt.has_distinct() && !stmt.has_having()
      && !stmt.has_window_function() && !stmt.has_window_function_filter()
      && !stmt.has_subquery() && !stmt.is_contains_assignment()
      && !stmt.has_for_update() && !stmt.has_select_into() && !stmt.is_calc_found_rows()
      && !stmt.has_recursive_cte() && stmt.get_semi_infos().empty()
      && stmt.get_joined_tables().empty() && stmt.get_pseudo_column_like_exprs().empty();
}

// A scalar COUNT(*) cannot observe which rows an ordinary LIMIT/OFFSET chooses:
// its value is min(limit, max(input_count - offset, 0)), regardless of ordering.
// Keep this exception narrow: dropping a sort must not drop an expression that
// can raise an error or introduce effects, nor stop before such a row expression.
bool count_star_cannot_observe_view_order(const ObSelectStmt &parent,
                                          const TableItem &view_item,
                                          const ObSelectStmt &view)
{
  bool can_remove = false;
  const ObAggFunRawExpr *count = nullptr;
  const TableItem *base_table = nullptr;
  if (!count_star_order_stmt_has_no_effects(parent)
      || !parent.is_scala_group_by() || parent.get_aggr_item_size() != 1
      || parent.get_select_item_size() != 1 || parent.get_condition_size() != 0
      || parent.has_order_by() || parent.has_limit()
      || parent.get_table_items().count() != 1 || parent.get_from_item_size() != 1
      || parent.get_table_item(0) != &view_item
      || !view_item.is_generated_table() || view_item.ref_query_ != &view
      || !view_item.exec_params_.empty() || parent.get_from_item(0).is_joined_
      || parent.get_from_item(0).table_id_ != view_item.table_id_
      || nullptr == (count = parent.get_aggr_item(0))
      || parent.get_select_item(0).expr_ != count
      || count->get_expr_type() != T_FUN_COUNT || count->is_param_distinct()
      || count->get_param_count() != 0 || !count_star_order_expr_has_no_effects(count)) {
  } else if (!count_star_order_stmt_has_no_effects(view) || view.has_group_by()
      || !view.has_order_by() || nullptr == view.get_limit_expr()
      || !view.get_limit_expr()->is_const_raw_expr()
      || !count_star_order_expr_has_no_effects(view.get_limit_expr())
      || (nullptr != view.get_offset_expr()
          && (!view.get_offset_expr()->is_const_raw_expr()
              || !count_star_order_expr_has_no_effects(view.get_offset_expr())))
      || nullptr != view.get_limit_percent_expr() || view.is_fetch_with_ties()
      || view.get_table_items().count() != 1 || view.get_from_item_size() != 1
      || nullptr == (base_table = view.get_table_item(0)) || !base_table->is_basic_table()
      || base_table->is_has_sample_info() || !base_table->exec_params_.empty()
      || view.get_from_item(0).is_joined_
      || view.get_from_item(0).table_id_ != base_table->table_id_) {
  } else {
    const uint64_t table_id = base_table->table_id_;
    can_remove = true;
    for (int64_t i = 0; can_remove && i < view.get_select_item_size(); ++i) {
      can_remove = count_star_order_leaf_is_safe(view.get_select_item(i).expr_, table_id);
    }
    for (int64_t i = 0; can_remove && i < view.get_condition_size(); ++i) {
      const ObRawExpr *condition = view.get_condition_exprs().at(i);
      can_remove = nullptr != count_star_order_membership_predicate(condition, table_id);
    }
    for (int64_t i = 0; can_remove && i < view.get_order_item_size(); ++i) {
      const ObRawExpr *order_expr = view.get_order_item(i).expr_;
      bool is_safe = count_star_order_leaf_is_safe(order_expr, table_id);
      for (int64_t j = 0; !is_safe && j < view.get_condition_size(); ++j) {
        is_safe = order_expr == count_star_order_membership_predicate(
            view.get_condition_exprs().at(j), table_id);
      }
      can_remove = is_safe;
    }
  }
  return can_remove;
}
}

int ObTransformSimplifyOrderby::remove_order_by_for_count_star_view_if_enabled(
    ObDMLStmt *stmt, bool &trans_happened)
{
  int ret = OB_SUCCESS;
  bool transformation_enabled = false;
  bool parent_enabled = false;
  bool view_enabled = false;
  trans_happened = false;
  set_transformer_type(SIMPLIFY_ORDERBY);
  if (OB_ISNULL(stmt) || OB_ISNULL(ctx_) || OB_ISNULL(ctx_->session_info_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (stmt->is_select_stmt() && stmt->get_table_items().count() == 1) {
    const TableItem *table_item = stmt->get_table_item(0);
    if (OB_ISNULL(table_item)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (table_item->is_generated_table() && OB_NOT_NULL(table_item->ref_query_)) {
      const ObSelectStmt &view = *table_item->ref_query_;
      if (OB_ISNULL(stmt->get_query_ctx()) || OB_ISNULL(view.get_query_ctx())
          || OB_ISNULL(stmt->get_stmt_hint().query_hint_)
          || OB_ISNULL(view.get_stmt_hint().query_hint_)) {
        // A view created earlier in preprocessing may not have hint metadata
        // yet. Keep its ordering when the optional rewrite cannot be checked.
      } else if (stmt->get_stmt_hint().query_hint_->has_outline_data()
          || view.get_stmt_hint().query_hint_->has_outline_data()) {
        // Preprocessing must not consume the regular rewrite outline cursor.
      } else if (OB_FAIL(ctx_->session_info_->is_transformation_enabled(transformation_enabled))) {
      } else if (!transformation_enabled
          || stmt->get_query_ctx()->get_global_hint().disable_query_transform()
          || view.get_query_ctx()->get_global_hint().disable_query_transform()) {
      } else if (OB_FAIL(check_hint_status(*stmt, parent_enabled))) {
      } else if (!parent_enabled) {
      } else if (OB_FAIL(check_hint_status(view, view_enabled))) {
      } else if (view_enabled) {
        bool projection_pruned = false;
        if (OB_FAIL(prune_count_star_view_projection(stmt, ctx_, projection_pruned))) {
        } else if (OB_FAIL(remove_order_by_for_count_star_view(stmt, trans_happened))) {
        } else {
          trans_happened |= projection_pruned;
        }
      }
    }
  }
  return ret;
}

int ObTransformSimplifyOrderby::prune_count_star_view_projection(
    ObDMLStmt *stmt, ObTransformerCtx *ctx, bool &trans_happened)
{
  int ret = OB_SUCCESS;
  bool parent_enabled = false;
  bool view_enabled = false;
  trans_happened = false;
  if (OB_ISNULL(stmt) || OB_ISNULL(ctx)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (stmt->is_select_stmt() && stmt->get_table_items().count() == 1) {
    const TableItem *table_item = stmt->get_table_item(0);
    if (OB_ISNULL(table_item)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (table_item->is_generated_table() && OB_NOT_NULL(table_item->ref_query_)
        && count_star_cannot_observe_view_order(
            *static_cast<ObSelectStmt *>(stmt), *table_item, *table_item->ref_query_)) {
      ObSelectStmt &view = *table_item->ref_query_;
      ObTransformProjectPruning pruning(ctx);
      if (OB_ISNULL(stmt->get_stmt_hint().query_hint_)
          || OB_ISNULL(view.get_stmt_hint().query_hint_)) {
        // Keep optional pruning conservative when hint metadata is unavailable.
      } else if (stmt->get_stmt_hint().query_hint_->has_outline_data()
          || view.get_stmt_hint().query_hint_->has_outline_data()) {
      } else if (OB_FAIL(pruning.check_hint_allowed_prune(*static_cast<ObSelectStmt *>(stmt), parent_enabled))) {
      } else if (!parent_enabled) {
      } else if (OB_FAIL(pruning.check_hint_allowed_prune(view, view_enabled))) {
      } else if (!view_enabled) {
      } else if (OB_ISNULL(ctx->expr_factory_) || OB_ISNULL(ctx->session_info_)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (view.get_select_item_size() == 1
          && view.get_select_item(0).expr_->is_const_raw_expr()) {
        // A constant already avoids document-to-rowkey and base-table lookups.
      } else {
        ObSqlBitSet<> removed;
        for (int64_t i = 0; OB_SUCC(ret) && i < view.get_select_item_size(); ++i) {
          ret = removed.add_member(i);
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(ObTransformUtils::remove_select_items(
            ctx, table_item->table_id_, view, *stmt, removed))) {
        } else if (OB_FAIL(stmt->formalize_stmt_expr_reference(ctx->expr_factory_, ctx->session_info_))) {
        } else {
          trans_happened = true;
        }
      }
    }
  }
  return ret;
}

int ObTransformSimplifyOrderby::remove_order_by_for_count_star_view(ObDMLStmt *stmt,
                                                                  bool &trans_happened)
{
  int ret = OB_SUCCESS;
  trans_happened = false;
  if (OB_ISNULL(stmt)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (stmt->is_select_stmt() && stmt->get_table_items().count() == 1) {
    const TableItem *table_item = stmt->get_table_item(0);
    if (OB_ISNULL(table_item)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (table_item->is_generated_table() && OB_NOT_NULL(table_item->ref_query_)
        && count_star_cannot_observe_view_order(
            *static_cast<ObSelectStmt *>(stmt), *table_item, *table_item->ref_query_)) {
      ret = do_remove_stmt_order_by(table_item->ref_query_, trans_happened);
    }
  }
  return ret;
}

int ObTransformSimplifyOrderby::remove_order_by_for_view_stmt(ObDMLStmt *stmt, bool &trans_happened, bool &force_serial_set_order)
{
  int ret = OB_SUCCESS;
  bool can_remove = false;
  trans_happened = false;
  if (OB_ISNULL(stmt)) {
    ret = OB_ERR_UNEXPECTED;
  //select stmt cannot have window function,eg:
  // SELECT last_value(c1) OVER (PARTITION BY c2) FROM (SELECT * FROM t1 ORDER BY c1, c2)s
  // ORDER BY c1, c2;
  } else if (!stmt->is_sel_del_upd()) {
    /*do nothing*/
  } else if (stmt->is_select_stmt() && static_cast<ObSelectStmt*>(stmt)->has_window_function()) {
    /*do nothing*/
  } else {
    can_remove = true;
  }
  if (OB_SUCC(ret) && can_remove) {
    ObSelectStmt *view_stmt = NULL;
    ObSelectStmt *select_stmt = NULL;
    bool happened = false;
    ObIArray<TableItem *> &table_items = stmt->get_table_items();
    for (int64_t i = 0; OB_SUCC(ret) && i < table_items.count(); ++i) {
      const TableItem * table_item = table_items.at(i);
      if (OB_ISNULL(table_item)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (!table_item->is_generated_table() &&
                 !table_item->is_lateral_table()) {
        /*do nothing*/
      } else if (OB_ISNULL(view_stmt = table_item->ref_query_)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (view_stmt->is_contains_assignment() ||
                 (force_serial_set_order && view_stmt->is_set_stmt())) {
        // do nothing
      } else if (1 == table_items.count()) {
        if (stmt->is_select_stmt()) {
          select_stmt = static_cast<ObSelectStmt*>(stmt);
          if (count_star_cannot_observe_view_order(*select_stmt, *table_item, *view_stmt)) {
            if (OB_FAIL(do_remove_stmt_order_by(view_stmt, happened))) {
            } else {
              trans_happened |= happened;
            }
          } else if ((select_stmt->has_group_by() || select_stmt->has_distinct() ||
                     select_stmt->has_order_by()) && !view_stmt->has_limit() &&
                     view_stmt->has_order_by() &&
                     OB_FAIL(do_remove_stmt_order_by(view_stmt, happened))) {
          } else {
            trans_happened |= happened;
          }
        }
      } else if (!view_stmt->has_limit() && view_stmt->has_order_by() &&
                 OB_FAIL(do_remove_stmt_order_by(view_stmt, happened))) {
      } else {
        trans_happened |= happened;
      }
    }
  }
  return ret;
}

int ObTransformSimplifyOrderby::do_remove_stmt_order_by(ObSelectStmt *select_stmt, bool &trans_happened)
{
  int ret = OB_SUCCESS;
  trans_happened = false;
  ObSEArray<OrderItem, 4> new_order_items;
  if (OB_ISNULL(select_stmt)) {
    ret = OB_ERR_UNEXPECTED;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < select_stmt->get_order_item_size(); ++i) {
    const OrderItem &order_item = select_stmt->get_order_item(i);
    if (OB_ISNULL(order_item.expr_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (order_item.expr_->has_flag(CNT_SUB_QUERY)) {
      // in case the subquery returns more than one rows,
      // we should keep the subquery which may returns error
      if (OB_FAIL(new_order_items.push_back(order_item))) {
      }
    }
  }
  if (OB_SUCC(ret) && select_stmt->get_order_item_size() != new_order_items.count()) {
    trans_happened = true;
    if (OB_FAIL(select_stmt->get_order_items().assign(new_order_items))) {
    }
  }
  return ret;
}

int ObTransformSimplifyOrderby::remove_order_by_duplicates(ObDMLStmt *stmt,
                                                           bool &trans_happened)
{
  int ret = OB_SUCCESS;
  trans_happened = false;
  if (OB_ISNULL(stmt)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!stmt->is_sel_del_upd() ||
             (stmt->is_select_stmt() && (static_cast<ObSelectStmt*>(stmt)->has_rollup()))) {
    //do nothing
  } else {
    ObIArray<OrderItem> &order_items = stmt->get_order_items();
    ObIArray<ObRawExpr*> &cond_exprs = stmt->get_condition_exprs();
    if (order_items.count() > 1) {
      ObArray<OrderItem> new_order_items;
      int64_t N = order_items.count();
      int64_t M = cond_exprs.count();
      bool is_exist = false;
      for (int64_t i = 0; OB_SUCC(ret) && i < N; ++i) {
        OrderItem &cur_item = order_items.at(i);
        is_exist = false;
        if (OB_FAIL(exist_item_by_expr(cur_item.expr_, new_order_items, is_exist))) {
        } else if (!is_exist) {
          for (int64_t j = 0; OB_SUCC(ret) && !is_exist && j < M; ++j) {
            ObRawExpr *op_expr = cond_exprs.at(j);
            if (OB_ISNULL(op_expr)) {
              ret = OB_ERR_UNEXPECTED;
            } else if (T_OP_EQ == op_expr->get_expr_type()) {
              if (op_expr->get_param_count() != 2) {
                ret = OB_ERR_UNEXPECTED;
              } else {
                ObRawExpr *left_param = op_expr->get_param_expr(0);
                ObRawExpr *right_param = op_expr->get_param_expr(1);
                bool is_consistent = false;
                if (OB_ISNULL(left_param) || OB_ISNULL(right_param)) {
                 ret = OB_ERR_UNEXPECTED;
                } else if (OB_FAIL(ObRelationalExprOperator::is_equal_transitive(
                                              left_param->get_result_type(),
                                              right_param->get_result_type(), is_consistent))) {
                } else if (!is_consistent) {
                  // do nothing
                } else if (right_param == cur_item.expr_) {
                  if (OB_FAIL(exist_item_by_expr(left_param, new_order_items, is_exist))) {
                  }
                } else if (left_param == cur_item.expr_) {
                  if (OB_FAIL(exist_item_by_expr(right_param, new_order_items, is_exist))) {
                  }
                }
              }
            } else {/* do nothing */}
          }
        } else {/*do nothing*/}
        if (OB_SUCC(ret) && !is_exist) {
          if (OB_FAIL(new_order_items.push_back(cur_item))) {
          }
        }
      }// for
      if (OB_SUCC(ret)) {
        if (N != new_order_items.count()) {
          trans_happened = true;
        }
        if (OB_FAIL(order_items.assign(new_order_items))) {
        }
      }
    }
  }
  return ret;
}

int ObTransformSimplifyOrderby::exist_item_by_expr(ObRawExpr *expr, ObIArray<OrderItem> &order_items, bool &is_exist)
{
  int ret = OB_SUCCESS;
  is_exist = false;
  int64_t N = order_items.count();
  if (OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < N && !is_exist; ++i) {
      OrderItem& this_item = order_items.at(i);
      if (OB_ISNULL(this_item.expr_)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (this_item.expr_ == expr) {
        is_exist = true;
      } else if (this_item.expr_->same_as(*expr)) {
        is_exist = true;
      }
    }
  }
  return ret;
}

int ObTransformSimplifyOrderby::remove_order_by_for_set_stmt(ObDMLStmt *&stmt, bool &trans_happened, bool &force_serial_set_order)
{
  int ret = OB_SUCCESS;
  trans_happened = false;
  if (OB_ISNULL(stmt) || OB_ISNULL(ctx_) || OB_ISNULL(ctx_->session_info_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!(stmt->is_select_stmt() && static_cast<ObSelectStmt*>(stmt)->is_set_stmt())) {
    //not set stmt, do nothing
  } else if (force_serial_set_order && 
             ObSelectStmt::UNION == static_cast<ObSelectStmt*>(stmt)->get_set_op() &&
             !static_cast<ObSelectStmt*>(stmt)->is_set_distinct()) {
    // When force_serial_set_order is set, UNION ALL keeps order items in child statements.
  } else {
    ObIArray<ObSelectStmt*> &child_stmts = static_cast<ObSelectStmt*>(stmt)->get_set_query();
    ObSelectStmt *child_stmt = NULL;
    for (int64_t i = 0; OB_SUCC(ret) && i < child_stmts.count(); ++i) {
      bool happened = false;
      if (OB_ISNULL(child_stmt = child_stmts.at(i))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (child_stmt->is_contains_assignment()) {
        // do nothing
      } else if (!child_stmt->has_limit() &&
                 child_stmt->has_order_by() &&
                 OB_FAIL(do_remove_stmt_order_by(child_stmt, happened))) {
      } else {
        trans_happened |= happened;
      }
    }
  }
  return ret;
}
