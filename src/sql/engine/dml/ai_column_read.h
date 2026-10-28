/* Copyright (c) 2026 OceanBase. Apache-2.0. */
#pragma once

namespace oceanbase::sql
{
class ObExecContext;
class ObPhysicalPlan;
class AiColumnRead
{
public:
  static int check(ObExecContext &context, const ObPhysicalPlan &plan);
};
}
