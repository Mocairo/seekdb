# FTS v18 integration validation

The integration starts at `f8417d0bd95b03c14fd770b7541965d7b60ea837` on
`fts-local-optimization`. It ports only the FTS changes from reference commits
`b21e005ee`, `d4fe87b82`, and `ef042e168`, as represented in reference snapshot
`f97428f9ec029e5fa329c7b2b16257a99c428055`, plus the validated v18 array-access
inlining and single-lookup charset comparisons. Other target-branch features
are retained. Uncommitted changes in the user's Windows checkout are excluded.

The changes reuse immutable built-in dictionaries, batch token and owned DOC_ID
processing, avoid scoring only where the score is unobservable, and simplify
count-only LIMIT plans. Generic scoring, collation and iterator fallbacks remain.
Shared comparison, JSON serialization, CRC and DDL-checksum paths have dedicated
regressions. The v18 accessors retain their assertions and debug checks; charset
comparison still delegates to the original collation callbacks.

All compilation and execution took place on the Linux server on 2026-10-29.
The integrated source was rebuilt with release/RelWithDebInfo, Clang 17, O2,
Unity, jemalloc and the existing CMake options. Tests used the newly built binary,
not the prior reference executable. The benchmark used a fresh database directory,
8 fixed physical CPU cores, memory_limit=8G and port 2887.

Validation passed:

- 137 C++ tests: 2 charset/array, 69 storage, 66 SQL tests; all three CTest modules.
- 10 relevance/Boolean/prepared-statement checks and 616 complete membership cases.
- 906 TOKENIZE, LIMIT and sorting result/error comparisons, including small-workarea sorts.
- 19 DDL completion, duplicate-key failure, index rebuild and later-DML checks.
- 736 complete token streams and 1472 detailed streams; eight concurrent connections match upstream.
- Complete MATCH results and scores match upstream; 403-term TAAT returns the same 11000 rows.
- One full official benchmark: ROWS=20000, BATCH=500, ROUNDS=3000,
  QUERY_ROUNDS=200, SAMPLES=3, WARMUP=30, SKIP_LOAD=0. Resource monitoring accepted
  the run: no build process, CPU throttling or OOM; background usage averaged 0.142 cores.

An external test-driver ordering error initially ran the large TAAT check before
its benchmark fixture existed (error 1146). It was moved after official data import
and passed. No product change was necessary. The original failure log is retained.
After compilation, only this task's completed build/cache file pages were advised
DONTNEED to restore memory headroom; database data and global caches were untouched.

`benchmark.txt` is the unedited official report. Its parent git_head and git_dirty
fields come from the unchanged script before committing; staged modifications are
not reflected in that script's dirty check. `validation.json` records the exact
compiled source and binary hashes and unchanged official input hashes.

| Metric | Integration run |
| --- | ---: |
| build_ik_all_sec | 4.386 |
| build_ik_content_sec | 3.439 |
| build_beng_en_sec | 1.634 |
| tokenize_ik_avg_ms | 0.1031 |
| tokenize_beng_avg_ms | 0.0627 |
| query_cn_avg_ms | 0.4055 |
| query_beng_avg_ms | 0.5281 |
| query_mixed_avg_ms | 0.4298 |
| query_limit_avg_ms | 0.2113 |

Official benchmark inputs, SQL, timing, scoring and fixed baseline are unchanged.
These server timings are not an official platform score. Earlier controlled
same-server measurements of isolated v18 averaged 50.40% lower category-weighted
time than upstream; that result must not be presented as a platform 50-point score.

Build and run the included C++ regressions after the normal release configuration:

```bash
cmake --build build_release --target sql_tests storage_tests share_tests -j8
ctest --test-dir build_release --output-on-failure
```

The standard FTS/global-index mysqltest suites also contain the added regression
fixtures. The unchanged official benchmark can be run with the parameters above,
then scored with tools/benchmark/fts_large_bench_score.py. Raw server evidence and
external differential runners are preserved in the accompanying integration archive.
