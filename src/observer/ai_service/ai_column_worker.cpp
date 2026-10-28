/* Copyright (c) 2026 OceanBase. Apache-2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "observer/ai_service/ai_column_worker.h"
#include "observer/ai_service/ob_ai_service_executor.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/object/ob_obj_cast.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/engine/expr/ob_expr_column_conv.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "query/vector/ob_vector_index_util.h"
#include "common/ob_timeout_ctx.h"
#include "share/ob_share_util.h"
#include <algorithm>

namespace oceanbase::observer
{
using namespace common;
using namespace common::sqlclient;
using namespace share;
using namespace share::schema;
using namespace share::ai_column;
using namespace sql;
namespace
{
constexpr int64_t COLUMN_LEASE_US = 30 * 1000 * 1000;
constexpr int64_t COLUMN_SQL_TIMEOUT_US = 5 * 1000 * 1000;
// Each seekdb process owns one isolated runtime database and its own outbox.
constexpr uint64_t COLUMN_DATABASE_SCOPE = 1;

class AiWorkerAccess
{
public:
  explicit AiWorkerAccess(ObMySQLTransaction &transaction, bool write)
      : session_(query::ObInnerSQLConnectionAccess::get_session(transaction.get_connection()))
  {
    if (session_ != nullptr) {
      read_ = session_->ai_column_internal_read();
      write_ = session_->ai_column_internal_write();
      session_->set_ai_column_internal_read(true);
      session_->set_ai_column_internal_write(write);
    }
  }
  ~AiWorkerAccess()
  {
    if (session_ != nullptr) {
      session_->set_ai_column_internal_read(read_);
      session_->set_ai_column_internal_write(write_);
    }
  }
private:
  ObSQLSessionInfo *session_ = nullptr;
  bool read_ = false;
  bool write_ = false;
};

int ai_worker_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name.ptr()[i] == '`') { ret = sql.append("``"); }
    else { ret = sql.append(name.ptr() + i, 1); }
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); }
  return ret;
}

int ai_worker_table_name(ObSchemaGetterGuard &guard, const ObTableSchema &table, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  const ObDatabaseSchema *database = nullptr;
  if (OB_FAIL(guard.get_database_schema(table.get_database_id(), database))) {
  } else if (database == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(ai_worker_identifier(sql, database->get_database_name_str()))) {
  } else if (OB_FAIL(sql.append("."))) {
  } else { ret = ai_worker_identifier(sql, table.get_table_name_str()); }
  return ret;
}

int ai_worker_parse_id(const ObString &text, RequestId &id)
{
  return parse_request_id(std::span<const char>(text.ptr(), text.length()), id) == StateResult::OK
      ? OB_SUCCESS : OB_INVALID_DATA;
}

int ai_worker_request_text(RequestId id, char (&text)[32])
{
  return format_request_id(id, text) == StateResult::OK ? OB_SUCCESS : OB_INVALID_ARGUMENT;
}

int ai_worker_string(ObIJsonBase *node, ObString &value)
{
  if (node == nullptr || node->json_type() != ObJsonNodeType::J_STRING
      || node->get_data_length() > INT32_MAX) { return OB_INVALID_DATA; }
  value.assign_ptr(node->get_data(), static_cast<int32_t>(node->get_data_length()));
  return OB_SUCCESS;
}

int ai_worker_current(ObMySQLTransaction &transaction, ObSchemaGetterGuard &guard,
    const ObTableSchema &table, const ObColumnSchemaV2 &column, const DurableTask &task, bool &current)
{
  int ret = OB_SUCCESS;
  current = false;
  AiWorkerAccess scope(transaction, false);
  const auto &definition = column.get_ai_column_schema();
  const auto *incarnation = table.get_column_schema(definition.get_incarnation_column_id());
  const auto *request = table.get_column_schema(definition.get_request_column_id());
  const auto *spec = table.get_column_schema(definition.get_result_spec_column_id());
  char incarnation_text[32], request_text[32];
  ObSqlString sql;
  if (incarnation == nullptr || request == nullptr || spec == nullptr) { ret = OB_INVALID_DATA; }
  else if (OB_FAIL(ai_worker_request_text(task.record.version.row_incarnation, incarnation_text))
      || OB_FAIL(ai_worker_request_text(task.record.version.request_id, request_text))) {
  } else if (OB_FAIL(sql.append("SELECT /*+ USE_PLAN_CACHE(NONE) */ 1 FROM "))) {
  } else if (OB_FAIL(ai_worker_table_name(guard, table, sql))) {
  } else if (OB_FAIL(sql.append(" WHERE ")) || OB_FAIL(sql.append(task.rowkey_predicate))) {
  } else if (OB_FAIL(sql.append(" AND ")) || OB_FAIL(ai_worker_identifier(sql, incarnation->get_column_name_str()))
      || OB_FAIL(sql.append("=")) || OB_FAIL(sql_append_hex_escape_str(ObString(32, incarnation_text), sql))
      || OB_FAIL(sql.append(" AND ")) || OB_FAIL(ai_worker_identifier(sql, request->get_column_name_str()))
      || OB_FAIL(sql.append("=")) || OB_FAIL(sql_append_hex_escape_str(ObString(32, request_text), sql))
      || OB_FAIL(sql.append(" AND ")) || OB_FAIL(ai_worker_identifier(sql, spec->get_column_name_str()))
      || OB_FAIL(sql.append_fmt("=%lu", task.record.version.definition_version))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, response) {
      if (OB_FAIL(transaction.read(response, sql.ptr()))) {
      } else if (response.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else if (OB_FAIL(response.get_result()->next())) {
        if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
      } else { current = true; }
    }
  }
  return ret;
}

int ai_worker_typed_result(ObArenaAllocator &allocator, const ObColumnSchemaV2 &column,
    const ObString &text, ObString &expression)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (column.is_json()) {
    ObIJsonBase *json = nullptr;
    if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, text,
            ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, json))) {
    } else if (json == nullptr) { ret = OB_INVALID_DATA; }
    else if (OB_FAIL(sql.append("CAST(CONVERT(")) || OB_FAIL(sql_append_hex_escape_str(text, sql))
        || OB_FAIL(sql.append(" USING utf8mb4) AS JSON)"))) {
    } else { ret = ob_write_string(allocator, sql.string(), expression); }
  } else if (column.is_collection()) {
    // MySQL's VECTOR column conversion accepts its validated array text;
    // VECTOR(n) is a column type, not a CAST target in this SQL grammar.
    if (OB_FAIL(sql.append("CONVERT(")) || OB_FAIL(sql_append_hex_escape_str(text, sql))
        || OB_FAIL(sql.append(" USING utf8mb4)"))) {
    } else { ret = ob_write_string(allocator, sql.string(), expression); }
  } else {
    ObObj source, converted;
    source.set_varchar(text);
    source.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ObExprResType type;
    type.set_type(column.get_data_type());
    type.set_collation_type(column.get_collation_type());
    type.set_accuracy(column.get_accuracy());
    ObCastCtx cast(&allocator, nullptr, CM_STRICT_MODE | CM_STRICT_JSON | CM_ERROR_ON_SCALE_OVER,
        column.get_collation_type());
    if (OB_FAIL(ObExprColumnConv::convert_with_null_check(converted, source, type, true,
            cast, &column.get_extended_type_info()))) {
    } else if (cast.warning_ != OB_SUCCESS) { ret = OB_INVALID_DATA; }
    else {
      char *literal = nullptr;
      int64_t size = 0, pos = 0;
      ObTimeZoneInfo utc;
      ObObjPrintParams params(&utc, CS_TYPE_UTF8MB4_BIN);
      params.character_hex_safe_represent_ = true;
      params.binary_string_print_hex_ = true;
      if (OB_FAIL(converted.print_sql_literal(literal, size, pos, allocator, params))) {
      } else { ret = ob_write_string(allocator, ObString(static_cast<int32_t>(pos), literal), expression); }
    }
  }
  return ret;
}

int ai_worker_infer(ObArenaAllocator &allocator, ObSchemaGetterGuard &guard,
    const ObColumnSchemaV2 &column, const ObString &snapshot, ObString &expression, bool &is_null)
{
  int ret = OB_SUCCESS;
  is_null = false;
  ObIJsonBase *root = nullptr;
  ObJsonArray *args = nullptr;
  bool json_prompt = false;
  if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, snapshot,
          ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, root))) {
  } else if (root == nullptr || root->json_type() != ObJsonNodeType::J_OBJECT) { ret = OB_INVALID_DATA; }
  else {
    auto *object = static_cast<ObJsonObject *>(root);
    auto *array = object->get_value("args");
    auto *kind = object->get_value("prompt_json");
    if (array == nullptr || array->json_type() != ObJsonNodeType::J_ARRAY || kind == nullptr) {
      ret = OB_INVALID_DATA;
    } else {
      args = static_cast<ObJsonArray *>(array);
      json_prompt = kind->json_type() == ObJsonNodeType::J_BOOLEAN ? kind->get_boolean()
          : kind->json_type() == ObJsonNodeType::J_INT && kind->get_int() != 0;
      if (args->element_count() < 2 || args->element_count() > 3) { ret = OB_INVALID_DATA; }
    }
  }
  if (OB_FAIL(ret)) { return ret; }
  if (args->get_value(0)->json_type() == ObJsonNodeType::J_NULL
      || args->get_value(1)->json_type() == ObJsonNodeType::J_NULL) {
    is_null = true;
    return OB_SUCCESS;
  }
  ObString model_name, prompt;
  ObAIFuncExprInfo *info = nullptr;
  ObAiModelEndpointInfo endpoint;
  if (OB_FAIL(ai_worker_string(args->get_value(0), model_name))) {
  } else if (json_prompt && !column.is_collection()) {
    auto *value = args->get_value(1);
    if (value->json_type() != ObJsonNodeType::J_OBJECT
        || !ObAIFuncPromptObjectUtils::is_valid_prompt_object(static_cast<ObJsonObject *>(value))) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      ret = ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
          allocator, static_cast<ObJsonObject *>(value), prompt);
    }
  } else { ret = ai_worker_string(args->get_value(1), prompt); }
  if (OB_FAIL(ret)) {
  } else if (model_name.empty() || prompt.empty()) { ret = OB_INVALID_ARGUMENT; }
  else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_name, guard, info))) {
  } else if (info == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(ObAiServiceExecutor::read_ai_endpoint_by_ai_model_name(allocator, model_name, endpoint))) {
  } else {
    ObAIFuncModel model(allocator, *info, endpoint);
    model.set_single_attempt(true);
    ObString raw;
    if (column.is_collection()) {
      int64_t dimension = 0;
      ObJsonObject config(&allocator);
      ObJsonInt dim(0);
      if (OB_FAIL(ObVectorIndexUtil::get_vector_dim_from_extend_type_info(column.get_extended_type_info(), dimension))) {
      } else if (args->element_count() == 3
          && (args->get_value(2)->json_type() != ObJsonNodeType::J_INT || args->get_value(2)->get_int() != dimension)) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        dim.set_value(dimension);
        if (OB_FAIL(config.add("dimensions", &dim))) {
        } else if (OB_FAIL(model.call_dense_embedding(prompt, &config, raw))) {
        } else { ret = ai_worker_typed_result(allocator, column, raw, expression); }
      }
    } else {
      ObJsonObject *config = nullptr;
      if (args->element_count() == 3 && args->get_value(2)->json_type() != ObJsonNodeType::J_NULL) {
        auto *value = args->get_value(2);
        if (value->json_type() == ObJsonNodeType::J_STRING) {
          ObString json;
          if (OB_FAIL(ai_worker_string(value, json))) {
          } else { ret = ObAIFuncJsonUtils::get_json_object_form_str(allocator, json, config); }
        } else if (value->json_type() == ObJsonNodeType::J_OBJECT) {
          config = static_cast<ObJsonObject *>(value);
        } else { ret = OB_INVALID_ARGUMENT; }
      }
      const ObString hint = column.is_json() ? ObString("Return exactly one valid JSON value.")
          : ob_is_integer_type(column.get_data_type()) ? ObString("Return exactly one base-10 integer.") : ObString();
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(model.call_completion(prompt, config, raw, hint))) {
      // call_completion prints the provider's string node with is_quoted=false:
      // raw already contains the decoded content, including JSON result text.
      } else { ret = ai_worker_typed_result(allocator, column, raw, expression); }
    }
  }
  return ret;
}

int ai_worker_supersede(ObMySQLTransaction &transaction, const DurableTask &task)
{
  int ret = OB_SUCCESS;
  char owner[32];
  ObSqlString sql;
  int64_t affected = 0;
  if (OB_FAIL(ai_worker_request_text(task.record.claim.owner, owner))) {
  } else if (OB_FAIL(sql.append_fmt("UPDATE oceanbase.__all_ai_column_task SET state=5,lease_deadline=NULL "
          "WHERE task_id='%.*s' AND state=1 AND claim_epoch=%lu AND lease_owner='%.*s'",
          task.task_id.length(), task.task_id.ptr(), task.record.claim.epoch, 32, owner))) {
  } else { ret = transaction.write(sql.ptr(), affected); }
  return ret;
}

struct AiPublishTask
{
  TaskRecord record;
  ObString value;
  bool is_null = false;
};

int ai_worker_erase_group(ObMySQLTransaction &transaction, const ObString &request)
{
  int ret = OB_SUCCESS;
  for (const char *table : {"__all_ai_column_task", "__all_ai_column_group"}) {
    if (OB_FAIL(ret)) { break; }
    ObSqlString sql;
    int64_t affected = 0;
    if (OB_FAIL(sql.append_fmt("DELETE FROM oceanbase.%s WHERE request_id=", table))
        || OB_FAIL(sql_append_hex_escape_str(request, sql))) {
    } else { ret = transaction.write(sql.ptr(), affected); }
  }
  return ret;
}

int ai_worker_publish(ObMySQLTransaction &transaction, ObArenaAllocator &allocator,
    ObSchemaGetterGuard &guard, uint64_t table_id, const ObString &request,
    const ObString &incarnation, const ObString &predicate, const ObString &manifest)
{
  int ret = OB_SUCCESS;
  int phase = 0;
  struct PublishDiagnostic {
    int &ret;
    int &phase;
    uint64_t table_id;
    ~PublishDiagnostic() {
      if (ret != OB_SUCCESS && ret != OB_ITER_END) {
        LOG_WARN("AI COLUMN group publication failed", K(ret), K(phase), K(table_id));
      }
    }
  } diagnostic{ret, phase, table_id};
  AiWorkerAccess scope(transaction, true);
  const ObTableSchema *table = nullptr;
  ObIJsonBase *json = nullptr;
  RequestId request_id, incarnation_id;
  ObSEArray<uint64_t, 4> members;
  if (OB_FAIL(ai_worker_parse_id(request, request_id)) || OB_FAIL(ai_worker_parse_id(incarnation, incarnation_id))) {
  } else if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, manifest,
          ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, json))) {
  } else if (json == nullptr || json->json_type() != ObJsonNodeType::J_ARRAY || json->element_count() == 0) {
    ret = OB_INVALID_DATA;
  } else {
    auto *array = static_cast<ObJsonArray *>(json);
    for (int64_t i = 0; OB_SUCC(ret) && i < array->element_count(); ++i) {
      auto *value = array->get_value(i);
      if (value == nullptr || value->json_type() != ObJsonNodeType::J_INT || value->get_int() <= 0) {
        ret = OB_INVALID_DATA;
      } else {
        const uint64_t id = value->get_int();
        for (int64_t j = 0; OB_SUCC(ret) && j < members.count(); ++j) {
          if (members.at(j) == id) { ret = OB_INVALID_DATA; }
        }
        if (OB_SUCC(ret)) { ret = members.push_back(id); }
      }
    }
  }
  if (OB_FAIL(ret)) { return ret; }
  phase = 1;
  if (OB_FAIL(guard.get_table_schema(table_id, table))) { return ret; }
  if (table == nullptr || !table->has_ai_column()) { return ai_worker_erase_group(transaction, request); }
  ObSEArray<const ObColumnSchemaV2 *, 4> columns;
  for (int64_t i = 0; OB_SUCC(ret) && i < members.count(); ++i) {
    const auto *column = table->get_column_schema(members.at(i));
    if (column != nullptr && column->is_ai_column()) { ret = columns.push_back(column); }
  }
  if (OB_FAIL(ret)) { return ret; }
  if (columns.empty()) { return ai_worker_erase_group(transaction, request); }
  std::sort(columns.get_data(), columns.get_data() + columns.count(),
      [](const ObColumnSchemaV2 *a, const ObColumnSchemaV2 *b) { return a->get_column_id() < b->get_column_id(); });
  // Lock the business row first, matching T1's row -> metadata/task lock order.
  // HTTP is already complete; no network request is made while these locks exist.
  ObSqlString sql;
  const auto *row_identity = table->get_column_schema(columns.at(0)->get_ai_column_schema().get_incarnation_column_id());
  if (row_identity == nullptr) { return OB_INVALID_DATA; }
  if (OB_FAIL(sql.append("SELECT /*+ USE_PLAN_CACHE(NONE) */ "))
      || OB_FAIL(ai_worker_identifier(sql, row_identity->get_column_name_str()))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    const auto &definition = columns.at(i)->get_ai_column_schema();
    const auto *marker = table->get_column_schema(definition.get_request_column_id());
    const auto *version = table->get_column_schema(definition.get_result_spec_column_id());
    if (marker == nullptr || version == nullptr) { ret = OB_INVALID_DATA; }
    else if (OB_FAIL(sql.append(",")) || OB_FAIL(ai_worker_identifier(sql, marker->get_column_name_str()))
        || OB_FAIL(sql.append(",")) || OB_FAIL(ai_worker_identifier(sql, version->get_column_name_str()))) {
    }
  }
  if (OB_SUCC(ret) && (OB_FAIL(sql.append(" FROM ")) || OB_FAIL(ai_worker_table_name(guard, *table, sql))
          || OB_FAIL(sql.append(" WHERE ")) || OB_FAIL(sql.append(predicate)) || OB_FAIL(sql.append(" FOR UPDATE")))) {
  }
  phase = 2;
  ObSEArray<ColumnVersion, 4> current;
  bool exists = false;
  if (OB_SUCC(ret)) {
    SMART_VAR(ObMySQLProxy::MySQLResult, response) {
      if (OB_FAIL(transaction.read(response, sql.ptr()))) {
      } else if (response.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else if (OB_FAIL(response.get_result()->next())) {
        if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
      } else {
        ObString actual_incarnation;
        if (OB_FAIL(response.get_result()->get_varchar(static_cast<int64_t>(0), actual_incarnation))) {
          if (ret == OB_ERR_NULL_VALUE) { ret = OB_SUCCESS; }
        } else if (actual_incarnation == incarnation) { exists = true; }
        else { LOG_INFO("AI COLUMN publication found a replaced row", K(table_id), K(request),
            K(actual_incarnation), "expected_incarnation", incarnation); }
        for (int64_t i = 0; OB_SUCC(ret) && exists && i < columns.count(); ++i) {
          ObString actual_request;
          uint64_t actual_version = 0;
          ColumnVersion version{COLUMN_DATABASE_SCOPE, table_id, columns.at(i)->get_column_id(), incarnation_id, {}, 0};
          ret = response.get_result()->get_varchar(i * 2 + 1, actual_request);
          if (ret == OB_ERR_NULL_VALUE) { ret = OB_SUCCESS; }
          else if (OB_SUCC(ret)) { ret = ai_worker_parse_id(actual_request, version.request_id); }
          if (OB_SUCC(ret) && OB_FAIL(response.get_result()->get_uint(i * 2 + 2, actual_version))) {
          } else if (OB_SUCC(ret)) {
            version.definition_version = actual_version;
            ret = current.push_back(version);
          }
        }
      }
    }
  }
  if (OB_FAIL(ret)) { return ret; }
  if (!exists) {
    LOG_INFO("AI COLUMN retiring group for missing or replaced row", K(table_id), K(request));
    return ai_worker_erase_group(transaction, request);
  }
  phase = 3;
  // Lock the immutable manifest after the row. It may have been removed by T1.
  sql.reset();
  if (OB_FAIL(sql.append("SELECT request_id FROM oceanbase.__all_ai_column_group WHERE request_id="))
      || OB_FAIL(sql_append_hex_escape_str(request, sql)) || OB_FAIL(sql.append(" FOR UPDATE"))) {
    return ret;
  }
  {
    SMART_VAR(ObMySQLProxy::MySQLResult, response) {
      if (OB_FAIL(transaction.read(response, sql.ptr()))) {
      } else if (response.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else { ret = response.get_result()->next(); }
    }
  }
  if (ret == OB_ITER_END) { return OB_SUCCESS; }
  if (OB_FAIL(ret)) { return ret; }
  // Definition rows fence concurrent ALTER, whose hidden row version may still
  // contain an old value until the newly-defined column has been regenerated.
  phase = 4;
  ObSEArray<int64_t, 4> versions;
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    int64_t version = 0;
    sql.reset();
    if (OB_FAIL(sql.append_fmt("SELECT definition_version FROM oceanbase.__all_ai_column "
            "WHERE table_id=%lu AND column_id=%lu FOR UPDATE", table_id, columns.at(i)->get_column_id()))) {
    } else {
      SMART_VAR(ObMySQLProxy::MySQLResult, response) {
        if (OB_FAIL(transaction.read(response, sql.ptr()))) {
        } else if (response.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
        else if (OB_FAIL(response.get_result()->next())) {
          if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
        } else { ret = response.get_result()->get_int(static_cast<int64_t>(0), version); }
      }
    }
    if (OB_SUCC(ret)) { ret = versions.push_back(version); }
  }
  phase = 5;
  ObSEArray<AiPublishTask, 4> tasks;
  sql.reset();
  if (OB_SUCC(ret) && (OB_FAIL(sql.append("SELECT column_id,definition_version,state,table_id,row_incarnation,"
          "CAST(rowkey_predicate AS CHAR) AS rowkey_predicate, result_value IS NULL AS is_null,"
          "CAST(IFNULL(result_value,'') AS CHAR) AS result_sql FROM oceanbase.__all_ai_column_task WHERE request_id="))
      || OB_FAIL(sql_append_hex_escape_str(request, sql)) || OB_FAIL(sql.append(" ORDER BY column_id FOR UPDATE")))) {
  }
  if (OB_SUCC(ret)) {
    SMART_VAR(ObMySQLProxy::MySQLResult, response) {
      if (OB_FAIL(transaction.read(response, sql.ptr()))) {
      } else if (response.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else {
        auto &result = *response.get_result();
        while (OB_SUCC(ret) && OB_SUCC(result.next())) {
          AiPublishTask task;
          int64_t column_id = 0, version = 0, state = 0, owner_table = 0, is_null = 0;
          ObString value, row_incarnation, row_predicate;
          if (OB_FAIL(result.get_int("column_id", column_id)) || OB_FAIL(result.get_int("definition_version", version))
              || OB_FAIL(result.get_int("state", state)) || OB_FAIL(result.get_int("table_id", owner_table))
              || OB_FAIL(result.get_int("is_null", is_null)) || OB_FAIL(result.get_varchar("result_sql", value))
              || OB_FAIL(result.get_varchar("row_incarnation", row_incarnation))
              || OB_FAIL(result.get_varchar("rowkey_predicate", row_predicate))) {
          } else if (column_id <= 0 || version <= 0 || owner_table != table_id || row_incarnation != incarnation
              || row_predicate != predicate || state < 0 || state > static_cast<int64_t>(TaskState::SUPERSEDED)) {
            ret = OB_INVALID_DATA;
          } else {
            bool belongs = false;
            for (int64_t i = 0; i < members.count(); ++i) { belongs |= members.at(i) == column_id; }
            for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
              if (tasks.at(i).record.version.column_id == column_id) { ret = OB_INVALID_DATA; }
            }
            if (!belongs) { ret = OB_INVALID_DATA; }
            task.record.version = {COLUMN_DATABASE_SCOPE, table_id, static_cast<uint64_t>(column_id),
                incarnation_id, request_id, static_cast<uint64_t>(version)};
            task.record.state = static_cast<TaskState>(state);
            task.is_null = is_null != 0;
            if (OB_SUCC(ret) && OB_FAIL(ob_write_string(allocator, value, task.value))) {
            } else if (OB_SUCC(ret)) { ret = tasks.push_back(task); }
          }
        }
        if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
      }
    }
  }
  if (OB_SUCC(ret) && tasks.count() != members.count()) { ret = OB_INVALID_DATA; }
  phase = 6;
  ObSEArray<uint64_t, 4> eligible;
  ObSEArray<TaskRecord, 4> ready;
  ObSEArray<ColumnVersion, 4> rows;
  ObSEArray<int64_t, 4> source_indices;
  for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
    for (int64_t j = 0; OB_SUCC(ret) && j < columns.count(); ++j) {
      if (columns.at(j)->get_column_id() == tasks.at(i).record.version.column_id
          && current.at(j) == tasks.at(i).record.version
          && versions.at(j) == tasks.at(i).record.version.definition_version
          && columns.at(j)->get_ai_column_schema().get_definition_version() == versions.at(j)) {
        if (OB_FAIL(eligible.push_back(columns.at(j)->get_column_id()))
            || OB_FAIL(ready.push_back(tasks.at(i).record)) || OB_FAIL(rows.push_back(current.at(j)))
            || OB_FAIL(source_indices.push_back(i))) {
        }
      } else if (columns.at(j)->get_column_id() == tasks.at(i).record.version.column_id) {
        LOG_INFO("AI COLUMN retiring stale definition or request", K(table_id), K(request),
            "column_id", columns.at(j)->get_column_id(), "row_version", current.at(j).definition_version,
            "task_version", tasks.at(i).record.version.definition_version, "metadata_version", versions.at(j),
            "schema_version", columns.at(j)->get_ai_column_schema().get_definition_version(),
            "row_request_hi", current.at(j).request_id.high, "row_request_lo", current.at(j).request_id.low,
            "task_request_hi", tasks.at(i).record.version.request_id.high, "task_request_lo", tasks.at(i).record.version.request_id.low);
      }
    }
  }
  if (OB_FAIL(ret)) { return ret; }
  if (eligible.empty()) { return ai_worker_erase_group(transaction, request); }
  phase = 7;
  ObSEArray<Publication, 4> output;
  if (OB_FAIL(output.prepare_allocate(eligible.count()))) { return ret; }
  GroupManifest group{COLUMN_DATABASE_SCOPE, table_id, incarnation_id, request_id,
      std::span<const uint64_t>(eligible.get_data(), eligible.count())};
  const auto publication = prepare_publication(group,
      std::span<const TaskRecord>(ready.get_data(), ready.count()),
      std::span<const ColumnVersion>(rows.get_data(), rows.count()),
      std::span<Publication>(output.get_data(), output.count()));
  if (publication == StateResult::INCOMPLETE_GROUP) { return OB_SUCCESS; }
  if (publication != StateResult::OK) { return OB_INVALID_DATA; }
  phase = 8;
  sql.reset();
  if (OB_FAIL(sql.append("UPDATE /*+ USE_PLAN_CACHE(NONE) */ "))
      || OB_FAIL(ai_worker_table_name(guard, *table, sql)) || OB_FAIL(sql.append(" SET "))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < output.count(); ++i) {
    const auto *column = table->get_column_schema(output.at(i).column_id);
    const auto *state_column = column == nullptr ? nullptr
        : table->get_column_schema(column->get_ai_column_schema().get_state_column_id());
    const auto &task = tasks.at(source_indices.at(i));
    const bool success = output.at(i).action == PublishAction::WRITE_READY;
    if (column == nullptr || state_column == nullptr) { ret = OB_INVALID_DATA; }
    else if (i > 0 && OB_FAIL(sql.append(","))) {
    } else if (OB_FAIL(ai_worker_identifier(sql, column->get_column_name_str())) || OB_FAIL(sql.append("="))) {
    } else if (!success || task.is_null) { ret = sql.append("NULL"); }
    else { ret = sql.append(task.value); }
    if (OB_SUCC(ret) && (OB_FAIL(sql.append(","))
        || OB_FAIL(ai_worker_identifier(sql, state_column->get_column_name_str()))
        || OB_FAIL(sql.append_fmt("=%d", success ? 1 : 2)))) {
    }
  }
  if (OB_SUCC(ret) && (OB_FAIL(sql.append(" WHERE ")) || OB_FAIL(sql.append(predicate)))) {
  } else if (OB_SUCC(ret)) {
    int64_t affected = 0;
    phase = 9;
    if (OB_FAIL(transaction.write(sql.ptr(), affected))) {
    } else if (affected != 1) { ret = OB_STATE_NOT_MATCH; }
  }
  if (OB_SUCC(ret)) { phase = 10; ret = ai_worker_erase_group(transaction, request); }
  return ret;
}
}

int AiColumnWorker::init(ObMySQLProxy &proxy)
{
  int ret = OB_SUCCESS;
  // Generate identities before starting threads; the UUID bootstrap path uses
  // shared state when SQL's UUID generator has not yet been initialized.
  for (auto &owner : owners_) {
    char uuid[36], compact[32];
    if (OB_FAIL(ObExprUuid::gen_server_uuid(uuid, sizeof(uuid)))) { return ret; }
    int64_t pos = 0;
    for (char c : uuid) { if (c != '-' && pos < 32) { compact[pos++] = c; } }
    if (pos != 32 || OB_FAIL(ai_worker_parse_id(ObString(32, compact), owner))) { return OB_INVALID_DATA; }
  }
  proxy_ = &proxy;
  return set_thread_count(2);
}

int AiColumnWorker::process_one(RequestId owner, bool &worked)
{
  int ret = OB_SUCCESS;
  worked = false;
  ObArenaAllocator allocator("AiColumnTask");
  DurableTask task;
  {
    ObTimeoutCtx timeout;
    ObMySQLTransaction transaction;
    if (OB_FAIL(ObShareUtil::set_default_timeout_ctx(timeout, COLUMN_SQL_TIMEOUT_US))) {
    } else if (OB_FAIL(transaction.start(proxy_))) {
    } else {
      ret = AiColumnTaskStore::claim_next(transaction, allocator, COLUMN_DATABASE_SCOPE, owner,
          ObTimeUtility::current_time(), COLUMN_LEASE_US, task, worked);
      const int end_ret = transaction.end(OB_SUCC(ret));
      if (OB_SUCC(ret)) { ret = end_ret; }
    }
  }
  if (OB_FAIL(ret) || !worked || task.record.state != TaskState::RUNNING) { return ret; }
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  const ObColumnSchemaV2 *column = nullptr;
  bool current = false;
  if (GCTX.schema_service_ == nullptr) { ret = OB_NOT_INIT; }
  else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(task.record.version.table_id, table))) {
  } else if (table != nullptr) {
    column = table->get_column_schema(task.record.version.column_id);
    if (column != nullptr && column->is_ai_column()
        && column->get_ai_column_schema().get_definition_version() == task.record.version.definition_version) {
      ObTimeoutCtx timeout;
      ObMySQLTransaction transaction;
      if (OB_FAIL(ObShareUtil::set_default_timeout_ctx(timeout, COLUMN_SQL_TIMEOUT_US))) {
      } else if (OB_FAIL(transaction.start(proxy_))) {
      } else {
        ret = ai_worker_current(transaction, guard, *table, *column, task, current);
        const int end_ret = transaction.end(OB_SUCC(ret));
        if (OB_SUCC(ret)) { ret = end_ret; }
      }
    }
  }
  if (OB_FAIL(ret)) { return ret; }
  ObString result;
  bool is_null = false;
  int execution_error = OB_SUCCESS;
  if (current) {
    const int64_t previous_timeout = THIS_WORKER.get_timeout_ts();
    THIS_WORKER.set_timeout_ts(ObTimeUtility::current_time() + 10 * 1000 * 1000);
    execution_error = ai_worker_infer(allocator, guard, *column, task.input_snapshot, result, is_null);
    THIS_WORKER.set_timeout_ts(previous_timeout);
  }
  {
    ObTimeoutCtx timeout;
    ObMySQLTransaction transaction;
    if (OB_FAIL(ObShareUtil::set_default_timeout_ctx(timeout, COLUMN_SQL_TIMEOUT_US))) {
    } else if (OB_FAIL(transaction.start(proxy_))) {
    } else {
      ret = current ? AiColumnTaskStore::stage(transaction, task, ObTimeUtility::current_time(),
          execution_error, result, is_null, 300000 * (1LL << std::min<uint32_t>(task.record.retry_count, 4)))
          : ai_worker_supersede(transaction, task);
      const int end_ret = transaction.end(OB_SUCC(ret));
      if (OB_SUCC(ret)) { ret = end_ret; }
    }
  }
  // T1 may legitimately supersede/remove a task while its HTTP call is running.
  if (ret == OB_STATE_NOT_MATCH) { ret = OB_SUCCESS; }
  return ret;
}

int AiColumnWorker::publish_one(bool &worked)
{
  int ret = OB_SUCCESS;
  worked = false;
  ObArenaAllocator allocator("AiColumnPublish");
  ObString request, incarnation, predicate, manifest;
  int64_t table_id = 0;
  {
    SMART_VAR(ObMySQLProxy::MySQLResult, response) {
      const char *sql = "SELECT g.request_id,g.table_id,g.row_incarnation,"
          "CAST(g.rowkey_predicate AS CHAR) AS rowkey_predicate,CAST(g.member_column_ids AS CHAR) AS members "
          "FROM oceanbase.__all_ai_column_group g WHERE NOT EXISTS ("
          "SELECT 1 FROM oceanbase.__all_ai_column_task t WHERE t.request_id=g.request_id AND t.state IN (0,1,3)) "
          "ORDER BY g.gmt_create,g.request_id LIMIT 1";
      if (OB_FAIL(proxy_->read(response, sql))) {
      } else if (response.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else if (OB_FAIL(response.get_result()->next())) {
        if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
      } else {
        ObString a, b, c, d;
        auto &r = *response.get_result();
        if (OB_FAIL(r.get_varchar("request_id", a)) || OB_FAIL(r.get_int("table_id", table_id))
            || OB_FAIL(r.get_varchar("row_incarnation", b)) || OB_FAIL(r.get_varchar("rowkey_predicate", c))
            || OB_FAIL(r.get_varchar("members", d)) || OB_FAIL(ob_write_string(allocator, a, request))
            || OB_FAIL(ob_write_string(allocator, b, incarnation)) || OB_FAIL(ob_write_string(allocator, c, predicate))
            || OB_FAIL(ob_write_string(allocator, d, manifest))) {
        } else { worked = true; }
      }
    }
  }
  if (OB_SUCC(ret) && worked) {
    ObTimeoutCtx timeout;
    ObMySQLTransaction transaction;
    ObSchemaGetterGuard guard;
    if (GCTX.schema_service_ == nullptr) { ret = OB_NOT_INIT; }
    else if (OB_FAIL(ObShareUtil::set_default_timeout_ctx(timeout, COLUMN_SQL_TIMEOUT_US))) {
    } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
    } else if (OB_FAIL(transaction.start(proxy_))) {
    } else {
      ret = ai_worker_publish(transaction, allocator, guard, table_id, request, incarnation, predicate, manifest);
      const int end_ret = transaction.end(OB_SUCC(ret));
      if (OB_SUCC(ret)) { ret = end_ret; }
    }
  }
  return ret;
}

int AiColumnWorker::backfill_one()
{
  int ret = OB_SUCCESS;
  uint64_t table_id = 0;
  // Cursor state is only an optimization. The durable missing/version markers
  // make a restart rescan safe and let multiple seeders race without lost work.
  for (int wrap = 0; OB_SUCC(ret) && table_id == 0 && wrap < 2; ++wrap) {
    ObSqlString sql;
    if (OB_FAIL(sql.append_fmt("SELECT table_id FROM oceanbase.__all_ai_column WHERE table_id>%lu "
            "ORDER BY table_id LIMIT 1", backfill_table_cursor_))) {
    } else {
      SMART_VAR(ObMySQLProxy::MySQLResult, response) {
        int64_t id = 0;
        if (OB_FAIL(proxy_->read(response, sql.ptr()))) {
        } else if (response.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
        else if (OB_FAIL(response.get_result()->next())) {
          if (ret == OB_ITER_END) { ret = OB_SUCCESS; backfill_table_cursor_ = 0; }
        } else if (OB_FAIL(response.get_result()->get_int(static_cast<int64_t>(0), id))) {
        } else if (id <= 0) { ret = OB_INVALID_DATA; }
        else { table_id = id; backfill_table_cursor_ = id; }
      }
    }
  }
  if (OB_FAIL(ret) || table_id == 0) { return ret; }
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  if (GCTX.schema_service_ == nullptr) { return OB_NOT_INIT; }
  if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))
      || OB_FAIL(guard.get_table_schema(table_id, table))) { return ret; }
  if (table == nullptr || !table->has_ai_column()) { return OB_SUCCESS; }
  const ObColumnSchemaV2 *ordinary = nullptr;
  for (auto it = table->column_begin(); it != table->column_end() && ordinary == nullptr; ++it) {
    if (!(*it)->is_hidden() && !(*it)->is_ai_column() && !(*it)->is_generated_column()) { ordinary = *it; }
  }
  if (ordinary == nullptr) { return OB_SUCCESS; }
  ObSqlString sql;
  if (OB_FAIL(sql.append("UPDATE /*+ USE_PLAN_CACHE(NONE) */ "))
      || OB_FAIL(ai_worker_table_name(guard, *table, sql)) || OB_FAIL(sql.append(" SET "))
      || OB_FAIL(ai_worker_identifier(sql, ordinary->get_column_name_str())) || OB_FAIL(sql.append("="))
      || OB_FAIL(ai_worker_identifier(sql, ordinary->get_column_name_str())) || OB_FAIL(sql.append(" WHERE "))) {
  }
  bool first = true;
  for (auto it = table->column_begin(); OB_SUCC(ret) && it != table->column_end(); ++it) {
    if (!(*it)->is_ai_column()) { continue; }
    const auto &definition = (*it)->get_ai_column_schema();
    const auto *request = table->get_column_schema(definition.get_request_column_id());
    const auto *version = table->get_column_schema(definition.get_result_spec_column_id());
    if (request == nullptr || version == nullptr) { ret = OB_INVALID_DATA; }
    else if (!first && OB_FAIL(sql.append(" OR "))) {
    } else if (OB_FAIL(sql.append("(")) || OB_FAIL(ai_worker_identifier(sql, request->get_column_name_str()))
        || OB_FAIL(sql.append(" IS NULL OR ")) || OB_FAIL(ai_worker_identifier(sql, version->get_column_name_str()))
        || OB_FAIL(sql.append_fmt("<>%lu)", definition.get_definition_version()))) {
    }
    first = false;
  }
  if (OB_SUCC(ret)) { ret = sql.append(" LIMIT 64"); }
  if (OB_FAIL(ret)) { return ret; }
  ObTimeoutCtx timeout;
  ObMySQLTransaction transaction;
  if (OB_FAIL(ObShareUtil::set_default_timeout_ctx(timeout, COLUMN_SQL_TIMEOUT_US))) {
  } else if (OB_FAIL(transaction.start(proxy_))) {
  } else {
    auto *session = query::ObInnerSQLConnectionAccess::get_session(transaction.get_connection());
    if (session == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else {
      const auto old_retry = session->ai_column_retry_id();
      session->set_ai_column_retry_id(BACKFILL_STALE_COLUMNS);
      int64_t affected = 0;
      ret = transaction.write(sql.ptr(), affected);
      session->set_ai_column_retry_id(old_retry);
    }
    const int end_ret = transaction.end(OB_SUCC(ret));
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  return ret;
}

void AiColumnWorker::run1()
{
  lib::set_thread_name("AiColumnWorker");
  const uint64_t index = get_thread_idx();
  if (index >= 2 || !owners_[index].valid()) { return; }
  const RequestId owner = owners_[index];
  int64_t next_backfill = 0;
  while (!has_set_stop()) {
    bool claimed = false, published = false;
    int ret = OB_SUCCESS;
    // Modules are constructed before recovery loads the full user schema. A
    // staged task must not be mistaken for a dropped table in that interval.
    if (share::check_server_runtime_ready() != OB_SUCCESS || !share::g_server_modules_ready
        || GCTX.start_service_time_ <= 0 || GCTX.status_ != SS_SERVING
        || !share::server_is_write_enabled()) {
    } else {
      const int64_t now = ObTimeUtility::current_time();
      if (index == 0 && now >= next_backfill) {
        next_backfill = now + 1000000;
        const int seed_ret = backfill_one();
        if (seed_ret != OB_SUCCESS) { LOG_WARN("AI COLUMN definition backfill failed", K(seed_ret)); }
      }
      if (OB_FAIL(process_one(owner, claimed))) {
        LOG_WARN("AI COLUMN worker task iteration failed", K(ret));
      } else if (OB_FAIL(publish_one(published))) {
        LOG_WARN("AI COLUMN worker publication iteration failed", K(ret));
      }
    }
    if (!claimed && !published) { usleep(100000); }
    else if (OB_FAIL(ret)) { usleep(100000); }
  }
}
}
