/* Copyright (c) 2026 OceanBase. Apache-2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/dml/ai_column_read.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "common/mysqlclient/ob_mysql_proxy.h"

namespace oceanbase::sql
{
using namespace common;
using namespace share;
using namespace share::schema;
namespace
{
int ai_read_identifier(ObSqlString &sql, const ObString &identifier)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < identifier.length(); ++i) {
    ret = identifier.ptr()[i] == '`' ? sql.append("``") : sql.append(identifier.ptr() + i, 1);
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); }
  return ret;
}
}

int AiColumnRead::check(ObExecContext &context, const ObPhysicalPlan &plan)
{
  int ret = OB_SUCCESS;
  auto *session = context.get_my_session();
  bool strict = false;
  if (session == nullptr) { return OB_NOT_INIT; }
  if (plan.ai_column_read_refs_.empty() || session->ai_column_internal_read()
      || session->ai_column_internal_write() || session->ai_column_retry_id() != OB_INVALID_ID) { return OB_SUCCESS; }
  if (OB_FAIL(session->get_sys_variable(SYS_VAR_AI_COLUMN_STRICT_READ, strict)) || !strict) { return ret; }
  if (plan.ai_column_read_refs_.count() % 2 != 0 || context.get_sql_ctx() == nullptr
      || context.get_sql_ctx()->schema_guard_ == nullptr) { return OB_ERR_UNEXPECTED; }
  auto &guard = *context.get_sql_ctx()->schema_guard_;
  sqlclient::ObISQLConnectionGuard connection_guard;
  bool created = false;
  if (session->get_inner_conn() == nullptr) {
    if (OB_FAIL(query::ObInnerSQLConnectionAccess::create_connection_with_external_session(session, connection_guard))) {
      return ret;
    }
    session->set_inner_conn(connection_guard.get_ptr());
    created = true;
  }
  auto *connection = as_inner_sql_connection(session->get_inner_conn());
  ObSQLSessionInfo::StmtSavedValue saved_session;
  ObIInnerSQLConnection::SavedValue saved_connection;
  bool nested = false;
  const bool old_read = session->ai_column_internal_read();
  session->set_ai_column_internal_read(true);
  if (connection == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(connection->begin_nested_session(saved_session, saved_connection, false))) {
  } else { nested = true; }
  for (int64_t i = 0; OB_SUCC(ret) && i < plan.ai_column_read_refs_.count(); i += 2) {
    const uint64_t tid = plan.ai_column_read_refs_.at(i);
    const uint64_t cid = plan.ai_column_read_refs_.at(i + 1);
    const ObTableSchema *table = nullptr;
    const ObDatabaseSchema *database = nullptr;
    const ObColumnSchemaV2 *column = nullptr;
    const ObColumnSchemaV2 *state = nullptr;
    const ObColumnSchemaV2 *version = nullptr;
    ObSqlString sql;
    if (OB_FAIL(guard.get_table_schema(tid, table))) {
    } else if (table == nullptr || (column = table->get_column_schema(cid)) == nullptr || !column->is_ai_column()
        || (state = table->get_column_schema(column->get_ai_column_schema().get_state_column_id())) == nullptr
        || (version = table->get_column_schema(column->get_ai_column_schema().get_result_spec_column_id())) == nullptr) {
      ret = OB_SCHEMA_EAGAIN;
    } else if (OB_FAIL(guard.get_database_schema(table->get_database_id(), database))) {
    } else if (database == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(sql.append("SELECT /*+ USE_PLAN_CACHE(NONE) */ 1 FROM "))
        || OB_FAIL(ai_read_identifier(sql, database->get_database_name_str())) || OB_FAIL(sql.append("."))
        || OB_FAIL(ai_read_identifier(sql, table->get_table_name_str())) || OB_FAIL(sql.append(" WHERE "))
        || OB_FAIL(ai_read_identifier(sql, state->get_column_name_str())) || OB_FAIL(sql.append(" IS NULL OR "))
        || OB_FAIL(ai_read_identifier(sql, state->get_column_name_str())) || OB_FAIL(sql.append("<>1 OR "))
        || OB_FAIL(ai_read_identifier(sql, version->get_column_name_str()))
        || OB_FAIL(sql.append_fmt("<>%lu LIMIT 1", column->get_ai_column_schema().get_definition_version()))) {
    } else {
      SMART_VAR(ObMySQLProxy::MySQLResult, response) {
        if (OB_FAIL(connection->execute_read(sql.ptr(), response))) {
        } else if (response.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
        else if (OB_FAIL(response.get_result()->next())) {
          if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
        } else {
          ret = OB_AI_COLUMN_NOT_READY;
          LOG_USER_ERROR(OB_AI_COLUMN_NOT_READY, tid, cid);
        }
      }
    }
  }
  if (nested) {
    const int end_ret = connection->end_nested_session(saved_session, saved_connection);
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  session->set_ai_column_internal_read(old_read);
  if (created) { session->set_inner_conn(nullptr); }
  return ret;
}
}
