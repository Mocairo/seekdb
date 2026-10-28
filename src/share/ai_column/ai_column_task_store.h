/* Copyright (c) 2026 OceanBase. Apache-2.0. */
#pragma once

#include "share/ai_column/ai_column_state.h"
#include "common/mysqlclient/ob_mysql_transaction.h"

namespace oceanbase::share::ai_column
{
struct DurableTask
{
  TaskRecord record;
  common::ObString task_id;
  common::ObString rowkey_predicate;
  common::ObString input_snapshot;
};

// The caller supplies a started transaction and commits the returned transition.
// No method sends HTTP or creates a second transaction/connection.
class AiColumnTaskStore
{
public:
  static int claim_next(common::ObMySQLTransaction &transaction, common::ObIAllocator &allocator,
      uint64_t database_scope, RequestId owner, int64_t now, int64_t lease_duration,
      DurableTask &task, bool &found);
  static int stage(common::ObMySQLTransaction &transaction, const DurableTask &task,
      int64_t now, int error, const common::ObString &result, bool result_is_null,
      int64_t retry_delay);
};
}
