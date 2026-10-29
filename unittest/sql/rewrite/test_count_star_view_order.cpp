/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <initializer_list>
#include "sql/rewrite/ob_transform_simplify_orderby.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
namespace sql
{
namespace
{
using namespace common;

struct CountStarViewAst
{
  CountStarViewAst() : allocator(), arithmetic(allocator), bool_match(allocator) {}

  static int add_select(ObSelectStmt &stmt, ObRawExpr &expr)
  {
    SelectItem item;
    item.expr_ = &expr;
    return stmt.add_select_item(item);
  }

  static void set_int(ObConstRawExpr &expr, const int64_t value)
  {
    ObObj obj;
    obj.set_int(value);
    expr.set_expr_type(T_INT);
    expr.set_value(obj);
  }

  int init()
  {
    int ret = OB_SUCCESS;
    count.set_expr_type(T_FUN_COUNT);
    extra_agg.set_expr_type(T_FUN_SUM);
    column.set_expr_type(T_REF_COLUMN);
    column.set_ref_id(50001, OB_APP_MIN_COLUMN_ID);
    column.set_data_type(ObIntType);
    set_int(limit, 20);
    set_int(offset, 5);
    set_int(literal, 1);
    arithmetic.set_expr_type(T_OP_DIV);
    ObObj search;
    search.set_varchar("database");
    search_key.set_expr_type(T_VARCHAR);
    search_key.set_value(search);
    match.set_expr_type(T_FUN_MATCH_AGAINST);
    match.set_search_key(&search_key);
    bool_match.set_expr_type(T_OP_BOOL);
    query_ref.set_ref_stmt(&subquery);
    base.table_id_ = 50001;
    base.type_ = TableItem::BASE_TABLE;
    generated.table_id_ = 50002;
    generated.type_ = TableItem::GENERATED_TABLE;
    generated.ref_query_ = &child;
    OrderItem order(&column, NULLS_LAST_DESC);
    if (OB_FAIL(parent.get_table_items().push_back(&generated))) {
    } else if (OB_FAIL(parent.add_from_item(generated.table_id_))) {
    } else if (OB_FAIL(parent.get_aggr_items().push_back(&count))) {
    } else if (OB_FAIL(add_select(parent, count))) {
    } else if (OB_FAIL(child.get_table_items().push_back(&base))) {
    } else if (OB_FAIL(child.add_from_item(base.table_id_))) {
    } else if (OB_FAIL(add_select(child, column))) {
    } else if (OB_FAIL(child.add_order_item(order))) {
    } else if (OB_FAIL(arithmetic.set_param_exprs(&literal, &offset))) {
    } else if (OB_FAIL(match.get_match_columns().push_back(&column))) {
    } else if (OB_FAIL(match.add_flag(IS_MATCH_EXPR))) {
    } else if (OB_FAIL(bool_match.set_param_expr(&match))) {
    } else if (OB_FAIL(bool_match.add_flag(CNT_MATCH_EXPR))) {
    } else if (OB_FAIL(query_ref.add_flag(IS_SUB_QUERY))) {
    } else {
      child.set_limit_offset(&limit, nullptr);
    }
    return ret;
  }

  int add_match_predicate()
  {
    return child.get_condition_exprs().push_back(&bool_match);
  }

  ObArenaAllocator allocator;
  ObSelectStmt parent;
  ObSelectStmt child;
  ObSelectStmt subquery;
  TableItem generated;
  TableItem base;
  ObColumnRefRawExpr column;
  ObAggFunRawExpr count;
  ObAggFunRawExpr extra_agg;
  ObConstRawExpr limit;
  ObConstRawExpr offset;
  ObConstRawExpr literal;
  ObConstRawExpr search_key;
  ObOpRawExpr arithmetic;
  ObMatchFunRawExpr match;
  ObOpRawExpr bool_match;
  ObQueryRefRawExpr query_ref;
  ObWinFunRawExpr window;
  ObPseudoColumnRawExpr rownum;
  ObSelectIntoItem into;
  SemiInfo semi;
  JoinedTable joined;
};

void check_count_star_view_order(CountStarViewAst &ast, const bool removable,
                                 const bool count_only_entry)
{
  SCOPED_TRACE(count_only_entry ? "count-only entry" : "general view entry");
  ObTransformSimplifyOrderby rule(nullptr);
  bool happened = false;
  bool force_serial_set_order = false;
  const auto remove_order = [&]() {
    return count_only_entry
        ? ObTransformSimplifyOrderby::remove_order_by_for_count_star_view(&ast.parent, happened)
        : rule.remove_order_by_for_view_stmt(&ast.parent, happened, force_serial_set_order);
  };
  const int64_t original_count = ast.child.get_order_item_size();
  ASSERT_GT(original_count, 0);
  ObRawExpr *original_order = ast.child.get_order_item(0).expr_;
  const ObOrderDirection original_direction = ast.child.get_order_item(0).order_type_;
  ObRawExpr *original_limit = ast.child.get_limit_expr();
  ObRawExpr *original_offset = ast.child.get_offset_expr();
  const int64_t parent_select_count = ast.parent.get_select_item_size();
  const int64_t child_select_count = ast.child.get_select_item_size();
  ASSERT_EQ(OB_SUCCESS, remove_order());
  EXPECT_EQ(removable, happened);
  EXPECT_EQ(removable ? 0 : original_count, ast.child.get_order_item_size());
  if (!removable) {
    EXPECT_EQ(original_order, ast.child.get_order_item(0).expr_);
    EXPECT_EQ(original_direction, ast.child.get_order_item(0).order_type_);
  }
  EXPECT_EQ(original_limit, ast.child.get_limit_expr());
  EXPECT_EQ(original_offset, ast.child.get_offset_expr());
  EXPECT_EQ(parent_select_count, ast.parent.get_select_item_size());
  EXPECT_EQ(child_select_count, ast.child.get_select_item_size());
  if (removable) {
    happened = true;
    ASSERT_EQ(OB_SUCCESS, remove_order());
    EXPECT_FALSE(happened);
    EXPECT_EQ(original_limit, ast.child.get_limit_expr());
    EXPECT_EQ(original_offset, ast.child.get_offset_expr());
  }
}

struct CountOrderMutation
{
  const char *name;
  void (*apply)(CountStarViewAst &);
};

struct CountStarProjectionAst
{
  CountStarProjectionAst()
      : table_allocator(), wrapper_allocator(&table_allocator),
        table_hash_allocator(OB_MALLOC_NORMAL_BLOCK_SIZE, wrapper_allocator),
        expr_factory(ast.allocator)
  {}

  static int add_column(ObSelectStmt &stmt, ObColumnRefRawExpr &expr)
  {
    ColumnItem item;
    item.expr_ = &expr;
    item.set_ref_id(expr.get_table_id(), expr.get_column_id());
    return stmt.add_column_item(item);
  }

  int init(const bool bool_wrapper, const bool with_offset)
  {
    int ret = OB_SUCCESS;
    context.allocator_ = &ast.allocator;
    context.expr_factory_ = &expr_factory;
    context.session_info_ = &session;
    ast.parent.set_query_ctx(&parent_query_context);
    ast.child.set_query_ctx(&child_query_context);
    ast.parent.get_stmt_hint().set_query_hint(&parent_query_context.get_query_hint());
    ast.child.get_stmt_hint().set_query_hint(&child_query_context.get_query_hint());
    body.set_expr_type(T_REF_COLUMN);
    body.set_ref_id(50001, OB_APP_MIN_COLUMN_ID + 1);
    body.set_data_type(ObVarcharType);
    outer_id.set_expr_type(T_REF_COLUMN);
    outer_id.set_ref_id(50002, OB_APP_MIN_COLUMN_ID);
    outer_id.set_data_type(ObIntType);
    if (OB_FAIL(ast.init())) {
    } else if (OB_FAIL(ast.parent.init_stmt(table_hash_allocator, wrapper_allocator))) {
    } else if (OB_FAIL(ast.child.init_stmt(table_hash_allocator, wrapper_allocator))) {
    } else if (OB_FAIL(ast.parent.rebuild_tables_hash())) {
    } else if (OB_FAIL(ast.child.rebuild_tables_hash())) {
    } else if (OB_FAIL(add_column(ast.child, ast.column))) {
    } else if (OB_FAIL(add_column(ast.child, body))) {
    } else if (OB_FAIL(add_column(ast.parent, outer_id))) {
    } else if (OB_FAIL(ast.match.add_flag(CNT_COLUMN))) {
    } else if (OB_FAIL(ast.child.get_match_exprs().push_back(&ast.match))) {
    } else {
      ast.match.get_match_columns().at(0) = &body;
      ast.child.get_order_item(0).expr_ = &ast.match;
      ast.child.set_limit_offset(&ast.limit, with_offset ? &ast.offset : nullptr);
      ObRawExpr *predicate = bool_wrapper
          ? static_cast<ObRawExpr *>(&ast.bool_match) : &ast.match;
      ret = ast.child.get_condition_exprs().push_back(predicate);
    }
    return ret;
  }

  int prune(bool &happened)
  {
    return ObTransformSimplifyOrderby::prune_count_star_view_projection(
        &ast.parent, &context, happened);
  }

  // Statements retain allocator pointers in their relation-id hash maps.
  // Keep these allocators alive until after both statements are destroyed.
  ObArenaAllocator table_allocator;
  ObWrapperAllocator wrapper_allocator;
  TableHashAllocator table_hash_allocator;
  CountStarViewAst ast;
  ObRawExprFactory expr_factory;
  ObSQLSessionInfo session;
  ObQueryCtx parent_query_context;
  ObQueryCtx child_query_context;
  ObTransformerCtx context;
  ObColumnRefRawExpr body;
  ObColumnRefRawExpr outer_id;
};

void check_projection_preserved(CountStarProjectionAst &fixture)
{
  CountStarViewAst &ast = fixture.ast;
  const int64_t select_count = ast.child.get_select_item_size();
  ObRawExpr *projection = ast.child.get_select_item(0).expr_;
  ObRawExpr *limit = ast.child.get_limit_expr();
  ObRawExpr *offset = ast.child.get_offset_expr();
  const int32_t id_refs = ast.column.get_explicited_reftype();
  const int32_t body_refs = fixture.body.get_explicited_reftype();
  const int32_t outer_refs = fixture.outer_id.get_explicited_reftype();
  bool happened = true;
  ASSERT_EQ(OB_SUCCESS, fixture.prune(happened));
  EXPECT_FALSE(happened);
  EXPECT_EQ(select_count, ast.child.get_select_item_size());
  EXPECT_EQ(projection, ast.child.get_select_item(0).expr_);
  ASSERT_EQ(1, ast.child.get_order_item_size());
  EXPECT_EQ(&ast.match, ast.child.get_order_item(0).expr_);
  EXPECT_EQ(limit, ast.child.get_limit_expr());
  EXPECT_EQ(offset, ast.child.get_offset_expr());
  EXPECT_EQ(2, ast.child.get_column_size());
  EXPECT_EQ(1, ast.parent.get_column_size());
  EXPECT_EQ(id_refs, ast.column.get_explicited_reftype());
  EXPECT_EQ(body_refs, fixture.body.get_explicited_reftype());
  EXPECT_EQ(outer_refs, fixture.outer_id.get_explicited_reftype());
  EXPECT_EQ(0, fixture.context.trans_list_loc_);
  EXPECT_TRUE(fixture.context.outline_trans_hints_.empty());
}

TEST(CountStarViewOrder, projection_pruning_releases_unused_columns_and_keeps_match_inputs)
{
  for (const bool bool_wrapper : {false, true}) {
    for (const bool with_offset : {false, true}) {
      for (const bool multiple_projections : {false, true}) {
        CountStarProjectionAst fixture;
        ASSERT_EQ(OB_SUCCESS, fixture.init(bool_wrapper, with_offset));
        CountStarViewAst &ast = fixture.ast;
        if (multiple_projections) {
          ASSERT_EQ(OB_SUCCESS, CountStarViewAst::add_select(ast.child, fixture.body));
          ASSERT_EQ(OB_SUCCESS, CountStarViewAst::add_select(ast.child, ast.literal));
        }
        ASSERT_TRUE(ast.column.is_referred_by_normal());
        ASSERT_TRUE(fixture.body.is_referred_by_normal());
        ObRawExpr *predicate = ast.child.get_condition_expr(0);
        bool happened = false;
        ASSERT_EQ(OB_SUCCESS, fixture.prune(happened));
        EXPECT_TRUE(happened);
        ASSERT_EQ(1, ast.child.get_select_item_size());
        ObRawExpr *dummy = ast.child.get_select_item(0).expr_;
        ASSERT_NE(nullptr, dummy);
        ASSERT_TRUE(dummy->is_const_raw_expr());
        EXPECT_EQ(ObIntType, dummy->get_data_type());
        EXPECT_EQ(1, static_cast<ObConstRawExpr *>(dummy)->get_value().get_int());
        EXPECT_EQ(0, ast.parent.get_column_size());
        ASSERT_EQ(1, ast.child.get_column_size());
        EXPECT_EQ(&fixture.body, ast.child.get_column_item(0)->expr_);
        EXPECT_FALSE(ast.column.is_explicited_reference());
        EXPECT_FALSE(fixture.body.is_referred_by_normal());
        EXPECT_NE(0, fixture.body.get_explicited_reftype() & REF_BY_MATCH_EXPR);
        EXPECT_EQ(predicate, ast.child.get_condition_expr(0));
        EXPECT_EQ(&fixture.body, ast.match.get_match_columns().at(0));
        ASSERT_EQ(1, ast.child.get_order_item_size());
        EXPECT_EQ(&ast.match, ast.child.get_order_item(0).expr_);
        EXPECT_EQ(&ast.limit, ast.child.get_limit_expr());
        EXPECT_EQ(with_offset ? &ast.offset : nullptr, ast.child.get_offset_expr());
        ASSERT_EQ(1, ast.parent.get_select_item_size());
        EXPECT_EQ(&ast.count, ast.parent.get_select_item(0).expr_);
        ASSERT_EQ(1, ast.parent.get_aggr_items().count());
        EXPECT_EQ(&ast.count, ast.parent.get_aggr_items().at(0));
        happened = true;
        ASSERT_EQ(OB_SUCCESS, fixture.prune(happened));
        EXPECT_FALSE(happened);
        EXPECT_EQ(dummy, ast.child.get_select_item(0).expr_);
        EXPECT_EQ(1, ast.child.get_column_size());
        EXPECT_EQ(0, ast.parent.get_column_size());
      }
    }
  }
}

TEST(CountStarViewOrder, projection_pruning_honors_parent_and_child_optouts)
{
  for (const bool parent_scope : {false, true}) {
    for (const ObItemType hint_type : {T_NO_PROJECT_PRUNE, T_NO_REWRITE}) {
      CountStarProjectionAst fixture;
      ASSERT_EQ(OB_SUCCESS, fixture.init(true, true));
      ObTransHint hint(hint_type);
      ObSelectStmt &hinted = parent_scope ? fixture.ast.parent : fixture.ast.child;
      ASSERT_EQ(OB_SUCCESS, hinted.get_stmt_hint().normal_hints_.push_back(&hint));
      check_projection_preserved(fixture);
    }
    CountStarProjectionAst fixture;
    ASSERT_EQ(OB_SUCCESS, fixture.init(true, true));
    ObQueryCtx &query_context = parent_scope
        ? fixture.parent_query_context : fixture.child_query_context;
    query_context.get_query_hint_for_update().is_valid_outline_ = true;
    check_projection_preserved(fixture);
    EXPECT_TRUE(fixture.context.used_trans_hints_.empty());
  }
}

TEST(CountStarViewOrder, projection_pruning_requires_unobservable_safe_projections)
{
  const std::initializer_list<CountOrderMutation> mutations = {
    {"count(column)", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.count.add_real_param_expr(&a.column)); }},
    {"outer filter", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.parent.get_condition_exprs().push_back(&a.literal)); }},
    {"failing projection", [](CountStarViewAst &a) {
      a.child.get_select_item(0).expr_ = &a.arithmetic; }},
    {"stateful projection", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.column.add_flag(CNT_STATE_FUNC)); }},
    {"with ties", [](CountStarViewAst &a) { a.child.set_fetch_with_ties(true); }},
    {"locking", [](CountStarViewAst &a) { a.base.for_update_ = true; }},
    {"already constant", [](CountStarViewAst &a) {
      a.child.get_select_item(0).expr_ = &a.literal; }},
  };
  for (const CountOrderMutation &mutation : mutations) {
    SCOPED_TRACE(mutation.name);
    CountStarProjectionAst fixture;
    ASSERT_EQ(OB_SUCCESS, fixture.init(true, true));
    mutation.apply(fixture.ast);
    check_projection_preserved(fixture);
  }
}

void check_preserved_orders(const std::initializer_list<CountOrderMutation> &mutations)
{
  for (const CountOrderMutation &mutation : mutations) {
    SCOPED_TRACE(mutation.name);
    for (const bool count_only_entry : {false, true}) {
      CountStarViewAst ast;
      ASSERT_EQ(OB_SUCCESS, ast.init());
      mutation.apply(ast);
      check_count_star_view_order(ast, false, count_only_entry);
    }
  }
}

TEST(CountStarViewOrder, missing_preprocess_hint_metadata_keeps_order_without_error)
{
  // Leave session variables uninitialized: absent metadata must skip the
  // optional optimization before consulting rewrite settings or hints.
  ObSQLSessionInfo session;
  ObTransformerCtx context;
  context.session_info_ = &session;
  for (const int missing : {0, 1, 2, 3, 4}) {
    SCOPED_TRACE(missing);
    ObQueryCtx query_context;
    CountStarViewAst ast;
    ASSERT_EQ(OB_SUCCESS, ast.init());
    if (missing != 4) {
      ast.parent.set_query_ctx(&query_context);
      ast.child.set_query_ctx(&query_context);
      ast.parent.get_stmt_hint().set_query_hint(&query_context.get_query_hint());
      ast.child.get_stmt_hint().set_query_hint(&query_context.get_query_hint());
      if (missing == 0) {
        ast.parent.set_query_ctx(nullptr);
      } else if (missing == 1) {
        ast.child.set_query_ctx(nullptr);
      } else if (missing == 2) {
        ast.parent.get_stmt_hint().set_query_hint(nullptr);
      } else {
        ast.child.get_stmt_hint().set_query_hint(nullptr);
      }
    }
    ObTransformSimplifyOrderby rule(&context);
    bool happened = true;
    ASSERT_EQ(OB_SUCCESS, rule.remove_order_by_for_count_star_view_if_enabled(&ast.parent, happened));
    EXPECT_FALSE(happened);
    ASSERT_EQ(1, ast.child.get_order_item_size());
    EXPECT_EQ(&ast.column, ast.child.get_order_item(0).expr_);
    EXPECT_EQ(&ast.limit, ast.child.get_limit_expr());
    EXPECT_EQ(nullptr, ast.child.get_offset_expr());
    EXPECT_EQ(0, context.trans_list_loc_);
    EXPECT_TRUE(context.outline_trans_hints_.empty());
    EXPECT_TRUE(context.used_trans_hints_.empty());
  }
}

TEST(CountStarViewOrder, ordinary_column_and_literal_orders_do_not_affect_count)
{
  for (const bool literal_order : {false, true}) {
    for (const bool with_offset : {false, true}) {
      for (const bool count_only_entry : {false, true}) {
        CountStarViewAst ast;
        ASSERT_EQ(OB_SUCCESS, ast.init());
        ast.child.get_order_item(0).expr_ = literal_order
            ? static_cast<ObRawExpr *>(&ast.literal) : &ast.column;
        ast.child.set_limit_offset(&ast.limit, with_offset ? &ast.offset : nullptr);
        check_count_star_view_order(ast, true, count_only_entry);
      }
    }
  }
}

TEST(CountStarViewOrder, literal_null_and_parameter_limits_remain_unchanged)
{
  for (const bool parameter : {false, true}) {
    for (const bool count_only_entry : {false, true}) {
      CountStarViewAst ast;
      ASSERT_EQ(OB_SUCCESS, ast.init());
      if (parameter) {
        ast.limit.set_expr_type(T_QUESTIONMARK);
        ast.offset.set_expr_type(T_QUESTIONMARK);
      } else {
        ObObj null_value;
        null_value.set_null();
        ast.limit.set_expr_type(T_NULL);
        ast.limit.set_value(null_value);
      }
      ast.child.set_limit_offset(&ast.limit, &ast.offset);
      check_count_star_view_order(ast, true, count_only_entry);
    }
  }
}

TEST(CountStarViewOrder, natural_match_order_is_unobservable_only_through_count)
{
  for (const bool bool_wrapper : {false, true}) {
    for (const bool count_only_entry : {false, true}) {
      CountStarViewAst ast;
      ASSERT_EQ(OB_SUCCESS, ast.init());
      ObRawExpr *predicate = bool_wrapper ? static_cast<ObRawExpr *>(&ast.bool_match) : &ast.match;
      ASSERT_EQ(OB_SUCCESS, ast.child.get_condition_exprs().push_back(predicate));
      ast.child.get_order_item(0).expr_ = &ast.match;
      check_count_star_view_order(ast, true, count_only_entry);
      ASSERT_EQ(1, ast.child.get_condition_size());
      EXPECT_EQ(predicate, ast.child.get_condition_expr(0));
      EXPECT_EQ(&ast.match, ast.bool_match.get_param_expr(0));
    }
  }
}

TEST(CountStarViewOrder, row_values_or_another_aggregate_preserve_ranked_subset)
{
  check_preserved_orders({
    {"count(nullable_column)", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.count.add_real_param_expr(&a.column)); }},
    {"distinct count", [](CountStarViewAst &a) { a.count.set_param_distinct(true); }},
    {"additional projection", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, CountStarViewAst::add_select(a.parent, a.literal)); }},
    {"additional aggregate", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.parent.get_aggr_items().push_back(&a.extra_agg)); }},
    {"different projected aggregate", [](CountStarViewAst &a) {
      a.parent.get_select_item(0).expr_ = &a.extra_agg; }},
    {"explicit group", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.parent.get_group_exprs().push_back(&a.column)); }},
    {"rollup", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.parent.get_rollup_exprs().push_back(&a.column)); }},
  });
}

TEST(CountStarViewOrder, parent_clauses_that_can_observe_or_change_rows_preserve_order)
{
  check_preserved_orders({
    {"where", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.parent.get_condition_exprs().push_back(&a.literal)); }},
    {"having", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.parent.get_having_exprs().push_back(&a.literal)); }},
    {"order", [](CountStarViewAst &a) {
      OrderItem order(&a.count, NULLS_LAST_DESC);
      ASSERT_EQ(OB_SUCCESS, a.parent.add_order_item(order)); }},
    {"window", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.parent.get_window_func_exprs().push_back(&a.window)); }},
    {"qualify", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.parent.get_qualify_filters().push_back(&a.literal)); }},
    {"distinct", [](CountStarViewAst &a) { a.parent.assign_distinct(); }},
    {"set", [](CountStarViewAst &a) { a.parent.assign_set_op(ObSelectStmt::UNION); }},
    {"recursive", [](CountStarViewAst &a) { a.parent.set_recursive_union(true); }},
    {"assignment", [](CountStarViewAst &a) { a.parent.set_contains_assignment(true); }},
    {"locking", [](CountStarViewAst &a) { a.generated.for_update_ = true; }},
    {"found rows", [](CountStarViewAst &a) { a.parent.set_calc_found_rows(true); }},
    {"limit", [](CountStarViewAst &a) { a.parent.set_limit_offset(&a.limit, nullptr); }},
    {"into", [](CountStarViewAst &a) { a.parent.set_select_into(&a.into); }},
    {"subquery", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.parent.get_subquery_exprs().push_back(&a.query_ref)); }},
    {"rownum", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.parent.get_pseudo_column_like_exprs().push_back(&a.rownum)); }},
  });
}

TEST(CountStarViewOrder, generated_source_must_be_the_only_exact_from_source)
{
  check_preserved_orders({
    {"join from", [](CountStarViewAst &a) { a.parent.get_from_item(0).is_joined_ = true; }},
    {"joined table", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.parent.add_joined_table(&a.joined)); }},
    {"semi join", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.parent.add_semi_info(&a.semi)); }},
    {"different from id", [](CountStarViewAst &a) { ++a.parent.get_from_item(0).table_id_; }},
    {"two from items", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.parent.add_from_item(a.base.table_id_)); }},
    {"two table items", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.parent.get_table_items().push_back(&a.base)); }},
    {"lateral source", [](CountStarViewAst &a) { a.generated.type_ = TableItem::LATERAL_TABLE; }},
  });
}

TEST(CountStarViewOrder, inner_limit_cardinality_and_effect_barriers_preserve_order)
{
  check_preserved_orders({
    {"with ties", [](CountStarViewAst &a) { a.child.set_fetch_with_ties(true); }},
    {"percent", [](CountStarViewAst &a) { a.child.set_limit_percent_expr(&a.literal); }},
    {"calculated limit", [](CountStarViewAst &a) { a.child.set_limit_offset(&a.arithmetic, nullptr); }},
    {"calculated offset", [](CountStarViewAst &a) { a.child.set_limit_offset(&a.limit, &a.arithmetic); }},
    {"offset without limit", [](CountStarViewAst &a) { a.child.set_limit_offset(nullptr, &a.offset); }},
    {"assignment", [](CountStarViewAst &a) { a.child.set_contains_assignment(true); }},
    {"locking", [](CountStarViewAst &a) { a.base.for_update_ = true; }},
    {"found rows", [](CountStarViewAst &a) { a.child.set_calc_found_rows(true); }},
    {"into", [](CountStarViewAst &a) { a.child.set_select_into(&a.into); }},
    {"subquery", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.child.get_subquery_exprs().push_back(&a.query_ref)); }},
    {"rownum", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.child.get_pseudo_column_like_exprs().push_back(&a.rownum)); }},
    {"join from", [](CountStarViewAst &a) { a.child.get_from_item(0).is_joined_ = true; }},
    {"semi join", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.child.add_semi_info(&a.semi)); }},
    {"group", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.child.get_group_exprs().push_back(&a.column)); }},
    {"aggregate", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.child.get_aggr_items().push_back(&a.extra_agg)); }},
    {"having", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.child.get_having_exprs().push_back(&a.literal)); }},
    {"window", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.child.get_window_func_exprs().push_back(&a.window)); }},
    {"qualify", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.child.get_qualify_filters().push_back(&a.literal)); }},
    {"distinct", [](CountStarViewAst &a) { a.child.assign_distinct(); }},
    {"set", [](CountStarViewAst &a) { a.child.assign_set_op(ObSelectStmt::UNION); }},
    {"recursive", [](CountStarViewAst &a) { a.child.set_recursive_union(true); }},
  });
}

TEST(CountStarViewOrder, potentially_failing_or_effectful_expressions_preserve_order)
{
  check_preserved_orders({
    {"arithmetic order", [](CountStarViewAst &a) { a.child.get_order_item(0).expr_ = &a.arithmetic; }},
    {"subquery order", [](CountStarViewAst &a) { a.child.get_order_item(0).expr_ = &a.query_ref; }},
    {"arithmetic projection", [](CountStarViewAst &a) { a.child.get_select_item(0).expr_ = &a.arithmetic; }},
    {"arithmetic predicate", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.child.get_condition_exprs().push_back(&a.arithmetic)); }},
    {"ordinary filter", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.child.get_condition_exprs().push_back(&a.literal)); }},
    {"stateful literal", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.literal.add_flag(CNT_STATE_FUNC));
      a.child.get_select_item(0).expr_ = &a.literal; }},
    {"random column", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.column.add_flag(CNT_RAND_FUNC)); }},
    {"UDF column", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.column.add_flag(CNT_PL_UDF)); }},
    {"user variable", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.column.add_flag(CNT_DYNAMIC_USER_VARIABLE)); }},
    {"assignment expression", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.column.add_flag(CNT_ASSIGN_EXPR)); }},
    {"sequence or pseudo column", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.column.add_flag(CNT_PSEUDO_COLUMN)); }},
    {"dependent generated column", [](CountStarViewAst &a) {
      a.column.set_dependant_expr(&a.arithmetic); }},
    {"column from another table", [](CountStarViewAst &a) {
      a.column.set_table_id(a.base.table_id_ + 1); }},
  });
}

TEST(CountStarViewOrder, match_order_requires_the_same_safe_membership_predicate)
{
  check_preserved_orders({
    {"match without predicate", [](CountStarViewAst &a) { a.child.get_order_item(0).expr_ = &a.match; }},
    {"boolean mode", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.add_match_predicate());
      a.match.set_mode_flag(BOOLEAN_MODE);
      a.child.get_order_item(0).expr_ = &a.match; }},
    {"field boost", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.add_match_predicate());
      ASSERT_EQ(OB_SUCCESS, a.match.get_columns_boosts().push_back(&a.literal));
      a.child.get_order_item(0).expr_ = &a.match; }},
    {"parameter text", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.add_match_predicate());
      a.match.set_param_text_expr(&a.search_key);
      a.child.get_order_item(0).expr_ = &a.match; }},
    {"ES match", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.add_match_predicate());
      a.match.set_expr_type(T_FUN_ES_MATCH);
      a.child.get_order_item(0).expr_ = &a.match; }},
    {"calculated search key", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.add_match_predicate());
      a.match.set_search_key(&a.arithmetic);
      a.child.get_order_item(0).expr_ = &a.match; }},
    {"calculated match column", [](CountStarViewAst &a) {
      ASSERT_EQ(OB_SUCCESS, a.add_match_predicate());
      a.match.get_match_columns().at(0) = &a.arithmetic;
      a.child.get_order_item(0).expr_ = &a.match; }},
  });
}
} // namespace
} // namespace sql
} // namespace oceanbase
