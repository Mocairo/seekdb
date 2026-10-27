/* Native Connector/C regression; SQL PREPARE through COM_QUERY is insufficient.
 * See README.prepared_protocol.md for compilation and connection settings. */
#include <mysql.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int ok, MYSQL_STMT *stmt, const char *operation)
{
  if (!ok) {
    fprintf(stderr, "FAIL %s: errno=%u error=%s\n", operation,
            mysql_stmt_errno(stmt), mysql_stmt_error(stmt));
    exit(1);
  }
}

static const char *setting(const char *name, const char *fallback)
{
  const char *value = getenv(name);
  return value ? value : fallback;
}

static MYSQL_STMT *prepare(MYSQL *db, const char *sql)
{
  MYSQL_STMT *stmt = mysql_stmt_init(db);
  if (!stmt) {
    fprintf(stderr, "FAIL mysql_stmt_init: %s\n", mysql_error(db));
    exit(1);
  }
  require(mysql_stmt_prepare(stmt, sql, strlen(sql)) == 0, stmt, "prepare");
  return stmt;
}

static void constant(MYSQL *db)
{
  MYSQL_STMT *stmt = prepare(db, "select 1");
  MYSQL_BIND output = {0};
  long long value = 0;
  output.buffer_type = MYSQL_TYPE_LONGLONG;
  output.buffer = &value;
  require(mysql_stmt_param_count(stmt) == 0, stmt, "zero parameters");
  require(mysql_stmt_bind_result(stmt, &output) == 0, stmt, "bind constant");
  require(mysql_stmt_execute(stmt) == 0, stmt, "execute select 1");
  require(mysql_stmt_fetch(stmt) == 0 && value == 1, stmt, "fetch select 1");
  require(mysql_stmt_fetch(stmt) == MYSQL_NO_DATA, stmt, "constant end of rows");
  mysql_stmt_close(stmt);
}

static void parameters(MYSQL *db)
{
  MYSQL_STMT *stmt = prepare(db, "select ?, ?, ?, json_object('k', ?)");
  MYSQL_BIND input[4] = {{0}}, output[4] = {{0}};
  long long number = 0, actual_number = 0;
  char text[] = "中文 / '\\ Unicode", optional[] = "present";
  char actual_text[128], actual_optional[128], actual_json[128], expected_json[128];
  unsigned long text_len = strlen(text), optional_len = strlen(optional), lengths[4] = {0};
  my_bool null_input = 0, nulls[4] = {0}, errors[4] = {0};
  input[0].buffer_type = MYSQL_TYPE_LONGLONG;
  input[0].buffer = &number;
  input[1].buffer_type = MYSQL_TYPE_STRING;
  input[1].buffer = text;
  input[1].buffer_length = sizeof(text);
  input[1].length = &text_len;
  input[2].buffer_type = MYSQL_TYPE_STRING;
  input[2].buffer = optional;
  input[2].buffer_length = sizeof(optional);
  input[2].length = &optional_len;
  input[2].is_null = &null_input;
  input[3] = input[0];
  output[0].buffer_type = MYSQL_TYPE_LONGLONG;
  output[0].buffer = &actual_number;
  output[1].buffer = actual_text;
  output[2].buffer = actual_optional;
  output[3].buffer = actual_json;
  for (int i = 0; i < 4; ++i) {
    output[i].is_null = &nulls[i];
    output[i].error = &errors[i];
    output[i].length = &lengths[i];
    if (i > 0) {
      output[i].buffer_type = MYSQL_TYPE_STRING;
      output[i].buffer_length = sizeof(actual_text);
    }
  }
  require(mysql_stmt_param_count(stmt) == 4, stmt, "four parameters");
  require(mysql_stmt_bind_param(stmt, input) == 0, stmt, "bind parameters");
  require(mysql_stmt_bind_result(stmt, output) == 0, stmt, "bind results");
  for (int iteration = 0; iteration < 4; ++iteration) {
    number = iteration % 2 ? -4294967297LL : 4294967298LL;
    null_input = iteration % 2;
    snprintf(expected_json, sizeof(expected_json), "{\"k\": %lld}", number);
    require(mysql_stmt_execute(stmt) == 0, stmt, "execute parameters");
    require(mysql_stmt_store_result(stmt) == 0, stmt, "store result");
    require(mysql_stmt_num_rows(stmt) == 1, stmt, "one row");
    require(mysql_stmt_fetch(stmt) == 0, stmt, "fetch parameters");
    require(!nulls[0] && !errors[0] && actual_number == number, stmt, "int64 value");
    require(!nulls[1] && !errors[1] && lengths[1] == text_len &&
            memcmp(actual_text, text, text_len) == 0, stmt, "Unicode and escapes");
    require(nulls[2] == null_input, stmt, "NULL propagation");
    if (!null_input) {
      require(!errors[2] && lengths[2] == optional_len &&
              memcmp(actual_optional, optional, optional_len) == 0, stmt, "non-NULL value");
    }
    require(!nulls[3] && !errors[3] && lengths[3] == strlen(expected_json) &&
            memcmp(actual_json, expected_json, lengths[3]) == 0, stmt, "JSON value");
    require(mysql_stmt_fetch(stmt) == MYSQL_NO_DATA, stmt, "parameter end of rows");
    mysql_stmt_free_result(stmt);
    if (iteration == 1) require(mysql_stmt_reset(stmt) == 0, stmt, "reset statement");
  }
  mysql_stmt_close(stmt);
}

int main(void)
{
  MYSQL *db = mysql_init(NULL);
  unsigned int timeout = 10;
  if (!db) return 1;
  mysql_options(db, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
  mysql_options(db, MYSQL_OPT_READ_TIMEOUT, &timeout);
  mysql_options(db, MYSQL_OPT_WRITE_TIMEOUT, &timeout);
  mysql_options(db, MYSQL_SET_CHARSET_NAME, "utf8mb4");
  if (!mysql_real_connect(db, setting("SEEKDB_TEST_HOST", "127.0.0.1"),
                          setting("SEEKDB_TEST_USER", "root"),
                          setting("SEEKDB_TEST_PASSWORD", ""), NULL,
                          (unsigned int)atoi(setting("SEEKDB_TEST_PORT", "2883")), NULL, 0)) {
    fprintf(stderr, "FAIL connect: %u %s\n", mysql_errno(db), mysql_error(db));
    mysql_close(db);
    return 1;
  }
  constant(db);
  parameters(db);
  MYSQL_STMT *bad = mysql_stmt_init(db);
  if (!bad) return 1;
  require(mysql_stmt_prepare(bad, "select from", 11) != 0 && mysql_stmt_errno(bad) != 0,
          bad, "invalid SQL reports error");
  mysql_stmt_close(bad);
  constant(db);
  if (mysql_query(db, "select 1") != 0) {
    fprintf(stderr, "FAIL text protocol after prepared errors: %s\n", mysql_error(db));
    return 1;
  }
  MYSQL_RES *result = mysql_store_result(db);
  MYSQL_ROW row = result ? mysql_fetch_row(result) : NULL;
  if (!row || !row[0] || strcmp(row[0], "1") != 0) return 1;
  mysql_free_result(result);
  mysql_close(db);
  puts("PASS: zero parameters, bound int64/Unicode/NULL/JSON, reuse/reset, error recovery, text protocol");
  return 0;
}
