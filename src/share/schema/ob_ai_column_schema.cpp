/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "share/schema/ob_ai_column_schema.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/utility/ob_print_utils.h"
#include <limits>

namespace oceanbase::share::schema
{
using namespace common;

void AiColumnSchema::reset()
{
  expression_.reset();
  normalized_expression_.reset();
  dependencies_.reset();
  max_retries_ = 2;
  definition_version_ = 1;
  state_column_id_ = request_column_id_ = result_spec_column_id_ = OB_INVALID_ID;
  incarnation_column_id_ = OB_INVALID_ID;
  ObSchema::reset();
}

int AiColumnSchema::set_options(const ObString &sync_mode, int64_t retries)
{
  int ret = OB_SUCCESS;
  if (sync_mode.case_compare("async") != 0 || retries < 0
      || retries > std::numeric_limits<int32_t>::max()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    max_retries_ = retries;
  }
  return ret;
}

int AiColumnSchema::set_expression(const ObString &original, const ObString &normalized)
{
  int ret = OB_SUCCESS;
  ObString original_copy, normalized_copy;
  if (original.empty() || normalized.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(deep_copy_str(original, original_copy))) {
  } else if (OB_FAIL(deep_copy_str(normalized, normalized_copy))) {
  } else {
    expression_ = original_copy;
    normalized_expression_ = normalized_copy;
  }
  return ret;
}

int AiColumnSchema::set_dependencies(const ObIArray<uint64_t> &ids)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < ids.count(); ++i) {
    if (ids.at(i) == OB_INVALID_ID || ids.at(i) == 0) {
      ret = OB_INVALID_ARGUMENT;
    }
    for (int64_t j = 0; OB_SUCC(ret) && j < i; ++j) {
      if (ids.at(i) == ids.at(j)) { ret = OB_INVALID_ARGUMENT; }
    }
  }
  if (OB_SUCC(ret)) { ret = dependencies_.assign(ids); }
  return ret;
}

int AiColumnSchema::assign(const AiColumnSchema &other)
{
  int ret = OB_SUCCESS;
  if (this != &other) {
    if (other.enabled() && OB_FAIL(set_expression(other.expression_, other.normalized_expression_))) {
    } else if (OB_FAIL(dependencies_.assign(other.dependencies_))) {
    } else {
      if (!other.enabled()) { expression_.reset(); normalized_expression_.reset(); }
      max_retries_ = other.max_retries_;
      definition_version_ = other.definition_version_;
      set_hidden_column_ids(other.state_column_id_, other.request_column_id_, other.result_spec_column_id_);
      incarnation_column_id_ = other.incarnation_column_id_;
    }
  }
  return ret;
}

bool AiColumnSchema::is_valid() const
{
  bool valid = enabled() && !normalized_expression_.empty() && definition_version_ > 0
      && max_retries_ >= 0 && max_retries_ <= std::numeric_limits<int32_t>::max()
      && state_column_id_ != OB_INVALID_ID && state_column_id_ > 0
      && request_column_id_ != OB_INVALID_ID && request_column_id_ > 0
      && result_spec_column_id_ != OB_INVALID_ID && result_spec_column_id_ > 0
      && incarnation_column_id_ != OB_INVALID_ID && incarnation_column_id_ > 0
      && incarnation_column_id_ != state_column_id_ && incarnation_column_id_ != request_column_id_
      && incarnation_column_id_ != result_spec_column_id_
      && state_column_id_ != request_column_id_
      && state_column_id_ != result_spec_column_id_
      && request_column_id_ != result_spec_column_id_;
  for (int64_t i = 0; valid && i < dependencies_.count(); ++i) {
    const uint64_t id = dependencies_.at(i);
    valid = id != 0 && id != OB_INVALID_ID && id != state_column_id_
        && id != request_column_id_ && id != result_spec_column_id_ && id != incarnation_column_id_;
    for (int64_t j = 0; valid && j < i; ++j) { valid = id != dependencies_.at(j); }
  }
  return valid;
}

int64_t AiColumnSchema::get_convert_size() const
{
  return sizeof(*this) + expression_.length() + normalized_expression_.length() + 2
      + dependencies_.count() * sizeof(uint64_t);
}

int AiColumnSchema::remap_column_ids(const ObIArray<uint64_t> &old_ids,
                                    const ObIArray<uint64_t> &new_ids)
{
  int ret = OB_SUCCESS;
  if (old_ids.count() != new_ids.count()) {
    ret = OB_INVALID_ARGUMENT;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < old_ids.count(); ++i) {
    if (old_ids.at(i) == 0 || old_ids.at(i) == OB_INVALID_ID
        || new_ids.at(i) == 0 || new_ids.at(i) == OB_INVALID_ID) {
      ret = OB_INVALID_ARGUMENT;
    }
    for (int64_t j = 0; OB_SUCC(ret) && j < i; ++j) {
      if (old_ids.at(i) == old_ids.at(j) || new_ids.at(i) == new_ids.at(j)) {
        ret = OB_INVALID_ARGUMENT;
      }
    }
  }
  ObSEArray<uint64_t, 8> mapped;
  // Validate every mapping before changing this definition.
  for (int64_t i = 0; OB_SUCC(ret) && i < dependencies_.count() + 4; ++i) {
    uint64_t id = i < dependencies_.count() ? dependencies_.at(i)
        : i == dependencies_.count() ? state_column_id_
        : i == dependencies_.count() + 1 ? request_column_id_
        : i == dependencies_.count() + 2 ? result_spec_column_id_ : incarnation_column_id_;
    bool found = id == OB_INVALID_ID;
    for (int64_t j = 0; !found && j < old_ids.count(); ++j) {
      if (old_ids.at(j) == id) { id = new_ids.at(j); found = true; }
    }
    if (!found) { ret = OB_ENTRY_NOT_EXIST; }
    else { ret = mapped.push_back(id); }
  }
  if (OB_SUCC(ret)) {
    for (int64_t i = 0; i < dependencies_.count(); ++i) { dependencies_.at(i) = mapped.at(i); }
    const int64_t n = dependencies_.count();
    set_hidden_column_ids(mapped.at(n), mapped.at(n + 1), mapped.at(n + 2));
    incarnation_column_id_ = mapped.at(n + 3);
  }
  return ret;
}

int AiColumnSchema::to_storage(ObIAllocator &allocator, ObString &text) const
{
  int ret = OB_SUCCESS;
  const int64_t size = get_serialize_size();
  char *binary = nullptr;
  char *hex = nullptr;
  int64_t pos = 0, hex_pos = 0;
  // A bounded ASCII representation is safe in __all_column's text field,
  // independently of the result column's character set or binary SQL data.
  if (size <= 0 || size > std::numeric_limits<int32_t>::max() / 2) {
    ret = OB_SIZE_OVERFLOW;
  } else if (OB_ISNULL(binary = static_cast<char *>(allocator.alloc(size)))
      || OB_ISNULL(hex = static_cast<char *>(allocator.alloc(size * 2)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(serialize(binary, size, pos))) {
  } else if (OB_FAIL(hex_print(binary, pos, hex, size * 2, hex_pos))) {
  } else {
    text.assign_ptr(hex, static_cast<int32_t>(hex_pos));
  }
  return ret;
}

int AiColumnSchema::from_storage(const ObString &text)
{
  int ret = OB_SUCCESS;
  if (text.empty()) {
    reset();
  } else if ((text.length() & 1) != 0) {
    ret = OB_INVALID_DATA;
  } else {
    ObArenaAllocator temporary;
    const int32_t size = text.length() / 2;
    char *binary = static_cast<char *>(temporary.alloc(size));
    if (OB_ISNULL(binary)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      unsigned char byte = 0;
      for (int32_t i = 0; OB_SUCC(ret) && i < text.length(); ++i) {
        const char c = text.ptr()[i];
        const int nibble = c >= '0' && c <= '9' ? c - '0'
            : c >= 'a' && c <= 'f' ? c - 'a' + 10
            : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (nibble < 0) {
          ret = OB_INVALID_DATA;
        } else {
          byte = static_cast<unsigned char>((byte << 4) | nibble);
          if ((i & 1) != 0) { binary[i / 2] = static_cast<char>(byte); byte = 0; }
        }
      }
      AiColumnSchema decoded(&temporary);
      int64_t pos = 0;
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(decoded.deserialize(binary, size, pos))) {
      } else if (pos != size || !decoded.is_valid()) {
        ret = OB_INVALID_DATA;
      } else {
        ret = assign(decoded);
      }
    }
  }
  return ret;
}

OB_DEF_SERIALIZE(AiColumnSchema)
{
  int ret = OB_SUCCESS;
  LST_DO_CODE(OB_UNIS_ENCODE, expression_, normalized_expression_, dependencies_,
      max_retries_, definition_version_, state_column_id_, request_column_id_, result_spec_column_id_, incarnation_column_id_);
  return ret;
}

OB_DEF_DESERIALIZE(AiColumnSchema)
{
  int ret = OB_SUCCESS;
  ObString expression, normalized;
  LST_DO_CODE(OB_UNIS_DECODE, expression, normalized, dependencies_, max_retries_,
      definition_version_, state_column_id_, request_column_id_, result_spec_column_id_, incarnation_column_id_);
  if (OB_SUCC(ret) && !expression.empty()) {
    ret = set_expression(expression, normalized);
  } else if (OB_SUCC(ret)) {
    expression_.reset(); normalized_expression_.reset();
  }
  return ret;
}

OB_DEF_SERIALIZE_SIZE(AiColumnSchema)
{
  int64_t len = 0;
  LST_DO_CODE(OB_UNIS_ADD_LEN, expression_, normalized_expression_, dependencies_,
      max_retries_, definition_version_, state_column_id_, request_column_id_, result_spec_column_id_, incarnation_column_id_);
  return len;
}
} // namespace oceanbase::share::schema
