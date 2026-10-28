/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ddl/ob_ddl_resolver.h"
#include "sql/resolver/ddl/ob_alter_table_resolver.h"
#include "sql/parser/ob_parser.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "query/vector/ob_vector_index_util.h"
#include <algorithm>

namespace oceanbase::sql
{
using namespace common;
using namespace share::schema;

namespace
{
int add_ai_dependency(const ObTableSchema &table, const ObColumnSchemaV2 &column,
                      ObIArray<uint64_t> &ids, int depth = 0)
{
  if (depth > 256) { return OB_SIZE_OVERFLOW; }
  if (column.is_ai_column() || column.is_hidden()) { return OB_NOT_SUPPORTED; }
  for (int64_t i = 0; i < ids.count(); ++i) {
    if (ids.at(i) == column.get_column_id()) { return OB_SUCCESS; }
  }
  int ret = ids.push_back(column.get_column_id());
  if (OB_SUCC(ret) && column.is_generated_column()) {
    ObSEArray<uint64_t, 4> sources;
    if (OB_FAIL(column.get_cascaded_column_ids(sources))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < sources.count(); ++i) {
      const auto *source = table.get_column_schema(sources.at(i));
      if (source == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else { ret = add_ai_dependency(table, *source, ids, depth + 1); }
    }
  }
  return ret;
}

int gather_ai_dependencies(const ParseNode *node, const ObTableSchema &table,
                           ObIArray<uint64_t> &ids, int64_t &selects,
                           int64_t &ai_calls, int64_t depth = 0)
{
  int ret = OB_SUCCESS;
  if (node == nullptr) { return ret; }
  if (depth > 256) { return OB_SIZE_OVERFLOW; }
  if (node->num_child_ > 0 && node->children_ == nullptr) { return OB_ERR_UNEXPECTED; }
  if (node->type_ == T_SELECT && ++selects > 1) {
    ret = OB_NOT_SUPPORTED;
  } else if (node->type_ == T_COLUMN_REF) {
    if (node->num_child_ != 3 || node->children_[2] == nullptr
        || node->children_[0] != nullptr || node->children_[1] != nullptr) {
      ret = OB_NOT_SUPPORTED;
    } else {
      const ParseNode *name = node->children_[2];
      const auto *column = table.get_column_schema(ObString(name->str_len_, name->str_value_));
      if (column == nullptr) {
        ret = OB_ERR_BAD_FIELD_ERROR;
        const ObString scope("AI COLUMN expression");
        LOG_USER_ERROR(OB_ERR_BAD_FIELD_ERROR, static_cast<int32_t>(name->str_len_), name->str_value_,
            scope.length(), scope.ptr());
      }
      else { ret = add_ai_dependency(table, *column, ids); }
    }
  } else if (node->type_ == T_FUN_SYS && node->num_child_ > 0 && node->children_[0] != nullptr) {
    const ParseNode *name = node->children_[0];
    const ObString function(name->str_len_, name->str_value_);
    if (function.case_compare("AI_COMPLETE") == 0 || function.case_compare("AI_EMBED") == 0) {
      if (++ai_calls > 1) { ret = OB_NOT_SUPPORTED; }
    } else if (function.case_compare("AI_RERANK") == 0) {
      ret = OB_NOT_SUPPORTED;
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
    ret = gather_ai_dependencies(node->children_[i], table, ids, selects, ai_calls, depth + 1);
  }
  return ret;
}
}

int ObDDLResolver::resolve_ai_column_attribute(ObColumnSchemaV2 &column, const ParseNode &node)
{
  int ret = OB_SUCCESS;
  if (column.is_ai_column() || node.num_child_ != 2 || node.children_[0] == nullptr) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const ParseNode *expression = node.children_[0];
    const ParseNode *args = nullptr;
    ObString function;
    if (expression->type_ != T_FUN_SYS || expression->num_child_ != 2
        || expression->children_[0] == nullptr || expression->children_[1] == nullptr) {
      ret = OB_NOT_SUPPORTED;
    } else {
      const auto *name = expression->children_[0];
      function.assign_ptr(name->str_value_, name->str_len_);
      args = expression->children_[1];
      if ((function.case_compare("AI_COMPLETE") != 0 && function.case_compare("AI_EMBED") != 0)
          || args->num_child_ < 2 || args->num_child_ > 3) {
        ret = OB_NOT_SUPPORTED;
      }
    }
    ObString mode("async");
    int64_t retries = 2;
    bool has_mode = false, has_retries = false;
    const ParseNode *options = node.children_[1];
    for (int64_t i = 0; OB_SUCC(ret) && options != nullptr && i < options->num_child_; ++i) {
      const auto *option = options->children_[i];
      if (option == nullptr || option->num_child_ != 2
          || option->children_[0] == nullptr || option->children_[1] == nullptr) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        const auto *key = option->children_[0];
        const auto *value = option->children_[1];
        ObString name(key->str_len_, key->str_value_);
        if (name.case_compare("sync_mode") == 0 && !has_mode && value->type_ == T_VARCHAR) {
          has_mode = true;
          mode.assign_ptr(value->str_value_, value->str_len_);
        } else if (name.case_compare("max_retries") == 0 && !has_retries && value->type_ == T_INT) {
          has_retries = true;
          retries = value->value_;
        } else {
          ret = OB_INVALID_ARGUMENT;
        }
      }
    }
    auto &definition = column.get_ai_column_schema();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(definition.set_options(mode, retries))) {
    } else if (column.is_autoincrement() || !column.is_nullable() || column.is_generated_column()) {
      ret = OB_NOT_SUPPORTED;
    } else {
      const ParseNode *model = args->children_[0];
      ObAIFuncExprInfo *info = nullptr;
      if (model == nullptr || model->type_ != T_VARCHAR || model->str_len_ <= 0) {
        ret = OB_INVALID_ARGUMENT;
      } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator_,
              ObString(model->str_len_, model->str_value_), info))) {
      } else if (info == nullptr) {
        ret = OB_ERR_UNEXPECTED;
      } else if (function.case_compare("AI_EMBED") == 0) {
        int64_t dimension = 0;
        if (!column.is_collection()) { ret = OB_INVALID_ARGUMENT; }
        else if (OB_FAIL(ObVectorIndexUtil::get_vector_dim_from_extend_type_info(
                    column.get_extended_type_info(), dimension))) {
        } else if (!ObAIFuncUtils::is_dense_embedding_type(info)) { ret = OB_INVALID_ARGUMENT; }
        else if (args->num_child_ == 3 && args->children_[2] != nullptr) {
          const auto *dim = args->children_[2];
          if (dim->type_ != T_INT || dim->value_ <= 0) { ret = OB_INVALID_ARGUMENT; }
          else if (dim->value_ != dimension) {
            ret = OB_ERR_INVALID_VECTOR_DIM;
            LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, static_cast<uint32_t>(dimension),
                static_cast<uint32_t>(dim->value_));
          }
        }
      } else if (!ObAIFuncUtils::is_completion_type(info) || column.is_collection()) {
        ret = OB_INVALID_ARGUMENT;
      }
    }
    if (OB_SUCC(ret)) {
      ObString original(expression->str_len_, expression->str_value_);
      ObSqlString normalized;
      const char *contract = column.is_json() ? "Return exactly one valid JSON value."
          : ob_is_integer_type(column.get_data_type()) ? "Return exactly one base-10 integer." : nullptr;
      if (OB_FAIL(normalized.append(original))) {
      } else if (contract != nullptr && OB_FAIL(normalized.append_fmt(" /* AI COLUMN result contract: %s */", contract))) {
      } else {
        ret = definition.set_expression(original, normalized.string());
      }
    }
  }
  if (ret == OB_NOT_SUPPORTED) { LOG_USER_ERROR(OB_NOT_SUPPORTED, "this AI COLUMN definition is"); }
  return ret;
}

int ObDDLResolver::finalize_ai_columns(ObTableSchema &table)
{
  int ret = OB_SUCCESS;
  ObSEArray<uint64_t, 4> ai_ids;
  for (auto it = table.column_begin(); OB_SUCC(ret) && it != table.column_end(); ++it) {
    if ((*it)->is_ai_column()) { ret = ai_ids.push_back((*it)->get_column_id()); }
  }
  uint64_t incarnation_id = OB_INVALID_ID;
  if (OB_SUCC(ret) && !ai_ids.empty()) {
    const ObString name("__ai_row_incarnation");
    auto *existing = table.get_column_schema(name);
    if (existing != nullptr) {
      if (!existing->is_hidden()) { ret = OB_ERR_COLUMN_DUPLICATE; }
      else { incarnation_id = existing->get_column_id(); }
    } else {
      ObColumnSchemaV2 hidden(allocator_);
      incarnation_id = table.get_max_used_column_id() + 1;
      hidden.set_table_id(table.get_table_id());
      hidden.set_column_id(incarnation_id);
      hidden.set_is_hidden(true);
      hidden.set_data_type(ObVarcharType);
      hidden.set_data_length(32);
      hidden.set_charset_type(CHARSET_BINARY);
      hidden.set_collation_type(CS_TYPE_BINARY);
      hidden.set_nullable(true);
      if (OB_FAIL(hidden.set_column_name(name))) {
      } else { ret = table.add_column(hidden); }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < ai_ids.count(); ++i) {
    auto *column = table.get_column_schema(ai_ids.at(i));
    if (column == nullptr) { ret = OB_ERR_UNEXPECTED; break; }
    auto &definition = column->get_ai_column_schema();
    definition.set_incarnation_column_id(incarnation_id);
    ObSqlString select;
    ParseResult parsed{};
    ObParser parser(*allocator_, session_info_->get_sql_mode());
    ObSEArray<uint64_t, 4> dependencies;
    int64_t selects = 0, calls = 0;
    if (OB_FAIL(select.append("SELECT "))) {
    } else if (OB_FAIL(select.append(definition.get_expression()))) {
    } else if (OB_FAIL(parser.parse(select.string(), parsed))) {
    } else if (OB_FAIL(gather_ai_dependencies(parsed.result_tree_, table, dependencies, selects, calls))) {
      // Keep the standard OB_NOT_SUPPORTED user error. The mysqltest contract
      // compares this diagnostic and expects the server's generic text.
    } else if (calls != 1) { ret = OB_INVALID_ARGUMENT; }
    else if (OB_FAIL(definition.set_dependencies(dependencies))) {
    } else if (!column->is_nullable() || column->is_rowkey_column()
        || column->is_autoincrement() || !column->get_cur_default_value().is_null()) {
      ret = OB_NOT_SUPPORTED;
    } else if (definition.get_state_column_id() == OB_INVALID_ID) {
      uint64_t hidden_ids[3];
      const char *roles[] = {"state", "request", "spec"};
      for (int64_t role = 0; OB_SUCC(ret) && role < 3; ++role) {
        ObColumnSchemaV2 hidden(allocator_);
        ObSqlString name;
        hidden_ids[role] = table.get_max_used_column_id() + 1;
        if (OB_FAIL(name.append_fmt("__ai_%s_%lu", roles[role], ai_ids.at(i)))) {
        } else if (table.get_column_schema(name.string()) != nullptr) {
          ret = OB_ERR_COLUMN_DUPLICATE;
        } else if (OB_FAIL(hidden.set_column_name(name.string()))) {
        } else {
          hidden.set_table_id(table.get_table_id());
          hidden.set_column_id(hidden_ids[role]);
          hidden.set_is_hidden(true);
          hidden.set_data_type(role == 1 ? ObVarcharType : ObUInt64Type);
          hidden.set_data_length(role == 1 ? 32 : 8);
          hidden.set_charset_type(CHARSET_BINARY);
          hidden.set_collation_type(CS_TYPE_BINARY);
          hidden.set_nullable(role == 1);
          ObObj value;
          if (role == 1) { value.set_null(); }
          else { value.set_uint64(role == 0 ? 0 : definition.get_definition_version()); }
          if (OB_FAIL(hidden.set_cur_default_value(value, false))) {
          } else if (OB_FAIL(hidden.set_orig_default_value(value))) {
          } else if (OB_FAIL(table.add_column(hidden))) {
          }
        }
      }
      if (OB_SUCC(ret)) {
        column = table.get_column_schema(ai_ids.at(i));
        column->get_ai_column_schema().set_hidden_column_ids(hidden_ids[0], hidden_ids[1], hidden_ids[2]);
      }
    }
    parser.free_result(parsed);
  }
  return ret;
}
int ObAlterTableResolver::finalize_ai_alter_columns()
{
  int ret = OB_SUCCESS;
  auto *statement = get_alter_table_stmt();
  if (statement == nullptr || table_schema_ == nullptr) { return OB_ERR_UNEXPECTED; }
  auto &changes = statement->get_alter_table_arg().alter_table_schema_;
  if (!changes.has_ai_column() && !table_schema_->has_ai_column()) { return ret; }
  ObTableSchema merged(allocator_);
  if (OB_FAIL(merged.assign(*table_schema_))) { return ret; }
  uint64_t next_id = merged.get_max_used_column_id();
  const int64_t count = changes.get_column_count();
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    auto *change = static_cast<AlterColumnSchema *>(changes.get_column_schema_by_idx(i));
    if (change == nullptr) { ret = OB_ERR_UNEXPECTED; break; }
    ObColumnSchemaV2 candidate(allocator_);
    if (OB_FAIL(candidate.assign(*change))) { break; }
    if (change->alter_type_ == OB_DDL_ADD_COLUMN) {
      candidate.set_column_id(++next_id);
      if (OB_FAIL(merged.add_column(candidate))) { break; }
    } else if (change->alter_type_ == OB_DDL_MODIFY_COLUMN || change->alter_type_ == OB_DDL_CHANGE_COLUMN) {
      const ObString &name = change->origin_column_name_.empty()
          ? change->get_column_name_str() : change->origin_column_name_;
      const auto *original = table_schema_->get_column_schema(name);
      if (original == nullptr) { ret = OB_ERR_UNEXPECTED; break; }
      // Stored expressions still use SQL identifiers. Refuse dependency
      // renames until expression rewriting is supported instead of accepting
      // a definition which the next DML statement can no longer resolve.
      if (original->get_column_name_str() != candidate.get_column_name_str()) {
        for (auto it = table_schema_->column_begin(); OB_SUCC(ret) && it != table_schema_->column_end(); ++it) {
          const auto &dependencies = (*it)->get_ai_column_schema().get_dependencies();
          for (int64_t j = 0; OB_SUCC(ret) && j < dependencies.count(); ++j) {
            if (dependencies.at(j) == original->get_column_id()) {
              ret = OB_NOT_SUPPORTED;
              LOG_USER_ERROR(OB_NOT_SUPPORTED, "renaming a dependency of an AI COLUMN is");
            }
          }
        }
        if (OB_FAIL(ret)) { break; }
      }
      if (candidate.is_ai_column() || original->is_ai_column()) {
        if (original->is_rowkey_column()) { ret = OB_NOT_SUPPORTED; break; }
        candidate.set_column_id(original->get_column_id());
        auto &definition = candidate.get_ai_column_schema();
        if (original->is_ai_column()) {
          const auto &old_definition = original->get_ai_column_schema();
          if (!definition.enabled() && OB_FAIL(definition.assign(old_definition))) { break; }
          if (old_definition.get_definition_version() == UINT64_MAX) { ret = OB_SIZE_OVERFLOW; break; }
          definition.set_definition_version(old_definition.get_definition_version() + 1);
          definition.set_hidden_column_ids(old_definition.get_state_column_id(),
              old_definition.get_request_column_id(), old_definition.get_result_spec_column_id());
        }
        if (OB_FAIL(merged.delete_column(name))) {
        } else if (OB_FAIL(merged.add_column(candidate))) {
        }
      }
    } else if (change->alter_type_ == OB_DDL_DROP_COLUMN) {
      ret = merged.delete_column(change->get_column_name_str());
    }
  }
  if (OB_SUCC(ret)) { ret = finalize_ai_columns(merged); }
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    auto *change = static_cast<AlterColumnSchema *>(changes.get_column_schema_by_idx(i));
    if (change->alter_type_ == OB_DDL_ADD_COLUMN || change->alter_type_ == OB_DDL_MODIFY_COLUMN
        || change->alter_type_ == OB_DDL_CHANGE_COLUMN) {
      const auto *column = merged.get_column_schema(change->get_column_name_str());
      if (column != nullptr && column->is_ai_column()) {
        ret = change->get_ai_column_schema().assign(column->get_ai_column_schema());
      }
    }
  }
  // Rootserver assigns ADD column IDs in this same order. All user additions
  // precede their hidden additions, so persisted definition IDs agree with it.
  for (uint64_t id = next_id + 1; OB_SUCC(ret) && id <= merged.get_max_used_column_id(); ++id) {
    const auto *column = merged.get_column_schema(id);
    AlterColumnSchema hidden;
    if (column == nullptr || !column->is_hidden()) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(hidden.ObColumnSchemaV2::assign(*column))) {
    } else {
      hidden.alter_type_ = OB_DDL_ADD_COLUMN;
      ret = statement->add_column(hidden);
    }
  }
  return ret;
}
namespace
{
int retry_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    ret = name.ptr()[i] == '`' ? sql.append("``") : sql.append(name.ptr() + i, 1);
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); }
  return ret;
}

int retry_status_references(const ParseNode *node, ObIArray<const ParseNode *> &references, int depth = 0)
{
  if (node == nullptr) { return OB_SUCCESS; }
  if (depth > 256) { return OB_SIZE_OVERFLOW; }
  // A nested query has its own column namespace. Its STATUS remains ordinary SQL.
  if (node->type_ == T_SELECT) { return OB_SUCCESS; }
  int ret = OB_SUCCESS;
  if (node->type_ == T_COLUMN_REF && node->num_child_ == 3 && node->children_[0] == nullptr
      && node->children_[1] == nullptr && node->children_[2] != nullptr) {
    const auto &name = *node->children_[2];
    if (!name.is_input_quoted_ && ObString(name.str_len_, name.str_value_).case_compare("status") == 0) {
      ret = references.push_back(node);
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
    ret = retry_status_references(node->children_[i], references, depth + 1);
  }
  return ret;
}
}

int ObAlterTableResolver::resolve_ai_retry(const ParseNode &node)
{
  int ret = OB_SUCCESS;
  auto *statement = get_alter_table_stmt();
  if (table_schema_ == nullptr || statement == nullptr || node.num_child_ != 2 || node.children_[0] == nullptr) {
    return OB_ERR_UNEXPECTED;
  }
  const auto *name = node.children_[0];
  const auto *column = table_schema_->get_column_schema(ObString(name->str_len_, name->str_value_));
  if (column == nullptr || !column->is_ai_column()) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying a column that is not an AI column");
    return OB_NOT_SUPPORTED;
  }
  const auto *state = table_schema_->get_column_schema(column->get_ai_column_schema().get_state_column_id());
  const ObColumnSchemaV2 *ordinary = nullptr;
  for (auto it = table_schema_->column_begin(); it != table_schema_->column_end() && ordinary == nullptr; ++it) {
    if (!(*it)->is_hidden() && !(*it)->is_ai_column() && !(*it)->is_generated_column()) { ordinary = *it; }
  }
  if (state == nullptr) { return OB_ERR_UNEXPECTED; }
  if (ordinary == nullptr) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "RETRY on a table without ordinary writable columns is");
    return OB_NOT_SUPPORTED;
  }
  ObSqlString sql;
  if (OB_FAIL(sql.append("UPDATE /*+ USE_PLAN_CACHE(NONE) */ "))
      || OB_FAIL(retry_identifier(sql, statement->get_org_database_name())) || OB_FAIL(sql.append("."))
      || OB_FAIL(retry_identifier(sql, table_schema_->get_table_name_str())) || OB_FAIL(sql.append(" SET "))
      || OB_FAIL(retry_identifier(sql, ordinary->get_column_name_str())) || OB_FAIL(sql.append("="))
      || OB_FAIL(retry_identifier(sql, ordinary->get_column_name_str()))) {
  }
  const auto *where = node.children_[1];
  if (OB_SUCC(ret) && where != nullptr) {
    ObSEArray<const ParseNode *, 4> references;
    if (where->num_child_ != 1 || where->str_value_ == nullptr || where->str_len_ <= 0) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(retry_status_references(where->children_[0], references))) {
    } else if (OB_FAIL(sql.append(" WHERE "))) {
    } else {
      std::sort(references.get_data(), references.get_data() + references.count(),
          [](const ParseNode *a, const ParseNode *b) { return a->stmt_loc_.first_column_ < b->stmt_loc_.first_column_; });
      int64_t copied = 0;
      for (int64_t i = 0; OB_SUCC(ret) && i < references.count(); ++i) {
        const auto &location = references.at(i)->stmt_loc_;
        const int64_t offset = location.first_column_ - 1 - where->value_;
        const int64_t length = location.last_column_ - location.first_column_ + 1;
        if (offset < copied || length <= 0 || offset + length > where->str_len_) { ret = OB_INVALID_DATA; }
        else if (OB_FAIL(sql.append(where->str_value_ + copied, offset - copied))
            || OB_FAIL(sql.append("(CASE ")) || OB_FAIL(retry_identifier(sql, state->get_column_name_str()))
            || OB_FAIL(sql.append(" WHEN 0 THEN 'PENDING' WHEN 1 THEN 'READY' WHEN 2 THEN 'FAILED' END)"))) {
        } else { copied = offset + length; }
      }
      if (OB_SUCC(ret)) { ret = sql.append(where->str_value_ + copied, where->str_len_ - copied); }
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(ob_write_string(*allocator_, sql.string(), statement->ai_retry_sql_))) {
  } else if (OB_SUCC(ret)) { statement->ai_retry_column_id_ = column->get_column_id(); }
  return ret;
}
} // namespace oceanbase::sql
