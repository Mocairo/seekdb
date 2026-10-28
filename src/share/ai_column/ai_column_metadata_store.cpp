/* Copyright (c) 2026 OceanBase. Apache-2.0. */
#include "share/ai_column/ai_column_metadata_store.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/schema/ob_schema_utils.h"

namespace oceanbase::share::ai_column
{
using namespace common;
using namespace schema;

int AiColumnMetadataStore::upsert(ObISQLClient &transaction, const ObColumnSchemaV2 &column)
{
  int ret = OB_SUCCESS;
  if (!column.is_ai_column()) { return ret; }
  const auto &definition = column.get_ai_column_schema();
  ObDMLSqlSplicer dml;
  ObSqlString dependencies;
  if (!definition.is_valid()) { return OB_INVALID_ARGUMENT; }
  if (OB_FAIL(dependencies.append("["))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < definition.get_dependencies().count(); ++i) {
    ret = dependencies.append_fmt("%s%lu", i == 0 ? "" : ",", definition.get_dependencies().at(i));
  }
  if (OB_SUCC(ret)) { ret = dependencies.append("]"); }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(dml.add_pk_column("table_id", ObSchemaUtils::get_extract_schema_id(column.get_table_id())))
      || OB_FAIL(dml.add_pk_column("column_id", column.get_column_id()))
      || OB_FAIL(dml.add_column("column_name", ObHexEscapeSqlStr(column.get_column_name_str())))
      || OB_FAIL(dml.add_column("original_expr", ObHexEscapeSqlStr(definition.get_expression())))
      || OB_FAIL(dml.add_column("normalized_expr", ObHexEscapeSqlStr(definition.get_normalized_expression())))
      || OB_FAIL(dml.add_column("dependency_column_ids", dependencies.string()))
      || OB_FAIL(dml.add_column("sync_mode", "async"))
      || OB_FAIL(dml.add_column("max_retries", definition.get_max_retries()))
      || OB_FAIL(dml.add_column("definition_version", definition.get_definition_version()))
      || OB_FAIL(dml.add_column("state_column_id", definition.get_state_column_id()))
      || OB_FAIL(dml.add_column("request_column_id", definition.get_request_column_id()))
      || OB_FAIL(dml.add_column("result_spec_column_id", definition.get_result_spec_column_id()))
      || OB_FAIL(dml.add_column("row_incarnation_column_id", definition.get_incarnation_column_id()))) {
  } else {
    ObDMLExecHelper helper(transaction);
    int64_t affected = 0;
    ret = helper.exec_replace("__all_ai_column", dml, affected);
  }
  return ret;
}

int AiColumnMetadataStore::erase(ObISQLClient &transaction, uint64_t table_id, uint64_t column_id)
{
  int ret = OB_SUCCESS;
  ObDMLSqlSplicer dml;
  if (OB_FAIL(dml.add_pk_column("table_id", ObSchemaUtils::get_extract_schema_id(table_id)))
      || OB_FAIL(dml.add_pk_column("column_id", column_id))) {
  } else {
    ObDMLExecHelper helper(transaction);
    int64_t affected = 0;
    ret = helper.exec_delete("__all_ai_column", dml, affected);
  }
  return ret;
}

int AiColumnMetadataStore::upsert_table(ObISQLClient &transaction, const ObTableSchema &table)
{
  int ret = OB_SUCCESS;
  for (auto it = table.column_begin(); OB_SUCC(ret) && it != table.column_end(); ++it) {
    if ((*it)->is_ai_column()) {
      ObColumnSchemaV2 column;
      if (OB_FAIL(column.assign(**it))) {
      } else {
        column.set_table_id(table.get_table_id());
        ret = upsert(transaction, column);
      }
    }
  }
  return ret;
}

int AiColumnMetadataStore::erase_table(ObISQLClient &transaction, uint64_t table_id)
{
  int ret = OB_SUCCESS;
  for (const char *table : {"__all_ai_column_task", "__all_ai_column_group", "__all_ai_column"}) {
    if (OB_FAIL(ret)) { break; }
    ObDMLSqlSplicer dml;
    if (OB_FAIL(dml.add_pk_column("table_id", ObSchemaUtils::get_extract_schema_id(table_id)))) {
    } else {
      ObDMLExecHelper helper(transaction);
      int64_t affected = 0;
      ret = helper.exec_delete(table, dml, affected);
    }
  }
  return ret;
}
}
