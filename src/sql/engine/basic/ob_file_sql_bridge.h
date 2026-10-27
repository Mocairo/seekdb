/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/resolver/ob_resolver_define.h"
namespace oceanbase {
namespace sql {
class ObBasicSessionInfo;
int bind_file_sql_expression(ObResolverParams &params, const ParseNode &call,
                             ObRawExpr *&expr);
bool is_file_sql_expression(const ObRawExpr *expr);
int check_file_sql_access(const ObBasicSessionInfo &session, std::string &root);
struct ObSelectIntoItem;
int bind_file_copy(ObResolverParams &params, const ParseNode &node,
                   ObSelectIntoItem &item);
bool contains_file_sql_node(const ParseNode *node);
}  // namespace sql
}  // namespace oceanbase
