# Native prepared-protocol regression

## Dependency fix

Ubuntu 22.04 x86_64 uses `deps/init/oceanbase.el9.x86_64.deps`. Both
`obclient` and `libobclient` are pinned to the official 2.2.13 packages there.
Refresh an existing checkout with `./build.sh release --init` before testing.
The fix changes test-client dependencies, not seekdb's SQL or wire protocol.
Other platform dependency lists have not been changed or validated by this fix.

Connector/C 2.2.3 declares `int ret;` in `mysql_stmt_execute()` and subsequently
tests `if (0 == ret)` without assigning it. The tracing macros do not initialize
it. In the tested Linux binary, the compiler has eliminated the execute-command
path: preparation and binding succeed, but execution returns 1 with errno 0
without sending `COM_STMT_EXECUTE`. Version 2.2.13 initializes `ret = 0`.
See the upstream [2.2.3 source](https://github.com/oceanbase/obconnector-c/blob/v2.2.3/libmariadb/mariadb_stmt.c)
and [2.2.13 source](https://github.com/oceanbase/obconnector-c/blob/v2.2.13/libmariadb/mariadb_stmt.c).

The packaged `mysqltest` statically links the connector. Updating only the
shared library or `LD_LIBRARY_PATH` does not repair that executable; update
both packages. Do not work around the failure by disabling `--ps-protocol`.

## C API check

Against a running disposable seekdb instance, from the repository root:

```bash
mkdir -p build_release/protocol-tests
client="$PWD/deps/3rd/u01/obclient"
cc -std=c99 -Wall -Wextra -Werror -I "$client/include" \
  unittest/sql/test_prepared_protocol.c -L "$client/lib" -lobclnt \
  -o build_release/protocol-tests/test_prepared_protocol
LD_LIBRARY_PATH="$client/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  build_release/protocol-tests/test_prepared_protocol
```

Connection variables are `SEEKDB_TEST_HOST` (127.0.0.1), `SEEKDB_TEST_PORT`
(2883), `SEEKDB_TEST_USER` (root), and `SEEKDB_TEST_PASSWORD` (empty).
The check creates no database objects. It exercises actual C API preparation,
zero parameters, bound signed 64-bit integers, Unicode/escaped strings, NULL
transitions, JSON results, repeated execution, statement reset, invalid SQL,
and recovery to prepared and text queries on the same connection.

## Native AI golden-result comparisons

Start the local fixture in a separate terminal; the supplied golden results
expect dimension 768. This fixture is not the organizer's Mock AI Service.

```bash
PYTHONPATH=unittest/sql/engine/expr python -c \
  "from ai_mock_service import MockAIService; s=MockAIService(('0.0.0.0',8000)); s.default_dimension=768; s.serve_forever()"
```

Use a disposable database and the original `.result` files. For example:

```bash
client="$PWD/deps/3rd/u01/obclient"
export LD_LIBRARY_PATH="$client/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
"$client/bin/obclient" -h127.0.0.1 -P2883 -uroot \
  -e 'create database if not exists ai_protocol_regression'
suite=tools/deploy/mysql_test/test_suite/ai_function
for name in ai_function_enhancement ai_prompt; do
  mkdir -p "build_release/protocol-tests/$name"
  "$client/bin/mysqltest" --host=127.0.0.1 --port=2883 --user=root \
    --database=ai_protocol_regression --ps-protocol \
    --test-file="$suite/t/$name.test" \
    --result-file="$suite/r/mysql/$name.result" \
    --logdir="build_release/protocol-tests/$name"
done
```

Also run these comparisons without `--ps-protocol` to check the text path.
On the same Linux server and unchanged seekdb binary, the old C client fails
at `select 1` with errno 0; the updated client passes the C check and both AI
golden comparisons in both protocol modes. This is separate from the contest's
`DROP_AI_MODEL` timeout (4012), which has not been reproduced locally.
