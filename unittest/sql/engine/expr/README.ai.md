# AI function regression tests

These tests exercise the C++ SQL implementation. The Python service is only a
deterministic HTTP fixture; it never implements an SQL function or contacts a
real model.

For native C API prepared statements and mysqltest `--ps-protocol`, see
[the protocol regression instructions](../../README.prepared_protocol.md).
On Ubuntu 22.04 x86_64, refresh dependencies to use the fixed 2.2.13 client pair;
the old 2.2.3 client can fail even on `select 1` before sending EXECUTE.

## First AI-management call

Immediately after starting a disposable instance, run:

```bash
python unittest/sql/engine/expr/test_ai_model_lifecycle.py
```

This diagnostic keeps `ob_query_timeout=10000000` and executes the Prompt/LOB
SQL preceding the contest's failing model cleanup. It then checks that the
first missing-model DROP returns errno 11118 and that subsequent model
creation/deletion works. Each operation reports its duration and error code.
Run it before the main integration suite: that suite prewarms the management
package and raises its session timeout to 60 seconds. A failed diagnostic
retains its uniquely named objects for inspection. It uses no HTTP fixture.
Connection settings are `SEEKDB_TEST_HOST`, `SEEKDB_TEST_PORT` (2883 by default),
`SEEKDB_TEST_USER` and `SEEKDB_TEST_PASSWORD`.

### Linux cold-start and restart regression

The first management CALL used to wait for background system-package loading
inside its own query deadline. Writable servers now wait for the loader's
published completion before starting the SQL listener. Loading still overlaps
the rest of startup; its existing timer and retry policy remain unchanged.
The startup wait uses `_ob_ddl_timeout`, and the user's SQL timeout stays at
10 seconds. An absent job no longer counts as ready: the first timer callback
may not have created the job yet. Read-only standby startup skips this local
loader check because its metadata arrives through replay.

Build seekdb, install PyMySQL in the test environment, then run:

```bash
python unittest/sql/engine/expr/test_ai_cold_start.py \
  --binary build_release/src/observer/seekdb \
  --output build_release/ai-cold-start-check --workers 20 --restart
```

The output directory must be new. The test starts an isolated 4 GiB instance
on port 2885, pins it to one allowed CPU, and starts 20 competing processes on
that same CPU immediately after the first connection. It runs the Prompt/LOB
prefix and model lifecycle with `ob_query_timeout=10000000`, then checks a
restart using the same persisted database. All child processes are stopped;
data, commands, server logs and per-query timings are retained for diagnosis.
Use `--workers 0` for an unloaded baseline, `--cpu` to select an allowed CPU,
or `--port` to avoid another local test instance. No Mock AI Service is needed.
Use `--sync-packages` to check the existing synchronous-bootstrap configuration
as well; the default exercises asynchronous loading.

The unmodified Linux binary reproduced errno 4012 on the first missing-model
DROP. Merely shortening timer delays and retry backoff was insufficient under
repeated CPU pressure, so those experimental optimizations are not included.
CPU pressure is a controlled reproduction condition, not a claim about the
organizer's undocumented hardware or a guarantee under arbitrary overload.
With the final startup-readiness fix, two cold runs and a restart under the
same pressure passed; the first missing-model DROP took 0.007--0.057 seconds.
Synchronous-loading cold/restart tests also passed. SQL acceptance happens
later (about 8 seconds for cold startup in this environment), after packages
are ready, rather than exposing their initialization delay to the first CALL.

## C++ tests

`ob_expr_ai_prompt_test.cpp` covers JSON arrays/objects, nested prompts, repeated
arguments, Unicode/escapes and invalid/overflowing indices.
`test_ob_openai_utils.cpp` covers response ordering, duplicate/out-of-range
indices, invalid vector elements and Ollama request/response handling.

For an existing Windows CMake build, from the repository root:

```powershell
./deps/3rd/tools/ninja/ninja.exe -C build_release -j 4 src/sql/ob_sql_static.lib
python unittest/sql/engine/expr/run_ai_cpp_tests_windows.py build_release
```

The adapter reuses the production compiler flags and libraries and writes all
generated objects and executables under `build_release/ai_function_tests`.
It does not change the production build configuration or require GoogleTest.

## SQL integration tests

Install PyMySQL into your test Python environment, then start a locally built
seekdb on port 2883 with a dedicated data directory. On this checkout the
configuration names are `memory_budget` and `log_disk_size`; the latter must be
at least 2G for a non-embedded server.

```powershell
python -m pip install pymysql
python unittest/sql/engine/expr/test_ai_function_enhancement.py
```

The connection can be overridden with `SEEKDB_TEST_PORT`, `SEEKDB_TEST_USER`
and `SEEKDB_TEST_PASSWORD`. Run only against a disposable local test instance:
the suite creates/drops `ai_enhancement_regression`, `reg_*` AI resources, and
the `enhance_*` AI resources used by the supplied SQL tests.

The fixture binds an available loopback port. Tests inspect actual HTTP request
bodies, verify vectors against each original input, and inject malformed JSON,
truncated responses, HTTP errors, missing/duplicate indices and a failed second
batch. The larger batch case uses 1,153 rows with different models/dimensions
and NULLs. Rerank cases cross the 20-document request boundary and verify stable
ties and preservation of original object fields.

The suite also executes the statements in `ai_prompt.test` and
`ai_function_enhancement.test`, honoring their expected-error directives. This
is a PyMySQL adapter, **not** the native mysqltest runner: it does not compare
the `.result` files. It substitutes the fixture's local URL for `0.0.0.0:8000`.
Independent assertions check the six required behaviors and request mappings.

`ai_mock_service.py` can also run standalone on `0.0.0.0:8000`; its default
embedding dimension is 8. This is a local regression fixture, not the contest
organizer's original Mock AI Service.
