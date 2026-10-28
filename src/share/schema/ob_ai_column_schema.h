/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "share/schema/ob_schema_struct.h"
#include "lib/container/ob_se_array.h"

namespace oceanbase::share::schema
{
// The definition is separate from the value/status of any individual row.
// Dependency IDs survive renames; definition_version fences in-flight tasks.
class AiColumnSchema : public ObSchema
{
  OB_UNIS_VERSION_V(1);
public:
  explicit AiColumnSchema(common::ObIAllocator *allocator = nullptr)
    : ObSchema(allocator) { reset(); }
  void reset() override;
  int assign(const AiColumnSchema &other);
  bool is_valid() const override;
  int64_t get_convert_size() const override;
  int set_expression(const common::ObString &original, const common::ObString &normalized);
  int set_dependencies(const common::ObIArray<uint64_t> &ids);
  int remap_column_ids(const common::ObIArray<uint64_t> &old_ids,
                      const common::ObIArray<uint64_t> &new_ids);
  int set_options(const common::ObString &sync_mode, int64_t max_retries);
  int to_storage(common::ObIAllocator &allocator, common::ObString &text) const;
  int from_storage(const common::ObString &text);
  const common::ObString &get_expression() const { return expression_; }
  const common::ObString &get_normalized_expression() const { return normalized_expression_; }
  const common::ObIArray<uint64_t> &get_dependencies() const { return dependencies_; }
  int64_t get_max_retries() const { return max_retries_; }
  uint64_t get_definition_version() const { return definition_version_; }
  void set_definition_version(uint64_t version) { definition_version_ = version; }
  uint64_t get_state_column_id() const { return state_column_id_; }
  uint64_t get_request_column_id() const { return request_column_id_; }
  uint64_t get_result_spec_column_id() const { return result_spec_column_id_; }
  uint64_t get_incarnation_column_id() const { return incarnation_column_id_; }
  void set_incarnation_column_id(uint64_t id) { incarnation_column_id_ = id; }
  void set_hidden_column_ids(uint64_t state, uint64_t request, uint64_t spec)
  { state_column_id_ = state; request_column_id_ = request; result_spec_column_id_ = spec; }
  bool enabled() const { return !expression_.empty(); }
  TO_STRING_KV(K_(expression), K_(normalized_expression), K_(dependencies),
               K_(max_retries), K_(definition_version), K_(state_column_id),
               K_(request_column_id), K_(result_spec_column_id), K_(incarnation_column_id));
private:
  common::ObString expression_;
  common::ObString normalized_expression_;
  common::ObSEArray<uint64_t, 4> dependencies_;
  int64_t max_retries_ = 2;
  uint64_t definition_version_ = 1;
  uint64_t state_column_id_ = common::OB_INVALID_ID;
  uint64_t request_column_id_ = common::OB_INVALID_ID;
  uint64_t result_spec_column_id_ = common::OB_INVALID_ID;
  uint64_t incarnation_column_id_ = common::OB_INVALID_ID;
};
} // namespace oceanbase::share::schema
