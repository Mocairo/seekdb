/* Copyright (c) 2026 OceanBase. Apache-2.0. */
#pragma once

namespace oceanbase::sql
{
struct ObDMLModifyRowNode;
// Runs after successful DAS writes, using the business session's transaction.
// It captures arguments and writes an outbox; it never invokes a model.
class AiColumnDml
{
public:
  static int after_row(ObDMLModifyRowNode &row);
};
}
