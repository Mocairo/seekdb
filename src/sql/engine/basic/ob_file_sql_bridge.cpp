/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/engine/basic/ob_file_sql_bridge.h"

#include <cstring>
#include <set>

#include "sql/plan_cache/ob_plan_cache_util.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/expr/ob_raw_expr.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/session/ob_sql_session_info.h"
namespace oceanbase {
namespace sql {
using namespace common;
bool contains_file_sql_node(const ParseNode *node) {
  if (node == nullptr) return false;
  std::vector<const ParseNode *> pending{node};
  while (!pending.empty()) {
    const ParseNode *current = pending.back();
    pending.pop_back();
    if (current->value_ == 1701 &&
        (current->type_ == T_TABLE_COLLECTION_EXPRESSION ||
         current->type_ == T_INTO_OUTFILE || current->type_ == T_SHOW_COLUMNS))
      return true;
    for (int i = 0; i < current->num_child_; ++i)
      if (current->children_[i] != nullptr)
        pending.push_back(current->children_[i]);
  }
  return false;
}

int bind_file_copy(ObResolverParams &params, const ParseNode &node,
                   ObSelectIntoItem &item) {
  int ret = OB_SUCCESS;
  std::string root;
  if (params.session_info_ == nullptr || params.allocator_ == nullptr)
    return OB_ERR_UNEXPECTED;
  if (OB_FAIL(check_file_sql_access(*params.session_info_, root))) return ret;
  try {
    auto text = [](const ParseNode *n) {
      if (n == nullptr || n->str_value_ == nullptr)
        throw file_sql::Error("missing COPY argument");
      return std::string(n->str_value_, n->str_len_);
    };
    auto lower = [](std::string s) {
      for (char &c : s)
        if (c >= 'A' && c <= 'Z') c += 32;
      return s;
    };
    if (node.num_child_ != 2 || node.children_[0] == nullptr ||
        node.children_[0]->type_ != T_VARCHAR)
      throw file_sql::Error("COPY requires a constant output path");
    file_sql::Spec spec;
    spec.path = text(node.children_[0]);
    spec.secure_root = root;
    spec.options.header = false;
    const ParseNode *options = node.children_[1];
    std::set<std::string> seen;
    for (int i = 0; options != nullptr && i < options->num_child_; ++i) {
      const ParseNode *option = options->children_[i];
      if (option == nullptr || option->num_child_ != 2)
        throw file_sql::Error("invalid COPY option");
      const ParseNode *value = option->children_[1];
      const std::string key = lower(text(option->children_[0]));
      if (!seen.insert(key).second)
        throw file_sql::Error("duplicate COPY option: " + key);
      auto character = [&]() {
        auto s = text(value);
        if (value->type_ != T_VARCHAR || s.size() != 1)
          throw file_sql::Error(key + " requires one character");
        return s[0];
      };
      if (key == "format") {
        if (lower(text(value)) != "csv")
          throw file_sql::Error("COPY supports CSV only");
      } else if (key == "header") {
        if (value == nullptr || value->type_ != T_BOOL)
          throw file_sql::Error("COPY header requires a boolean");
        spec.options.header = value->value_ != 0;
      } else if (key == "delim" || key == "delimiter")
        spec.options.delimiter = character();
      else if (key == "quote")
        spec.options.quote = character();
      else if (key == "escape")
        spec.options.escape = character();
      else if (key == "nullstr" || key == "null") {
        if (value->type_ != T_VARCHAR)
          throw file_sql::Error("COPY nullstr requires a string");
        spec.options.nullstr = text(value);
      } else
        throw file_sql::Error("unknown COPY option: " + key);
    }
    const std::string encoded = spec.encode();
    file_sql::Spec::decode(encoded);
    OZ(ob_write_string(*params.allocator_,
                       ObString(int32_t(encoded.size()), encoded.data()),
                       item.file_copy_spec_));
    item.outfile_name_.set_varchar(
        ObString(node.children_[0]->str_len_, node.children_[0]->str_value_));
    item.outfile_name_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  } catch (const std::exception &e) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, e.what());
  }
  return ret;
}
int check_file_sql_access(const ObBasicSessionInfo &session,
                          std::string &root) {
  int ret = OB_SUCCESS;
  bool enabled = false;
  ObString secure;
  if (OB_FAIL(
          session.get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
  } else if (!enabled) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED,
                   "file SQL is disabled; set enable_file_sql=ON");
  } else if (OB_FAIL(session.get_secure_file_priv(secure))) {
  } else {
    root.clear();
    if (secure.length() > 0) root.assign(secure.ptr(), secure.length());
    if (root.empty() || secure.case_compare("NULL") == 0) {
      ret = OB_ERR_NO_PRIVILEGE;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "secure_file_priv forbids file SQL");
    }
  }
  return ret;
}
bool is_file_sql_expression(const ObRawExpr *e) {
  if (e == nullptr || !e->is_const_raw_expr()) return false;
  const ObObj &v = static_cast<const ObConstRawExpr *>(e)->get_value();
  return v.is_varchar() && v.get_string().prefix_match(ObString("FSQ1 "));
}
int bind_file_sql_expression(ObResolverParams &params, const ParseNode &call,
                             ObRawExpr *&expr) {
  int ret = OB_SUCCESS;
  std::string root;
  if (params.session_info_ == nullptr || params.allocator_ == nullptr ||
      params.expr_factory_ == nullptr)
    return OB_ERR_UNEXPECTED;
  if (OB_FAIL(check_file_sql_access(*params.session_info_, root))) return ret;
  try {
    if (call.num_child_ != 2 || !call.children_[0] || !call.children_[1])
      throw file_sql::Error("file function requires a file path");
    auto text = [](const ParseNode *n) {
      if (!n) throw file_sql::Error("missing file argument");
      return std::string(n->str_value_ ? n->str_value_ : "",
                         size_t(n->str_len_));
    };
    auto lower = [](std::string s) {
      for (char &c : s)
        if (c >= 'A' && c <= 'Z') c += 32;
      return s;
    };
    std::string name = lower(text(call.children_[0]));
    file_sql::Options options;
    if (name == "read_csv")
      options.format = "csv";
    else if (name == "read_json" || name == "read_ndjson")
      options.format = "json";
    else if (name == "read_parquet")
      options.format = "parquet";
    else if (name == "glob")
      options.format = "glob";
    else
      throw file_sql::Error("unknown file table function: " + name);
    const ParseNode *args = call.children_[1];
    if (args->num_child_ < 1) throw file_sql::Error("missing file path");
    const ParseNode *path = args->children_[0];
    if (!path || path->type_ != T_VARCHAR)
      throw file_sql::Error("file path must be a constant string");
    std::set<std::string> seen;
    for (int i = 1; i < args->num_child_; ++i) {
      const ParseNode *arg = args->children_[i];
      if (!arg || arg->type_ != T_OP_EQ || arg->num_child_ != 2) {
        ret = OB_ERR_PARSE_SQL;
        throw file_sql::Error("file options must be named arguments");
      }
      const ParseNode *key = arg->children_[0], *value = arg->children_[1];
      if (key && key->type_ == T_COLUMN_REF && key->num_child_ == 3)
        key = key->children_[2];
      std::string k = lower(text(key)), v = lower(text(value));
      if (!seen.insert(k).second)
        throw file_sql::Error("duplicate file option: " + k);
      auto string_value = [&]() {
        if (value->type_ != T_VARCHAR)
          throw file_sql::Error(k + " requires a string");
        return text(value);
      };
      auto character = [&]() {
        auto s = string_value();
        if (s.size() != 1) throw file_sql::Error(k + " requires one byte");
        return s[0];
      };
      if (k == "sample_size") {
        if ((value->type_ == T_OP_NEG && value->num_child_ == 1 &&
             value->children_[0]->value_ == 1) ||
            (value->type_ == T_INT && value->value_ == -1))
          options.sample_size = -1;
        else {
          ret = OB_NOT_SUPPORTED;
          throw file_sql::Error("sample_size supports only -1");
        }
      } else if (k == "header") {
        if (value->type_ != T_BOOL)
          throw file_sql::Error("header requires boolean");
        options.header = value->value_ != 0;
      } else if (k == "delim" || k == "delimiter")
        options.delimiter = character();
      else if (k == "quote")
        options.quote = character();
      else if (k == "escape")
        options.escape = character();
      else if (k == "nullstr" || k == "null")
        options.nullstr = string_value();
      else if (k == "format" &&
               (name == "read_json" || name == "read_ndjson")) {
        if (lower(string_value()) != "newline_delimited")
          throw file_sql::Error("JSON supports only newline_delimited format");
      } else
        throw file_sql::Error("unsupported file option: " + k);
    }
    file_sql::Spec spec = file_sql::bind(text(path), root, options);
    std::string encoded = spec.encode();
    ObString copied;
    ObConstRawExpr *constant = nullptr;
    if (OB_FAIL(ob_write_string(
            *params.allocator_,
            ObString(int32_t(encoded.size()), encoded.data()), copied))) {
    } else if (OB_FAIL(params.expr_factory_->create_raw_expr(T_VARCHAR,
                                                             constant))) {
    } else {
      ObObj value;
      value.set_varchar(copied);
      value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      constant->set_value(value);
      constant->set_data_type(ObVarcharType);
      constant->set_collation_type(CS_TYPE_UTF8MB4_BIN);
      constant->set_collation_level(CS_LEVEL_COERCIBLE);
      if (OB_SUCC(ret = constant->formalize(params.session_info_)))
        expr = constant;
    }
  } catch (const std::exception &e) {
    if (OB_SUCC(ret)) ret = OB_INVALID_ARGUMENT;
    if (ret == OB_NOT_SUPPORTED)
      LOG_USER_ERROR(OB_NOT_SUPPORTED, e.what());
    else if (ret == OB_ERR_PARSE_SQL)
      LOG_USER_ERROR(OB_ERR_PARSE_SQL, ob_errpkt_strerror(OB_ERR_PARSER_SYNTAX),
                     int32_t(strlen(e.what())), e.what(), 1);
    else
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, e.what());
  }
  return ret;
}
}  // namespace sql
}  // namespace oceanbase
