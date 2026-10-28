/* Copyright (c) 2026 OceanBase. Apache-2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/dml/ai_column_dml.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/printer/ob_raw_expr_printer.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/resolver/ob_schema_checker.h"
#include "share/ob_dml_sql_splicer.h"

namespace oceanbase::sql
{
using namespace common;
using namespace share;
using namespace share::schema;
namespace
{
class AiDmlReadScope
{
public:
  explicit AiDmlReadScope(ObSQLSessionInfo &session)
      : session_(session), old_(session.ai_column_internal_read())
  { session_.set_ai_column_internal_read(true); }
  ~AiDmlReadScope() { session_.set_ai_column_internal_read(old_); }
private:
  ObSQLSessionInfo &session_;
  bool old_;
};

int ai_dml_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name.ptr()[i] == '`') { ret = sql.append("``"); }
    else { ret = sql.append(name.ptr() + i, 1); }
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); }
  return ret;
}

int64_t ai_dml_index(const ObDMLBaseCtDef &definition, uint64_t id)
{
  for (int64_t i = 0; i < definition.column_ids_.count(); ++i) {
    if (definition.column_ids_.at(i) == id) { return i; }
  }
  return -1;
}

int ai_dml_value(ObTableModifyOp &op, const ObDMLBaseCtDef &definition,
                 const ExprFixedArray &row, uint64_t id, ObIAllocator &allocator, ObObj &value)
{
  int ret = OB_SUCCESS;
  const int64_t index = ai_dml_index(definition, id);
  ObDatum *datum = nullptr;
  if (index < 0 || index >= row.count() || row.at(index) == nullptr) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(row.at(index)->eval(op.get_eval_ctx(), datum))) {
  } else if (datum == nullptr) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(datum->to_obj(value, row.at(index)->obj_meta_))) {
  } else if (!value.is_null() && (value.is_lob_storage() || value.is_json())) {
    ObString bytes;
    const auto collation = value.get_collation_type();
    const bool is_json = value.is_json();
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(op.get_exec_ctx(), &allocator, value, bytes))) {
    } else if (is_json) {
      value.set_json_value(ObJsonType, bytes.ptr(), bytes.length());
    } else {
      value.set_varchar(bytes);
      value.set_collation_type(collation);
    }
  }
  return ret;
}

int ai_dml_predicate(ObTableModifyOp &op, const ObDMLBaseCtDef &definition,
                     const ExprFixedArray &row, const ObTableSchema &table,
                     ObIAllocator &allocator, ObSqlString &predicate)
{
  int ret = OB_SUCCESS;
  const auto &key = table.get_rowkey_info();
  for (int64_t i = 0; OB_SUCC(ret) && i < key.get_size(); ++i) {
    const auto *column = table.get_column_schema(key.get_column(i)->column_id_);
    ObObj value;
    char *literal = nullptr;
    int64_t length = 0, pos = 0;
    ObObjPrintParams params = op.get_obj_print_params();
    params.character_hex_safe_represent_ = true;
    params.binary_string_print_hex_ = true;
    if (column == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else if (i > 0 && OB_FAIL(predicate.append(" AND "))) {
    } else if (OB_FAIL(ai_dml_identifier(predicate, column->get_column_name_str()))) {
    } else if (OB_FAIL(predicate.append(" = "))) {
    } else if (OB_FAIL(ai_dml_value(op, definition, row, column->get_column_id(), allocator, value))) {
    } else if (value.is_null()) { ret = OB_INVALID_DATA; }
    else if (value.is_timestamp() && value.get_timestamp() != ObTimeConverter::ZERO_DATETIME) {
      // The worker does not inherit the business session's time zone. A numeric
      // Unix instant is interpreted in the receiving session by FROM_UNIXTIME.
      const int64_t epoch = value.get_timestamp();
      const uint64_t magnitude = epoch < 0 ? static_cast<uint64_t>(-(epoch + 1)) + 1 : epoch;
      ret = predicate.append_fmt("FROM_UNIXTIME(%s%lu.%06lu)", epoch < 0 ? "-" : "",
          magnitude / 1000000, magnitude % 1000000);
    }
    else if (OB_FAIL(value.print_sql_literal(literal, length, pos, allocator, params))) {
    } else { ret = predicate.append(literal, pos); }
  }
  if (OB_SUCC(ret) && predicate.empty()) { ret = OB_ERR_UNEXPECTED; }
  return ret;
}

int ai_dml_print(ObRawExpr *expression, ObSchemaGetterGuard &guard, ObIAllocator &allocator,
                 ObObjPrintParams params, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  params.character_hex_safe_represent_ = true;
  params.binary_string_print_hex_ = true;
  for (int64_t size = 8192; size <= OB_MAX_SQL_LENGTH; size *= 2) {
    char *buffer = static_cast<char *>(allocator.alloc(size));
    if (buffer == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; break; }
    int64_t pos = 0;
    ObRawExprPrinter printer(buffer, size, &pos, &guard, params);
    ret = printer.do_print(expression, T_NONE_SCOPE);
    if (ret == OB_SIZE_OVERFLOW || ret == OB_BUF_NOT_ENOUGH) { allocator.free(buffer); continue; }
    if (OB_SUCC(ret)) { ret = sql.append(buffer, pos); }
    allocator.free(buffer);
    break;
  }
  return ret;
}

int ai_dml_capture(ObTableModifyOp &op, const ObDMLBaseCtDef &definition,
                   const ObTableSchema &table, const ObColumnSchemaV2 &column,
                   ObSchemaGetterGuard &guard, ObIAllocator &allocator, ObString &snapshot)
{
  int ret = OB_SUCCESS;
  ObSchemaChecker checker;
  ObRawExprFactory factory(allocator);
  ObRawExpr *expression = nullptr;
  ObSEArray<ObQualifiedName, 4> names;
  auto &session = *op.get_exec_ctx().get_my_session();
  if (OB_FAIL(checker.init(guard))) {
  } else if (OB_FAIL(ObRawExprUtils::build_generated_column_expr(
          column.get_ai_column_schema().get_expression(), factory, session, expression,
          names, &table, nullptr, &checker, ObResolverUtils::DISABLE_CHECK))) {
  } else if (expression == nullptr || expression->get_param_count() < 2 || expression->get_param_count() > 3) {
    ret = OB_INVALID_DATA;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
    const auto *input = table.get_column_schema(names.at(i).col_name_);
    ObObj value;
    ObConstRawExpr *constant = nullptr;
    if (input == nullptr || input->is_ai_column() || names.at(i).ref_expr_ == nullptr) { ret = OB_INVALID_DATA; }
    else if (OB_FAIL(ai_dml_value(op, definition, definition.new_row_, input->get_column_id(), allocator, value))) {
    } else if (OB_FAIL(ObRawExprUtils::build_const_obj_expr(factory, value, constant))) {
    } else { ret = ObRawExprUtils::replace_ref_column(expression, names.at(i).ref_expr_, constant); }
  }
  ObSqlString query;
  const bool prompt_json = OB_SUCC(ret) && expression->get_param_expr(1)->get_data_type() == ObJsonType;
  if (OB_SUCC(ret)) { ret = query.append("SELECT /*+ USE_PLAN_CACHE(NONE) */ CAST(JSON_OBJECT('args',JSON_ARRAY("); }
  for (int64_t i = 0; OB_SUCC(ret) && i < expression->get_param_count(); ++i) {
    if (i > 0 && OB_FAIL(query.append(","))) {
    } else {
      const bool string_input = i == 0 || (i == 1 && !prompt_json);
      if (string_input) { ret = query.append("CAST("); }
      if (OB_SUCC(ret)) { ret = ai_dml_print(expression->get_param_expr(i), guard, allocator, op.get_obj_print_params(), query); }
      if (OB_SUCC(ret) && string_input) { ret = query.append(" AS CHAR CHARACTER SET utf8mb4)"); }
    }
  }
  if (OB_SUCC(ret)) {
    ret = query.append_fmt("),'prompt_json',%s) AS CHAR CHARACTER SET utf8mb4)", prompt_json ? "TRUE" : "FALSE");
  }
  if (OB_SUCC(ret)) {
    SMART_VAR(ObMySQLProxy::MySQLResult, response) {
      if (OB_FAIL(op.execute_read(query.ptr(), response))) {
      } else if (response.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else {
        ObString captured;
        if (OB_FAIL(response.get_result()->next())) {
        } else if (OB_FAIL(response.get_result()->get_varchar(static_cast<int64_t>(0), captured))) {
        } else { ret = ob_write_string(allocator, captured, snapshot); }
        const int close_ret = response.get_result()->close();
        if (OB_SUCC(ret)) { ret = close_ret; }
      }
    }
  }
  return ret;
}

int ai_dml_cleanup(ObTableModifyOp &op, uint64_t table_id, const ObString &predicate)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.append_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND rowkey_predicate=", table_id))) {
  } else if (OB_FAIL(sql_append_hex_escape_str(predicate, sql))) {
  } else { ret = op.execute_write(sql.ptr()); }
  if (OB_SUCC(ret)) {
    sql.reset();
    if (OB_FAIL(sql.append_fmt("DELETE FROM oceanbase.__all_ai_column_group WHERE table_id=%lu AND rowkey_predicate=", table_id))) {
    } else if (OB_FAIL(sql_append_hex_escape_str(predicate, sql))) {
    } else { ret = op.execute_write(sql.ptr()); }
  }
  return ret;
}

int ai_dml_current_row(ObTableModifyOp &op, ObSchemaGetterGuard &guard, const ObTableSchema &table,
    const ObColumnSchemaV2 &column, const ObString &predicate, const ObString &request, bool &current)
{
  int ret = OB_SUCCESS;
  current = false;
  const ObDatabaseSchema *database = nullptr;
  const auto *marker = table.get_column_schema(column.get_ai_column_schema().get_request_column_id());
  ObSqlString sql;
  if (marker == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(guard.get_database_schema(table.get_database_id(), database))) {
  } else if (database == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(sql.append("SELECT /*+ USE_PLAN_CACHE(NONE) */ 1 FROM "))) {
  } else if (OB_FAIL(ai_dml_identifier(sql, database->get_database_name_str()))) {
  } else if (OB_FAIL(sql.append("."))) {
  } else if (OB_FAIL(ai_dml_identifier(sql, table.get_table_name_str()))) {
  } else if (OB_FAIL(sql.append(" WHERE ")) || OB_FAIL(sql.append(predicate)) || OB_FAIL(sql.append(" AND "))) {
  } else if (OB_FAIL(ai_dml_identifier(sql, marker->get_column_name_str()))) {
  } else if (OB_FAIL(sql.append("=")) || OB_FAIL(sql_append_hex_escape_str(request, sql))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, response) {
      if (OB_FAIL(op.execute_read(sql.ptr(), response))) {
      } else if (response.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else {
        ret = response.get_result()->next();
        if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
        else if (OB_SUCC(ret)) { current = true; }
        const int close_ret = response.get_result()->close();
        if (OB_SUCC(ret)) { ret = close_ret; }
      }
    }
  }
  return ret;
}
}

int AiColumnDml::after_row(ObDMLModifyRowNode &row)
{
  int ret = OB_SUCCESS;
  if (row.dml_ctdef_ == nullptr || !row.dml_ctdef_->has_ai_column_) { return ret; }
  auto &op = *row.dml_op_;
  const auto &definition = *row.dml_ctdef_;
  auto *session = op.get_exec_ctx().get_my_session();
  auto *context = op.get_exec_ctx().get_sql_ctx();
  const ObTableSchema *table = nullptr;
  if (session == nullptr || context == nullptr || context->schema_guard_ == nullptr) { return OB_ERR_UNEXPECTED; }
  auto &guard = *context->schema_guard_;
  if (OB_FAIL(guard.get_table_schema(definition.das_base_ctdef_.index_tid_, table))) {
  } else if (table == nullptr || !table->has_ai_column()) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(op.open_inner_conn())) {
  } else if (OB_FAIL(op.begin_nested_session(true))) {
  } else {
    AiDmlReadScope scope(*session);
    ObArenaAllocator allocator("AiColumnDml");
    ObSqlString predicate;
    const bool deleting = row.dml_event_ == ObDmlEventType::DE_DELETING;
    const bool updating = row.dml_event_ == ObDmlEventType::DE_UPDATING;
    if (OB_FAIL(ai_dml_predicate(op, definition, deleting ? definition.old_row_ : definition.new_row_,
            *table, allocator, predicate))) {
    } else if (deleting) {
      ret = ai_dml_cleanup(op, table->get_table_id(), predicate.string());
    } else {
      ObSqlString members;
      ObString group_request;
      ObSEArray<const ObColumnSchemaV2 *, 4> columns;
      if (OB_FAIL(members.append("["))) {
      }
      for (auto it = table->column_begin(); OB_SUCC(ret) && it != table->column_end(); ++it) {
        const auto &column = **it;
        if (!column.is_ai_column()) { continue; }
        const auto &ai = column.get_ai_column_schema();
        ObObj current, previous;
        if (OB_FAIL(ai_dml_value(op, definition, definition.new_row_, ai.get_request_column_id(), allocator, current))) {
        } else if (updating && OB_FAIL(ai_dml_value(op, definition, definition.old_row_, ai.get_request_column_id(), allocator, previous))) {
        } else if (updating && current == previous) {
        } else if (current.is_null() || current.get_string().length() != 32) { ret = OB_INVALID_DATA; }
        else {
          if (group_request.empty()) { ret = ob_write_string(allocator, current.get_string(), group_request); }
          else if (group_request != current.get_string()) { ret = OB_INVALID_DATA; }
          if (OB_SUCC(ret) && OB_FAIL(members.append_fmt("%s%lu", columns.empty() ? "" : ",", column.get_column_id()))) {
          } else if (OB_SUCC(ret)) { ret = columns.push_back(&column); }
        }
      }
      if (OB_SUCC(ret)) { ret = members.append("]"); }
      bool current = false;
      if (OB_SUCC(ret) && !columns.empty()) {
        ret = ai_dml_current_row(op, guard, *table, *columns.at(0), predicate.string(), group_request, current);
        if (OB_SUCC(ret) && !current) { columns.reuse(); }
      }
      if (OB_SUCC(ret) && !columns.empty()) {
        ObObj incarnation_value;
        if (OB_FAIL(ai_dml_value(op, definition, definition.new_row_,
                columns.at(0)->get_ai_column_schema().get_incarnation_column_id(), allocator, incarnation_value))) {
        } else if (incarnation_value.is_null() || incarnation_value.get_string().length() != 32) {
          ret = OB_INVALID_DATA;
        }
        const ObString incarnation = incarnation_value.is_null() ? ObString() : incarnation_value.get_string();
        if (OB_SUCC(ret) && updating) {
          ObSqlString previous_key;
          if (OB_FAIL(ai_dml_predicate(op, definition, definition.old_row_, *table, allocator, previous_key))) {
          } else if (previous_key.string() != predicate.string()) {
            // Every AI column is invalidated by a primary-key change.
            ret = ai_dml_cleanup(op, table->get_table_id(), previous_key.string());
          } else {
            for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
              ObObj previous;
              ObSqlString supersede;
              if (OB_FAIL(ai_dml_value(op, definition, definition.old_row_,
                      columns.at(i)->get_ai_column_schema().get_request_column_id(), allocator, previous))) {
              } else if (previous.is_null()) {
              } else if (OB_FAIL(supersede.append_fmt(
                      "UPDATE oceanbase.__all_ai_column_task SET state=5,lease_deadline=NULL "
                      "WHERE table_id=%lu AND column_id=%lu AND request_id=",
                      table->get_table_id(), columns.at(i)->get_column_id()))) {
              } else if (OB_FAIL(sql_append_hex_escape_str(previous.get_string(), supersede))) {
              } else { ret = op.execute_write(supersede.ptr()); }
            }
          }
        }
        ObDMLSqlSplicer group;
        ObSqlString sql;
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(group.add_pk_column("request_id", group_request))
            || OB_FAIL(group.add_column("table_id", table->get_table_id()))
            || OB_FAIL(group.add_column("row_incarnation", incarnation))
            || OB_FAIL(group.add_column("rowkey_predicate", ObHexEscapeSqlStr(predicate.string())))
            || OB_FAIL(group.add_column("member_column_ids", members.string()))
            || OB_FAIL(group.splice_insert_sql("oceanbase.__all_ai_column_group", sql))) {
        } else { ret = op.execute_write(sql.ptr()); }
        for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
          const auto &column = *columns.at(i);
          ObString snapshot;
          ObDMLSqlSplicer task;
          char full_task_id[36];
          char task_id[32];
          sql.reset();
          if (OB_FAIL(ai_dml_capture(op, definition, *table, column, guard, allocator, snapshot))) {
          } else if (OB_FAIL(ObExprUuid::gen_server_uuid(full_task_id, sizeof(full_task_id)))) {
          } else {
            int64_t pos = 0;
            for (char c : full_task_id) {
              if (c != '-') {
                if (pos >= sizeof(task_id)) { ret = OB_ERR_UNEXPECTED; break; }
                task_id[pos++] = c;
              }
            }
            if (OB_SUCC(ret) && pos != sizeof(task_id)) { ret = OB_ERR_UNEXPECTED; }
          }
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(task.add_pk_column("task_id", ObString(32, task_id)))
              || OB_FAIL(task.add_column("table_id", table->get_table_id()))
              || OB_FAIL(task.add_column("column_id", column.get_column_id()))
              || OB_FAIL(task.add_column("row_incarnation", incarnation))
              || OB_FAIL(task.add_column("request_id", group_request))
              || OB_FAIL(task.add_column("definition_version", column.get_ai_column_schema().get_definition_version()))
              || OB_FAIL(task.add_column("rowkey_predicate", ObHexEscapeSqlStr(predicate.string())))
              || OB_FAIL(task.add_column("input_snapshot", ObHexEscapeSqlStr(snapshot)))
              || OB_FAIL(task.add_column("max_retries", column.get_ai_column_schema().get_max_retries()))
              || OB_FAIL(task.add_time_column("eligible_ts", ObTimeUtility::current_time()))
              || OB_FAIL(task.splice_insert_sql("oceanbase.__all_ai_column_task", sql))) {
          } else { ret = op.execute_write(sql.ptr()); }
        }
      }
    }
    const int end_ret = op.end_nested_session();
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  if (OB_FAIL(ret)) { LOG_WARN("AI COLUMN transactional outbox failed", K(ret)); }
  return ret;
}
}
