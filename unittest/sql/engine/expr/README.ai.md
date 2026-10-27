# AI function regression tests

These tests exercise the C++ SQL implementation. The Python service is only a
deterministic HTTP fixture; it never implements an SQL function or contacts a
real model.

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
