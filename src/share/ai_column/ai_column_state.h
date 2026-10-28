/* Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0. */
#pragma once

#include <cstdint>
#include <limits>
#include <span>

namespace oceanbase::share::ai_column
{
// Reserved kernel SQL scope: repair missing/stale row markers after ADD/MODIFY.
// Physical column IDs never use this sentinel.
constexpr uint64_t BACKFILL_STALE_COLUMNS = std::numeric_limits<uint64_t>::max() - 1;
// This layer makes no SQL or HTTP calls and owns no memory. Its transitions
// must be persisted using a transaction/CAS by the task store. A successful
// local transition alone never grants permission to publish a database row.
enum class Status : uint8_t { PENDING, READY, FAILED };
enum class TaskState : uint8_t {
  QUEUED = 0, RUNNING = 1, SUCCEEDED = 2, RETRY_WAIT = 3,
  FAILED = 4, SUPERSEDED = 5
};
enum class StateResult : uint8_t {
  OK, INVALID_ARGUMENT, NOT_ELIGIBLE, LOST_LEASE, STALE_REQUEST,
  INCOMPLETE_GROUP, INVALID_GROUP, COUNTER_EXHAUSTED, RETRY_EXHAUSTED
};

struct RequestId
{
  uint64_t high = 0;
  uint64_t low = 0;
  constexpr bool valid() const { return high != 0 || low != 0; }
  constexpr bool operator==(const RequestId &) const = default;
};

inline StateResult parse_request_id(std::span<const char> text, RequestId &result)
{
  if (text.size() != 32) { return StateResult::INVALID_ARGUMENT; }
  RequestId parsed;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    const int digit = c >= '0' && c <= '9' ? c - '0'
        : c >= 'a' && c <= 'f' ? c - 'a' + 10
        : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (digit < 0) { return StateResult::INVALID_ARGUMENT; }
    auto &word = i < 16 ? parsed.high : parsed.low;
    word = (word << 4) | static_cast<uint64_t>(digit);
  }
  if (!parsed.valid()) { return StateResult::INVALID_ARGUMENT; }
  result = parsed;
  return StateResult::OK;
}

inline StateResult format_request_id(RequestId id, std::span<char> text)
{
  if (!id.valid() || text.size() != 32) { return StateResult::INVALID_ARGUMENT; }
  constexpr char digits[] = "0123456789abcdef";
  for (std::size_t i = 0; i < 32; ++i) {
    const uint64_t word = i < 16 ? id.high : id.low;
    text[i] = digits[(word >> ((15 - (i % 16)) * 4)) & 15];
  }
  return StateResult::OK;
}

struct ColumnVersion
{
  uint64_t tenant_id = 0;
  uint64_t table_id = 0;
  uint64_t column_id = 0;
  RequestId row_incarnation;
  RequestId request_id;
  uint64_t definition_version = 0;
  constexpr bool valid() const
  {
    return tenant_id != 0 && table_id != 0 && column_id != 0 && row_incarnation.valid()
        && request_id.valid() && definition_version != 0;
  }
  constexpr bool operator==(const ColumnVersion &) const = default;
};

struct ClaimToken
{
  RequestId owner;
  uint64_t epoch = 0;
  constexpr bool operator==(const ClaimToken &) const = default;
};

struct TaskRecord
{
  ColumnVersion version;
  TaskState state = TaskState::QUEUED;
  uint32_t retry_count = 0;
  uint32_t max_retries = 0; // Number of retries after the first HTTP attempt.
  uint64_t attempt_count = 0; // Persisted at claim, including uncertain sends.
  int64_t eligible_ts = 0;
  int64_t lease_deadline = 0;
  ClaimToken claim;
};

constexpr bool terminal(TaskState state)
{
  return state == TaskState::SUCCEEDED || state == TaskState::FAILED
      || state == TaskState::SUPERSEDED;
}

inline bool owns_live_lease(const TaskRecord &task, const ClaimToken &claim, int64_t now)
{
  return now >= 0 && task.state == TaskState::RUNNING && claim.owner.valid()
      && claim.epoch != 0 && task.claim == claim && now < task.lease_deadline;
}

inline StateResult claim_task(TaskRecord &task, RequestId owner, int64_t now,
                              int64_t lease_duration, ClaimToken &token)
{
  if (!task.version.valid() || !owner.valid() || now < 0 || lease_duration <= 0
      || now > std::numeric_limits<int64_t>::max() - lease_duration
      || task.retry_count > task.max_retries
      || (task.state == TaskState::RUNNING
          && (!task.claim.owner.valid() || task.claim.epoch == 0 || task.attempt_count == 0
              || task.lease_deadline <= 0))) {
    return StateResult::INVALID_ARGUMENT;
  }
  const bool queued = task.state == TaskState::QUEUED || task.state == TaskState::RETRY_WAIT;
  const bool expired = task.state == TaskState::RUNNING && now >= task.lease_deadline;
  if ((!queued && !expired) || now < task.eligible_ts) {
    return StateResult::NOT_ELIGIBLE;
  }
  if (task.claim.epoch == std::numeric_limits<uint64_t>::max()) {
    return StateResult::COUNTER_EXHAUSTED;
  }
  if (task.attempt_count >= static_cast<uint64_t>(task.max_retries) + 1) {
    // Persist this terminal transition even though no new lease was granted.
    // A process crash must not reset the finite retry budget.
    task.state = TaskState::FAILED;
    task.lease_deadline = 0;
    return StateResult::RETRY_EXHAUSTED;
  }
  if (expired) { ++task.retry_count; }
  ++task.attempt_count;
  task.state = TaskState::RUNNING;
  task.claim = {owner, task.claim.epoch + 1};
  task.lease_deadline = now + lease_duration;
  token = task.claim;
  return StateResult::OK;
}

inline StateResult renew_lease(TaskRecord &task, const ClaimToken &token,
                               int64_t now, int64_t lease_duration)
{
  if (lease_duration <= 0 || now < 0
      || now > std::numeric_limits<int64_t>::max() - lease_duration) {
    return StateResult::INVALID_ARGUMENT;
  }
  if (!owns_live_lease(task, token, now)) { return StateResult::LOST_LEASE; }
  // A renewal never shortens a live lease.
  const int64_t deadline = now + lease_duration;
  if (deadline > task.lease_deadline) { task.lease_deadline = deadline; }
  return StateResult::OK;
}

inline StateResult stage_success(TaskRecord &task, const ClaimToken &token,
                                 const ColumnVersion &current, int64_t now)
{
  if (!owns_live_lease(task, token, now)) { return StateResult::LOST_LEASE; }
  if (!current.valid() || task.version != current) { return StateResult::STALE_REQUEST; }
  // The task store persists the typed result and this state in one transaction.
  task.state = TaskState::SUCCEEDED;
  task.lease_deadline = 0;
  return StateResult::OK;
}

inline StateResult record_failure(TaskRecord &task, const ClaimToken &token,
                                  const ColumnVersion &current, int64_t now,
                                  int64_t retry_delay)
{
  if (retry_delay < 0 || now < 0
      || now > std::numeric_limits<int64_t>::max() - retry_delay) {
    return StateResult::INVALID_ARGUMENT;
  }
  if (!owns_live_lease(task, token, now)) { return StateResult::LOST_LEASE; }
  if (!current.valid() || task.version != current) { return StateResult::STALE_REQUEST; }
  if (task.retry_count < task.max_retries) {
    ++task.retry_count;
    task.state = TaskState::RETRY_WAIT;
    task.eligible_ts = now + retry_delay;
  } else {
    task.state = TaskState::FAILED;
  }
  task.lease_deadline = 0;
  return StateResult::OK;
}

struct GroupManifest
{
  uint64_t tenant_id = 0;
  uint64_t table_id = 0;
  RequestId row_incarnation;
  RequestId request_id;
  std::span<const uint64_t> member_column_ids;
};

enum class PublishAction : uint8_t { KEEP_CURRENT, WRITE_READY, WRITE_FAILED };
struct Publication
{
  uint64_t column_id = 0;
  PublishAction action = PublishAction::KEEP_CURRENT;
};

// Validates the entire manifest before touching output. Missing/duplicate task
// rows are corruption, not evidence that the group has finished. Replaced
// members retire from the old group without blocking unaffected members.
// The caller must lock/revalidate these versions and atomically apply ALL
// publications plus task cleanup; applying individual entries is forbidden.
inline StateResult prepare_publication(const GroupManifest &group,
                                       std::span<const TaskRecord> tasks,
                                       std::span<const ColumnVersion> current,
                                       std::span<Publication> output)
{
  const size_t count = group.member_column_ids.size();
  if (group.tenant_id == 0 || group.table_id == 0 || !group.row_incarnation.valid() || !group.request_id.valid()
      || count == 0 || tasks.size() != count || current.size() != count
      || output.size() != count) {
    return StateResult::INVALID_GROUP;
  }
  for (size_t i = 0; i < count; ++i) {
    const auto &version = tasks[i].version;
    const auto &row = current[i];
    const uint64_t column = group.member_column_ids[i];
    if (column == 0 || !version.valid() || version.tenant_id != group.tenant_id || version.table_id != group.table_id
        || version.column_id != column || version.row_incarnation != group.row_incarnation
        || version.request_id != group.request_id) {
      return StateResult::INVALID_GROUP;
    }
    for (size_t j = 0; j < i; ++j) {
      if (group.member_column_ids[j] == column) { return StateResult::INVALID_GROUP; }
    }
    if (!row.valid() || row.tenant_id != group.tenant_id || row.table_id != group.table_id || row.column_id != column
        || row.row_incarnation != group.row_incarnation) {
      return StateResult::STALE_REQUEST; // Deleted row or same-key new incarnation.
    }
    if (row == version) {
      if (tasks[i].state == TaskState::SUPERSEDED) { return StateResult::INVALID_GROUP; }
      if (!terminal(tasks[i].state)) { return StateResult::INCOMPLETE_GROUP; }
    }
  }
  for (size_t i = 0; i < count; ++i) {
    PublishAction action = PublishAction::KEEP_CURRENT;
    if (tasks[i].version == current[i]) {
      action = tasks[i].state == TaskState::SUCCEEDED
          ? PublishAction::WRITE_READY : PublishAction::WRITE_FAILED;
    }
    output[i] = {group.member_column_ids[i], action};
  }
  return StateResult::OK;
}
} // namespace oceanbase::share::ai_column
