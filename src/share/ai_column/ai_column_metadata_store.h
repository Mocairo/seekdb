/* Copyright (c) 2026 OceanBase. Apache-2.0. */
#pragma once
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_table_schema.h"
#include "common/mysqlclient/ob_isql_client.h"

namespace oceanbase::share::ai_column
{
// All methods use the caller's transaction. They never acquire their own
// connection or commit independently of the surrounding Schema operation.
class AiColumnMetadataStore
{
public:
  static int upsert(common::ObISQLClient &transaction,
                    const schema::ObColumnSchemaV2 &column);
  static int erase(common::ObISQLClient &transaction, uint64_t table_id, uint64_t column_id);
  static int upsert_table(common::ObISQLClient &transaction, const schema::ObTableSchema &table);
  static int erase_table(common::ObISQLClient &transaction, uint64_t table_id);
};
}
