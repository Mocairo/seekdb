/* Copyright (c) 2026 OceanBase. Apache-2.0. */
#include "share/ai_column/ai_column_state.h"
#include <array>
#include <cstdio>
#include <cstdlib>

using namespace oceanbase::share::ai_column;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (false)

static ColumnVersion version(uint64_t column = 16, uint64_t request = 1)
{ return {1, 100, column, {3, 1}, {7, request}, 1}; }

static void test_lease_recovery()
{
  TaskRecord task{version()};
  task.max_retries = 1;
  ClaimToken old, current;
  CHECK(claim_task(task, {1, 1}, 10, 100, old) == StateResult::OK);
  CHECK(claim_task(task, {1, 2}, 109, 100, current) == StateResult::NOT_ELIGIBLE);
  // Recreating the record represents reloading durable task state, not an
  // in-memory queue that disappears on restart.
  TaskRecord recovered = task;
  CHECK(claim_task(recovered, {1, 2}, 110, 100, current) == StateResult::OK);
  CHECK(current.epoch > old.epoch);
  CHECK(stage_success(recovered, old, version(), 111) == StateResult::LOST_LEASE);
  CHECK(record_failure(recovered, old, version(), 111, 1) == StateResult::LOST_LEASE);
  CHECK(renew_lease(recovered, old, 111, 100) == StateResult::LOST_LEASE);
  CHECK(renew_lease(recovered, current, 120, 1) == StateResult::OK);
  CHECK(recovered.lease_deadline == 210);
  CHECK(stage_success(recovered, current, version(), 121) == StateResult::OK);
  CHECK(claim_task(recovered, {1, 3}, 1000, 100, old) == StateResult::NOT_ELIGIBLE);
}

static void test_retry_budget()
{
  for (uint32_t budget : {0, 1, 2, 10}) {
    TaskRecord task{version()};
    task.max_retries = budget;
    ClaimToken token;
    int64_t now = 10;
    for (uint32_t attempt = 0; attempt <= budget; ++attempt) {
      CHECK(claim_task(task, {1, 1}, now, 100, token) == StateResult::OK);
      CHECK(record_failure(task, token, version(), now, 10) == StateResult::OK);
      CHECK(task.retry_count == (attempt < budget ? attempt + 1 : budget));
      if (attempt < budget) {
        CHECK(task.state == TaskState::RETRY_WAIT);
        CHECK(claim_task(task, {1, 1}, now + 9, 100, token) == StateResult::NOT_ELIGIBLE);
      } else {
        CHECK(task.state == TaskState::FAILED);
      }
      now += 10;
    }
    CHECK(claim_task(task, {1, 2}, now, 100, token) == StateResult::NOT_ELIGIBLE);
  }
}

static void test_versions_and_reinsert()
{
  TaskRecord task{version()};
  ClaimToken token;
  CHECK(claim_task(task, {1, 1}, 10, 100, token) == StateResult::OK);
  for (int field = 0; field < 6; ++field) {
    auto changed = version();
    switch (field) {
      case 0: ++changed.table_id; break;
      case 1: ++changed.column_id; break;
      case 2: ++changed.request_id.low; break;
      case 3: ++changed.definition_version; break;
      case 4: ++changed.row_incarnation.low; break;
      case 5: ++changed.tenant_id; break;
    }
    CHECK(stage_success(task, token, changed, 11) == StateResult::STALE_REQUEST);
    CHECK(record_failure(task, token, changed, 11, 1) == StateResult::STALE_REQUEST);
    CHECK(task.state == TaskState::RUNNING);
  }
  CHECK(stage_success(task, token, version(), 110) == StateResult::LOST_LEASE);
}

static void test_group_publication()
{
  std::array<uint64_t, 2> ids{16, 17};
  GroupManifest group{1, 100, {3, 1}, {7, 1}, ids};
  std::array<TaskRecord, 2> tasks{TaskRecord{version(16)}, TaskRecord{version(17)}};
  std::array<ColumnVersion, 2> rows{version(16), version(17)};
  std::array<Publication, 2> output{{{91, PublishAction::KEEP_CURRENT}, {92, PublishAction::KEEP_CURRENT}}};
  tasks[0].state = TaskState::SUCCEEDED;
  CHECK(prepare_publication(group, tasks, rows, output) == StateResult::INCOMPLETE_GROUP);
  CHECK(output[0].column_id == 91 && output[1].column_id == 92);
  tasks[1].state = TaskState::FAILED;
  CHECK(prepare_publication(group, tasks, rows, output) == StateResult::OK);
  CHECK(output[0].action == PublishAction::WRITE_READY);
  CHECK(output[1].action == PublishAction::WRITE_FAILED);
  // A targeted retry supersedes one member without cancelling the other.
  rows[0] = version(16, 2);
  tasks[0].state = TaskState::RUNNING;
  CHECK(prepare_publication(group, tasks, rows, output) == StateResult::OK);
  CHECK(output[0].action == PublishAction::KEEP_CURRENT);
  CHECK(output[1].action == PublishAction::WRITE_FAILED);
  ++rows[1].row_incarnation.low;
  CHECK(prepare_publication(group, tasks, rows, output) == StateResult::STALE_REQUEST);
  rows[1] = version(17);
  CHECK(prepare_publication(group, std::span(tasks).first(1), rows, output) == StateResult::INVALID_GROUP);
  ids[1] = 16;
  CHECK(prepare_publication(group, tasks, rows, output) == StateResult::INVALID_GROUP);
}

static void test_boundaries()
{
  TaskRecord task{version()};
  ClaimToken token;
  CHECK(claim_task(task, {}, 0, 1, token) == StateResult::INVALID_ARGUMENT);
  CHECK(claim_task(task, {1, 1}, -1, 1, token) == StateResult::INVALID_ARGUMENT);
  CHECK(claim_task(task, {1, 1}, INT64_MAX, 1, token) == StateResult::INVALID_ARGUMENT);
  CHECK(task.state == TaskState::QUEUED && task.claim.epoch == 0);
  task.claim.epoch = UINT64_MAX;
  CHECK(claim_task(task, {1, 1}, 0, 1, token) == StateResult::COUNTER_EXHAUSTED);
  task.claim.epoch = 0;
  CHECK(claim_task(task, {1, 1}, INT64_MAX - 1, 1, token) == StateResult::OK);
  CHECK(record_failure(task, token, version(), INT64_MAX - 1, 2) == StateResult::INVALID_ARGUMENT);
}

static void test_crash_budget()
{
  for (uint32_t budget : {0, 1, 2, 10}) {
    TaskRecord task{version()};
    task.max_retries = budget;
    ClaimToken token;
    for (uint32_t attempt = 0; attempt <= budget; ++attempt) {
      CHECK(claim_task(task, {1, attempt + 1}, attempt * 10, 10, token) == StateResult::OK);
      CHECK(task.attempt_count == attempt + 1);
      CHECK(task.retry_count == attempt);
      // No failure acknowledgment: simulate process death after an uncertain send.
      TaskRecord restarted = task;
      task = restarted;
    }
    CHECK(claim_task(task, {2, 1}, (budget + 1) * 10, 10, token) == StateResult::RETRY_EXHAUSTED);
    CHECK(task.state == TaskState::FAILED);
    CHECK(stage_success(task, token, version(), (budget + 1) * 10) == StateResult::LOST_LEASE);
  }
}

static void test_request_ids()
{
  for (RequestId original : {RequestId{1, 0}, RequestId{0, 1}, RequestId{UINT64_MAX, UINT64_MAX},
                             RequestId{0x0123456789abcdef, 0xfedcba9876543210}}) {
    std::array<char, 32> text{};
    CHECK(format_request_id(original, text) == StateResult::OK);
    RequestId parsed;
    CHECK(parse_request_id(text, parsed) == StateResult::OK && parsed == original);
    for (auto &c : text) { if (c >= 'a' && c <= 'f') { c -= 'a' - 'A'; } }
    CHECK(parse_request_id(text, parsed) == StateResult::OK && parsed == original);
    text[17] = 'x';
    CHECK(parse_request_id(text, parsed) == StateResult::INVALID_ARGUMENT && parsed == original);
    CHECK(parse_request_id(std::span(text).first(31), parsed) == StateResult::INVALID_ARGUMENT);
  }
  std::array<char, 32> zero{};
  zero.fill('0');
  RequestId parsed{1, 1};
  CHECK(parse_request_id(zero, parsed) == StateResult::INVALID_ARGUMENT);
  CHECK((parsed == RequestId{1, 1}));
  CHECK(format_request_id({}, zero) == StateResult::INVALID_ARGUMENT);
  CHECK(format_request_id(parsed, std::span(zero).first(31)) == StateResult::INVALID_ARGUMENT);
}

int main()
{
  test_lease_recovery();
  test_retry_budget();
  test_versions_and_reinsert();
  test_group_publication();
  test_boundaries();
  test_crash_budget();
  test_request_ids();
  std::puts("AI column state: leases, retry budgets, stale versions, atomic publication and boundaries passed");
}
