/* Copyright (c) 2026 OceanBase. Apache-2.0. */
#include "share/ai_column/ai_column_task_store.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "share/ob_dml_sql_splicer.h"

namespace oceanbase::share::ai_column
{
using namespace common;
namespace
{
int task_store_id(const ObString &text, RequestId &id)
{
  return parse_request_id(std::span<const char>(text.ptr(), text.length()), id) == StateResult::OK
      ? OB_SUCCESS : OB_INVALID_DATA;
}

int task_store_copy(sqlclient::ObMySQLResult &result, const char *name, ObIAllocator &allocator, ObString &value)
{
  int ret = OB_SUCCESS;
  ObString text;
  if (OB_FAIL(result.get_varchar(name, text))) {
  } else { ret = ob_write_string(allocator, text, value); }
  return ret;
}

int task_store_decode(sqlclient::ObMySQLResult &result, ObIAllocator &allocator, uint64_t scope, DurableTask &task)
{
  int ret = OB_SUCCESS;
  int64_t table_id = 0, column_id = 0, version = 0, state = 0, retries = 0, max_retries = 0;
  int64_t attempts = 0, epoch = 0, eligible = 0, lease = 0;
  ObString request, incarnation, owner;
  RequestId task_id;
  if (OB_FAIL(task_store_copy(result, "task_id", allocator, task.task_id))
      || OB_FAIL(task_store_copy(result, "rowkey_predicate", allocator, task.rowkey_predicate))
      || OB_FAIL(task_store_copy(result, "input_snapshot", allocator, task.input_snapshot))
      || OB_FAIL(result.get_varchar("request_id", request))
      || OB_FAIL(result.get_varchar("row_incarnation", incarnation))
      || OB_FAIL(result.get_varchar("owner", owner))
      || OB_FAIL(result.get_int("table_id", table_id))
      || OB_FAIL(result.get_int("column_id", column_id))
      || OB_FAIL(result.get_int("definition_version", version))
      || OB_FAIL(result.get_int("state", state))
      || OB_FAIL(result.get_int("retry_count", retries))
      || OB_FAIL(result.get_int("max_retries", max_retries))
      || OB_FAIL(result.get_int("attempt_count", attempts))
      || OB_FAIL(result.get_int("claim_epoch", epoch))
      || OB_FAIL(result.get_int("eligible_us", eligible))
      || OB_FAIL(result.get_int("lease_us", lease))) {
  } else if (scope == 0 || table_id <= 0 || column_id <= 0 || version <= 0
      || state < 0 || state > static_cast<int64_t>(TaskState::SUPERSEDED)
      || retries < 0 || max_retries < 0 || max_retries > INT32_MAX || retries > max_retries
      || attempts < 0 || epoch < 0 || eligible < 0 || lease < 0) {
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(task_store_id(task.task_id, task_id))
      || OB_FAIL(task_store_id(request, task.record.version.request_id))
      || OB_FAIL(task_store_id(incarnation, task.record.version.row_incarnation))) {
  } else if (!owner.empty() && OB_FAIL(task_store_id(owner, task.record.claim.owner))) {
  } else {
    task.record.version.tenant_id = scope;
    task.record.version.table_id = table_id;
    task.record.version.column_id = column_id;
    task.record.version.definition_version = version;
    task.record.state = static_cast<TaskState>(state);
    task.record.retry_count = static_cast<uint32_t>(retries);
    task.record.max_retries = static_cast<uint32_t>(max_retries);
    task.record.attempt_count = attempts;
    task.record.claim.epoch = epoch;
    task.record.eligible_ts = eligible;
    task.record.lease_deadline = lease;
  }
  return ret;
}
}

int AiColumnTaskStore::claim_next(ObMySQLTransaction &transaction, ObIAllocator &allocator,
    uint64_t scope, RequestId owner, int64_t now, int64_t lease_duration, DurableTask &task, bool &found)
{
  int ret = OB_SUCCESS;
  found = false;
  if (!transaction.is_started() || scope == 0 || !owner.valid() || now < 0 || lease_duration <= 0) {
    return OB_INVALID_ARGUMENT;
  }
  task = DurableTask{};
  ObSqlString sql;
  if (OB_FAIL(sql.append_fmt(
      "SELECT task_id,table_id,column_id,row_incarnation,request_id,definition_version,"
      "CAST(rowkey_predicate AS CHAR) AS rowkey_predicate,CAST(input_snapshot AS CHAR) AS input_snapshot,"
      "state,max_retries,retry_count,attempt_count,claim_epoch,IFNULL(lease_owner,'') AS owner,"
      "TIME_TO_USEC(eligible_ts) AS eligible_us,IFNULL(TIME_TO_USEC(lease_deadline),0) AS lease_us "
      "FROM oceanbase.__all_ai_column_task WHERE "
      "(state IN (0,3) AND eligible_ts<=USEC_TO_TIME(%ld)) OR "
      "(state=1 AND lease_deadline<=USEC_TO_TIME(%ld)) "
      "ORDER BY eligible_ts,task_id LIMIT 1 FOR UPDATE SKIP LOCKED", now, now))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, response) {
      if (OB_FAIL(transaction.read(response, sql.ptr()))) {
      } else if (response.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else if (OB_FAIL(response.get_result()->next())) {
        if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
      } else if (OB_FAIL(task_store_decode(*response.get_result(), allocator, scope, task))) {
      } else { found = true; }
    }
  }
  if (OB_SUCC(ret) && found) {
    const uint64_t previous_epoch = task.record.claim.epoch;
    const TaskState previous_state = task.record.state;
    ClaimToken token;
    const StateResult transition = claim_task(task.record, owner, now, lease_duration, token);
    if (transition != StateResult::OK && transition != StateResult::RETRY_EXHAUSTED) {
      ret = transition == StateResult::COUNTER_EXHAUSTED ? OB_SIZE_OVERFLOW : OB_INVALID_DATA;
    } else {
      sql.reset();
      ObDMLSqlSplicer update;
      char owner_text[32];
      if (OB_FAIL(update.add_pk_column("task_id", task.task_id))
          || OB_FAIL(update.add_column("state", static_cast<int64_t>(task.record.state)))
          || OB_FAIL(update.add_column("retry_count", task.record.retry_count))
          || OB_FAIL(update.add_column("attempt_count", task.record.attempt_count))
          || OB_FAIL(update.add_column("claim_epoch", task.record.claim.epoch))) {
      } else if (task.record.claim.owner.valid()) {
        if (format_request_id(task.record.claim.owner, owner_text) != StateResult::OK) { ret = OB_INVALID_DATA; }
        else { ret = update.add_column("lease_owner", ObString(32, owner_text)); }
      }
      if (OB_SUCC(ret)) {
        ret = task.record.lease_deadline > 0
            ? update.add_time_column("lease_deadline", task.record.lease_deadline)
            : update.add_column(true, "lease_deadline");
      }
      if (OB_SUCC(ret) && OB_FAIL(update.get_extra_condition().append_fmt(
              "claim_epoch=%lu AND state=%ld", previous_epoch, static_cast<int64_t>(previous_state)))) {
      } else if (OB_SUCC(ret) && OB_FAIL(update.splice_update_sql("oceanbase.__all_ai_column_task", sql))) {
      } else if (OB_SUCC(ret)) {
        int64_t affected = 0;
        if (OB_FAIL(transaction.write(sql.ptr(), affected))) {
        } else if (affected != 1) { ret = OB_STATE_NOT_MATCH; }
      }
    }
  }
  return ret;
}

int AiColumnTaskStore::stage(ObMySQLTransaction &transaction, const DurableTask &task,
    int64_t now, int error, const ObString &result, bool result_is_null, int64_t retry_delay)
{
  int ret = OB_SUCCESS;
  if (!transaction.is_started()) { return OB_INVALID_ARGUMENT; }
  TaskRecord next = task.record;
  const StateResult transition = error == OB_SUCCESS
      ? stage_success(next, task.record.claim, task.record.version, now)
      : record_failure(next, task.record.claim, task.record.version, now, retry_delay);
  if (transition != StateResult::OK) { return OB_STATE_NOT_MATCH; }
  char owner[32];
  RequestId id;
  if (format_request_id(task.record.claim.owner, owner) != StateResult::OK
      || task_store_id(task.task_id, id) != OB_SUCCESS) { return OB_INVALID_ARGUMENT; }
  ObSqlString sql;
  if (OB_FAIL(sql.append_fmt("UPDATE oceanbase.__all_ai_column_task SET state=%ld,"
          "retry_count=%u,eligible_ts=USEC_TO_TIME(%ld),lease_deadline=NULL,error_code=%d,result_value=",
          static_cast<int64_t>(next.state), next.retry_count, next.eligible_ts, error))) {
  } else if (error != OB_SUCCESS || result_is_null) { ret = sql.append("NULL"); }
  else { ret = sql_append_hex_escape_str(result, sql); }
  if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt(
          " WHERE task_id='%.*s' AND state=1 AND claim_epoch=%lu AND lease_owner='%.*s' "
          "AND lease_deadline>USEC_TO_TIME(%ld)", task.task_id.length(), task.task_id.ptr(),
          task.record.claim.epoch, 32, owner, now))) {
  } else if (OB_SUCC(ret)) {
    int64_t affected = 0;
    if (OB_FAIL(transaction.write(sql.ptr(), affected))) {
    } else if (affected != 1) { ret = OB_STATE_NOT_MATCH; }
  }
  return ret;
}
}
