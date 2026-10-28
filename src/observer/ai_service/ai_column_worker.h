/* Copyright (c) 2026 OceanBase. Apache-2.0. */
#pragma once

#include "share/ob_thread_pool.h"
#include "share/ai_column/ai_column_task_store.h"

namespace oceanbase::observer
{
class AiColumnWorker : public share::ObThreadPool
{
public:
  int init(common::ObMySQLProxy &proxy);
  void run1() override;
private:
  int process_one(share::ai_column::RequestId owner, bool &worked);
  int publish_one(bool &worked);
  int backfill_one();
  common::ObMySQLProxy *proxy_ = nullptr;
  share::ai_column::RequestId owners_[2];
  uint64_t backfill_table_cursor_ = 0;
};
}
